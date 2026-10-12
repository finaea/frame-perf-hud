// %LOCALAPPDATA%\frame-perf-hud\settings.json: PC id, remembered headsets, manual IP, window state. Platform-free
// (tests run it anywhere); saved with core's atomicWriteFile, so a crash mid-save leaves the old file.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fph {
namespace pc {

/** Most headsets remembered, and most IPs kept per headset. */
constexpr size_t kMaxRememberedHeadsets = 8;
constexpr size_t kMaxIpsPerHeadset = 4;

struct RememberedHeadset {
    std::string id;    ///< 32 hex chars
    std::string name;
    std::vector<std::string> ips;  ///< newest first, dotted IPv4
};

/** Window placement in virtual-screen pixels (restored only if it's still on some monitor). */
struct WindowState {
    bool valid = false;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool maximized = false;
};

struct Settings {
    std::string pcId;  ///< 32 hex chars; made on first run
    std::vector<RememberedHeadset> headsets;  ///< newest first
    std::string manualIp;  ///< "" = none
    WindowState window;
    bool onTop = false;
    bool trayHintShown = false;  ///< the one-time "still running in the tray" toast
    int cameraFps = 10;          ///< ticket 10

    bool operator==(const Settings& o) const;
    bool operator!=(const Settings& o) const { return !(*this == o); }
};

/** @return the settings as pretty JSON */
std::string settingsToJson(const Settings& s);

/**
 * Reads settings JSON. Unknown keys are ignored, bad values keep their defaults (with a warning).
 * @return false if the text isn't a JSON object at all
 */
bool settingsFromJson(const std::string& text, Settings& out, std::vector<std::string>& warnings, std::string& error);

/** What loadSettings found. */
enum class LoadResult {
    Loaded,   ///< read fine
    Missing,  ///< no file: defaults (first run)
    Broken,   ///< unreadable: moved aside to settings.json.broken, defaults used
};

/**
 * Loads the file; fills in a new PC id if there is none.
 * @param path settings.json (UTF-8)
 * @param out the settings
 * @param idCreated set when a new PC id was made (save the file)
 */
LoadResult loadSettings(const std::string& path, Settings& out, bool& idCreated, std::vector<std::string>& warnings);

/** Saves atomically. @return false + error if it failed */
bool saveSettings(const std::string& path, const Settings& s, std::string& error);

/**
 * Puts a headset first in the remembered list with these IPs first (deduped, capped).
 * @return true if anything changed
 */
bool rememberHeadset(Settings& s, const std::string& id, const std::string& name, const std::vector<std::string>& ips);

/** @return every remembered IP (all headsets) */
std::vector<std::string> rememberedIps(const Settings& s);

}  // namespace pc
}  // namespace fph
