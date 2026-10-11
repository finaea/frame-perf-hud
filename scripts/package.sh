#!/usr/bin/env bash
# Builds the release tarball. Run it on the Steam Frame (it links the headset's cairo, libnl and SteamVR's OpenVR).
#   scripts/package.sh   -> dist/frame-perf-hud-<version>.tar.gz, dist/SHA256SUMS
# Contents: the binary, install.sh, contrib (unit, .desktop, icons, config example), the bundled font,
#           LICENSE, vendor/frame-updater/frame-update.sh (used to update from the dashboard), etc.
set -euo pipefail
cd "$(dirname "$0")/.."

name="frame-perf-hud"
build_dir="build-release"

if [[ "$(uname -m)" != "aarch64" ]]; then
    echo "Run this on the Steam Frame (aarch64). This machine is $(uname -m)." >&2
    exit 1
fi

# Check vendor/frame-updater/ wasn't edited by hand (and against frame-updater itself if it sits next to this repo)
sh vendor/frame-updater/verify.sh

# Clean Release build in its own folder (tests run with ctest in the dev build/, not here)
cmake -G Ninja -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build "$build_dir" --clean-first

version="$("$build_dir/$name" --version | awk '{print $2}')"
if [[ -z "$version" || "$version" == "unknown" ]]; then
    echo "Could not read the version from $build_dir/$name --version" >&2
    exit 1
fi

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
root="$stage/$name"
mkdir -p "$root/contrib/icons" "$root/vendor/frame-updater" "$root/assets/fonts"
install -m755 "$build_dir/$name" "$root/$name"
strip "$root/$name"
install -m755 install.sh "$root/"
install -m644 LICENSE THIRD_PARTY_LICENSES.md README.md CHANGELOG.md "$root/"
install -m644 assets/fonts/NotoSans-Regular.ttf assets/fonts/NotoSans-Bold.ttf assets/fonts/OFL.txt "$root/assets/fonts/"
install -m644 "contrib/$name.service" "contrib/$name.desktop" contrib/config.example.json "$root/contrib/"
install -m644 contrib/icons/*.png "$root/contrib/icons/"
install -Dm644 licenses/LGPL-2.1.txt "$root/licenses/LGPL-2.1.txt"
# The update script install.sh puts in ~/.local/share/<name>/ (cpp, python, strings.md and verify.sh are
# either compiled in or only needed while developing, so they stay out of the release)
install -m755 vendor/frame-updater/frame-update.sh "$root/vendor/frame-updater/"

mkdir -p dist
out="dist/$name-$version.tar.gz"
# No owner names in the archive (everything as uid 0)
tar -C "$stage" --owner=0 --group=0 --numeric-owner --sort=name -czf "$out" "$name"
echo
tar -tzvf "$out"

(cd dist && sha256sum "$(basename "$out")" >SHA256SUMS)
echo
cat "dist/SHA256SUMS"

echo
echo "To publish (this script does not do it):"
echo "  gh release create v$version $out dist/SHA256SUMS --title v$version --generate-notes"
