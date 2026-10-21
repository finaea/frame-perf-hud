#include "layout_authority.h"

#include <cstdio>
#include <utility>

LayoutAuthority::LayoutAuthority(Config& config, SaveFn save, ApplyFn apply)
    : config_(config), save_(std::move(save)), apply_(std::move(apply)) {}

LayoutAuthority::Result LayoutAuthority::trySetLayout(uint32_t baseVersion, const frame_perf_overlay::Layout& layout) {
    Result result;
    const uint32_t current = config_.layout.version;
    if (baseVersion != current) {
        result.outcome = Outcome::Stale;
        result.version = current;
        return result;
    }
    // Save first, from a copy: a failed save leaves the live config and the file as they were
    Config next = config_;
    next.layout = layout;
    next.layout.version = current + 1;
    if (save_ && !save_(next, result.error)) {
        std::fprintf(stderr, "[layout] Edit on version %u not applied: saving failed: %s\n", current,
                     result.error.c_str());
        result.outcome = Outcome::SaveFailed;
        result.version = current;
        return result;
    }
    config_.layout = next.layout;
    if (apply_) apply_(config_.layout);
    result.outcome = Outcome::Applied;
    result.version = config_.layout.version;
    std::fprintf(stderr, "[layout] Applied an edit: version %u -> %u\n", current, result.version);
    return result;
}
