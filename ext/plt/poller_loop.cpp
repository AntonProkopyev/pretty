#include "poller_loop.h"

#include <std/sys/crt.h>
#include <std/str/view.h>
#include <std/sys/throw.h>
#include <std/alg/minmax.h>
#include <std/alg/xchg.h>
#include <std/dbg/insist.h>
#include <std/lib/list.h>
#include <std/lib/vector.h>
#include <std/map/treap.h>
#include <std/map/treap_node.h>
#include <std/mem/obj_list.h>
#include <std/mem/obj_pool.h>

#include <climits>
#include <cstdint>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
#else
    #include <cerrno>
    #include <poll.h>
#endif

using namespace plt;
using namespace stl;

namespace {
    struct TimerEntry final: public TreapNode {
        TimerEntry(TimerCallback* callback_, u64 deadline_, u64 sequence_)
            : callback(callback_)
            , deadline(deadline_)
            , sequence(sequence_)
            , nextDue(nullptr)
        {
        }

        TimerCallback* callback;
        u64 deadline;
        u64 sequence;
        TimerEntry* nextDue;
    };

    // Deadline-ordered timers with the Poller contract: scheduling a callback
    // replaces its previous deadline, cancel() also stops a timer that is due
    // in the current dispatch round, and a timer armed from a callback never
    // fires in the round which armed it.
    class TimerQueue {
        struct Order final: public Treap {
            bool cmp(void* a, void* b) const noexcept override;
        };

        TimerEntry* takeScheduled(TimerCallback& callback);

        Order order_;
        ObjList<TimerEntry> entries_;
        TimerEntry* due_ = nullptr;
        u64 nextSequence_ = 1;

    public:
        explicit TimerQueue(ObjPool& owner);

        void schedule(u64 deadline, TimerCallback& callback);
        void cancel(TimerCallback& callback);
        // Invokes every callback whose deadline is at or before now.
        void dispatch(u64 now);
        u64 nextDeadline() const;
    };

    bool TimerQueue::Order::cmp(void* a, void* b) const noexcept {
        const TimerEntry* const left = (const TimerEntry*)(a);
        const TimerEntry* const right = (const TimerEntry*)(b);
        if (left->deadline != right->deadline) {
            return left->deadline < right->deadline;
        }
        return left->sequence < right->sequence;
    }

    TimerQueue::TimerQueue(ObjPool& owner)
        : entries_(&owner)
    {
    }

    TimerEntry* TimerQueue::takeScheduled(TimerCallback& callback) {
        TimerEntry* found = nullptr;
        order_.visit([&found, &callback](TreapNode* node) {
            TimerEntry* const entry = (TimerEntry*)(node);
            if (entry->callback == &callback) {
                found = entry;
            }
        });
        if (found != nullptr) {
            order_.remove(found);
            return found;
        }
        for (TimerEntry** current = &due_; *current != nullptr; current = &(*current)->nextDue) {
            if ((*current)->callback == &callback) {
                found = *current;
                *current = found->nextDue;
                found->nextDue = nullptr;
                return found;
            }
        }
        return nullptr;
    }

    void TimerQueue::schedule(u64 deadline, TimerCallback& callback) {
        TimerEntry* entry = takeScheduled(callback);
        if (entry == nullptr) {
            entry = entries_.make(&callback, deadline, nextSequence_++);
        } else {
            entry->deadline = deadline;
            entry->sequence = nextSequence_++;
        }
        order_.insert(entry);
    }

    void TimerQueue::cancel(TimerCallback& callback) {
        if (TimerEntry* const entry = takeScheduled(callback)) {
            entries_.release(entry);
        }
    }

    void TimerQueue::dispatch(u64 now) {
        // Collect the due entries first so timers armed from a callback wait for
        // the next round instead of firing while this one is still running.
        TimerEntry** tail = &due_;
        while (*tail != nullptr) {
            tail = &(*tail)->nextDue;
        }
        while (TimerEntry* const entry = (TimerEntry*)(order_.min())) {
            if (entry->deadline > now) {
                break;
            }
            order_.remove(entry);
            entry->nextDue = nullptr;
            *tail = entry;
            tail = &entry->nextDue;
        }
        while (due_ != nullptr) {
            TimerEntry* const entry = due_;
            due_ = entry->nextDue;
            TimerCallback* const callback = entry->callback;
            entries_.release(entry);
            callback->ready();
        }
    }

    u64 TimerQueue::nextDeadline() const {
        const TimerEntry* const entry = (const TimerEntry*)(order_.min());
        return entry == nullptr ? UINT64_MAX : entry->deadline;
    }

    struct PollerLoopImpl final: public PollerLoop {
        explicit PollerLoopImpl(ObjPool& owner);

        void arm(PollWaiter& waiter) override;
        void cancel(PollWaiter& waiter) override;
        void timeout(u64 microseconds, TimerCallback& callback) override;
        void deadline(u64 monotonicMicroseconds, TimerCallback& callback) override;
        void cancel(TimerCallback& callback) override;
        void defer(TimerCallback& callback) override;

        void wait(u64 monotonicDeadline) override;
        void dispatchTimers() override;
        u64 nextDeadline() const override;
#if defined(_WIN32)
        PollerLoop& armNative(void* handle, TimerCallback& callback) override;
#endif

        IntrusiveList armed;
#if !defined(_WIN32)
        Vector<struct pollfd> pollFDs;
        Vector<PollWaiter*> pending;
#endif
        Vector<TimerCallback*> deferred;
        Vector<TimerCallback*> deferredRound;
        TimerQueue timers;
#if defined(_WIN32)
        Vector<void*> nativeHandles;
        Vector<TimerCallback*> nativeCallbacks;
#endif
    };
}

PollerLoopImpl::PollerLoopImpl(ObjPool& owner)
    : timers(owner)
{
}

void PollerLoopImpl::arm(PollWaiter& waiter) {
    waiter.unlink();
    armed.pushBack(&waiter);
}

void PollerLoopImpl::cancel(PollWaiter& waiter) {
    // Works whichever list currently holds the node, including the ready
    // list of a dispatch round in progress.
    waiter.unlink();
}

void PollerLoopImpl::timeout(u64 microseconds, TimerCallback& callback) {
    timers.schedule(monotonicNowUs() + microseconds, callback);
}

void PollerLoopImpl::deadline(u64 monotonicMicroseconds, TimerCallback& callback) {
    if (monotonicMicroseconds == 0) {
        monotonicMicroseconds = monotonicNowUs();
    }
    timers.schedule(monotonicMicroseconds, callback);
}

void PollerLoopImpl::cancel(TimerCallback& callback) {
    timers.cancel(callback);
    for (size_t index = 0; index != deferred.length(); ++index) {
        if (deferred[index] == &callback) {
            deferred.mutData()[index] = nullptr;
        }
    }
    for (size_t index = 0; index != deferredRound.length(); ++index) {
        if (deferredRound[index] == &callback) {
            deferredRound.mutData()[index] = nullptr;
        }
    }
}

void PollerLoopImpl::defer(TimerCallback& callback) {
    deferred.pushBack(&callback);
}

u64 PollerLoopImpl::nextDeadline() const {
    // A pending deferred callback turns the next poll into a non-blocking
    // round: descriptors are still polled, then the callback runs.
    return deferred.empty() ? timers.nextDeadline() : 0;
}

void PollerLoopImpl::dispatchTimers() {
    timers.dispatch(monotonicNowUs());
}

#if defined(_WIN32)
PollerLoop& PollerLoopImpl::armNative(void* handle, TimerCallback& callback) {
    STD_INSIST(handle != nullptr);
    STD_INSIST(nativeHandles.length() < MAXIMUM_WAIT_OBJECTS - 1);
    nativeHandles.pushBack(handle);
    nativeCallbacks.pushBack(&callback);
    return *this;
}
#endif

void PollerLoopImpl::wait(u64 monotonicDeadline) {
#if defined(_WIN32)
    STD_INSIST(armed.empty());
    const DWORD timeoutMilliseconds = [&]() -> DWORD {
        if (monotonicDeadline == UINT64_MAX) {
            return INFINITE;
        }
        const u64 now = monotonicNowUs();
        const u64 timeoutUs = monotonicDeadline > now ? monotonicDeadline - now : 0;
        return static_cast<DWORD>(
            min<u64>((timeoutUs + 999) / 1000, MAXDWORD - 1)
        );
    }();
    if (nativeHandles.empty()) {
        const DWORD result = MsgWaitForMultipleObjectsEx(
            0,
            nullptr,
            timeoutMilliseconds,
            QS_ALLINPUT,
            MWMO_INPUTAVAILABLE
        );
        STD_INSIST(result == WAIT_OBJECT_0 || result == WAIT_TIMEOUT);
    } else {
        const DWORD count = static_cast<DWORD>(nativeHandles.length());
        const DWORD result = MsgWaitForMultipleObjectsEx(
            count,
            reinterpret_cast<const HANDLE*>(nativeHandles.data()),
            timeoutMilliseconds,
            QS_ALLINPUT,
            MWMO_INPUTAVAILABLE
        );
        STD_INSIST(
            (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + count)
            || result == WAIT_OBJECT_0 + count
            || result == WAIT_TIMEOUT
        );
        if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + count) {
            nativeCallbacks[result - WAIT_OBJECT_0]->ready();
        }
    }
#else
    pollFDs.clear();
    pending.clear();
    for (IntrusiveNode* node = armed.mutFront(); node != armed.mutEnd(); node = node->next) {
        PollWaiter* const waiter = static_cast<PollWaiter*>(node);
        pending.pushBack(waiter);
        pollFDs.pushBack({
            .fd = waiter->fd.fd,
            .events = waiter->fd.toPollEvents(),
            .revents = 0,
        });
    }

    int timeoutMilliseconds = -1;
    if (monotonicDeadline != UINT64_MAX) {
        const u64 now = monotonicNowUs();
        const u64 timeoutUs = monotonicDeadline > now ? monotonicDeadline - now : 0;
        timeoutMilliseconds = (int)(min<u64>((timeoutUs + 999) / 1000, INT_MAX));
    }
    int result;
    do {
        result = ::poll(pollFDs.mutData(), pollFDs.length(), timeoutMilliseconds);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        Errno(errno == 0 ? EINVAL : errno).raise(StringView(u8"poll failed"));
    }

    // Detach every ready waiter before the first callback runs: a callback
    // that cancels or re-arms another waiter simply pulls it out of this
    // round's list.
    IntrusiveList ready;
    for (size_t index = 0; index != pending.length(); ++index) {
        if (pollFDs[index].revents == 0) {
            continue;
        }
        PollWaiter* const waiter = pending[index];
        waiter->readyFlags = pollFDs[index].revents;
        waiter->unlink();
        ready.pushBack(waiter);
    }
    while (!ready.empty()) {
        PollWaiter* const waiter = static_cast<PollWaiter*>(ready.popFront());
        waiter->callback->ready({
            .fd = waiter->fd.fd,
            .flags = PollFD::fromPollEvents((short)(waiter->readyFlags)),
        });
    }
#endif

    // Deferred callbacks run once the round's descriptor waiters have been
    // dispatched; a callback deferring again lands in the next round.
    xchg(deferredRound, deferred);
    for (size_t index = 0; index != deferredRound.length(); ++index) {
        TimerCallback* const callback = deferredRound[index];
        if (callback != nullptr) {
            callback->ready();
        }
    }
    deferredRound.clear();
}

PollerLoop* PollerLoop::create(ObjPool& owner) {
    return owner.make<PollerLoopImpl>(owner);
}
