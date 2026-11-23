#!/bin/bash
# one tuner job per trace: results/knob-carbs-spec-v2/<trace>.json   (v2 knob space)
# usage: launch.sh <trace names...> ; extra args via EXTRA="--exhaustive --jobs 4"
cd "$(dirname "$0")/.."
PY=$HOME/pin-dl/venv/bin/python
export PYTHONPATH=$PWD
for t in "$@"; do
  nohup $PY knob_carbs.py --kernel $t --trace $HOME/spec-traces/$t.champsimtrace.xz \
    --out results/knob-carbs-spec-v2/$t.json --budget 20 --seeds 2 --warmup 1000000 --sim 3000000 $EXTRA \
    > results/knob-carbs-spec-v2/$t.log 2>&1 &
done
