// What the dashboard tab's Profiles page shows (main.cpp builds it from the ProfileStore and the config).
// Kept apart so the settings panel doesn't pull in the store.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/** The saved profiles and the live layout's state against them. */
struct ProfilesUiState {
    struct Item {
        std::string id;    ///< "profile-3" (pass it back through SettingsPanel::selectedProfileId())
        std::string name;
    };
    std::vector<Item> items;  ///< by id number
    std::string activeId;     ///< the active profile, "" = none
    /**
     * Use / Reset would lose the live layout: it differs from the active profile, or (no active profile) it isn't
     * saved in any profile.
     */
    bool unsaved = false;
    size_t maxProfiles = 8;
    std::string status;     ///< the last action's result ("Saved to Gaming"), "" = nothing yet
    uint64_t revision = 0;  ///< bumps whenever anything above changes (redraw)
};
