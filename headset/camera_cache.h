// T10: reading buffers the camera hardware wrote. The capture hardware doesn't snoop the CPU caches, so a line read
// earlier can keep returning an old frame long after the camera rewrote memory. DC CIVAC (clean + invalidate,
// allowed from user space on arm64 Linux) over the range, then DSB ISH, makes the next read come from memory; it
// cleans first, so it never discards anyone else's write. Ported from camviewer/cachecopy.c:19-34 (and
// framestream.c:249-290). A no-op off aarch64, so tests build anywhere.
#pragma once

#include "camera_frame.h"

#include <cstddef>

namespace camcache {

/** Clean+invalidate every cache line of [p, p + len), then dsb ish. */
void dropRange(const void* p, size_t len);

/** Clean+invalidate one line, no barrier (call barrier() after a batch). */
void dropLine(const void* p);
void barrier();

/** @return CacheOps for core/camera_frame's copy and sampling */
inline fph::cam::CacheOps ops() {
    fph::cam::CacheOps o;
    o.dropRange = &dropRange;
    return o;
}

}  // namespace camcache
