#!/bin/bash
# Installs the camera helper (fph-camgrab) as a file holding only cap_sys_ptrace. Run it once with sudo:
# that's all the camera widgets need, and nothing else in frame-perf-hud ever needs sudo.
#
#   sudo ./install-camera-helper.sh                  # install the fph-camgrab next to this script (or build/'s)
#   sudo ./install-camera-helper.sh path/to/fph-camgrab
#   sudo ./install-camera-helper.sh --uninstall      # remove it
#
# It goes to /home/.frame-perf-hud/fph-camgrab: root-owned 0755, in a root-owned 0755 directory. /home is used
# because it's its own partition that survives SteamOS updates (/var is per A/B slot and can be lost).
# Replacing the file clears the capability, so a new helper version means running this again.
# Same approach as eyecam's install_grab.sh.
set -euo pipefail

DEST_DIR=/home/.frame-perf-hud
DEST="$DEST_DIR/fph-camgrab"

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this with sudo: sudo $0 $*" >&2
    exit 1
fi

# Drop the capability before replacing or removing the file, so a copy that survives under another name
# (a hard link) can't keep it.
drop_cap() {
    if [ -f "$1" ] && [ ! -L "$1" ]; then
        setcap -r "$1" 2>/dev/null || true
    fi
}

if [ "${1:-}" = "--uninstall" ]; then
    drop_cap "$DEST"
    rm -f "$DEST"
    rmdir "$DEST_DIR" 2>/dev/null || true
    echo "Removed: $DEST"
    exit 0
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -n "${1:-}" ]; then
    SRC="$1"
elif [ -f "$HERE/fph-camgrab" ]; then
    SRC="$HERE/fph-camgrab"
else
    SRC="$HERE/build/fph-camgrab"
fi
if [ ! -f "$SRC" ] || [ -L "$SRC" ]; then
    echo "fph-camgrab not found: $SRC" >&2
    exit 1
fi
if [ -L "$DEST_DIR" ] || { [ -e "$DEST_DIR" ] && [ ! -d "$DEST_DIR" ]; }; then
    echo "$DEST_DIR is not a plain directory, leaving it alone" >&2
    exit 1
fi

echo "Installing: $SRC"
sha256sum "$SRC"

# A root-owned 0755 copy in a directory nobody else can write to gets the capability, then is renamed into
# place, so a half-set-up file is never visible as $DEST.
install -d -o root -g root -m 0755 "$DEST_DIR"
chown root:root "$DEST_DIR"
chmod 0755 "$DEST_DIR"
tmp="$(mktemp "$DEST_DIR/.fph-camgrab.XXXXXX")"
trap 'rm -f "$tmp"' EXIT
install -o root -g root -m 0755 "$SRC" "$tmp"
setcap cap_sys_ptrace=ep "$tmp"
drop_cap "$DEST"
mv -f "$tmp" "$DEST"
trap - EXIT

caps="$(getcap "$DEST")"
echo "$caps"
case "$caps" in
    *cap_sys_ptrace=ep*) ;;
    *) echo "The capability is not set (setcap did not take effect)" >&2; exit 1 ;;
esac
ls -l "$DEST"
sha256sum "$DEST"
echo "Installed. The camera widgets pick it up by themselves: no restart needed."
