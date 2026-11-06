#!/bin/bash
# Usage: ./build_libfunc.sh RunBench/<benchmark_dir> [...]
# Builds libfunc.so into each benchmark dir so CARBS can time the kernel.
here="$(cd "$(dirname "$0")" && pwd)"
for d in "$@"; do
  gcc -shared -fPIC -O2 -o "$d/libfunc.so" "$here/libfunc.c" && echo "built $d/libfunc.so"
done
