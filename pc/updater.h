// Release G: the PC app's self-updater. Asks GitHub for the latest release of finaea/frame-perf-hud (WinHTTP, at
// start and at most once a day, plus Check now), and when it's newer than this build, downloads
// FramePerfHud-<version>.exe from the same release, checks it against the release's SHA256SUMS (BCrypt) and swaps it
// in: the running exe is renamed to <exe>.old (Windows allows renaming a running exe), the new one takes its name,
// it's started with --wait-pid, and this one exits through the normal path (BYE). The next start deletes the .old.
// Only Windows' own winhttp / bcrypt, so the exe stays one standalone file. Same "newer" rule as the headset's
// frame-updater (vendor/frame-updater/cpp/update_check.cpp: compareVersions).
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace fph {
namespace pc {
namespace update {

constexpr const char* kRepo = "finaea/frame-perf-hud";
constexpr int64_t kCheckIntervalSec = 24 * 3600;  ///< automatic checks: at most once a day
constexpr int64_t kRetryAfterFailSec = 3600;      ///< a failed automatic check tries again after an hour

/**
 * Compares the numeric x.y.z part of two versions ("v0.4.0-rc1" counts as 0.4.0), as the headset does.
 * @return -1, 0 or 1; 0 also when either isn't a version
 */
int compareVersions(const std::string& a, const std::string& b);

/** @return "FramePerfHud-<version>.exe", the release asset for that version */
std::string assetName(const std::string& version);

/** What the latest release offers. */
struct Release {
    std::string tag;       ///< "v0.2.0"
    std::string version;   ///< the tag without its "v"
    std::string page;      ///< html_url
    bool draft = false;
    bool prerelease = false;
    std::string asset;     ///< assetName(version)
    std::string assetUrl;  ///< "" = the release has no PC exe
    std::string sumsUrl;   ///< "" = no SHA256SUMS
};

/**
 * Reads GitHub's /releases/latest answer.
 * @return false (with error) if it isn't a release object with a tag_name
 */
bool parseRelease(const std::string& json, Release& out, std::string& error);

/**
 * Finds a file's SHA-256 in a sha256sum-format text ("<64 hex>  <name>" or "<hex> *<name>", one per line). Other
 * lines (the headset tarball's) are skipped.
 * @param hex out: the lowercase hex
 * @return false if there's no well-formed line for that name
 */
bool findChecksum(const std::string& sums, const std::string& name, std::string& hex);

/**
 * Only GitHub over https (github.com, api.github.com, codeload.github.com, *.githubusercontent.com), as frame-update.sh.
 * @param insecure the test override: any http:// or https:// URL
 */
bool urlAllowed(const std::string& url, bool insecure);

/**
 * Whether an automatic check is due.
 * @param enabled the Settings switch
 * @param lastCheckedAt last answer from GitHub (Unix s, 0 = never; saved in settings.json)
 * @param lastFailedAt last failed attempt in this run (Unix s, 0 = none)
 * @param now Unix s
 */
bool checkDue(bool enabled, int64_t lastCheckedAt, int64_t lastFailedAt, int64_t now);

/** @return "<exe>.old", where the replaced exe goes */
std::string oldExePath(const std::string& exe);

/**
 * Puts `newFile` in place of `exe` (which may be running): any leftover <exe>.old is deleted, `exe` is renamed to
 * <exe>.old, `newFile` is moved to `exe` (copied if it's on another drive). If the move fails, <exe>.old goes back.
 * @return false + error if nothing changed
 */
bool swapInExe(const std::string& exe, const std::string& newFile, std::string& error);

/**
 * Undoes swapInExe after Windows refused to start the new exe: <exe>.old goes back to `exe`, replacing the rejected
 * file, while the old copy keeps running.
 * @return false + error if the old exe couldn't be put back (it's still at <exe>.old)
 */
bool undoSwap(const std::string& exe, std::string& error);

/** Deletes <exe>.old if it's there (the next start after an update). @return true if one was deleted */
bool removeOldExe(const std::string& exe);

/** @return true if a file can be created in `dir` (it's created and deleted again) */
bool folderWritable(const std::string& dir);

/** @return the file's SHA-256 as lowercase hex (Windows BCrypt), or "" + error */
std::string sha256File(const std::string& path, std::string& error);

/** What the window and the Settings dialog show. */
enum class State {
    Idle,            ///< nothing known yet (or checking is off)
    Checking,        ///< asking GitHub (the other fields keep the last answer)
    UpToDate,
    Available,       ///< latest is newer; see installable
    Downloading,     ///< Install was pressed: fetching and checking the exe
    Ready,           ///< downloaded and verified (readyPath): the app swaps it in and restarts
    CheckFailed,     ///< see error
    InstallFailed,   ///< see error; nothing was changed unless View::changed
};

struct View {
    State state = State::Idle;
    std::string current;      ///< this build
    std::string latest;       ///< newest release ("" = unknown)
    std::string page;         ///< its release page
    bool installable = false; ///< Available, and the release has the exe and SHA256SUMS
    std::string error;        ///< CheckFailed / InstallFailed, English
    bool changed = false;     ///< InstallFailed: the exe on disk is no longer the working one (see error)
    std::string readyPath;    ///< Ready: the verified exe
    int64_t checkedAt = 0;    ///< last answer from GitHub (Unix s)
};

/** @return one line for the Settings dialog ("Up to date (0.1.0)", "Version 0.2.0 is available", ...) */
std::string describe(const View& v);

struct UpdaterConfig {
    std::string current;      ///< FRAME_PERF_HUD_VERSION
    std::string downloadDir;  ///< %LOCALAPPDATA%\frame-perf-hud\update
    std::string apiBase = "https://api.github.com";  ///< FPH_UPDATE_API_URL overrides (tests)
    bool insecure = false;    ///< FPH_UPDATE_INSECURE=1 (tests): plain http to any host
};

/** @return the config with FPH_UPDATE_API_URL / FPH_UPDATE_INSECURE applied */
UpdaterConfig configFromEnvironment(const std::string& current, const std::string& downloadDir);

/**
 * The checker. Checks and downloads run on a worker thread; the app reads view() from its tick (revision() changes
 * whenever it does). One job at a time. The worker is detached on destruction (a request in flight times out on its
 * own; it only touches shared state the worker co-owns).
 */
class Updater {
public:
    explicit Updater(UpdaterConfig config);
    ~Updater();
    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;

    /** The last answer saved in settings.json, so "Update available" shows before the next check. */
    void restore(int64_t checkedAt, const std::string& latest, const std::string& page);

    /** Starts an automatic check when one is due (checkDue). Cheap; call it from the app's tick. */
    void tick(bool enabled, int64_t now);

    /** Check now (the Settings button), whether or not checking is on. Ignored while a job runs. */
    void checkNow();

    /** Install: fetches the latest release again, downloads and verifies its exe (state Downloading -> Ready). */
    void install();

    /**
     * The app couldn't swap a Ready exe in: show this.
     * @param changed true if it couldn't undo its own changes (the error says how to recover by hand)
     */
    void installFailed(const std::string& error, bool changed = false);

    View view() const;
    uint64_t revision() const;

    /**
     * A successful check since the last call: what to save in settings.json.
     * @return true once per answer
     */
    bool takeChecked(int64_t& checkedAt, std::string& latest, std::string& page);

private:
    struct Shared;
    UpdaterConfig config_;
    std::shared_ptr<Shared> shared_;
    int64_t lastFailedAt_ = 0;
    void start(bool installJob);
};

}  // namespace update
}  // namespace pc
}  // namespace fph
