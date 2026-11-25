#!/usr/bin/env bash
# Build ChampSim with the hint-free, TAGE-inspired prefetch-setting learner.
#
# Output: $TAGE_BUILD/bin/champsim_tage (default: ~/champsim-tage-linux)
# CHAMPSIM_DEPS may name a tree containing vcpkg_installed; by default the
# standard Linux setup at ~/champsim-linux is used.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/source-linux"
deps="${CHAMPSIM_DEPS:-$HOME/champsim-linux}"
build="${TAGE_BUILD:-$HOME/champsim-tage-linux}"

case "$build" in *" "*) echo "TAGE_BUILD must not contain spaces: $build" >&2; exit 1;; esac
[[ -d "$src/inc" ]] || { echo "$src is not a ChampSim source tree" >&2; exit 1; }
[[ -d "$src/vcpkg_installed" || -d "$deps/vcpkg_installed" ]] || {
    echo "No vcpkg_installed tree found; run champsim/setup_linux.sh first" >&2
    exit 1
}

mkdir -p "$build"
cp -a "$src/." "$build/"
[[ -d "$build/vcpkg_installed" ]] || cp -a "$deps/vcpkg_installed" "$build/vcpkg_installed"
rm -rf "$build/tage_modules" "$build/absolute.options" "$build/.csconfig" \
    "$build/inc/cache_modules.h" "$build/inc/ooo_cpu_modules.h"
cp -r "$here/modules" "$build/tage_modules"

cd "$build"
python3 ./config.sh --module-dir "$build/tage_modules" "$here/champsim_config_tage.json"
make -j"${JOBS:-$(nproc)}"
echo "Built $build/bin/champsim_tage"
