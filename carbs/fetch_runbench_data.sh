#!/bin/bash
# Restores the input data / non-source files of the benchmarks present in RunBench/
# (they are git-ignored here; only sources are tracked). Existing files are never overwritten.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
git clone -q --depth 1 https://github.com/lv2020/RunBench.git "$tmp/RunBench"
for d in "$here"/RunBench/*/; do
  b="$(basename "$d")"
  [ -d "$tmp/RunBench/$b" ] && rsync -a --ignore-existing "$tmp/RunBench/$b/" "$d" && echo "restored $b"
done
