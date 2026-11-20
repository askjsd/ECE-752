#!/usr/bin/env bash
# ptrace-free ChampSim tracing: DynamoRIO drmemtrace -> to_champsim.py -> xz
#
# usage: trace_dr.sh -o OUT.champsim.xz [-s SKIP] [-t COUNT] [-S SYMBOL] [-W WINDOW] -- ./prog args...
#   -o OUT      output; written xz-compressed if it ends in .xz, otherwise raw 64-byte records
#   -s SKIP     instructions to skip (counted from process start, or from SYMBOL if -S is given)
#   -t COUNT    instructions to write (default 1000000)
#   -S SYMBOL   start counting at the first call of this function (needs a symbol table; non-PIE)
#   -P PRESKIP  with -S: let DynamoRIO skip this many instructions before recording
#               (must stay below the symbol's first call; saves conversion time)
#   -W WINDOW   with -S: how many instructions DynamoRIO records after PRESKIP
#               (must reach the symbol and then cover SKIP + COUNT; default 50000000)
# env: DR=path to the DynamoRIO tree, PY=python with capstone (default: python3)
set -euo pipefail
DR=${DR:?set DR to the extracted DynamoRIO directory}
PY=${PY:-python3}
HERE=$(cd "$(dirname "$0")" && pwd)
SKIP=0; COUNT=1000000; SYM=""; PRE=0; WINDOW=50000000; OUT=""
while getopts "o:s:t:S:P:W:" opt; do
  case $opt in o) OUT=$OPTARG;; s) SKIP=$OPTARG;; t) COUNT=$OPTARG;; S) SYM=$OPTARG;; P) PRE=$OPTARG;; W) WINDOW=$OPTARG;; *) exit 2;; esac
done
shift $((OPTIND - 1)); [[ ${1:-} == -- ]] && shift
[[ -n $OUT && $# -gt 0 ]] || { sed -n '4,14p' "$0"; exit 2; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/drtrace.XXXXXX"); trap 'rm -rf "$TMP"' EXIT
if [[ -n $SYM ]]; then
  PC=$(nm "$1" | awk -v s="$SYM" '$3==s {print "0x"$1; exit}')
  [[ -n $PC ]] || { echo "symbol $SYM not found in $1" >&2; exit 1; }
  DROPTS=(-trace_after_instrs "$PRE" -trace_for_instrs "$WINDOW"); CONV=(--start-pc "$PC" -s "$SKIP")
else
  # small slack: DynamoRIO's instruction counter and ours can differ slightly
  DROPTS=(-trace_after_instrs "$SKIP" -trace_for_instrs $((COUNT + COUNT / 10 + 1000))); CONV=(-s 0)
fi

# setarch -R disables ASLR for this run only (no privileges needed) -> reproducible addresses
setarch -R "$DR/bin64/drrun" -t drmemtrace -offline -outdir "$TMP" "${DROPTS[@]}" -- "$@" > "$TMP/app.out"
# the converter exits as soon as it has COUNT records, so drrun may die of SIGPIPE: only check the converter
set +o pipefail
"$DR/bin64/drrun" -t drmemtrace -indir "$TMP"/drmemtrace.*.dir -tool view 2>&1 >/dev/null \
  | "$PY" "$HERE/to_champsim.py" --from drview "${CONV[@]}" -t "$COUNT" -o "$TMP/trace"
[[ ${PIPESTATUS[1]} -eq 0 ]] || { echo "conversion failed" >&2; exit 1; }
set -o pipefail
if [[ $OUT == *.xz ]]; then xz -T0 -c "$TMP/trace" > "$OUT"; else cp "$TMP/trace" "$OUT"; fi
echo "wrote $OUT" >&2
