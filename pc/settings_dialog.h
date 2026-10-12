// The Settings dialog: manual headset IP, "Start with Windows", and what this PC / the headset are.
#pragma once

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
};

/** @return true on OK (data updated), false on Cancel */
bool showSettingsDialog(HINSTANCE instance, HWND parent, SettingsDialogData& data);

}  // namespace pc
}  // namespace fph
