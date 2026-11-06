#!/bin/bash
# End-to-end sanity run: N=4 flags, one kernel, tiny budgets. Uses separate work/lut/result dirs.
# usage: ./run_sanity.sh [kernel] [workers]
set -euo pipefail
cd "$(dirname "$0")"
K="${1:-k_stencil}"; J="${2:-$(nproc)}"
W=work_sanity; L=luts_sanity; R=results_sanity
python3 test_tuners.py
python3 build_and_hash.py --work $W -n 4 --kernels "$K" -j "$J"
python3 simulate.py --work $W --kernels "$K" -j "$J" --determinism 5
python3 build_lut.py --work $W --out $L --kernels "$K"
python3 evaluate.py --luts $L --out $R --kernels "$K" --seeds 50 --budgets 2 4 6 8 10
echo "sanity OK -> $R/summary.md"
