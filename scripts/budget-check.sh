#!/usr/bin/env bash
# M1 budget run (ticket 05): the HUD's own CPU and its power cost, measured on the Frame with the headset worn.
#
#   scripts/budget-check.sh [OUT_DIR]
#
# What it does, after a warm-up with the HUD on:
#   - PAIRS × (HUD on for WINDOW s, HUD off for WINDOW s), alternating. "Off" = the service stopped, so the
#     compositor has no HUD overlay either. The first SETTLE s of every window are thrown away (start-up, rail lag).
#   - vph power (max34417 hwmon, µW) every second; the HUD's CPU time from /proc/<pid>/stat over each on window.
#   - Then one WINDOW with the HUD running but hidden (and no PC): its CPU should be ~0.
# For the run it uses the largest supported layout (scripts/budget-layout.json) attached to the head, so the HUD is
# composited the whole time. Your config.json is backed up first and put back at the end, whatever happens.
#
# Needs: SteamVR running, frame-perf-hud installed as the user service, python3. The dashboard must stay closed.
set -euo pipefail

unit="frame-perf-hud.service"
bin="$HOME/.local/bin/frame-perf-hud"
cfg="${XDG_CONFIG_HOME:-$HOME/.config}/frame-perf-hud/config.json"
here="$(cd "$(dirname "$0")" && pwd)"
out="${1:-$HOME/projects/fph-t05/budget/$(date +%Y%m%d-%H%M%S)}"
warmup="${WARMUP:-180}"
window="${WINDOW:-75}"
settle="${SETTLE:-15}"
pairs="${PAIRS:-4}"
tck="$(getconf CLK_TCK)"

mkdir -p "$out"
systemctl --user is-active --quiet steamvr.service || { echo "SteamVR isn't running" >&2; exit 1; }
[[ -f "$cfg" ]] || { echo "No $cfg" >&2; exit 1; }

# vph: the hwmon power channel labelled vph
vph=""
for label in /sys/class/hwmon/hwmon*/power*_label; do
    if [[ "$(cat "$label" 2>/dev/null)" == "vph" ]]; then vph="${label%_label}_input"; break; fi
done
[[ -n "$vph" ]] || { echo "No vph power channel found" >&2; exit 1; }

cp -p "$cfg" "$out/config.json.before"
sampler=""
restore() {
    [[ -n "$sampler" ]] && kill "$sampler" 2>/dev/null || true
    cp -p "$out/config.json.before" "$cfg"  # the running HUD reloads it within 0.5 s
    systemctl --user start "$unit" || true
    echo "Restored $cfg and started $unit."
}
trap restore EXIT

# Test config: yours, with the head attachment, shown, and the largest layout
python3 - "$cfg" "$here/budget-layout.json" <<'EOF'
import json, sys
cfg_path, layout_path = sys.argv[1], sys.argv[2]
cfg = json.load(open(cfg_path))
layout = json.load(open(layout_path))
layout["version"] = int(cfg.get("layout", {}).get("version", 1)) + 1
cfg.update({"attachment": "head", "visible": True, "layout": layout})
tmp = cfg_path + ".budget-tmp"
json.dump(cfg, open(tmp, "w"), indent=2)
import os; os.replace(tmp, cfg_path)
EOF

phase_file="$out/phase"
echo warmup >"$phase_file"
( while true; do
      printf '%s,%s,%s\n' "$(date +%s.%N)" "$(cat "$phase_file")" "$(cat "$vph")"
      sleep 1
  done ) >"$out/vph.csv" &
sampler=$!

main_pid() { systemctl --user show -p MainPID --value "$unit"; }
cpu_ticks() { awk '{ s = $0; sub(/^.*\) /, "", s); split(s, f, " "); print f[12] + f[13] }' "/proc/$1/stat"; }

# One on window: CPU ticks over the measured part
measure_on() {
    local name="$1" pid t0 c0 t1 c1
    systemctl --user start "$unit"
    sleep "$settle"
    pid="$(main_pid)"
    [[ "$pid" -gt 0 ]] || { echo "$unit isn't running" >&2; exit 1; }
    echo "$name" >"$phase_file"
    t0="$(date +%s.%N)"; c0="$(cpu_ticks "$pid")"
    sleep "$((window - settle))"
    t1="$(date +%s.%N)"; c1="$(cpu_ticks "$pid")"
    echo gap >"$phase_file"
    echo "$name,$pid,$t0,$t1,$c0,$c1,$tck" >>"$out/cpu.csv"
}

echo "window,pid,t0,t1,ticks0,ticks1,clk_tck" >"$out/cpu.csv"
start_iso="$(date --iso-8601=seconds)"
echo "Warm-up ${warmup}s with the HUD on..."
systemctl --user start "$unit"
sleep "$warmup"
for i in $(seq 1 "$pairs"); do
    echo "Pair $i/$pairs: on"
    systemctl --user restart "$unit"  # same start-up cost in every on window
    measure_on "on$i"
    echo "Pair $i/$pairs: off"
    systemctl --user stop "$unit"
    sleep "$settle"
    echo "off$i" >"$phase_file"
    sleep "$((window - settle))"
    echo gap >"$phase_file"
done

echo "Hidden, no PC: one window"
systemctl --user start "$unit"
sleep "$settle"
"$bin" >/dev/null 2>&1 || true  # a second launch toggles the running one: hidden now
sleep 2
measure_on hidden

kill "$sampler" 2>/dev/null || true
sampler=""
journalctl --user -u "$unit" --since "$start_iso" --no-pager | grep -E '\[demand\]|\[dashboard\]' >"$out/journal-demand.txt" || true

python3 - "$out" <<'EOF'
import csv, statistics, sys
out = sys.argv[1]
rows = [r for r in csv.reader(open(f"{out}/vph.csv")) if len(r) == 3 and r[2].strip()]
by = {}
for t, phase, uw in rows:
    by.setdefault(phase, []).append(int(uw) / 1e6)
lines = ["M1 budget run", ""]
diffs = []
i = 1
while f"on{i}" in by and f"off{i}" in by:
    on, off = statistics.mean(by[f"on{i}"]), statistics.mean(by[f"off{i}"])
    diffs.append(on - off)
    lines.append(f"pair {i}: vph on {on:.3f} W ({len(by[f'on{i}'])} s), off {off:.3f} W ({len(by[f'off{i}'])} s), on-off {on - off:+.3f} W")
    i += 1
if diffs:
    sd = statistics.stdev(diffs) if len(diffs) > 1 else float("nan")
    lines.append(f"vph on-off: mean {statistics.mean(diffs):+.3f} W, sd {sd:.3f} W over {len(diffs)} pairs (budget <= 0.1 W)")
lines.append("")
for r in csv.DictReader(open(f"{out}/cpu.csv")):
    secs = float(r["t1"]) - float(r["t0"])
    cpu = (int(r["ticks1"]) - int(r["ticks0"])) / int(r["clk_tck"])
    lines.append(f"{r['window']}: own CPU {100 * cpu / secs:.2f}% of one core over {secs:.0f} s (pid {r['pid']})")
lines.append("")
lines.append("Budgets: own CPU <= 2% of one core (on windows); hidden ~0; vph on-off <= 0.1 W.")
lines.append("Check journal-demand.txt: the HMD activity level must not go to 3 (standby) during the run.")
open(f"{out}/summary.txt", "w").write("\n".join(lines) + "\n")
print("\n".join(lines))
EOF
echo "Results: $out"
