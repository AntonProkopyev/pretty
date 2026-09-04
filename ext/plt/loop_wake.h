#pragma once

namespace stl {
    class ObjPool;
}

namespace plt {
    struct PollerLoop;
    struct TimerCallback;

    // A cross-thread doorbell for the platform loop. signal() may be called
    // from any thread and coalesces freely; the callback runs on the
    // platform thread at least once after each signal. A doorbell lives for
    // the process: there is deliberately no teardown.
    struct LoopWake {
        virtual void signal() = 0;

        // The portable pipe-backed doorbell for poll-driven backends;
        // Platform::createLoopWake picks the loop's native mechanism.
        static LoopWake* create(stl::ObjPool& owner, PollerLoop& poller, TimerCallback& callback);
    };
}
