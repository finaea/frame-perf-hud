# frame-perf-hud

A performance HUD for the Valve Steam Frame. It stays in view inside SteamVR and shows the app's frame rate,
GPU and CPU load, temperatures, power draw, battery levels and the wireless link. It runs on the headset itself
(aarch64 SteamOS) as an OpenVR overlay, with no sudo.

frame-perf-hud is a fork of [frame-perf-overlay](https://github.com/sasaken1102r/frame-perf-overlay) by
sasaken1102r (MIT, forked at `8baaf7e`). The plan is modular widgets you can turn on and off and drag around,
a Windows companion app and session logs.

**Status: early.** The HUD is a grid of widgets in frame-perf-overlay's look. You arrange it from the SteamVR
dashboard, and it can sit on your head, on a wrist, or at a fixed spot in the room. Underneath, every value goes
through a metric registry and a demand-driven scheduler. That means a sensor is only read while an enabled widget needs
it **and** someone is looking (the HUD is shown and the headset is on, or the dashboard tab is open). Hide the HUD
or take the headset off, and nothing is read at all.

![The performance panel](docs/images/panel-en.png)

## What it shows

| Row | Main number | Details | Graph (last 30 s) |
|---|---|---|---|
| Frame | The app's frame rate and the display refresh rate (e.g. `72 fps / 90Hz`) | GPU usage (estimate) next to the title; GPU and CPU time per frame (ms), reprojection ratio, dropped frames | Frame rate, with the refresh rate as a dotted line and GPU usage as a shaded area. A yellow badge shows when SteamVR holds the app at half (or a third) of the refresh rate |
| Power | Power draw of the main supply rail (W) | Sum of all measured channels, fan speed | Main rail power |
| Temp | CPU temperature (hottest core) | GPU, battery, and near the display / exhaust / heat sink | CPU and GPU, with the warning thresholds. A red badge appears while heat throttles the CPU or GPU |
| Bottom line 1 | CPU usage (overall and busiest core), fastest cluster clock, GPU clock | | |
| Bottom line 2 | Wireless: **Link** when a PC is on the Steam Link dongle link, otherwise **Wi-Fi** for the access point the headset is joined to. Signal, data flowing (down / up, Mbps) and link rates | | |
| Bottom line 3 | Headset battery, controller batteries (a bolt while charging), memory | | |
| Bottom line 4 | The headset's clock (24-hour by default; 12-hour or off in the settings) | | |

Numbers over a threshold turn yellow, then red past a second one. The thresholds are in the settings file.

## Install

There's no release yet, so build it on the headset. You need a Steam Frame with Developer Mode on and SSH access.
The toolchain and libraries are already on SteamOS: cmake, ninja, g++, pkg-config, and the cairo, freetype2,
vulkan and libnl-genl-3.0 dev files.

```sh
git clone https://github.com/finaea/frame-perf-hud ~/projects/frame-perf-hud
cd ~/projects/frame-perf-hud
cmake -G Ninja -S . -B build
cmake --build build
./install.sh
```

`install.sh` puts these in your home folder:

- the program: `~/.local/bin/frame-perf-hud`
- the bundled font (Noto Sans, SIL OFL 1.1): `~/.local/share/frame-perf-hud/fonts/`
- the launcher entry and icons: `~/.local/share/applications/frame-perf-hud.desktop`, `~/.local/share/icons/hicolor/*/apps/frame-perf-hud.png`
- a systemd user service that starts with SteamVR: `~/.config/systemd/user/frame-perf-hud.service`
- the updater script: `~/.local/share/frame-perf-hud/frame-update.sh`
- folders for settings and (later) session logs: `~/.config/frame-perf-hud/`, `~/.local/state/frame-perf-hud/`

If upstream frame-perf-overlay is set to autostart, `install.sh` asks whether to disable it, so you don't get two
HUDs. It only asks in a terminal. Without one (e.g. the in-app updater), it leaves it alone and prints the
command instead.

Options: `./install.sh --no-autostart` installs without enabling the service.

### Uninstall

```sh
cd ~/projects/frame-perf-hud
./install.sh --uninstall
```

That stops it and removes everything listed above, plus the update checker's cache in
`~/.cache/frame-perf-hud/`, except your settings in `~/.config/frame-perf-hud/` (it prints the command to delete
those too). Session logs in `~/.local/state/frame-perf-hud/` go with it. If you disabled frame-perf-overlay during the install, turn it back on with
`systemctl --user enable --now frame-perf-overlay.service`.

## Usage

- **The HUD** shows up whenever SteamVR runs (with autostart on). It needs no input.
- **Settings:** open the SteamVR dashboard and pick the **Frame Perf HUD** tab. Point with the laser and click.
  The tab has two pages, switched at the top: **Layout** and **Placement**.
- **Layout page:**
  - Left: the HUD's columns, one card per widget. Press on a card, drag it, and let go where the pink marker shows.
    That works within a column, into another one, or into the dashed **New column** lane. A column that ends up empty
    goes away. Scroll with the thumbstick or trackpad when the columns are taller than the card.
  - Right: **Widgets**. Tap one to add it (under the selected card, or at the end of the shortest column), or drag
    it into a column. Ticked widgets are already on; Network can be added more than once. To remove a widget, drag
    its card onto this card, or select it and press **Remove**.
  - Tap a card to select it. Its settings show under the widgets: rows and "one row per name" for the process
    lists, the adapter (Dongle / Home Wi-Fi) for Network.
  - Every change is saved at once. If the companion PC app changed the layout in the meantime, the tab says so
    and shows the newer layout. Do it again from there.
- **Placement page:**

  | Control | What it does |
  |---|---|
  | Show: On / Off | Show or hide the HUD. While hidden (and the tab is closed), nothing is read or drawn |
  | Column − / + | Size (per column): width of one widget column in 2 cm steps (6 cm to 1 m). The HUD is that × its number of columns |
  | Opacity − / + | 10 % steps (20 % to 100 %) |
  | Clock: Off / 12h / 24h | The Time row in the General widget: hidden, 12-hour or 24-hour |
  | Reset | Back to the default show, attachment, position, facing, fade, clock, size and opacity |
  | Attach to: Head / L hand / R hand / Room | In front of your eyes, on a controller like a wristwatch, or pinned at a spot in the room (it stays there in games too). Each hand keeps its own position and facing |
  | Bring it in front of me (room) | Put it 1 m in front of you at eye height, facing you. The first switch to Room does this too |
  | Moving it (room) | While the dashboard is open, point the laser at the HUD itself, hold the trigger and move it. Let go to leave it there. Closing the dashboard while holding it counts as letting go |
  | Top L / Top R / Bottom L / Bottom C / Bottom R (head) | Move it to that corner of your view, facing you |
  | Default wrist position (hand) | Put that hand's HUD back to the standard place and facing |
  | Nudge: Left / Right / Up / Down, Closer / Farther | Head: 2 cm (5 cm for closer / farther). Hand: 1 cm along the HUD's own axes. Room: 2 cm (5 cm) along the HUD's own axes |
  | Facing: Left / Right / Up / Down, Rotate L / R | Turn or spin the HUD by the chosen step (By 1° / By 5°) |
  | Face me / Face ahead (head) | Turn it to face your head, or back to parallel with your face |
  | Face me (room) | Turn it where it is to face you, upright |
  | Fade on tilt, Fade angle (hand) | Fade the wrist HUD as its face turns away from you |
  | Autostart: On / Off | Enable or disable the systemd service (from the next SteamVR start) |
  | Quit app | Click twice within 3 seconds to quit |

- **The + button** ("launch a program") lists **Frame Perf HUD**. Launching it while it runs toggles the HUD.
- **Quitting:** use the tab icon's close button or **Quit app**. It stays off until the next SteamVR start. A crash
  or a SteamVR-only restart brings it back after 5 s.

## Language

English only. The UI strings all sit in one table (`headset/i18n.cpp`), so **other languages are welcome**: add a
`Language` value, a `UiText` table and a branch in each text function, then send a PR.

## Settings file

`~/.config/frame-perf-hud/config.json` (under `$XDG_CONFIG_HOME` if set). Missing keys keep their defaults.
Changes are picked up while it runs. [`contrib/config.example.json`](contrib/config.example.json) has every key at
its default. The keys are the same as frame-perf-overlay's, except:

- `language`: `"en"` is the only value for now.
- `attachment`: also `"detached"` (Room). `detached_pose` holds its spot: 12 numbers, a 3×4 matrix row by row in
  SteamVR's standing space. Without it, the HUD is put in front of you the next time it starts.
- `font` / `font_bold`: empty (the default) means the bundled Noto Sans. Set a path to use another font.
- `update_interval_ms`: the HUD never redraws faster than 2 Hz, whatever this says.
- `width_m`: the width of **one column** of widgets (default 0.2 m), so text keeps the same size whatever the
  layout. A three-column HUD is three times as wide. Old configs are read as-is.
- `layout`: the widget grid, written on first run. `columns` is a list of columns, each a list of widgets top
  to bottom. A widget that isn't listed is off and reads nothing. Types: `frame`, `cpu`, `gpu_ram`, `power`,
  `battery`, `temp`, `cpu_items` / `ram_items` (`max_items`, `sum_same_name`), `network` (`adapter`: `"dongle"`
  or `"home"`, add it twice for both), `general`, and `streaming`, `headset_camera`, `eye_camera` (not live yet).
  `version` goes up on every edit. A broken `layout` falls back to the default one, with a log line.

## Updates

The dashboard tab shows the running version and checks GitHub (`finaea/frame-perf-hud`) for a newer release at
start and at most once a day. `"update_check": false` turns the automatic check off; **Check now** still works.
Installing an update downloads the release tar.gz, checks it against the release's `SHA256SUMS` and runs its
`install.sh`.

## Troubleshooting

- **Logs:** `journalctl --user -u frame-perf-hud -f`. `[demand]` lines say when reading starts and pauses.
- **Debug dump:** `frame-perf-hud --debug-dump` prints the running instance's state: the "someone looking" signals,
  every source with its interval, reference count and read counter, and every metric with its value or N/A reason.
  Run it twice: if the read counters don't move, nothing is being read.
- **`--` instead of a value:** that sensor wasn't found or couldn't be read. `frame-perf-hud --print` lists what was found.
- **No HUD:** check that SteamVR runs, that **Show** is on, and `systemctl --user status frame-perf-hud`.

## Privacy

No telemetry. The only network access is the GitHub update check. It reads sensors from sysfs and `/proc` and
never writes there. Wi-Fi: signal, link rates and byte counters only, with no MAC addresses or SSIDs. Files
written: its settings, a lock file and the debug dump in `/run/user/<uid>` (memory only), and the updater's cache
in `~/.cache/frame-perf-hud/`.

## Disclaimer

Unofficial. Not affiliated with or endorsed by Valve Corporation. Steam, Steam Frame, SteamVR and Steam Link are
trademarks of Valve Corporation. Use at your own risk; there's no warranty (see [LICENSE](LICENSE)).

## Development

```sh
cmake -G Ninja -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure   # core registry + scheduler, upstream's wrist/clock/config/panel tests
scripts/package.sh                           # release build: dist/frame-perf-hud-<version>.tar.gz + SHA256SUMS
```

`core/` (registry, value store, scheduler) is platform-free. `cmake -DFPH_BUILD_HEADSET=OFF` builds just it and its
tests anywhere. The layout:

| Folder | What |
|---|---|
| `core/` | shared C++17: metric registry, value store with status / reason, demand-driven scheduler |
| `headset/` | the Frame agent (upstream `src/`): sensors, OpenVR overlay, Vulkan texture, dashboard tab |
| `pc/`, `helper/` | the Windows app and the camera helper (not started yet) |
| `assets/` | the bundled font |
| `tools/` | reference scripts |

Useful options without SteamVR:

```sh
./build/frame-perf-hud --print --count 5
./build/frame-perf-hud --dump-png panel.png --seconds 30 --fake-frames
./build/frame-perf-hud --dump-settings-png settings.png
./build/frame-perf-hud --contrast-report
```

`vendor/frame-updater/` is a copy of upstream's updater. Don't edit it: `scripts/package.sh` checks it.

## License

MIT, see [LICENSE](LICENSE) (the original copyright line is kept). Third-party code, the bundled font and the
system libraries used at run time are listed in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

## Credits

- [frame-perf-overlay](https://github.com/sasaken1102r/frame-perf-overlay) by sasaken1102r, which this is forked from.
- Wrist attachment, the clock and the tilt fade in frame-perf-overlay by [TaiKeid](https://github.com/TaiKeid)
  ([#1](https://github.com/sasaken1102r/frame-perf-overlay/pull/1)).
