# pc/

The Windows app, `FramePerfHud.exe`. It mirrors the headset's widgets in a desktop window, edits the layout and the
headset's profiles over the UDP link (no SSH), reads the PC's side of a Steam Link stream, keeps its own copy of
the session logs, and updates itself. One self-contained exe: static CRT, cairo, freetype and libjpeg-turbo linked
in, fonts embedded.

## Build

```bat
pc\build.cmd            :: configure + build -> out\pc\build\pc\FramePerfHud.exe, plus the tests and tools
pc\build.cmd test       :: ... and run the pc-* tests (tests\pc_*_test.cpp)
pc\build.cmd release    :: only the exe, clean, no tests
pc\package.cmd          :: a release: build in out\pc\build-release, dist\FramePerfHud-<version>.exe, SHA256SUMS line
```

It needs Visual Studio 2022 with the C++ workload (its CMake and Ninja are used) and a vcpkg checkout. It looks
in `FPH_VCPKG`, else in `..\vcpkg` next to the repo:
`git clone https://github.com/microsoft/vcpkg ..\vcpkg && ..\vcpkg\bootstrap-vcpkg.bat -disableMetrics`.
The first configure builds cairo, freetype, libpng, zlib and libjpeg-turbo for `x64-windows-static` (~10 min).
Packages go to `out\pc\vcpkg_installed`, and vcpkg's binary cache to `..\vcpkg-cache`.

`FPH_PC_BUILD_DIR` builds somewhere other than `out\pc\build`, so a copy running from there isn't touched (a
running exe can't be relinked). The version comes from `project(VERSION)` in the root `CMakeLists.txt`.
Releasing both halves is in [`docs/RELEASING.md`](../docs/RELEASING.md).

## Files

| File | What |
| --- | --- |
| `link_client.*` | The link protocol on the PC side: discovery, lease, layout sync and edits, the Profiles / Logs controls. No sockets, so tests drive it |
| `udp_socket.*` | The one Winsock socket, plus per-interface directed broadcasts (255.255.255.255 only leaves through the default interface) |
| `app_window.*`, `main.cpp` | The window, tray icon, menus, status bar, drawing and editor input |
| `panel_view.*` | The widgets drawn as vectors at the window's scale (the shared `core/render` code) |
| `pc_editor.*`, `editor_model.*` | The layout editor: hit-testing, drop logic, the palette and per-widget settings |
| `pc_pages.*` | The Layout / Profiles / Logs tabs and the two pages |
| `session_logs.*` | The PC's copy of the session logs, Save Logs, the save when the headset goes silent. Sessions end like on the headset: a minute after the game quits, at the moment it left |
| `updater.*` | The self-updater: GitHub's latest release (WinHTTP), SHA-256 check (BCrypt), the exe swap |
| `camera_feed.*` | Camera pictures from the headset, decoded for the camera widgets |
| `streaming/` | The Streaming widget's PC side: background OpenVR ping, read-only vrserver statistics, allow-listed vrlink log counters |
| `settings.*` | `%LOCALAPPDATA%\frame-perf-hud\settings.json`, saved atomically |
| `settings_dialog.*`, `app.rc.in`, `resource.h` | The Settings and Rename dialogs, the fonts and the manifest as resources |
| `single_instance.*`, `platform.*` | Named mutex, data folder, computer name, "Start with Windows" (HKCU Run) |

## Run

- **X hides to the tray.** Exit (in the tray menu or the window's right-click menu) quits and sends `BYE`, and so
  does Windows shutting down. A second start shows the running window instead.
- **Command-line options:**
  - `--tray`: start hidden. "Start with Windows" uses it.
  - `--data-dir DIR`: use DIR instead of `%LOCALAPPDATA%\frame-perf-hud`.
  - `--offline`: open no socket and send nothing.
  - `--screenshot PNG`: write the window's picture once, after ~1.5 s.
  - `--exit-after N`: Exit after N seconds.
  - `--wait-pid PID` and `--updated`: used by the updater when it starts the new exe.
- **Log:** `%LOCALAPPDATA%\frame-perf-hud\pc.log`, rewritten on each start.
- **Layout cache:** `layout-cache.json` sits next to the log. It's drawn greyed until the headset answers.
- **Clock:** the PC uses 24 h with the Time row shown; it doesn't follow the headset's clock setting.

## Tabs

- **Layout:** the panel, or the editor with **Edit layout** (Ctrl+E).
- **Profiles:** the headset's profiles. The PC sends the button (`CONTROL_ACTION`) and the headset carries it out
  with the same rules as its own dashboard, including the second tap when unsaved changes would be lost. The list
  comes from the headset's `CONTROL_STATE`. A headset older than these messages leaves the tab with an "update it"
  note and its buttons disabled.
- **Logs:** the PC's Save Logs, Open logs folder and last session card; the headset's Record and crash recorder
  switches and last power-off reason.

## Updates

At start and then about once a day (a failed check is retried after an hour; a switch in Settings, plus
**Check now**), the app asks GitHub for the latest release of `finaea/frame-perf-hud`. When it's newer than this build, the status bar and the tray menu offer
**Install**: the app downloads `FramePerfHud-<version>.exe` to `%LOCALAPPDATA%\frame-perf-hud\update\`, checks
it against the release's `SHA256SUMS`, renames the running exe to `.old`, puts the new one in its place, starts it
and exits. The next start deletes the `.old`. If the exe's folder isn't writable, it opens the release page
instead. Only Windows' own WinHTTP and BCrypt are used, so the exe stays one file.

## Streaming

Enable Streaming in the editor to read PC telemetry and send it to the headset at 1 Hz. It uses SteamVR's
installed OpenVR DLL, so there is nothing extra to install. Quitting SteamVR releases OpenVR immediately;
the app stays open and reconnects when SteamVR returns. With Streaming off, its worker sleeps and holds no
SteamVR or vrserver handle.

The detailed streaming values are read from statistics SteamVR already maintains in `vrserver.exe`, with
query and read access only. The app never writes memory, injects code, enables a debug graph, or changes
SteamVR files or settings. It checks the loaded build against the DLL, resolves anchors, checks object
identity and named keys, and compares each detail layer against live named statistics. A changed layout
shows N/A with a build note and retries; public ping and unaffected layers keep working. No recent frames
means "no frames". The debug graph can stay off.

The PC log records build/layer status and cache resets. SteamVR log lines never enter packets or the PC
log: only reset, bad-link, encoder-reinit and throttle counters are extracted. Those counters are since
the app began watching, including the current log's existing events.

For a read-only diagnostic, run `out\pc\build\pc\fph-streaming-probe.exe --seconds 15`. Its
`--force-header-mismatch` option changes only the probe's expected header, to exercise a layer failure without
touching SteamVR. `--anchors <driver_vrlink.dll>` checks disk anchors without SteamVR running.
`tools/t1_transfer_latency.py` is the standalone public-ping proof; `tools/streaming-reference/` preserves
the original research scripts.

An unsigned build may be flagged by antivirus heuristics because it opens another process for reading.
If that happens, inspect the detection and this source before deciding whether to allow it. Read access
does not require administrator rights.

## Preview tools

These draw with the same code as the app, from made-up sample values, without a headset:

- `fph-docs-images.exe OUT_DIR`: the README's HUD images and the window with each tab.
- `fph-pc-pages-preview.exe OUT_DIR`: the three tabs and the status bar at 100 % and 200 %.
- `fph-streaming-preview.exe OUT_DIR`: the Streaming widget in its states, full and compact.
