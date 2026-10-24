// The one place the running app changes its layout: a compare-and-set on Layout::version.
// The dashboard editor's edits and the Profiles page go through trySetLayout, so an edit made on a stale layout
// (the file was reloaded meanwhile) is rejected instead of silently overwriting a newer one.
// Single-threaded: call it from the main loop.
#pragma once

#include "config.h"
#include "layout.h"

#include <cstdint>
#include <functional>
#include <string>

class LayoutAuthority {
public:
    /** Saves the whole config (ConfigWatcher::save). @return false + error if it failed */
    using SaveFn = std::function<bool(const Config& config, std::string& error)>;
    /** Applies a layout live: renderer + scheduler. */
    using ApplyFn = std::function<void(const frame_perf_overlay::Layout& layout)>;

    enum class Outcome : uint8_t {
        Applied = 0,     ///< saved, version bumped, applied live
        Stale = 1,       ///< baseVersion isn't the current version (someone else edited first)
        SaveFailed = 2,  ///< the base was current, but the save failed: nothing changed
    };

    struct Result {
        Outcome outcome = Outcome::Stale;
        uint32_t version = 0;  ///< Applied: the new version. Otherwise: the current one
        std::string error;     ///< SaveFailed: why
        bool applied() const { return outcome == Outcome::Applied; }
    };

    /**
     * @param config the app's live config (its layout is the source of truth; a file reload that replaces it
     *               is picked up, since this only keeps a reference)
     * @param save saves the config
     * @param apply applies a layout live
     */
    LayoutAuthority(Config& config, SaveFn save, ApplyFn apply);

    /**
     * Applies layout iff baseVersion is the current version: the new layout gets version current + 1, is saved
     * atomically, then applied live. The incoming layout's own version field is ignored.
     * @param baseVersion the version the edit was made on (captured when the drag started)
     * @param layout the edited layout
     * @return Applied + new version, or Stale / SaveFailed + the current version (the layout is unchanged)
     */
    Result trySetLayout(uint32_t baseVersion, const frame_perf_overlay::Layout& layout);

    /** @return the current layout version */
    uint32_t version() const { return config_.layout.version; }
    /** @return the current layout */
    const frame_perf_overlay::Layout& layout() const { return config_.layout; }

private:
    Config& config_;
    SaveFn save_;
    ApplyFn apply_;
};
