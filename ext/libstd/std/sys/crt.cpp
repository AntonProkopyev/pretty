#include "crt.h"

#include <std/dbg/insist.h>
#include <std/ios/output.h>

#include <time.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
#endif

void* stl::allocateMemory(size_t len) {
    if (auto ret = malloc(len); ret) {
        return ret;
    }

    STD_INSIST(false);

    return nullptr;
}

void stl::freeMemory(void* ptr) noexcept {
    free(ptr);
}

int stl::memCmp(const void* l, const void* r, size_t len) noexcept {
    if (len == 0) {
        return 0;
    }

    return memcmp(l, r, len);
}

void* stl::memCpy(void* to, const void* from, size_t len) noexcept {
    if (len) {
        memcpy(to, from, len);
    }

    return len + (u8*)to;
}

size_t stl::strLen(const u8* s) noexcept {
    return s ? strlen((const char*)s) : 0;
}

void stl::memZero(void* from, void* to) noexcept {
    const size_t len = (u8*)to - (u8*)from;

    if (len) {
        memset(from, 0, len);
    }
}

#ifndef STL_EXTERNAL_MONOTONIC_NOW_US
u64 stl::monotonicNowUs() noexcept {
#if defined(_WIN32)
    static const u64 frequency = [] {
        LARGE_INTEGER value;
        STD_INSIST(QueryPerformanceFrequency(&value) != 0);
        return static_cast<u64>(value.QuadPart);
    }();
    LARGE_INTEGER counter;
    STD_INSIST(QueryPerformanceCounter(&counter) != 0);
    const u64 seconds = static_cast<u64>(counter.QuadPart) / frequency;
    const u64 remainder = static_cast<u64>(counter.QuadPart) % frequency;
    return seconds * 1'000'000ULL
        + remainder * 1'000'000ULL / frequency;
#else
    timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (u64)ts.tv_sec * 1000000ULL + (u64)ts.tv_nsec / 1000;
#endif
}
#endif
