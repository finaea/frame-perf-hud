// Layout profiles. A profile is a named copy of the layout (columns, widget order, every per-widget setting, in
// core/layout's own JSON), one file each in ~/.config/frame-perf-overlay/profiles/ (next to config.json), written
// atomically. Placement and app settings aren't part of it. Files are only read at start and written on a button press.
#pragma once

#include "layout.h"

#include <cstddef>
#include <string>
#include <vector>

/** One saved profile. */
struct Profile {
    std::string id;      ///< "profile-3": the file name without .json; stays the same through renames
    std::string name;    ///< shown on the tab ("Profile 3" until renamed)
    fph::Layout layout;  ///< its version field is meaningless (Use gives the live layout a new version)
};

/** The profiles folder. Single-threaded (the main loop). */
class ProfileStore {
public:
    static constexpr size_t kMaxProfiles = 8;
    static constexpr size_t kMaxNameBytes = 31;  ///< a longer name is cut (on a UTF-8 boundary)

    /** @param dir the folder (created on the first write) */
    explicit ProfileStore(std::string dir);

    /**
     * Reads every profile-N.json in the folder (N 1-8). A file that can't be read is skipped with a warning; the
     * others still load.
     */
    void load(std::vector<std::string>& warnings);

    /** @return the profiles, by id number */
    const std::vector<Profile>& profiles() const { return profiles_; }
    /** @return the profile with this id, nullptr if none */
    const Profile* find(const std::string& id) const;
    /** @return true while there's room for another (fewer than kMaxProfiles) */
    bool canCreate() const { return profiles_.size() < kMaxProfiles; }

    /**
     * Whether Use / Reset would lose the live layout ("unsaved changes"): it differs from the active profile, or,
     * with no active profile, it isn't saved in any. Compares widgets only (not the version), so it follows every
     * edit (the editor's, a profile's or a file reload's).
     * @param live the live layout
     * @param activeId config.activeProfile ("" or an id that's gone = no active profile)
     */
    bool unsaved(const fph::Layout& live, const std::string& activeId) const;

    /**
     * Saves layout as a new profile, "Profile N" with N the lowest free number.
     * @param id the new profile's id
     * @return false with error if all 8 are used or the file can't be written
     */
    bool create(const fph::Layout& layout, std::string& id, std::string& error);
    /** Overwrites a profile's layout. @return false with error if it doesn't exist or can't be written */
    bool save(const std::string& id, const fph::Layout& layout, std::string& error);
    /** Renames a profile (trimmed, cut to kMaxNameBytes; empty = unchanged). @return false with error as above */
    bool rename(const std::string& id, const std::string& name, std::string& error);
    /** Deletes a profile's file. @return false with error if it doesn't exist or can't be deleted */
    bool remove(const std::string& id, std::string& error);

    const std::string& dir() const { return dir_; }
    /** @return the file of a profile id */
    std::string pathOf(const std::string& id) const;
    /** @return bumps on every change (redraw) */
    uint64_t revision() const { return revision_; }

    /** @return name trimmed and cut to kMaxNameBytes without splitting a UTF-8 character */
    static std::string cleanName(const std::string& name);
    /** @return the file's JSON: { "name": ..., "layout": <layoutToJson> } */
    static std::string toJson(const Profile& profile);
    /** Reads a profile file's text. @return false with error if it isn't a profile */
    static bool fromJson(const std::string& text, Profile& out, std::vector<std::string>& warnings, std::string& error);

private:
    std::string dir_;
    std::vector<Profile> profiles_;
    uint64_t revision_ = 0;

    bool write(const Profile& profile, std::string& error);
    Profile* findMutable(const std::string& id);
};
