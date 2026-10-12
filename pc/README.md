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
| `streaming/` | Background OpenVR ping, read-only vrserver telemetry, and allow-listed vrlink event counts |

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
- **Clock:** the PC uses 24 h with the Time row shown; it does not follow the headset's clock setting.

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
`--force-header-mismatch` option changes only the probe's expected header, to exercise L3 failure without
touching SteamVR. `--anchors <driver_vrlink.dll>` checks disk anchors without SteamVR running.
`tools/t1_transfer_latency.py` is the standalone public-ping proof; `tools/streaming-reference/` preserves
the original research scripts.

An unsigned build may be flagged by antivirus heuristics because it opens another process for reading.
If that happens, inspect the detection and this source before deciding whether to allow it. Read access
does not require administrator rights.
