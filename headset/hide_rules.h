// R2: the "Hide system" rules for the CPU / RAM usage-items widgets, kept in a file Jack can edit
// (~/.config/frame-perf-hud/hide_rules.json). The HUD writes the built-in list there once, at start, if the file is
// missing; from then on the file is the source of truth and is re-read when its mtime changes.
//
//   { "version": 1, "hide": [ {"name": "vrserver", "note": "SteamVR server"}, {"cgroup": "/system.slice/*"} ] }
//
// "name" matches the process's comm or its exe basename; "cgroup" matches its cgroup v2 path (the "0::" line of
// /proc/<pid>/cgroup). Both are globs ('*' any run of characters, '?' one). A rule with both needs both. On the
// Frame, Steam games run inside steam.service (no per-app scope), so Steam and SteamVR are named one by one rather
// than hidden by cgroup.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

/** One rule. Empty name / cgroup = not checked; a rule with neither matches nothing. */
struct HideRule {
    std::string name;    ///< glob on comm or the exe basename
    std::string cgroup;  ///< glob on the cgroup v2 path
    std::string note;    ///< free text, ignored
};

/**
 * @param pattern a glob: '*' matches any run of characters ('/' included), '?' one character, the rest itself
 * @param text the text
 * @return true if the whole text matches
 */
bool globMatch(const std::string& pattern, const std::string& text);

/**
 * @param rule a rule
 * @param comm /proc/<pid>/stat's comm
 * @param exe basename of /proc/<pid>/exe ("" if unreadable)
 * @param cgroup the cgroup v2 path ("" if unknown: a cgroup rule then doesn't match)
 * @return true if the rule hides this process
 */
bool hideRuleMatches(const HideRule& rule, const std::string& comm, const std::string& exe, const std::string& cgroup);

/** @return the list written on first run (SteamOS, Steam and SteamVR as seen on Jack's Frame, 2026-10-17) */
const std::vector<HideRule>& defaultHideRules();

/** @return the file's text for these rules (version 1, one rule per line) */
std::string hideRulesToJson(const std::vector<HideRule>& rules);

/**
 * Reads a rules file's text.
 * @param out the rules (unchanged on failure)
 * @param error why it failed
 * @return false if it isn't { "hide": [ {...}, ... ] } with every rule an object of strings naming name or cgroup
 */
bool parseHideRules(const std::string& text, std::vector<HideRule>& out, std::string& error);

/**
 * @param text /proc/<pid>/cgroup's contents
 * @return the path on its "0::" line ("/user.slice/..."), "" if there is none (cgroup v1 only)
 */
std::string cgroupV2Path(const std::string& text);

/** The rules in use, from the file. Not thread-safe; the process scan uses it from the main loop. */
class HideRules {
public:
    /**
     * @param path the rules file
     * @param checkSeconds the file's mtime is looked at no more often than this
     */
    explicit HideRules(std::string path, double checkSeconds = 5.0);

    /**
     * Writes the built-in list to the file if there's no file there (an existing file, even a broken one, is never
     * touched). Call once at start.
     * @param error why the write failed
     * @return true if the file exists afterwards
     */
    bool seedIfMissing(std::string& error);

    /**
     * Re-reads the file if its mtime or size changed since the last look (a stat at most every checkSeconds).
     * A file that fails to parse logs one warning and leaves the last good rules in use.
     * @param now monotonic seconds
     * @return true if the rules in use changed
     */
    bool refresh(double now);

    /** @return true if any rule hides this process (see hideRuleMatches) */
    bool hides(const std::string& comm, const std::string& exe, const std::string& cgroup) const;

    const std::vector<HideRule>& rules() const { return rules_; }
    /** @return bumped every time the rules in use change (cached verdicts compare against it) */
    uint64_t generation() const { return generation_; }
    const std::string& path() const { return path_; }

private:
    std::string path_;
    double checkSeconds_;
    double lastCheck_ = -1e300;
    bool seen_ = false;          ///< a stat result is in mtime_ / size_ (else the file was missing)
    int64_t mtimeNs_ = 0;
    int64_t size_ = -1;
    std::vector<HideRule> rules_;
    uint64_t generation_ = 1;
};
