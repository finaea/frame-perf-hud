// T10: see camera_cache.h
#include "camera_cache.h"

#include <cstdint>

namespace camcache {

namespace {

size_t lineSize() {
    static const size_t line = [] {
#if defined(__aarch64__)
        uint64_t ctr;
        __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
        return static_cast<size_t>(4) << ((ctr >> 16) & 0xf);  // DminLine, in bytes
#else
        return static_cast<size_t>(64);
#endif
    }();
    return line;
}

}  // namespace

void dropLine(const void* p) {
#if defined(__aarch64__)
    __asm__ volatile("dc civac, %0" ::"r"(p) : "memory");
#else
    (void)p;
#endif
}

void barrier() {
#if defined(__aarch64__)
    __asm__ volatile("dsb ish" ::: "memory");
#endif
}

void dropRange(const void* p, size_t len) {
    if (len == 0) return;
    const size_t line = lineSize();
    uintptr_t a = reinterpret_cast<uintptr_t>(p) & ~static_cast<uintptr_t>(line - 1);
    const uintptr_t end = reinterpret_cast<uintptr_t>(p) + len;
    for (; a < end; a += line) dropLine(reinterpret_cast<const void*>(a));
    barrier();
}

}  // namespace camcache
