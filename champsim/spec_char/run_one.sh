#!/bin/bash
# usage: run_one.sh <trace-name-without-suffix>   (no-prefetch ChampSim, 10M warmup / 50M sim)
T=$HOME/spec-traces/$1.champsimtrace.xz
OUT="$(dirname "$0")/raw/$1.json.out"
s=$(date +%s)
$HOME/champsim-nopf-linux/bin/champsim_nopf --json --hide-heartbeat -w 10000000 -i 50000000 "$T" > "$OUT" 2> "$OUT.err"
echo "$1 rc=$? $(( $(date +%s) - s ))s" >> "$(dirname "$0")/raw/timing.txt"
