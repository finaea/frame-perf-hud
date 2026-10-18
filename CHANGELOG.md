# Changelog

## 0.4.1 — 2026-10-18

- **GPU load replaces GPU usage.** On SteamOS 0.5.6 (kernel 6.18) the kernel's GPU counter that GPU usage came from
  (`drm-engine-gpu` in DRM fdinfo) is broken and grows by months per second, so GPU usage sat at 100 %. Its
  neighbour `drm-cycles-gpu` misses most of a heavy game (about 4 % in Half-Life: Alyx). GPU & RAM now shows GPU
  load instead: the GPU time per frame SteamVR measures × the app's frame rate, capped at 100 %. It counts the VR
  app and the compositor only, and is N/A while no app submits frames. The metric (`gpu.busy_pct`) and the
  `gpu_warn_pct` / `gpu_crit_pct` thresholds keep their names, so session logs and older PC apps still read it.
  Ported from frame-perf-overlay (sasaken1102r, `187252c`).
- The fdinfo reader is gone, and with it a `/proc` scan of up to 20 ms every 30 s. `--print` no longer shows a GPU
  figure, since it doesn't read SteamVR.
- **Session logs keep every second.** A session could miss every other second while the HUD's main loop ran a little
  slow (17 of 351 s in one real session), because a stopwatch decided when to sample but samples were filed by
  clock second. Sampling now happens once per clock second, so only a real stall of a second or more leaves a gap.
- The README opens with a demo video and says what this adds to frame-perf-overlay.

## 0.4.0 — 2026-10-17

The first release of frame-perf-hud. It's a fork of
[frame-perf-overlay](https://github.com/sasaken1102r/frame-perf-overlay) 0.3.0 (`8baaf7e`), so the version
numbers carry on from there; frame-perf-overlay's own history is in its CHANGELOG.

### A HUD made of widgets

- The fixed panel became a grid of widgets that can be added, removed and dragged around: up to 8 columns, up to
  16 widgets each.
- Widgets: **Frame**, **CPU**, **GPU & RAM**, **Power**, **Battery**, **Temp**, **CPU usage items**, **RAM usage
  items**, **Network** (once per adapter: the Steam Link dongle and home Wi-Fi), **Streaming**, **General**,
  **Headset camera** and **Eye camera**.
- Frame shows CPU and GPU frame-time graphs under the frame rate, plotting the worst frame of every half second
  against a dotted frame-budget line, so single hitches stand out.
- CPU shows every core, the clock of each cluster, and when a cluster is capped below its hardware maximum.
- Power lists every measured rail with its voltage, plus what no rail accounts for.
- Battery shows the real gauge percentage, the power flowing in or out, time to full or empty, health, cycle count,
  the charger's mode, and what a USB-C charger offers against what actually arrives (with the voltage drop).
- Temp adds the NPU, cameras, video block and four board spots next to the CPU, GPU, battery and fan.
- The process lists show what uses the CPU and memory, with a row count, an option for one row per process name,
  and **Hide system**, which leaves SteamOS, Steam and SteamVR processes out. The rules live in an editable
  `hide_rules.json`.
- General shows the controllers' batteries, the time, uptime, the running app, the HUD's own CPU, memory and power
  cost, why the headset last powered off, and when a crash log was kept.
- **Compact** cards for Frame, Power, Battery, Temp and Streaming show only the main numbers.
- Values that can't be read show "N/A" with a short reason (not read yet, sensor missing, no SteamVR, ...) instead
  of a blank.
- Sensors are read only while a widget that needs them is on **and** someone is looking: the HUD is shown and the
  headset is worn, the dashboard tab is open, a PC is connected, or Record is on. Otherwise those reads pause; only
  the optional crash recorder keeps its own few sensors going.

### The dashboard tab

- Four pages: **Placement**, **Layout**, **Profiles** and **Logs**.
- Layout: drag widget cards between columns, add widgets from a palette, change a widget's settings, remove it.
- The HUD can also be pinned in the room (**Room**), moved there with the laser, and curved around the viewer by
  0° to 90° in 10° steps.
- Every on/off setting, on the dashboard and in both layout editors, uses the same On / Off buttons.

### The Windows app

- `FramePerfHud.exe`, one standalone exe. It finds the headset on the network by itself (a manual IP is there as a
  fallback) and connects once the headset's dashboard allows that PC.
- It mirrors the HUD in a resizable window, sharp at any display scale, and the layout can be edited there by
  dragging, like on the dashboard.
- Tabs: **Layout**, **Profiles** (the headset's profiles: use, save, rename, reset, delete) and **Logs** (the PC's
  own Save Logs and last session, plus the headset's Record and crash recorder switches and last power-off reason).
- Lives in the tray, can start with Windows, and sends a clean goodbye to the headset on exit and on Windows
  shutdown.

### Streaming stats

- With PC streaming (Steam Link), the Streaming widget shows the end-to-end latency (game to photon) and its stages
  (game, encode, send, network, decode, slack), the bitrate split into video, FEC, audio, retry and logic traffic,
  each wireless link's share and ping, and counters for retransmits, skipped and late frames, stream resets, bad
  links, encoder resets and throttling.
- The PC app reads these from statistics SteamVR already keeps, read-only, and sends them to the headset once a
  second. The headset adds its own side: which link carries the stream and vrlink's FEC retries and dropped frames.

### Cameras

- **Headset camera** and **Eye camera** widgets show the outward and eye cameras live on the HUD and in the PC app.
- They need a small helper, installed once with sudo (`install-camera-helper.sh`). Everything else needs no sudo.
- The cameras are only read while their widget is on, pause while the headset isn't worn, and never disturb
  tracking: the helper only borrows the frames the headset's tracking service already captures.

### Session logs

- Game sessions are kept in memory and written once, when they end: a summary (averages, minimum, maximum, p95,
  histograms of frame and latency times, drops and throttling) and a compressed per-second timeline. The last 10
  are kept. Samples are taken while something is reading the sensors, so a session with less than a minute of
  samples is skipped, even if the game ran longer. A session ends a minute after its game quits with nothing else
  started, at the moment the game left, without that minute. A short trip to SteamVR Home, or the same game back
  within the minute, doesn't split a game.
- An end-of-session card shows on the HUD for 10 seconds and stays on the Logs page.
- **Save Logs** writes the last 10 minutes and the session so far, and **Record** writes every second until it's
  turned off.
- The PC app keeps its own copy of each session, and saves it right away if the headset goes silent mid-session,
  so a crash or power cut doesn't lose it.
- The optional **crash recorder** keeps the last 10 minutes of battery, power and temperatures on disk and turns
  them into a crash log when the HUD didn't stop cleanly.
- **Last power-off** shows why the headset last switched off (a normal restart, a normal shutdown, or the raw
  power-controller registers when the reason isn't known yet).
- Session samples stay in memory during play; Save Logs, Record and the crash recorder can also write session data
  while a game runs.

### Profiles

- Up to 8 named layouts on the headset: save the current layout as a new profile, use one, save changes back,
  rename it with SteamVR's keyboard (or a text box on the PC), reset to the default layout, delete.
- A profile is only the layout; placement and app settings stay as they are.
- The active profile shows "unsaved changes" after any edit, from the headset or the PC, and using another profile
  or resetting then asks for a second tap.

### Updates

- The headset checks GitHub at start and about once a day (an hour after a failed check), and installs a newer
  release from the dashboard after checking it against the release's SHA-256 sums.
- The PC app now updates itself the same way: it checks with Windows' own WinHTTP, verifies the download's SHA-256,
  swaps the exe in place and restarts. Each side updates on its own, so both should be kept on the same release.

### Changes from frame-perf-overlay

- Renamed to frame-perf-hud throughout: program, systemd service, dashboard tab and folders. `install.sh` offers to
  turn off frame-perf-overlay's autostart so two HUDs don't run at once.
- English only for now, with the strings in one table so other languages can be added.
- Ships its own Noto Sans font, so the headset and the PC draw the same text.
- `--debug-dump` prints what the running HUD reads and how often.
- Ported from frame-perf-overlay after the fork: control characters in a font path no longer break the settings
  file, a smaller release build (unused code removed at link time), and the GPU usage scan no longer reads every
  file twice.
