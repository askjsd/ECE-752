#!/usr/bin/env bash
# Build ChampSim for Linux from champsim/source-linux. No root needed.
#
#   usage: champsim/setup_linux.sh
#   env:   CHAMPSIM_BUILD  build directory  (default: ~/champsim-linux)
#          JOBS            parallel make jobs
# Output: $CHAMPSIM_BUILD/bin/champsim   (pass $CHAMPSIM_BUILD as --champsim-root)
#
# What it does:
#   1. copies source-linux into a build directory. ChampSim's Makefile breaks on
#      paths containing spaces, so the build never happens inside the repo; the
#      source tree itself is left untouched.
#   2. fetches the pinned vcpkg and installs ChampSim's dependencies (CLI11, fmt,
#      zlib, ...) into the build directory.
#   3. configures with champsim/champsim_config.json and builds.
# Rerun it after editing champsim_config.json or the sources. Dependencies are
# only installed the first time.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/source-linux"
build="${CHAMPSIM_BUILD:-$HOME/champsim-linux}"
vcpkg_rev=6d7bf7ef2193e2d1c5798a5ff8811d533104c861   # the revision ChampSim pins

case "$build" in *" "*) echo "CHAMPSIM_BUILD must not contain spaces: $build" >&2; exit 1;; esac
[[ -d "$src/inc" ]] || { echo "$src is not a ChampSim source tree" >&2; exit 1; }

mkdir -p "$build"
cp -a "$src/." "$build/"

if [[ ! -x "$build/vcpkg/vcpkg" ]]; then
    if [[ ! -d "$build/vcpkg/.git" ]]; then
        git init -q "$build/vcpkg"
        git -C "$build/vcpkg" remote add origin https://github.com/microsoft/vcpkg.git
    fi
    git -C "$build/vcpkg" fetch -q --depth 1 origin "$vcpkg_rev"
    git -C "$build/vcpkg" checkout -q FETCH_HEAD
    "$build/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
fi
if [[ ! -d "$build/vcpkg_installed" ]]; then
    (cd "$build" && ./vcpkg/vcpkg install)
fi

# Regenerate the configuration from scratch so config changes take effect.
rm -rf "$build/absolute.options" "$build/.csconfig" "$build/inc/cache_modules.h" "$build/inc/ooo_cpu_modules.h"
cd "$build"
python3 ./config.sh "$here/champsim_config.json"
make -j"${JOBS:-$(nproc)}"
echo "ChampSim built at $build/bin/champsim"
