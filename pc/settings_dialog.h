// The Settings dialog: manual headset IP, "Start with Windows", and what this PC / the headset are.
#pragma once

#include <functional>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace fph {
namespace pc {

struct SettingsDialogData {
    std::string manualIp;      ///< in/out; "" = none (validated: empty or dotted IPv4)
    bool autostart = false;    ///< in/out
    int cameraFps = 10;        ///< in/out: T10 camera frames per second sent to this PC (1-30)
    std::string info;          ///< in: read-only text (PC name + id, connection, remembered headsets)
    bool forgetHeadsets = false;  ///< out: "Forget remembered headsets" was pressed
    // Release G: the updater
    bool updateCheck = true;                    ///< in/out: check at start and once a day
    std::function<void()> checkNow;             ///< the Check now button (runs at once, OK or not)
    std::function<std::string()> updateStatus;  ///< its state in one line, re-read while the dialog is open
};

/** @return true on OK (data updated), false on Cancel */
bool showSettingsDialog(HINSTANCE instance, HWND parent, SettingsDialogData& data);

/**
 * R3: a text box for a headset profile's new name.
 * @param name in: the current name (selected); out: the new one, trimmed
 * @return true on Rename with a non-empty name that differs; false on Cancel
 */
bool showRenameDialog(HINSTANCE instance, HWND parent, std::string& name);

/**
 * HTTP camera: a text box for the widget's address. An address parseHttpUrl refuses gets a short message and the
 * dialog stays open.
 * @param url in: the current address (selected); out: the new one, trimmed
 * @return true on OK with a valid address that differs; false on Cancel
 */
bool showHttpCameraDialog(HINSTANCE instance, HWND parent, std::string& url);

}  // namespace pc
}  // namespace fph
