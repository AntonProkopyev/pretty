#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <future>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {
    struct Handle final {
        explicit Handle(HANDLE value_ = INVALID_HANDLE_VALUE)
            : value(value_)
        {
        }

        Handle(Handle&& other) noexcept
            : value(other.release())
        {
        }

        Handle& operator=(Handle&& other) noexcept {
            close();
            value = other.release();
            return *this;
        }

        ~Handle() noexcept {
            close();
        }

        bool valid() const {
            return value != nullptr && value != INVALID_HANDLE_VALUE;
        }

        HANDLE release() {
            const HANDLE result = value;
            value = INVALID_HANDLE_VALUE;
            return result;
        }

        bool close() {
            if (!valid()) {
                return false;
            }
            CloseHandle(value);
            value = INVALID_HANDLE_VALUE;
            return true;
        }

        HANDLE value;
    };

    struct PseudoConsole final {
        bool create(COORD size, HANDLE input, HANDLE output) {
            return SUCCEEDED(CreatePseudoConsole(size, input, output, 0, &value));
        }

        bool resize(COORD size) const {
            return SUCCEEDED(ResizePseudoConsole(value, size));
        }

        bool close() {
            if (value == nullptr) {
                return false;
            }
            ClosePseudoConsole(value);
            value = nullptr;
            return true;
        }

        ~PseudoConsole() noexcept {
            close();
        }

        HPCON value = nullptr;
    };

    struct Attributes final {
        bool initialize(HPCON console) {
            SIZE_T bytes = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
            storage.resize(bytes);
            list = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
            return InitializeProcThreadAttributeList(list, 1, 0, &bytes) != 0
                && UpdateProcThreadAttribute(
                    list,
                    0,
                    PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                    console,
                    sizeof(console),
                    nullptr,
                    nullptr
                ) != 0;
        }

        ~Attributes() noexcept {
            if (list != nullptr) {
                DeleteProcThreadAttributeList(list);
            }
        }

        std::vector<unsigned char> storage;
        PPROC_THREAD_ATTRIBUTE_LIST list = nullptr;
    };

    std::wstring quoteArgument(const std::wstring& argument) {
        if (!argument.empty()
                && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
            return argument;
        }
        std::wstring result(1, L'"');
        size_t slashes = 0;
        for (const wchar_t character : argument) {
            if (character == L'\\') {
                ++slashes;
                continue;
            }
            if (character == L'"') {
                result.append(2 * slashes + 1, L'\\');
                result.push_back(L'"');
                slashes = 0;
                continue;
            }
            result.append(slashes, L'\\');
            slashes = 0;
            result.push_back(character);
        }
        result.append(2 * slashes, L'\\');
        result.push_back(L'"');
        return result;
    }

    std::wstring commandLine(const std::vector<std::wstring>& arguments) {
        std::wstring result;
        for (const std::wstring& argument : arguments) {
            if (!result.empty()) {
                result.push_back(L' ');
            }
            result += quoteArgument(argument);
        }
        return result;
    }

    bool writeReceipt(const char* content) {
        wchar_t path[32768];
        const DWORD length = GetEnvironmentVariableW(
            L"SHITTY_CONPTY_RECEIPT",
            path,
            static_cast<DWORD>(std::size(path))
        );
        if (length == 0 || length >= std::size(path)) {
            return false;
        }
        Handle file(CreateFileW(
            path,
            GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        ));
        DWORD written = 0;
        const DWORD size = static_cast<DWORD>(std::char_traits<char>::length(content));
        return file.valid()
            && WriteFile(file.value, content, size, &written, nullptr) != 0
            && written == size;
    }

    struct SessionResult final {
        DWORD exitCode = 0;
        bool timedOut = false;
        bool resized = false;
        std::string output;
    };

    SessionResult execute(
        const std::vector<std::wstring>& arguments,
        const std::string& input,
        DWORD timeout,
        bool resize
    ) {
        SessionResult result;
        Handle inputRead;
        Handle inputWrite;
        Handle outputRead;
        Handle outputWrite;
        if (CreatePipe(&inputRead.value, &inputWrite.value, nullptr, 0) == 0
                || CreatePipe(&outputRead.value, &outputWrite.value, nullptr, 0) == 0) {
            result.exitCode = ERROR_BROKEN_PIPE;
            return result;
        }
        PseudoConsole console;
        if (!console.create({80, 24}, inputRead.value, outputWrite.value)) {
            result.exitCode = ERROR_INVALID_HANDLE;
            return result;
        }
        Attributes attributes;
        if (!attributes.initialize(console.value)) {
            result.exitCode = ERROR_BAD_ENVIRONMENT;
            return result;
        }
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attributes.list;
        PROCESS_INFORMATION processInfo{};
        std::wstring command = commandLine(arguments);
        if (CreateProcessW(
                nullptr,
                command.data(),
                nullptr,
                nullptr,
                FALSE,
                EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                nullptr,
                nullptr,
                &startup.StartupInfo,
                &processInfo
            ) == 0) {
            result.exitCode = GetLastError();
            return result;
        }
        Handle process(processInfo.hProcess);
        Handle processThread(processInfo.hThread);
        inputRead.close();
        outputWrite.close();

        constexpr size_t outputLimit = 4u << 20;
        std::thread reader([&result, handle = outputRead.value] {
            char block[16384];
            DWORD count = 0;
            while (ReadFile(handle, block, sizeof(block), &count, nullptr) != 0
                    && count != 0) {
                const size_t room = outputLimit - result.output.length();
                result.output.append(block, std::min<size_t>(count, room));
            }
        });
        std::thread writer([payload = input, handle = inputWrite.value] {
            size_t offset = 0;
            while (offset != payload.length()) {
                DWORD written = 0;
                const DWORD request = static_cast<DWORD>(std::min<size_t>(
                    payload.length() - offset,
                    16384
                ));
                if (WriteFile(
                        handle,
                        payload.data() + offset,
                        request,
                        &written,
                        nullptr
                    ) == 0) {
                    break;
                }
                offset += written;
            }
        });
        if (resize) {
            result.resized = console.resize({120, 40});
        }
        writer.join();
        const DWORD wait = WaitForSingleObject(process.value, timeout);
        result.timedOut = wait == WAIT_TIMEOUT;
        if (result.timedOut) {
            TerminateProcess(process.value, 0xdead);
            WaitForSingleObject(process.value, 5000);
        }
        GetExitCodeProcess(process.value, &result.exitCode);
        inputWrite.close();
        console.close();
        reader.join();
        return result;
    }

    std::vector<std::wstring> commandPrompt(const std::wstring& script) {
        return {
            L"cmd.exe",
            L"/D",
            L"/Q",
            L"/V:ON",
            L"/C",
            script,
        };
    }
}

int main() {
    if (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) != FILE_TYPE_CHAR) {
        FreeConsole();
        AllocConsole();
    }
    wchar_t perfPath[32768];
    const DWORD perfLength = GetEnvironmentVariableW(
        L"SHITTY_CONPTY_PERF_FILE",
        perfPath,
        static_cast<DWORD>(std::size(perfPath))
    );
    if (perfLength != 0 && perfLength < std::size(perfPath)) {
        wchar_t perfCommand[32768];
        const DWORD commandLength = GetEnvironmentVariableW(
            L"SHITTY_CONPTY_PERF_COMMAND",
            perfCommand,
            static_cast<DWORD>(std::size(perfCommand))
        );
        Handle file(CreateFileW(
            perfPath,
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        ));
        LARGE_INTEGER bytes{};
        if (!file.valid() || GetFileSizeEx(file.value, &bytes) == 0) {
            return 5;
        }
        const auto started = std::chrono::steady_clock::now();
        const SessionResult perf = execute(
            commandPrompt(
                commandLength != 0 && commandLength < std::size(perfCommand)
                    ? std::wstring(perfCommand)
                    : L"type \"" + std::wstring(perfPath) + L"\""
            ),
            {},
            120000,
            false
        );
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started
        ).count();
        std::printf(
            "conpty_perf bytes=%lld seconds=%.6f MiB/s=%.3f exit=%lu\n",
            bytes.QuadPart,
            seconds,
            static_cast<double>(bytes.QuadPart) / (1 << 20) / seconds,
            perf.exitCode
        );
        return perf.exitCode == 0 ? 0 : 6;
    }
    const SessionResult unicode = execute(
        commandPrompt(
            L"chcp 65001>nul & set /p line= & echo OUT:!line! & exit /b 7"
        ),
        "Привет 🙂\r\n",
        10000,
        true
    );
    if (unicode.exitCode != 7
            || !unicode.resized
            || unicode.output.find("OUT:Привет 🙂") == std::string::npos) {
        std::printf("unicode exit=%lu bytes=%zu resize=%d\n", unicode.exitCode, unicode.output.length(), unicode.resized);
        std::fwrite(unicode.output.data(), 1, unicode.output.length(), stdout);
        return 1;
    }

    const SessionResult flood = execute(
        commandPrompt(
            L"for /L %i in (1,1,65536) do @echo 0123456789abcdef"
        ),
        {},
        15000,
        false
    );
    if (flood.exitCode != 0 || flood.output.length() < 1048576) {
        std::printf("flood exit=%lu bytes=%zu\n", flood.exitCode, flood.output.length());
        return 2;
    }

    auto one = std::async(std::launch::async, [] {
        return execute(
            commandPrompt(L"echo session one & exit /b 11"),
            {},
            10000,
            false
        );
    });
    auto two = std::async(std::launch::async, [] {
        return execute(
            commandPrompt(L"echo session two & exit /b 12"),
            {},
            10000,
            false
        );
    });
    const SessionResult first = one.get();
    const SessionResult second = two.get();
    if (first.exitCode != 11 || second.exitCode != 12) {
        return 3;
    }

    const SessionResult forced = execute(
        commandPrompt(L"for /L %i in (0,0,0) do @echo zzzzzzzzzzzzzzzz"),
        {},
        250,
        false
    );
    if (!forced.timedOut || forced.exitCode != 0xdead) {
        return 4;
    }
    std::printf(
        "unicode=%zu flood=%zu multi=2 forced=%lu\n",
        unicode.output.length(),
        flood.output.length(),
        forced.exitCode
    );
    char receipt[160];
    std::snprintf(
        receipt,
        sizeof(receipt),
        "unicode=%zu flood=%zu multi=2 forced=%lu\n",
        unicode.output.length(),
        flood.output.length(),
        forced.exitCode
    );
    writeReceipt(receipt);
    return 0;
}
