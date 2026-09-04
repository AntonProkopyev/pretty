#include "fiber.h"

#include "poller.h"

#include <std/thr/context.h>
#include <std/thr/poll_fd.h>
#include <std/thr/runable.h>
#include <std/mem/obj_pool.h>
#include <std/mem/small_obj_allocator.h>
#include <std/dbg/assert.h>

#if defined(_WIN32)
    #include <malloc.h>
#else
    #include <alloca.h>
#endif
#include <new>

using namespace plt;
using namespace stl;

namespace {
    constexpr size_t stackAlign = 16;

    size_t aligned(size_t size) {
        return (size + stackAlign - 1) & ~(stackAlign - 1);
    }

    struct SchedulerImpl;

    // The pool-owned facade of a fiber. Its stack and this stable handle
    // die with owner; the scheduler's smaller runtime handle remains
    // separate so an armed poller reference can outlive that memory as a
    // tombstone. A body that finishes simply detaches the runtime, leaving
    // wake() on the facade as a no-op until the pool is destroyed.
    struct OwnedFiber final: public Fiber, public Runable {
        OwnedFiber(SchedulerImpl& scheduler, Runable& entry);
        ~OwnedFiber() noexcept;

        void run() override;
        void park() override;
        bool parkFor(u64 timeoutUs) override;
        void wake() override;
        void release() override;

        SchedulerImpl& scheduler;
        Runable& entry;
        Fiber* runtime = nullptr;
    };

    // The mortal half of a fiber: everything only a live one needs. Carved
    // from the scheduler's small-object allocator at spawn and returned
    // the moment the fiber finishes or is released; the context it points
    // at is carved from the caller's stack and dies with it.
    struct FiberStore {
        explicit FiberStore(stl::Runable& entry);

        stl::Runable& entry;
        Context* context = nullptr;
        Context* resumeTo = nullptr;
        bool fdReady = false;
        bool timerFired = false;
        bool parked = false;
        bool wakePending = false;
        bool finished = false;
    };

    // The immortal half: the handle every outside party points at - the
    // spawner's Fiber*, the poller's timer table, its waiter list. State
    // hides behind data so release() can drop it early: a released handle
    // is a tombstone kept only for the poller references armed at that
    // moment, and collect() buries it when the first of them fires. The
    // descriptor waiter node is embedded here rather than on the await
    // frame exactly so it survives a release() made mid-await.
    struct FiberImpl final: public Fiber, public PollCallback, public TimerCallback, public Runable {
        FiberImpl(SchedulerImpl& scheduler, stl::Runable& entry);

        void ready(stl::PollFD event) override;
        void ready() override;
        void run() override;
        void park() override;
        bool parkFor(u64 timeoutUs) override;
        void wake() override;
        void release() override;

        void block();
        void collect();

        SchedulerImpl& scheduler;
        FiberStore* data;
        PollWaiter waiter;
        bool timerArmed = false;
        bool waiterArmed = false;
    };

    struct SchedulerImpl final: public Scheduler {
        SchedulerImpl(ObjPool& owner, Poller& poller);

        void spawn(stl::Runable& entry, void* stack, size_t size) override;
        Fiber* create(stl::ObjPool& owner, stl::Runable& entry, size_t stackSize) override;
        bool awaitReadable(int fd, u64 timeoutUs) override;
        bool awaitWritable(int fd, u64 timeoutUs) override;
        void yield() override;
        Fiber* current() override;

        bool awaitFd(int fd, u32 flags, u64 timeoutUs);
        void resume(FiberImpl& fiber);

        Poller& poller;
        SmallObjAllocator* const blocks;
        FiberImpl* active = nullptr;
    };
}

FiberStore::FiberStore(Runable& entry_)
    : entry(entry_)
{
}

OwnedFiber::OwnedFiber(SchedulerImpl& scheduler_, Runable& entry_)
    : scheduler(scheduler_)
    , entry(entry_)
{
}

OwnedFiber::~OwnedFiber() noexcept {
    release();
}

void OwnedFiber::run() {
    runtime = scheduler.current();
    entry.run();
    // No suspension follows: the scheduler frees the runtime as soon as
    // this frame returns, while the pool-owned facade remains valid.
    runtime = nullptr;
}

void OwnedFiber::park() {
    STD_ASSERT(runtime != nullptr);
    runtime->park();
}

bool OwnedFiber::parkFor(u64 timeoutUs) {
    STD_ASSERT(runtime != nullptr);
    return runtime->parkFor(timeoutUs);
}

void OwnedFiber::wake() {
    if (runtime != nullptr) {
        runtime->wake();
    }
}

void OwnedFiber::release() {
    if (runtime == nullptr) {
        return;
    }
    STD_ASSERT(runtime != scheduler.current());
    Fiber* const live = runtime;
    runtime = nullptr;
    live->release();
}

FiberImpl::FiberImpl(SchedulerImpl& scheduler_, Runable& entry_)
    : scheduler(scheduler_)
    , data(scheduler_.blocks->make<FiberStore>(entry_))
{
    waiter.callback = this;
}

void FiberImpl::run() {
    data->entry.run();
    data->finished = true;
    // The final switch out; resume() frees the control block behind it,
    // the caller owns the stack again, and nothing may run here anymore.
    data->context->switchTo(*data->resumeTo);
}

void FiberImpl::ready(PollFD) {
    // One-shot: the poller unlinked the waiter before calling here.
    waiterArmed = false;
    if (data == nullptr) {
        collect();
        return;
    }
    data->fdReady = true;
    scheduler.resume(*this);
}

void FiberImpl::ready() {
    timerArmed = false;
    if (data == nullptr) {
        collect();
        return;
    }
    data->timerFired = true;
    scheduler.resume(*this);
}

void FiberImpl::park() {
    if (data->wakePending) {
        data->wakePending = false;
        return;
    }
    data->parked = true;
    block();
}

bool FiberImpl::parkFor(u64 timeoutUs) {
    if (data->wakePending) {
        data->wakePending = false;
        return true;
    }
    data->parked = true;
    data->timerFired = false;
    timerArmed = true;
    scheduler.poller.timeout(timeoutUs, *this);
    block();
    if (data->parked) {
        // The timer resumed us while still parked.
        data->parked = false;
        return false;
    }
    // A wake resumed us; the pending timer must not fire later.
    scheduler.poller.cancel(*this);
    timerArmed = false;
    return true;
}

void FiberImpl::wake() {
    if (!data->parked) {
        data->wakePending = true;
        return;
    }
    data->parked = false;
    scheduler.resume(*this);
}

void FiberImpl::release() {
    STD_ASSERT(scheduler.active != this);
    // The blocked fiber never resumes: its state goes back to the
    // allocator at once and the stack is the caller's to drop. Whatever
    // the poller still holds armed - the deadline of a parkFor, the
    // waiter of an await - collects the handle when it fires; with
    // nothing armed, nobody is left to come and the handle goes now.
    scheduler.blocks->release(data);
    data = nullptr;
    if (!timerArmed && !waiterArmed) {
        scheduler.blocks->release(this);
    }
}

void FiberImpl::collect() {
    // The sibling reference must not outlive the tombstone: a released
    // await-with-deadline holds both a waiter and a timer, and whichever
    // fired second would dispatch into freed memory.
    if (waiterArmed) {
        scheduler.poller.cancel(waiter);
        waiterArmed = false;
    }
    if (timerArmed) {
        scheduler.poller.cancel(*this);
        timerArmed = false;
    }
    scheduler.blocks->release(this);
}

void FiberImpl::block() {
    data->context->switchTo(*data->resumeTo);
}

SchedulerImpl::SchedulerImpl(ObjPool& owner, Poller& poller_)
    : poller(poller_)
    , blocks(SmallObjAllocator::create(&owner))
{
}

void SchedulerImpl::resume(FiberImpl& fiber) {
    // The host context lives on the resumer's stack frame, which stays alive
    // for as long as the fiber runs; nested resumes each bring their own.
    Context* const host = Context::create(
#if defined(_WIN32)
        _alloca(Context::implSize())
#else
        alloca(Context::implSize())
#endif
    );
    FiberImpl* const previous = active;
    active = &fiber;
    fiber.data->resumeTo = host;
    host->switchTo(*fiber.data->context);
    active = previous;
    if (fiber.data->finished) {
        // Nothing outside points at a finished fiber - no timer, no
        // waiter - and the spawner's handle dies with this frame.
        blocks->release(fiber.data);
        blocks->release(&fiber);
    }
}

void SchedulerImpl::spawn(Runable& entry, void* stack, size_t size) {
    // Only the context implementation is carved from the provided stack;
    // the control block comes from the allocator so it can outlive the
    // stack when the fiber is released mid-wait.
    u8* const base = static_cast<u8*>(stack);
    const size_t reserved = aligned(Context::implSize());
    FiberImpl* const fiber = blocks->make<FiberImpl>(*this, entry);
    fiber->data->context = Context::create(base, base + reserved, size - reserved, *fiber);
    resume(*fiber);
}

Fiber* SchedulerImpl::create(ObjPool& owner, Runable& entry, size_t stackSize) {
    void* const stack = owner.allocate(stackSize);
    OwnedFiber* const fiber = owner.make<OwnedFiber>(*this, entry);
    spawn(*fiber, stack, stackSize);
    return fiber;
}

bool SchedulerImpl::awaitFd(int fd, u32 flags, u64 timeoutUs) {
    FiberImpl& fiber = *active;
    fiber.waiter.fd = {
        .fd = fd,
        .flags = flags,
    };
    fiber.data->fdReady = false;
    fiber.data->timerFired = false;
    fiber.waiterArmed = true;
    poller.arm(fiber.waiter);
    if (timeoutUs != 0) {
        fiber.timerArmed = true;
        poller.timeout(timeoutUs, fiber);
    }
    fiber.block();
    if (fiber.data->fdReady) {
        if (timeoutUs != 0) {
            poller.cancel(fiber);
            fiber.timerArmed = false;
        }
        return true;
    }
    poller.cancel(fiber.waiter);
    fiber.waiterArmed = false;
    return false;
}

bool SchedulerImpl::awaitReadable(int fd, u64 timeoutUs) {
    return awaitFd(fd, PollFlag::In, timeoutUs);
}

bool SchedulerImpl::awaitWritable(int fd, u64 timeoutUs) {
    return awaitFd(fd, PollFlag::Out, timeoutUs);
}

void SchedulerImpl::yield() {
    // Not a zero timer: a deferred wake runs after the next poll round, so
    // a fiber yielding in a hot loop cannot starve the descriptor waiters.
    FiberImpl& fiber = *active;
    fiber.data->timerFired = false;
    fiber.timerArmed = true;
    poller.defer(fiber);
    fiber.block();
}

Fiber* SchedulerImpl::current() {
    return active;
}

Scheduler* Scheduler::create(ObjPool& owner, Poller& poller) {
    return owner.make<SchedulerImpl>(owner, poller);
}
