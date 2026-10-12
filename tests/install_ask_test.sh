#!/usr/bin/env bash
# T05: install.sh's "disable the old overlay?" question, against a throwaway user unit. Run on the headset.
#   tests/install_ask_test.sh [path/to/install.sh]
# Only offer_to_disable_old_overlay is run (cut out of install.sh), never the whole installer, so nothing is installed.
# The dummy unit is fph-dummy-old-<random>.service in ~/.config/systemd/user, removed again on exit. FPH_OLD_UNIT is
# pointed at it, never at frame-perf-overlay. Exit 77 = skipped (no user systemd / no pty helper).
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
install_sh="${1:-$here/../install.sh}"

skip() { echo "SKIP: $*"; exit 77; }
fail() { echo "FAIL: $*"; failures=$((failures + 1)); }
failures=0

[[ -f "$install_sh" ]] || { echo "no install.sh at $install_sh"; exit 1; }
command -v systemctl >/dev/null 2>&1 || skip "no systemctl"
command -v script >/dev/null 2>&1 || skip "no script(1) to give the test a pty"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
systemctl --user show-environment >/dev/null 2>&1 || skip "no user systemd session"

suffix="$(head -c 6 /dev/urandom | od -An -tx1 | tr -d ' \n')"
dummy="fph-dummy-old-$suffix"
# Never the real thing
[[ "$dummy" == fph-dummy-old-* && "$dummy" != *frame-perf-overlay* ]] || { echo "bad dummy name"; exit 1; }
unit_dir="$HOME/.config/systemd/user"
unit_file="$unit_dir/$dummy.service"
work="$(mktemp -d)"

cleanup() {
    systemctl --user disable --now "$dummy.service" >/dev/null 2>&1 || true
    rm -f "$unit_file"
    rm -rf "$work"
    systemctl --user daemon-reload >/dev/null 2>&1 || true
}
trap cleanup EXIT

# The function, cut out of install.sh (from its header line to the closing brace at column 0)
runner="$work/run.sh"
{
    echo 'set -euo pipefail'
    echo "old_name=\"$dummy\""
    sed -n '/^offer_to_disable_old_overlay() {$/,/^}$/p' "$install_sh"
    echo 'offer_to_disable_old_overlay'
} > "$runner"
grep -q '^offer_to_disable_old_overlay() {' "$runner" || { echo "could not find the function in $install_sh"; exit 1; }
# install.sh must read the unit name from FPH_OLD_UNIT, defaulting to the real upstream unit
grep -q 'old_name="${FPH_OLD_UNIT:-frame-perf-overlay}"' "$install_sh" || fail "install.sh does not read FPH_OLD_UNIT"

make_dummy() {
    mkdir -p "$unit_dir"
    cat > "$unit_file" <<EOF
[Unit]
Description=Frame Perf HUD install-ask test dummy (safe to delete)

[Service]
ExecStart=/bin/sleep infinity

[Install]
WantedBy=default.target
EOF
    systemctl --user daemon-reload
    systemctl --user enable --now "$dummy.service" >/dev/null 2>&1
}

is_enabled() { systemctl --user is-enabled --quiet "$dummy.service" 2>/dev/null; }
is_active() { systemctl --user is-active --quiet "$dummy.service" 2>/dev/null; }

# Runs the function with `answer` typed into a pty; output in $work/out
ask() {
    local answer="$1"
    printf '%s\n' "$answer" | script -qec "bash $runner" /dev/null > "$work/out" 2>&1 || true
}

# ---- 1. answer "n": left alone ----
make_dummy
is_enabled && is_active || { fail "dummy unit did not start (setup)"; exit 1; }
ask n
grep -q "Disable and stop it? \[y/N\]" "$work/out" || fail "n: the question was not asked"
grep -q "Left $dummy.service as it is" "$work/out" || fail "n: no 'left as it is' line"
is_enabled || fail "n: dummy was disabled"
is_active || fail "n: dummy was stopped"

# ---- 2. just Enter (the default is No) ----
ask ""
grep -q "Left $dummy.service as it is" "$work/out" || fail "enter: default is not No"
is_enabled && is_active || fail "enter: dummy was touched"

# ---- 3. something else ("nope", "yes please" starts with y so that is case 4) ----
ask nope
is_enabled && is_active || fail "nope: dummy was touched"

# ---- 4. answer "y": disabled and stopped ----
ask y
grep -q "Disabled $dummy.service" "$work/out" || fail "y: no 'Disabled' line"
is_enabled && fail "y: dummy is still enabled"
is_active && fail "y: dummy is still running"
[[ -f "$unit_file" ]] || fail "y: the unit file was deleted (it should only be disabled)"

# ---- 5. already disabled: no question at all ----
ask y
grep -q "Disable and stop it" "$work/out" && fail "disabled: it still asked"
grep -q "Disabled $dummy.service" "$work/out" && fail "disabled: it disabled again"

# ---- 6. capital Y ----
make_dummy
ask Y
is_enabled && fail "Y: dummy is still enabled"
is_active && fail "Y: dummy is still running"

# ---- 7. not a terminal: prints the note, leaves it alone ----
make_dummy
bash "$runner" < /dev/null > "$work/out" 2>&1
grep -q "Note: $dummy.service is also enabled" "$work/out" || fail "no tty: no note"
grep -q "systemctl --user disable --now $dummy.service" "$work/out" || fail "no tty: no hint on how to turn it off"
grep -q "Disable and stop it" "$work/out" && fail "no tty: it asked"
is_enabled && is_active || fail "no tty: dummy was touched"
# ... also with a pipe on stdin that has a "y" waiting
printf 'y\n' | bash "$runner" > "$work/out" 2>&1
is_enabled && is_active || fail "piped y: dummy was touched (an unattended install must never disable it)"

# ---- 8. the unit isn't there at all: silent ----
cleanup_unit() { systemctl --user disable --now "$dummy.service" >/dev/null 2>&1; rm -f "$unit_file"; systemctl --user daemon-reload; }
cleanup_unit
ask y
# (the pty echoes the typed "y", so look for the function's own lines instead of an empty output)
grep -qE "Disable|Note:|Left " "$work/out" && fail "missing unit: it said something: $(cat "$work/out")"

if [[ "$failures" == 0 ]]; then
    echo "install-ask-test: all passed"
    exit 0
fi
echo "install-ask-test: $failures failed"
exit 1
