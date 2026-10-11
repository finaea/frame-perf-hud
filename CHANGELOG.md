# Changelog

## 0.1.0 (unreleased)

Forked from [frame-perf-overlay](https://github.com/sasaken1102r/frame-perf-overlay) 0.3.0 (`8baaf7e`).
Its history is in that repository's CHANGELOG.

- Renamed to frame-perf-hud everywhere: binary, systemd unit, dashboard tab, `~/.config/frame-perf-hud/`,
  `~/.local/state/frame-perf-hud/`. `install.sh` offers to disable frame-perf-overlay's autostart.
- New repo layout: `core/`, `headset/`, `pc/`, `helper/`, `assets/`, `tools/`.
- Metric registry in `core/`: every metric has a stable id, one owner widget, a source, a read interval, a wire
  type, and a status (ok or N/A + reason). A unit test fails on a duplicate id, no owner or two owners.
- Demand-driven scheduler: a source is read only while an enabled widget needs it and someone is looking. Hiding
  the HUD (with the dashboard tab closed) or taking the headset off stops every read.
- Main loop sleeps in one `poll()` on separate deadlines (metrics 2 Hz, input 50 / 100 ms only while the dashboard
  tab is visible, a camera eventfd stub). Signals wake it at once.
- `--debug-dump` prints the running instance's sources, reference counts, read counters and metrics.
- English only (the i18n layer stays); log and command-line output are English now.
- Bundled Noto Sans (SIL OFL 1.1) instead of the system Noto Sans CJK.
- Update check points at `finaea/frame-perf-hud`.
