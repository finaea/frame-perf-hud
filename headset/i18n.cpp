// The UI string tables. English only for now; to add a language, add a Language value, a UiText table and
// a branch in each function below.
#include "i18n.h"

#include <cmath>
#include <cstdio>

namespace {

const UiText kEnglish = {
    // Performance panel
    "Frame", "Measuring", "No SteamVR", "Reproj ", "Drop ",
    "Power", "All ch ", "Fan ",
    "Temp", "°C CPU", "°C", "Batt ", "Disp ", "Exh ", "Sink ",
    " (max core ", ")",
    "Link", "Wi-Fi", "Not connected", "Rate ",
    "Batt ", "L", "R", "No controllers", "Mem",
    // Settings panel. The bundled Noto Sans has no arrow glyphs (U+2190-2193), so buttons use ‹ › or words
    "Frame Perf HUD", "Panel", ". ", "Panel shown", "Panel hidden",
    "Show", "Language", "Position", "Nudge", "Size", "Opacity",
    "Autostart", "Autostart changes apply from the next SteamVR start",
    "The autostart unit is not installed, so it can't be switched (see \"Autostart\" in the README)",
    "Switching autostart…", "Failed to switch autostart (see the log)",
    "On", "Off",
    "Bottom L", "Bottom C", "Bottom R", "Top L", "Top R",
    "‹ Left", "Right ›", "Up", "Down", "Closer", "Farther",
    "Reset", "Quit app", "Press again to quit",
    "Now", "x", "y", "z",
    "Facing", "‹ Left", "Right ›", "Up", "Down", "Rotate L", "Rotate R", "Face me", "Face ahead", "By 1°", "By 5°",
    "Now", "yaw", "pitch", "roll",
    "Changes apply instantly and are saved",
    // Updates
    "Check for updates",
    "Up to date (%s)", "Checking for updates…", "Version %s is available",
    "Update", "This version can't be installed from here. Update by hand from GitHub", "Release page:",
    "Update to %s?", "It downloads and installs the new version. This panel may close and reopen meanwhile",
    "Update", "Cancel",
    "Updating: %s", "%s is installed. Reopen to use it",
    "The update failed (nothing was changed):", "Couldn't check for updates:",
    "Check now", "Try again", "Close",
    "Details: ~/.cache/frame-perf-hud/update.log",
    // Attachment, wrist and clock
    "Attach to", "Head", "L hand", "R hand", "Default wrist position", "%.0f cm from default",
    "Fade on tilt", "Fade angle", "%.0f–%.0f°",
    "Clock", "12h", "24h", "Local time",
};

/** @return the value with no decimals */
std::string whole(double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.0f", value);
    return text;
}

}  // namespace

const UiText& uiText(Language /*language*/) {
    return kEnglish;
}

std::string throttleBadgeText(Language /*language*/, double hz, int throttled) {
    const std::string fraction = "1/" + std::to_string(throttled + 1);
    return "Throttled to " + fraction + " of " + whole(hz) + "Hz (reproj)";
}

std::string thermalBadgeText(Language /*language*/, bool cpu, bool gpu) {
    const std::string which = cpu && gpu ? "CPU+GPU" : (cpu ? "CPU" : "GPU");
    return "Thermal limit " + which;
}

std::string batteryStatusText(Language /*language*/, const std::string& status, bool chargerOnline) {
    if (status == "Charging") return "Charging";
    if (status == "Discharging") return "On battery";
    if (status == "Full") return "Full";
    if (status == "Not charging") return chargerOnline ? "Plugged" : "Idle";
    return status;
}

std::string updateStepText(Language /*language*/, const std::string& step) {
    if (step == "start") return "Preparing";
    if (step == "download") return "Downloading";
    if (step == "verify") return "Verifying";
    if (step == "extract") return "Unpacking";
    if (step == "install") return "Installing";
    return step;
}

std::string updateErrorText(Language /*language*/, const std::string& error) {
    if (error == "network") return "Can't reach GitHub";
    if (error == "rate-limited") return "GitHub's rate limit was hit. Try again in an hour";
    if (error == "not-found") return "No published release";
    if (error == "bad-response") return "Couldn't read GitHub's answer";
    if (error == "bad-version") return "Couldn't read the version number";
    if (error == "bad-url") return "Stopped: the download led outside GitHub";
    if (error == "missing-tool") return "A required command (python3) is missing";
    if (error == "no-checksums") return "This release has no SHA256SUMS. Update by hand";
    if (error == "no-asset") return "This release has no file to install";
    if (error == "checksum-mismatch") return "The download is corrupt (checksum mismatch)";
    if (error == "unsafe-archive") return "Stopped: the archive has unsafe contents";
    if (error == "no-installer") return "The archive has no install.sh";
    if (error == "install-failed") return "install.sh failed";
    if (error == "bad-args") return "The saved install options are invalid";
    if (error == "busy") return "Another update is running";
    if (error == "not-newer") return "Already up to date";
    if (error == "detach-failed") return "Couldn't start the update (systemd-run)";
    if (error == "interrupted") return "The update was interrupted";
    if (error == "io") return "Couldn't write files";
    if (error == "usage" || error == "script-failed" || error == "spawn-failed") return "The updater didn't run";
    return "Something went wrong";
}

const char* languageCode(Language /*language*/) {
    return "en";
}

Language systemLanguage() {
    return Language::En;
}

bool parseLanguage(const std::string& code, Language& language) {
    if (code == "en") {
        language = Language::En;
        return true;
    }
    return false;
}
