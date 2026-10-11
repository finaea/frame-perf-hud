// Where frame-perf-hud keeps things on the Frame.
#pragma once

#include <string>

namespace paths {

/** App name used for every directory, the unit, the lock file and the binary. */
constexpr const char* kAppName = "frame-perf-hud";

/** @return $XDG_RUNTIME_DIR, else /run/user/<uid> if writable, else /tmp (no trailing slash) */
std::string runtimeDir();

/** @return $XDG_DATA_HOME, else ~/.local/share */
std::string dataHome();

/** @return $XDG_STATE_HOME/frame-perf-hud, else ~/.local/state/frame-perf-hud */
std::string stateDir();

/** @return where a running instance writes its debug dump (tmpfs, never the disk) */
std::string debugDumpPath();

/**
 * The font file to load.
 * @param configured the "font" / "font_bold" value from config.json; empty means the bundled font
 * @param bold true for the bold face
 * @return configured if non-empty; else the bundled Noto Sans from ~/.local/share/frame-perf-hud/fonts
 *         (installed by install.sh) or from the repo's assets/fonts next to a dev build; else "" (FontSet
 *         then falls back to fontconfig)
 */
std::string resolveFontPath(const std::string& configured, bool bold);

}  // namespace paths
