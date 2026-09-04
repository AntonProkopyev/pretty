/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "brand.h"
#include "composer.h"
#include "application.h"

#include <lib/vterm/fatal.h>
#include <lib/vterm/vt_headless.h>

#ifdef SHITTY_HEAP_PROFILE
    #include "heap_profile.h"
#endif

#include <std/ios/in_fd.h>
#include <std/ios/sys.h>
#include <std/lib/buffer.h>
#include <std/lib/vector.h>
#include <std/mem/obj_pool.h>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
    #include <shellapi.h>
    #include <shlobj.h>
    #include <cstdio>
    #include <string>
    #include <vector>
#endif
#include <std/str/builder.h>
#include <std/str/view.h>
#include <std/sys/fd.h>
#include <std/sys/crt.h>
#include <std/sys/fs.h>
#include <std/sys/throw.h>

#include <string.h>
#include <exception>

#include <fcntl.h>

using namespace stl;

namespace {
    static constexpr size_t perfFeedChunk = 8 * 1024;

    struct PerfFile {
        size_t pathOffset;
    };

    static void writeRepeated(ZeroCopyOutput& output, u8 byte, size_t count) {
        if (count == 0) {
            return;
        }
        u8* const bytes = static_cast<u8*>(output.imbue(count).ptr);
        memset(bytes, byte, count);
        output.commit(count);
    }

    static void writeTenths(ZeroCopyOutput& output, double value) {
        const u64 tenths = (u64)(value * 10 + 0.5);
        output << tenths / 10 << StringView(u8".") << tenths % 10;
    }

    static void showPerfProgress(size_t done, size_t total, size_t bytes, u64 startedUs) {
        constexpr size_t width = 40;
        const size_t filled = total == 0 ? width : done * width / total;
        const double elapsed = (double)(monotonicNowUs() - startedUs) / 1e6;
        const double mib = bytes / (1024.0 * 1024.0);
        const double mibPerSecond = elapsed > 0 ? mib / elapsed : 0;
        OutBuf output(stderrStream());
        output << StringView(u8"\r[");
        writeRepeated(output, u8'#', filled);
        writeRepeated(output, u8' ', width - filled);
        output << StringView(u8"] ") << (u64)(done) << StringView(u8"/") << (u64)(total) << StringView(u8" ");
        writeTenths(output, mib);
        output << StringView(u8" MiB, ");
        writeTenths(output, mibPerSecond);
        output << StringView(u8" MiB/s") << flsH;
    }

    static int runPerf(Brand& brand, int argc, char* argv[]) {
        if (argc < 3) {
            StringBuilder message;
            message << StringView(u8"usage: ") << brand.executableName() << StringView(u8" perf DIRECTORY...");
            raiseError(StringView(message));
        }

        Buffer paths;
        Vector<PerfFile> files;
        for (int index = 2; index < argc; ++index) {
            const StringView directory(argv[index]);
            listDir(directory, [&](const TPathInfo& entry) {
                if (!entry.isDir) {
                    const PerfFile file{.pathOffset = paths.used()};
                    paths.append(directory.data(), directory.length());
                    if (directory.empty() || directory.back() != '/') {
                        const u8 slash = '/';
                        paths.append(&slash, 1);
                    }
                    paths.append(entry.item.data(), entry.item.length());
                    const u8 zero = 0;
                    paths.append(&zero, 1);
                    files.pushBack(file);
                }
            });
        }

        ObjPool::Ref pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr(), brand);
        VtermHeadless* vterm = VtermHeadless::create(*composer.pool, *composer.vtConfig.config, nullptr);
        Buffer data;
        size_t bytes = 0;
        const u64 started = monotonicNowUs();
        showPerfProgress(0, files.length(), bytes, started);
        for (size_t index = 0; index < files.length(); ++index) {
            const char* path = (const char*)paths.data() + files[index].pathOffset;
            const int rawFd = open(path, O_RDONLY);
            if (rawFd < 0) {
                Errno().raise(StringBuilder() << StringView(u8"cannot open ") << StringView(path));
            }

            ScopedFD fd(rawFd);
            data.reset();
            FDInput(fd).readAll(data);
            const u8* input = (const u8*)data.data();
            size_t remaining = data.used();
            while (remaining != 0) {
                const size_t length = remaining < perfFeedChunk ? remaining : perfFeedChunk;
                vterm->feed(input, length);
                input += length;
                remaining -= length;
            }
            bytes += data.used();
            if ((index + 1) % 256 == 0 || index + 1 == files.length()) {
                showPerfProgress(index + 1, files.length(), bytes, started);
            }
        }
        sysE << endL;
#ifdef SHITTY_HEAP_PROFILE
        dumpHeapProfile();
#endif
        return 0;
    }
}

int runMain(Brand& brand, int argc, char* argv[]) {
    int status = 1;
    try {
#ifdef SHITTY_HEAP_PROFILE
        initializeHeapProfile();
#endif
        if (argc > 1 && StringView(argv[1]) == StringView(u8"perf")) {
            status = runPerf(brand, argc, argv);
        } else {
            ObjPool::Ref pool = ObjPool::fromMemory();
            Composer& composer = *pool->make<Composer>(pool.mutPtr(), brand);
            composer.application = Application::create(composer);
            status = composer.application->run(argc, argv);
#ifdef SHITTY_HEAP_PROFILE
            dumpHeapProfile();
#endif
        }
    } catch (Exception& error) {
        const StringView message = error.description();
        sysE << StringView(u8"Error: ") << message << endL;
    }
    return status;
}

#if defined(_WIN32)
namespace {
    bool diagnosticArguments(const std::vector<std::string>& arguments) {
        if (arguments.size() < 2) {
            return false;
        }
        const std::string& option = arguments[1];
        return option == "-v" || option == "-version"
            || option == "-help" || option == "-listres";
    }

    bool redirectWindowsDiagnostics(Brand& brand, bool attachParent) {
        if (attachParent) {
            AttachConsole(ATTACH_PARENT_PROCESS);
            if (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) != FILE_TYPE_CHAR) {
                FreeConsole();
                AllocConsole();
            }
            return _wfreopen(L"CONOUT$", L"w", stdout) != nullptr
                && _wfreopen(L"CONOUT$", L"w", stderr) != nullptr;
        }
        PWSTR root = nullptr;
        if (FAILED(SHGetKnownFolderPath(
                FOLDERID_LocalAppData,
                KF_FLAG_DEFAULT,
                nullptr,
                &root
            ))) {
            return false;
        }
        std::wstring directory(root);
        CoTaskMemFree(root);
        directory += L"\\";
        for (const u8 byte : brand.identifier()) {
            directory.push_back(static_cast<wchar_t>(byte));
        }
        CreateDirectoryW(directory.c_str(), nullptr);
        const std::wstring log = directory + L"\\startup.log";
        return _wfreopen(log.c_str(), L"w", stdout) != nullptr
            && _wfreopen(log.c_str(), L"w", stderr) != nullptr;
    }
}

int runWindowsMain(Brand& brand) {
    int count = 0;
    wchar_t** const wideArguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wideArguments == nullptr || count <= 0) {
        return 1;
    }
    std::vector<std::string> encoded(static_cast<size_t>(count));
    std::vector<char*> arguments(static_cast<size_t>(count) + 1, nullptr);
    for (int index = 0; index != count; ++index) {
        const int size = WideCharToMultiByte(
            CP_UTF8,
            0,
            wideArguments[index],
            -1,
            nullptr,
            0,
            nullptr,
            nullptr
        );
        if (size <= 0) {
            LocalFree(wideArguments);
            return 1;
        }
        encoded[index].resize(static_cast<size_t>(size));
        if (WideCharToMultiByte(
                CP_UTF8,
                0,
                wideArguments[index],
                -1,
                encoded[index].data(),
                size,
                nullptr,
                nullptr
            ) != size) {
            LocalFree(wideArguments);
            return 1;
        }
        encoded[index].pop_back();
        arguments[index] = encoded[index].data();
    }
    LocalFree(wideArguments);
    redirectWindowsDiagnostics(brand, diagnosticArguments(encoded));
    if (encoded.size() > 1
            && (encoded[1] == "-v" || encoded[1] == "-version")) {
        const StringView name = brand.displayName();
        std::fwrite(name.data(), 1, name.length(), stdout);
        std::fprintf(stdout, " %s\nCopyright (C) 2026 ", SHITTY_VERSION);
        std::fwrite(name.data(), 1, name.length(), stdout);
        std::fputs(" team\n", stdout);
        std::fflush(stdout);
        return 0;
    }
    return runMain(brand, count, arguments.data());
}
#endif
