#pragma once

#include "poller.h"

namespace stl {
    class ObjPool;
}

namespace plt {
    // The portable poll(2) loop shared by the Wayland and headless
    // platforms; a run loop drives it with dispatchTimers/wait rounds.
    // The Cocoa backend borrows its timer half and never calls wait().
    struct PollerLoop: public Poller {
        virtual void wait(u64 monotonicDeadline) = 0;
        virtual void dispatchTimers() = 0;
        virtual u64 nextDeadline() const = 0;
#if defined(_WIN32)
        virtual PollerLoop& armNative(void* handle, TimerCallback& callback) = 0;
#endif

        static PollerLoop* create(stl::ObjPool& owner);
    };
}
