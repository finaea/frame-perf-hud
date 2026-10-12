// A tiny log: one line per event with a wall-clock timestamp. The app opens %LOCALAPPDATA%\frame-perf-hud\pc.log
// (rewritten each start, so it never grows past one run); before that, and in tests, lines go to stderr.
#pragma once

#include <string>

namespace fph {
namespace pc {

/** Opens the log file (UTF-8 path), replacing the previous run's. @return false if it couldn't */
bool openLog(const std::string& path);

/** Writes one line (printf-style), flushed at once. */
void logLine(const char* format, ...);

}  // namespace pc
}  // namespace fph
