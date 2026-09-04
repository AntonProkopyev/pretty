#include <plt/fiber.h>
#include <plt/loop_wake.h>
#include <plt/platform.h>
#include <plt/poller.h>

#include <std/mem/obj_pool.h>
#include <std/thr/runable.h>

using namespace plt;
using namespace stl;

namespace {
    struct alignas(16) XmmValue {
        u64 low;
        u64 high;

        bool operator==(const XmmValue&) const = default;
    };

    struct Stop final: TimerCallback {
        explicit Stop(Platform& platform_)
            : platform(platform_)
        {
        }

        void ready() override {
            ++calls;
            platform.stop();
        }

        Platform& platform;
        size_t calls = 0;
    };

    struct StopFiber final: Runable {
        StopFiber(Platform& platform_, Scheduler& scheduler_)
            : platform(platform_)
            , scheduler(scheduler_)
        {
        }

        void run() override {
#if defined(_WIN32) && defined(__x86_64__)
            const XmmValue clobber = {
                .low = 0xccccccccccccccccULL,
                .high = 0xddddddddddddddddULL,
            };
            __asm__ volatile("movdqu %0, %%xmm6" : : "m"(clobber) : "xmm6");
#endif
            scheduler.yield();
            ++calls;
            platform.stop();
        }

        Platform& platform;
        Scheduler& scheduler;
        size_t calls = 0;
    };
}

int main() {
    ObjPool::Ref pool = ObjPool::fromMemory();
    Platform& platform = *Platform::create(*pool);
    Scheduler& scheduler = *platform.scheduler();
    StopFiber fiber(platform, scheduler);
    void* const stack = pool->allocate(lightFiberStack);
#if defined(_WIN32) && defined(__x86_64__)
    const XmmValue expected = {
        .low = 0x1111111111111111ULL,
        .high = 0x2222222222222222ULL,
    };
    __asm__ volatile("movdqu %0, %%xmm6" : : "m"(expected) : "xmm6");
#endif
    scheduler.spawn(fiber, stack, lightFiberStack);
#if defined(_WIN32) && defined(__x86_64__)
    XmmValue observed{};
    __asm__ volatile("movdqu %%xmm6, %0" : "=m"(observed));
    if (!(observed == expected)) {
        return 3;
    }
#endif
    platform.run();
    if (fiber.calls != 1) {
        return 1;
    }

    Stop stop(platform);
    LoopWake& wake = *platform.createLoopWake(*pool, stop);

    wake.signal();
    platform.run();

    return stop.calls == 1 ? 0 : 2;
}
