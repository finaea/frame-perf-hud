// Where frame-perf-overlay keeps things on the Frame.
#pragma once

#include <string>

namespace paths {

/** App name used for every directory, the unit, the lock file and the binary. */
constexpr const char* kAppName = "frame-perf-overlay";

/** @return $XDG_RUNTIME_DIR, else /run/user/<uid> if writable, else /tmp (no trailing slash) */
std::string runtimeDir();

/** @return $XDG_DATA_HOME, else ~/.local/share */
std::string dataHome();

/** @return where a running instance writes its debug dump (tmpfs, never the disk) */
std::string debugDumpPath();

}  // namespace paths
