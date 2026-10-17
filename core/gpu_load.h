// GPU load from SteamVR's frame timing (gpu.busy_pct). Ported from frame-perf-overlay 187252c.
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace fph {

/**
 * GPU load: the GPU time per frame SteamVR measures (m_flTotalRenderGpuMs) × the app's frame rate. It counts only
 * the VR app and the SteamVR compositor, not the whole headset. The kernel's DRM fdinfo counters aren't used: on
 * SteamOS 0.5.6 (kernel 6.18) msm's drm-engine-gpu grows by months per second and drm-cycles-gpu misses most of a
 * heavy game (~4% in Half-Life: Alyx).
 * @param gpuMs average GPU time per frame (ms)
 * @param fps the app's new frames per second
 * @return load (%, 0-100); NaN when either input is missing or negative
 * @example estimateGpuLoadPct(16.5, 58.6) // 96.69 (Half-Life: Alyx at 72 Hz)
 */
inline double estimateGpuLoadPct(double gpuMs, double fps) {
    if (!std::isfinite(gpuMs) || !std::isfinite(fps) || gpuMs < 0 || fps < 0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    // GPU work that overlaps into the next frame would pass 100%
    return std::clamp(gpuMs * fps / 10.0, 0.0, 100.0);
}

}  // namespace fph
