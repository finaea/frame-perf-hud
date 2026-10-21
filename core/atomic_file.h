// Crash-safe file replace: write a temp file next to the target, flush it to disk, then rename it over the
// target. A reader (or the next start after a kill -9 mid-save) sees either the old file or the new one,
// never half of one. POSIX and Windows branches live in the .cpp, so core/ stays portable.
#pragma once

#include <string>

namespace frame_perf_overlay {

/**
 * Replaces a file atomically: `<path>.tmp` is written, fsync'd (FlushFileBuffers on Windows) and renamed
 * over path (rename / MoveFileExW with MOVEFILE_REPLACE_EXISTING). Creates missing parent folders.
 * @param path target file (UTF-8)
 * @param content new contents
 * @param error why it failed
 * @return true if the new contents are in place
 */
bool atomicWriteFile(const std::string& path, const std::string& content, std::string& error);

/**
 * Creates a folder and its missing parents (mkdir -p).
 * @param dir folder (UTF-8; '/' or '\\' separators)
 * @return true if it exists afterwards
 */
bool makeDirectories(const std::string& dir);

}  // namespace frame_perf_overlay
