// frame-perf-hud for Windows (ticket 07). One instance per Windows session; a second launch brings the first forward.
//   FramePerfHud.exe [--tray] [--data-dir DIR] [--offline] [--screenshot PNG] [--exit-after SECONDS]
#include "app_window.h"
#include "single_instance.h"
#include "win_text.h"

#include <shellapi.h>

#include <cstdlib>
#include <string>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    fph::pc::AppOptions options;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv != nullptr && i < argc; ++i) {
        const std::wstring a = argv[i];
        const bool next = i + 1 < argc;
        if (a == L"--tray") options.startInTray = true;
        else if (a == L"--data-dir" && next) options.dataDir = fph::pc::narrow(argv[++i]);
        else if (a == L"--screenshot" && next) options.screenshotPath = fph::pc::narrow(argv[++i]);
        else if (a == L"--offline") options.offline = true;
        else if (a == L"--exit-after" && next) options.exitAfterSec = _wtof(argv[++i]);
    }
    if (argv != nullptr) LocalFree(argv);

    fph::pc::SingleInstance instanceLock;
    if (!instanceLock.isFirst()) {
        fph::pc::activateFirstInstance();
        return 0;
    }
    return fph::pc::runApp(instance, options);
}
