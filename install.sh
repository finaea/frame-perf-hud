#!/usr/bin/env bash
# Installs, updates or removes Frame Perf HUD on a Steam Frame. Run it on the headset as the normal user (no sudo).
#   ./install.sh                 install or update, turn on autostart, and (re)start it if SteamVR is running
#   ./install.sh --no-autostart  install or update with autostart off (start it from the + button in the dashboard)
#   ./install.sh --uninstall     remove it (settings are kept)
# Works from an unpacked release tarball and from a built repo (binary in build/).
set -euo pipefail

name="frame-perf-hud"
old_name="frame-perf-overlay"  # the upstream app this is forked from
here="$(cd "$(dirname "$0")" && pwd)"
config_home="${XDG_CONFIG_HOME:-$HOME/.config}"
data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
state_home="${XDG_STATE_HOME:-$HOME/.local/state}"
bin="$HOME/.local/bin/$name"
unit="$name.service"
unit_dir="$config_home/systemd/user"
desktop="$data_home/applications/$name.desktop"
icons="$data_home/icons/hicolor"
icon_sizes="48 128 256"
config_dir="$config_home/$name"
state_dir="$state_home/$name"
font_dir="$data_home/$name/fonts"

usage() {
    cat <<'EOF'
Usage: ./install.sh [--no-autostart | --uninstall]
  (no option)     install or update, turn on autostart, and (re)start it if SteamVR is running
  --no-autostart  install or update with autostart off (start it from the + button in the dashboard)
  --uninstall     remove it (your settings in ~/.config/frame-perf-hud/ are kept)
EOF
}

uninstall=0
autostart=1
for arg in "$@"; do
    case "$arg" in
        --uninstall) uninstall=1 ;;
        --no-autostart) autostart=0 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; usage >&2; exit 2 ;;
    esac
done

# Stops an instance started by hand (+ in the dashboard, outside the service) with SIGTERM.
# The app handles SIGTERM with its normal cleanup (destroy overlays, then VR_Shutdown).
stop_manual_instance() {
    local lock="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$name.lock"
    [[ -r "$lock" ]] || return 0
    local pid
    pid="$(head -n 1 "$lock" 2>/dev/null | tr -dc '0-9')"
    [[ -n "$pid" ]] || return 0
    # The lock file outlives the process, so the PID may be gone or reused: check it's still us.
    # (/proc/<pid>/comm is cut at 15 characters, so compare the executable name from cmdline.)
    # Reading a missing file with < would stop the whole script under set -e, so check first.
    [[ -r "/proc/$pid/cmdline" ]] || return 0
    local argv0
    argv0="$( { tr '\0' '\n' < "/proc/$pid/cmdline"; } 2>/dev/null | head -n 1 || true)"
    [[ "$(basename -- "${argv0:-x}")" == "$name" ]] || return 0
    local service_pid
    service_pid="$(systemctl --user show -p MainPID --value "$unit" 2>/dev/null || echo 0)"
    [[ "$pid" != "$service_pid" ]] || return 0
    echo "Stopping the running $name (PID $pid) that was started outside the service..."
    kill -TERM "$pid" 2>/dev/null || return 0
    for _ in $(seq 1 50); do
        [[ -d "/proc/$pid" ]] || return 0
        sleep 0.1
    done
    echo "Warning: PID $pid is still running. Close it from the SteamVR dashboard (Frame Perf HUD icon > Close)." >&2
}

# Rebuilds the .desktop and icon caches (skipped if the commands don't exist)
refresh_caches() {
    if command -v update-desktop-database >/dev/null 2>&1 && [[ -d "$data_home/applications" ]]; then
        update-desktop-database "$data_home/applications" || true
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1 && [[ -f "$icons/index.theme" ]]; then
        gtk-update-icon-cache -q "$icons" || true
    fi
}

# If upstream frame-perf-overlay is set to autostart, offer to turn it off so there aren't two HUDs.
# Only asks on a terminal; an unattended install (the in-app updater) never touches it.
offer_to_disable_old_overlay() {
    systemctl --user is-enabled --quiet "$old_name.service" 2>/dev/null || return 0
    if [[ ! -t 0 ]]; then
        echo "Note: $old_name.service is also enabled, so both HUDs will start with SteamVR."
        echo "      To turn it off: systemctl --user disable --now $old_name.service"
        return 0
    fi
    local answer=""
    read -r -p "$old_name (the HUD this is forked from) also starts with SteamVR. Disable and stop it? [y/N] " answer || true
    if [[ "$answer" == [yY]* ]]; then
        systemctl --user disable --now "$old_name.service" || true
        echo "Disabled $old_name.service (its files are untouched; re-enable with: systemctl --user enable $old_name.service)"
    else
        echo "Left $old_name.service as it is. Both HUDs will run."
    fi
}

if [[ "$uninstall" == 1 ]]; then
    # --now stops with SIGTERM, so the app's normal cleanup runs
    systemctl --user disable --now "$unit" 2>/dev/null || true
    stop_manual_instance
    rm -f "$unit_dir/$unit" "$bin" "$desktop" "$data_home/$name/frame-update.sh"
    rm -rf "$font_dir"
    rmdir "$data_home/$name" 2>/dev/null || true  # only if empty
    for size in $icon_sizes; do
        rm -f "$icons/${size}x${size}/apps/$name.png"
    done
    systemctl --user daemon-reload || true
    refresh_caches
    echo "Removed $name."
    if [[ -d "$config_dir" ]]; then
        echo "Your settings are kept in $config_dir. To delete them too: rm -r \"$config_dir\""
    fi
    exit 0
fi

if [[ "$(uname -m)" != "aarch64" ]]; then
    echo "This build is for the Steam Frame (aarch64), but this machine is $(uname -m)." >&2
    exit 1
fi

# Binary: next to install.sh in a release, build/ in the repo
if [[ -f "$here/$name" ]]; then
    src_bin="$here/$name"
elif [[ -f "$here/build/$name" ]]; then
    src_bin="$here/build/$name"
else
    echo "Cannot find the $name binary next to install.sh or in build/. Build it first (see README)." >&2
    exit 1
fi
for f in "contrib/$unit" "contrib/$name.desktop" "assets/fonts/NotoSans-Regular.ttf" "assets/fonts/NotoSans-Bold.ttf"; do
    if [[ ! -f "$here/$f" ]]; then
        echo "Missing $here/$f" >&2
        exit 1
    fi
done

# Binary (written to a temp file and renamed, so a running copy isn't broken)
install -Dm755 "$src_bin" "$bin.new"
mv -f "$bin.new" "$bin"

# Bundled font (Noto Sans, SIL OFL 1.1) and its licence
install -Dm644 "$here/assets/fonts/NotoSans-Regular.ttf" "$font_dir/NotoSans-Regular.ttf"
install -Dm644 "$here/assets/fonts/NotoSans-Bold.ttf" "$font_dir/NotoSans-Bold.ttf"
install -Dm644 "$here/assets/fonts/OFL.txt" "$font_dir/OFL.txt"

# Icons (Steam looks in hicolor/<size>/apps; 256x256 alone wasn't found, so install 3 sizes)
for size in $icon_sizes; do
    install -Dm644 "$here/contrib/icons/$name-$size.png" "$icons/${size}x${size}/apps/$name.png"
done

# .desktop (listed under + in the dashboard). Exec gets the absolute path: Steam may start it without
# ~/.local/bin on PATH
mkdir -p "$(dirname "$desktop")"
sed "s|@BINARY@|$bin|" "$here/contrib/$name.desktop" > "$desktop.tmp"
chmod 644 "$desktop.tmp"
mv -f "$desktop.tmp" "$desktop"
if command -v desktop-file-validate >/dev/null 2>&1; then
    desktop-file-validate "$desktop" || true
fi
refresh_caches

# systemd user unit
install -Dm644 "$here/contrib/$unit" "$unit_dir/$unit"
systemctl --user daemon-reload

# Updater (vendor/frame-updater/frame-update.sh into ~/.local/share/<name>/. Without it, updating from the
# dashboard won't work, but everything else does)
update_script="$here/vendor/frame-updater/frame-update.sh"
if [[ -f "$update_script" ]]; then
    install -Dm755 "$update_script" "$data_home/$name/frame-update.sh"
else
    echo "Warning: $update_script not found. Update from the dashboard won't work until you install a release that includes it." >&2
fi

# Options for the next automatic update (frame-update.sh install): one per line, # starts a comment
mkdir -p "$config_dir" "$state_dir"
install_args="$config_dir/install-args"
: >"$install_args.new"
[[ "$autostart" == 0 ]] && echo "--no-autostart" >>"$install_args.new"
mv -f "$install_args.new" "$install_args"

offer_to_disable_old_overlay

steamvr_running=0
if systemctl --user is-active --quiet steamvr.service; then
    steamvr_running=1
fi

if [[ "$autostart" == 1 ]]; then
    systemctl --user enable "$unit"
    if [[ "$steamvr_running" == 1 ]]; then
        stop_manual_instance
        # restart also stops with SIGTERM, so the old one cleans up before the new one starts
        systemctl --user restart "$unit"
        sleep 2
        systemctl --user --no-pager status "$unit" | head -n 5 || true
    fi
else
    systemctl --user disable "$unit" 2>/dev/null || true
    # If it's running as the service, restart it on the new binary (it won't start with SteamVR any more)
    if systemctl --user is-active --quiet "$unit"; then
        systemctl --user restart "$unit"
    fi
fi

echo
echo "$name $("$bin" --version 2>/dev/null | awk '{print $2}') is installed."
if [[ "$autostart" == 1 ]]; then
    if [[ "$steamvr_running" == 1 ]]; then
        echo "  It is running now and starts together with SteamVR from now on."
    else
        echo "  It starts together with SteamVR from the next SteamVR start."
    fi
else
    echo "  Autostart is off. Start it from the + button in the SteamVR dashboard (Frame Perf HUD)."
fi
cat <<EOF
  Settings: SteamVR dashboard > Frame Perf HUD, or $config_dir/config.json
  Updates:  the dashboard tab shows the version and checks GitHub for a newer one (at start and at most once a day; "update_check": false in config.json turns that off, "Check now" still works)
  Logs:     journalctl --user -u $name -f
  Debug:    $bin --debug-dump   (sources, read counters, every metric)
  Remove:   ./install.sh --uninstall
EOF
