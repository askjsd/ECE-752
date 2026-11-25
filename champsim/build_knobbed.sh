#!/usr/bin/env bash
# Build ChampSim with the knobbed prefetcher (champsim/modules/prefetcher/knobbed).
#
#   usage: champsim/build_knobbed.sh [linux|windows]      (default: linux)
#
# The module is compiled against one of the two ChampSim source trees in this repo:
#   source-linux    champsim/source-linux    (the supported way to build on Linux)
#   source-windows  champsim/source-windows  (the tree the Windows runs use; here it
#                                             is only built on Linux to check that
#                                             the module compiles against it)
# ChampSim's Makefile breaks on paths containing spaces, so the build happens in a
# space-free copy, ~/champsim-knobbed-<linux|windows>. The source tree is copied
# there each run and is never modified.
#
# Neither tree contains the compiled vcpkg libraries. They are copied from
# CHAMPSIM_DEPS: by default the build that setup_linux.sh made (~/champsim-linux),
# or, failing that, the older champsim/.upstream build.
#
#   env:   CHAMPSIM_DEPS  tree with vcpkg_installed  (default: ~/champsim-linux)
#          KNOBBED_BUILD  build directory            (default: ~/champsim-knobbed-<which>)
#          JOBS           parallel make jobs
# Output: $KNOBBED_BUILD/bin/champsim_knobbed
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
which="${1:-linux}"
case "$which" in
    linux|windows) ;;
    *) echo "usage: $0 [linux|windows]" >&2; exit 2;;
esac
src="$here/source-$which"
deps="${CHAMPSIM_DEPS:-$HOME/champsim-linux}"
[[ -d "$deps/vcpkg_installed" ]] || deps="$here/.upstream"
build="${KNOBBED_BUILD:-$HOME/champsim-knobbed-$which}"
case "$build" in *" "*) echo "KNOBBED_BUILD must not contain spaces: $build" >&2; exit 1;; esac
[[ -d "$src/inc" ]] || { echo "$src is not a ChampSim source tree" >&2; exit 1; }
[[ -d "$src/vcpkg_installed" || -d "$deps/vcpkg_installed" ]] || { echo "no vcpkg_installed in $src or $deps; run champsim/setup_linux.sh first" >&2; exit 1; }

# Refresh the sources from the repo, but keep object files for speed.
mkdir -p "$build"
cp -a "$src/." "$build/"
[[ -d "$build/vcpkg_installed" ]] || cp -a "$deps/vcpkg_installed" "$build/vcpkg_installed"
# source-windows links zlib as -llibz (the vcpkg Windows name). Fix that in the
# build copy so it links against the Linux vcpkg libraries.
sed -i 's/-llibz/-lz/' "$build/Makefile"
rm -rf "$build/knobbed_modules" "$build/absolute.options" "$build/.csconfig" "$build/inc/cache_modules.h" "$build/inc/ooo_cpu_modules.h"
cp -r "$here/modules" "$build/knobbed_modules"
# Skip modules the source tree already ships under the same path (source-windows has
# prefetcher/tage as well as modules/prefetcher/tage); the linker would see them twice.
for m in "$build"/knobbed_modules/*/*/; do
    rel="${m#"$build"/knobbed_modules/}"
    [[ -d "$build/$rel" ]] && rm -rf "$m"
done

cd "$build"
python3 ./config.sh --module-dir "$build/knobbed_modules" "$here/champsim_config_knobbed.json"
make -j"${JOBS:-$(nproc)}"
echo "Built $build/bin/champsim_knobbed (source-$which)"
