// frame-perf-hud for Windows (ticket 07). One instance per Windows session; a second launch brings the first forward.
//   FramePerfHud.exe [--tray] [--data-dir DIR] [--offline] [--screenshot PNG] [--exit-after SECONDS]
//   (the updater starts the new exe with --wait-pid PID --updated: it waits for the old one to exit first)
#include "app_window.h"
#include "single_instance.h"
#include "win_text.h"

#include <shellapi.h>

#include <cstdlib>
#include <memory>
#include <string>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    fph::pc::AppOptions options;
    DWORD waitPid = 0;
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
        else if (a == L"--wait-pid" && next) waitPid = wcstoul(argv[++i], nullptr, 10);  // Release G
        else if (a == L"--updated") options.justUpdated = true;
    }
    if (argv != nullptr) LocalFree(argv);

    // Release G: started by the updater. The old process holds the instance mutex until it has said BYE and exited,
    // which can take a while (it shuts OpenVR down). No time limit: giving up here would leave nothing running.
    // OpenProcess failing means it's already gone.
    if (waitPid != 0) {
        if (HANDLE previous = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, waitPid)) {
            DWORD waited = WaitForSingleObject(previous, INFINITE);
            while (waited == WAIT_FAILED) {  // shouldn't happen with a SYNCHRONIZE handle: poll instead
                Sleep(500);
                DWORD code = 0;
                if (!GetExitCodeProcess(previous, &code) || code != STILL_ACTIVE) break;
            }
            CloseHandle(previous);
        }
    }
    auto instanceLock = std::make_unique<fph::pc::SingleInstance>();
    // Its mutex goes with it, but give that a moment rather than treat the old copy as a running instance
    for (int i = 0; waitPid != 0 && !instanceLock->isFirst() && i < 50; ++i) {
        instanceLock.reset();
        Sleep(100);
        instanceLock = std::make_unique<fph::pc::SingleInstance>();
    }
    if (!instanceLock->isFirst()) {
        fph::pc::activateFirstInstance();
        return 0;
    }
    return fph::pc::runApp(instance, options);
}
