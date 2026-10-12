// Small Windows helpers: where the app keeps its files, the computer name, "Start with Windows" (HKCU Run).
#pragma once

#include <string>

namespace fph {
namespace pc {

/** @return %LOCALAPPDATA%\frame-perf-hud (UTF-8), or "" if the folder is unknown */
std::string defaultDataDir();

/** @return the computer name (UTF-8), "Windows PC" if unknown */
std::string computerName();

/** @return this exe's full path (UTF-8) */
std::string exePath();

/** HKCU Run key and value the autostart uses. Tests pass their own key so the real Run key is never touched. */
extern const wchar_t* const kRunKey;
extern const wchar_t* const kRunValue;

/** @return true if the Run value exists and points at this exe */
bool autostartEnabled(const wchar_t* key = kRunKey, const wchar_t* value = kRunValue);

/**
 * Adds (`"<exe>" --tray`) or removes the Run value.
 * @return false if the registry refused
 */
bool setAutostart(bool enable, const wchar_t* key = kRunKey, const wchar_t* value = kRunValue);

}  // namespace pc
}  // namespace fph
