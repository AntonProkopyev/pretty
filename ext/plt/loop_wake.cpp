#include "loop_wake.h"

#include "poller.h"
#include "poller_loop.h"

#include <std/dbg/insist.h>
#include <std/lib/buffer.h>
#include <std/mem/obj_pool.h>
#include <std/str/view.h>
#include <std/sys/throw.h>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
#else
    #include <errno.h>
    #include <fcntl.h>
    #include <unistd.h>
#endif

using namespace plt;
using namespace stl;

namespace {
#if defined(_WIN32)
    struct EventLoopWake final: public LoopWake {
        explicit EventLoopWake(HANDLE event_)
            : event(event_)
        {
        }

        ~EventLoopWake() noexcept {
            CloseHandle(event);
        }

        EventLoopWake(const EventLoopWake&) = delete;
        EventLoopWake& operator=(const EventLoopWake&) = delete;
        EventLoopWake(EventLoopWake&&) = delete;
        EventLoopWake& operator=(EventLoopWake&&) = delete;

        void signal() override {
            STD_INSIST(SetEvent(event) != 0);
        }

        HANDLE const event;
    };
#else
    struct PipeLoopWake final: public LoopWake, public PollCallback {
        PipeLoopWake(Poller& poller_, TimerCallback& callback_)
            : poller(poller_)
            , callback(callback_)
        {
            int fds[2];
            if (::pipe(fds) != 0) {
                Errno(errno).raise(StringView(u8"loop wake pipe failed"));
            }
            for (const int fd : fds) {
                ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
                ::fcntl(fd, F_SETFD, FD_CLOEXEC);
            }
            readFd = fds[0];
            writeFd = fds[1];
            waiter.fd = {
                .fd = readFd,
                .flags = PollFlag::In,
            };
            waiter.callback = this;
            poller.arm(waiter);
        }

        void signal() override {
            const u8 byte = 0;
            // A full pipe already holds a pending wake.
            (void)!::write(writeFd, &byte, 1);
        }

        void ready(PollFD) override {
            u8 pending[256];
            while (::read(readFd, pending, sizeof(pending)) > 0) {
            }
            // Waits are one-shot: re-arm before the callback so a signal
            // sent while it runs is not lost.
            poller.arm(waiter);
            callback.ready();
        }

        Poller& poller;
        TimerCallback& callback;
        PollWaiter waiter;
        int readFd = -1;
        int writeFd = -1;
    };
#endif
}

LoopWake* LoopWake::create(ObjPool& owner, PollerLoop& poller, TimerCallback& callback) {
#if defined(_WIN32)
    HANDLE const event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    STD_INSIST(event != nullptr);
    poller.armNative(event, callback);
    return owner.make<EventLoopWake>(event);
#else
    return owner.make<PipeLoopWake>(poller, callback);
#endif
}
