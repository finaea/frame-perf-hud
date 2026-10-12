# pc/

The Windows app, `FramePerfHud.exe`. It mirrors the headset's widgets in a desktop window and edits the layout
over the UDP link (no SSH). One self-contained exe: static CRT, cairo + freetype linked in, fonts embedded.

## Build

```
pc\build.cmd          :: configure + build -> out\pc\build\pc\FramePerfHud.exe
pc\build.cmd test     :: ... and run the pc-* tests (tests\pc_*_test.cpp)
```

It needs Visual Studio 2022 with the C++ workload (its CMake and Ninja are used) and a vcpkg checkout. It looks
in `FPH_VCPKG`, else in `..\vcpkg` next to the repo:
`git clone https://github.com/microsoft/vcpkg ..\vcpkg && ..\vcpkg\bootstrap-vcpkg.bat -disableMetrics`.
The first configure builds cairo, freetype, libpng, zlib and libjpeg-turbo for `x64-windows-static` (~10 min).
Packages go to `out\pc\vcpkg_installed`, and vcpkg's binary cache to `..\vcpkg-cache`.

## Files

| File | What |
| --- | --- |
| `link_client.*` | The link protocol on the PC side (discovery, lease, layout sync, edits). No sockets, so tests drive it |
| `udp_socket.*` | The one Winsock socket, plus per-interface directed broadcasts (255.255.255.255 only leaves through the default interface) |
| `settings.*` | `%LOCALAPPDATA%\frame-perf-hud\settings.json`, saved atomically |
| `editor_model.*` | The PC editor's hit-testing and drop logic on core's grid geometry |
| `app_window.*`, `main.cpp` | The window, tray icon, menus, drawing and editor input |
| `settings_dialog.*`, `app.rc.in`, `resource.h` | The Settings dialog, the fonts and the manifest as resources |
| `single_instance.*`, `platform.*` | Named mutex, data folder, computer name, "Start with Windows" (HKCU Run) |

## Run

- **X hides to the tray.** Exit (in the tray menu or the window's right-click menu) quits and sends `BYE`.
- **Command-line options:**
  - `--tray`: start hidden. "Start with Windows" uses it.
  - `--data-dir DIR`: use DIR instead of `%LOCALAPPDATA%\frame-perf-hud`.
  - `--offline`: open no socket and send nothing.
  - `--screenshot PNG`: write the window's picture once, after ~1.5 s.
  - `--exit-after N`: Exit after N seconds.
- **Log:** `%LOCALAPPDATA%\frame-perf-hud\pc.log`, rewritten on each start.
- **Layout cache:** `layout-cache.json` sits next to the log. It's drawn greyed until the headset answers.
