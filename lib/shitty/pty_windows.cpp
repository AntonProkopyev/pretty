/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00

#include "pty.h"
#include "startup.h"

#include <plt/fiber.h>
#include <plt/loop_wake.h>
#include <plt/platform.h>
#include <plt/poller.h>

#include <std/dbg/insist.h>
#include <std/mem/obj_pool.h>
#include <std/str/view.h>

#include <windows.h>

#include <algorithm>
#include <climits>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace stl;

namespace {
    constexpr size_t readQueueLimit = 4u << 20;
    constexpr size_t writeQueueLimit = 1u << 20;
    constexpr size_t blockSize = 16u << 10;

    struct WinHandle final {
        explicit WinHandle(HANDLE value_ = INVALID_HANDLE_VALUE)
            : value(value_)
        {
        }

        WinHandle(WinHandle&& other) noexcept
            : value(other.release())
        {
        }

        WinHandle& operator=(WinHandle&& other) noexcept {
            close();
            value = other.release();
            return *this;
        }

        ~WinHandle() noexcept {
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

        HPCON release() {
            const HPCON result = value;
            value = nullptr;
            return result;
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

    std::wstring wide(const char* text) {
        const size_t bytes = __builtin_strlen(text);
        if (bytes == 0) {
            return {};
        }
        STD_INSIST(bytes <= INT_MAX);
        const int length = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            text,
            static_cast<int>(bytes),
            nullptr,
            0
        );
        STD_INSIST(length > 0);
        std::wstring result(static_cast<size_t>(length), L'\0');
        STD_INSIST(MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            text,
            static_cast<int>(bytes),
            result.data(),
            length
        ) == length);
        return result;
    }

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
            } else if (character == L'"') {
                result.append(2 * slashes + 1, L'\\');
                result.push_back(L'"');
                slashes = 0;
            } else {
                result.append(slashes, L'\\');
                slashes = 0;
                result.push_back(character);
            }
        }
        result.append(2 * slashes, L'\\');
        result.push_back(L'"');
        return result;
    }

    std::wstring commandLine(const LaunchCommand& command) {
        std::wstring result;
        for (size_t index = 0; index != command.offsets.length(); ++index) {
            if (!result.empty()) {
                result.push_back(L' ');
            }
            result += quoteArgument(wide(command.argument(index)));
        }
        return result;
    }

    struct ChunkImpl final: PtyHandle::Chunk {
        explicit ChunkImpl(size_t size)
            : storage(size)
        {
        }

        void* data() override {
            return storage.data();
        }

        size_t length() override {
            return used;
        }

        Chunk* next() override {
            return next_;
        }

        std::vector<u8> storage;
        size_t used = 0;
        ChunkImpl* next_ = nullptr;
    };

    struct PtyImpl;

    struct PtyHandleImpl final: PtyHandle {
        PtyHandleImpl(PtyImpl& pty_, ObjPool& owner_)
            : pty(pty_)
            , owner(owner_)
        {
        }

        ~PtyHandleImpl() noexcept;

        PtyHandleImpl& initialize(const LaunchCommand& command);
        void readerLoop();
        void writerLoop();
        void waiterLoop();
        void wakeFibers();
        bool removeRead(ChunkImpl*& chunks);
        bool removeWrite(ChunkImpl*& chunk);

        void resize(const PtySize& size) override;
        void engage() override;
        Chunk* allocate(size_t len) override;
        void send(Chunk* chunk, size_t len) override;
        Chunk* acquire() override;
        void release(Chunk* chunks) override;
        PtyExitResult exitResult() const override;

        PtyImpl& pty;
        ObjPool& owner;
        PseudoConsole console;
        WinHandle inputWrite;
        WinHandle outputRead;
        WinHandle process;
        WinHandle processThread;
        std::thread reader;
        std::thread writer;
        std::thread waiter;
        mutable std::mutex mutex;
        std::condition_variable condition;
        ChunkImpl* readHead = nullptr;
        ChunkImpl* readTail = nullptr;
        ChunkImpl* writeHead = nullptr;
        ChunkImpl* writeTail = nullptr;
        size_t readBytes = 0;
        size_t writeBytes = 0;
        plt::Fiber* waitingReader = nullptr;
        plt::Fiber* waitingWriter = nullptr;
        PtyExitResult exit_;
        bool eof = false;
        bool stopping = false;
        bool forced = false;
        bool engaged_ = false;
    };

    struct PtyImpl final: Pty, plt::TimerCallback {
        PtyImpl(ObjPool& owner_, plt::Scheduler& scheduler_, plt::Platform& platform_)
            : owner(owner_)
            , scheduler(scheduler_)
            , platform(platform_)
        {
            doorbell = platform.createLoopWake(owner, *this);
        }

        PtyHandle* spawn(ObjPool& childOwner, const LaunchCommand& command) override {
            PtyHandleImpl* const handle = childOwner.make<PtyHandleImpl>(
                *this,
                childOwner
            );
            return &handle->initialize(command);
        }

        void ready() override {
            std::vector<PtyHandleImpl*> current;
            {
                std::lock_guard lock(registryMutex);
                current = handles;
            }
            for (PtyHandleImpl* const handle : current) {
                handle->wakeFibers();
            }
        }

        bool attach(PtyHandleImpl& handle) {
            std::lock_guard lock(registryMutex);
            handles.push_back(&handle);
            return true;
        }

        bool detach(PtyHandleImpl& handle) {
            std::lock_guard lock(registryMutex);
            const auto found = std::find(handles.begin(), handles.end(), &handle);
            if (found == handles.end()) {
                return false;
            }
            handles.erase(found);
            return true;
        }

        ObjPool& owner;
        plt::Scheduler& scheduler;
        plt::Platform& platform;
        plt::LoopWake* doorbell = nullptr;
        std::mutex registryMutex;
        std::vector<PtyHandleImpl*> handles;
    };

    PtyHandleImpl& PtyHandleImpl::initialize(const LaunchCommand& command) {
        WinHandle inputRead;
        WinHandle outputWrite;
        STD_INSIST(CreatePipe(&inputRead.value, &inputWrite.value, nullptr, 0) != 0);
        STD_INSIST(CreatePipe(&outputRead.value, &outputWrite.value, nullptr, 0) != 0);
        STD_INSIST(console.create({80, 24}, inputRead.value, outputWrite.value));
        Attributes attributes;
        STD_INSIST(attributes.initialize(console.value));
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attributes.list;
        PROCESS_INFORMATION information{};
        std::wstring line = commandLine(command);
        STD_INSIST(CreateProcessW(
            nullptr,
            line.data(),
            nullptr,
            nullptr,
            FALSE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
            nullptr,
            nullptr,
            &startup.StartupInfo,
            &information
        ) != 0);
        process = WinHandle(information.hProcess);
        processThread = WinHandle(information.hThread);
        inputRead.close();
        outputWrite.close();
        pty.attach(*this);
        reader = std::thread([this] { readerLoop(); });
        writer = std::thread([this] { writerLoop(); });
        waiter = std::thread([this] { waiterLoop(); });
        return *this;
    }

    PtyHandleImpl::~PtyHandleImpl() noexcept {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        condition.notify_all();
        if (writer.joinable()) {
            CancelSynchronousIo(writer.native_handle());
        }
        inputWrite.close();
        if (writer.joinable()) {
            writer.join();
        }
        if (process.valid() && WaitForSingleObject(process.value, 500) == WAIT_TIMEOUT) {
            {
                std::lock_guard lock(mutex);
                forced = true;
            }
            TerminateProcess(process.value, 0xdead);
            WaitForSingleObject(process.value, 5000);
        }
        if (waiter.joinable()) {
            waiter.join();
        }
        console.close();
        if (reader.joinable()) {
            reader.join();
        }
        pty.detach(*this);
        release(readHead);
        release(writeHead);
    }

    void PtyHandleImpl::readerLoop() {
        for (;;) {
            ChunkImpl* block = new ChunkImpl(blockSize);
            DWORD count = 0;
            const BOOL read = ReadFile(
                outputRead.value,
                block->storage.data(),
                static_cast<DWORD>(block->storage.size()),
                &count,
                nullptr
            );
            if (read == 0 || count == 0) {
                delete block;
                break;
            }
            block->used = count;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [this, count] {
                    return stopping || readBytes + count <= readQueueLimit;
                });
                if (stopping) {
                    delete block;
                    break;
                }
                if (readTail == nullptr) {
                    readHead = block;
                } else {
                    readTail->next_ = block;
                }
                readTail = block;
                readBytes += count;
            }
            pty.doorbell->signal();
        }
        {
            std::lock_guard lock(mutex);
            eof = true;
        }
        pty.doorbell->signal();
    }

    void PtyHandleImpl::waiterLoop() {
        WaitForSingleObject(process.value, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(process.value, &code);
        HPCON closing = nullptr;
        {
            std::lock_guard lock(mutex);
            exit_ = {
                .state = forced ? PtyExitState::Terminated : PtyExitState::Exited,
                .code = code,
            };
            closing = console.release();
        }
        if (closing != nullptr) {
            ClosePseudoConsole(closing);
        }
        pty.doorbell->signal();
    }

    void PtyHandleImpl::writerLoop() {
        for (;;) {
            ChunkImpl* block = nullptr;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [this] {
                    return stopping || writeHead != nullptr;
                });
                if (stopping && writeHead == nullptr) {
                    break;
                }
                removeWrite(block);
            }
            size_t offset = 0;
            while (offset != block->used) {
                DWORD written = 0;
                if (WriteFile(
                        inputWrite.value,
                        block->storage.data() + offset,
                        static_cast<DWORD>(block->used - offset),
                        &written,
                        nullptr
                    ) == 0) {
                    break;
                }
                offset += written;
            }
            delete block;
            pty.doorbell->signal();
        }
    }

    bool PtyHandleImpl::removeWrite(ChunkImpl*& chunk) {
        if (writeHead == nullptr) {
            return false;
        }
        chunk = writeHead;
        writeHead = writeHead->next_;
        if (writeHead == nullptr) {
            writeTail = nullptr;
        }
        chunk->next_ = nullptr;
        writeBytes -= chunk->used;
        condition.notify_all();
        return true;
    }

    void PtyHandleImpl::wakeFibers() {
        plt::Fiber* readerFiber = nullptr;
        plt::Fiber* writerFiber = nullptr;
        {
            std::lock_guard lock(mutex);
            if (readHead != nullptr || eof) {
                readerFiber = waitingReader;
                waitingReader = nullptr;
            }
            if (writeBytes < writeQueueLimit) {
                writerFiber = waitingWriter;
                waitingWriter = nullptr;
            }
        }
        if (readerFiber != nullptr) {
            readerFiber->wake();
        }
        if (writerFiber != nullptr) {
            writerFiber->wake();
        }
    }

void PtyHandleImpl::resize(const PtySize& size) {
    STD_INSIST(size.columns > 0 && size.rows > 0);
    std::lock_guard lock(mutex);
    if (console.value == nullptr || exit_.state != PtyExitState::Running) {
        return;
    }
    (void)console.resize({
        static_cast<SHORT>(std::min<u32>(size.columns, SHRT_MAX)),
        static_cast<SHORT>(std::min<u32>(size.rows, SHRT_MAX)),
    });
}

    void PtyHandleImpl::engage() {
        engaged_ = true;
    }

    PtyHandle::Chunk* PtyHandleImpl::allocate(size_t len) {
        return new ChunkImpl(std::min(len, blockSize));
    }

    void PtyHandleImpl::send(Chunk* chunk, size_t len) {
        auto* const block = static_cast<ChunkImpl*>(chunk);
        STD_INSIST(len <= block->storage.size());
        block->used = len;
        for (;;) {
            {
                std::lock_guard lock(mutex);
                if (stopping) {
                    delete block;
                    return;
                }
                if (writeBytes + len <= writeQueueLimit) {
                    if (writeTail == nullptr) {
                        writeHead = block;
                    } else {
                        writeTail->next_ = block;
                    }
                    writeTail = block;
                    writeBytes += len;
                    condition.notify_all();
                    return;
                }
                waitingWriter = pty.scheduler.current();
            }
            STD_INSIST(waitingWriter != nullptr);
            waitingWriter->park();
        }
    }

    PtyHandle::Chunk* PtyHandleImpl::acquire() {
        STD_INSIST(engaged_);
        for (;;) {
            ChunkImpl* chunks = nullptr;
            {
                std::lock_guard lock(mutex);
                if (removeRead(chunks)) {
                    return chunks;
                }
                if (eof) {
                    return nullptr;
                }
                waitingReader = pty.scheduler.current();
            }
            STD_INSIST(waitingReader != nullptr);
            waitingReader->park();
        }
    }

    bool PtyHandleImpl::removeRead(ChunkImpl*& chunks) {
        if (readHead == nullptr) {
            return false;
        }
        chunks = readHead;
        readHead = nullptr;
        readTail = nullptr;
        readBytes = 0;
        condition.notify_all();
        return true;
    }

    void PtyHandleImpl::release(Chunk* chunks) {
        while (chunks != nullptr) {
            Chunk* const next = chunks->next();
            delete static_cast<ChunkImpl*>(chunks);
            chunks = next;
        }
        condition.notify_all();
    }

    PtyExitResult PtyHandleImpl::exitResult() const {
        std::lock_guard lock(mutex);
        return exit_;
    }
}

Pty* createPty(ObjPool& owner, plt::Scheduler& scheduler, plt::Platform* platform) {
    STD_INSIST(platform != nullptr);
    return owner.make<PtyImpl>(owner, scheduler, *platform);
}
