// The main window + tray icon: mirrors the headset's widgets, shows the link status, hosts the PC editor.
#pragma once

#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace fph {
namespace pc {

struct AppOptions {
    std::string dataDir;         ///< "" = %LOCALAPPDATA%\frame-perf-hud
    bool startInTray = false;    ///< --tray (Start with Windows)
    std::string screenshotPath;  ///< --screenshot PATH: write the window's picture after it first draws (tests)
    double exitAfterSec = 0;     ///< --exit-after N: Exit (BYE included) after N s (tests)
    bool offline = false;        ///< --offline: no socket, nothing sent (look at the window without a headset)
};

/** Runs the app until Exit. @return the process exit code */
int runApp(HINSTANCE instance, const AppOptions& options);

}  // namespace pc
}  // namespace fph
