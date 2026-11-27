#!/usr/bin/env python3
"""ARCHIVED (v2 knob space, 12 bits). Kept so the v2 results on disk (for example
champsim/results/knob-carbs-spec-v2/, "encoding": "v2") can be decoded. Use knob_carbs.py
(v3) for new runs: v2 packs `source` into 2 bits, so Markov (both bits set) is invisible
to CARBS, which learns each bit separately.

Run the existing flagbench tuners (CARBS, random search, hill climbing) over the
knobbed prefetcher's knobs instead of compiler flags.

The tuners in flagbench/tuners.py search a space of N on/off "flags" and only need an
object with .n, .o3, .evaluate(config_id), .bits(config_id) and .to_id(bits). Here a
config id is a 12-bit pattern that is decoded into a KNOBBED_HINT string plus the
KNOBBED_INSERT setting, and evaluate() runs champsim_knobbed on one trace and returns
cycles (lower is better).

Bit layout (least significant bit first). Every field uses ALL of its codes, so no
setting is an alias of another. Ordered fields use Gray code so that neighbouring
values differ by one bit.

  bits 0-1   source    0 off, 1 nextline, 2 stride, 3 markov
  bit  2     trigger   0 tagged, 1 access
  bits 3-5   level     aggressiveness ladder 0-7 (Gray coded); see knobbed.cc for rungs
  bits 6-7   fill      0 always L1D, 1 gated low, 2 gated high, 3 always L2 (Gray coded)
  bit  8     throttle  0 off, 1 on (can only step the level down, never above `level`)
  bit  9     confirm   0 -> 1, 1 -> 2   (stride and markov only)
  bit  10    fallback  0 off, 1 next-line fallback when the engine has nothing to issue
  bit  11    insert    0 prefetch fills MRU, 1 prefetch fills LRU (lru_pf replacement)

canonical(cid) zeroes fields that cannot affect the result (everything when the
source is off; confirm and fallback for next-line), so equivalent configs share one
simulation and the distinct-config count is exact.

Parallelism: --jobs simulations run at once. The exhaustive sweep uses all of them,
and the tuner runs (3 tuners x --seeds) run side by side, each one still sequential
inside. Results do not depend on --jobs: every tuner has its own seeded RNG and
every simulation is deterministic. Two runs that ask for the same config at the same
time share one simulation.

Trace length: if --warmup + --sim is longer than the trace, both are scaled down
to fit (same ratio), because ChampSim would otherwise replay the trace from its
start. Uncompressed traces are sized from the file (64 bytes per instruction);
.xz traces are decompressed once to count.

Usage (one trace; run_knob_tuning_windows.py runs several traces at once):
  knob_carbs.py --kernel k_mix --trace k_mix.trace --out results/knob-carbs/k_mix.json
  knob_carbs.py --kernel k_mix --trace k_mix.trace --out ... --exhaustive --jobs 8
  knob_carbs.py --summarize results/knob-carbs        # table over every *.json there
"""
import argparse
import json
import math
import os
import re
import statistics
import lzma
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))  # repo root, for flagbench.tuners
from flagbench import tuners  # noqa: E402

N_BITS = 12
SOURCE = ["off", "nextline", "stride", "markov"]
TRIGGER = ["tagged", "access"]
FILL = ["l1d", "low", "high", "l2"]


def gray(n):
    return n ^ (n >> 1)


def ungray(g):
    n = 0
    while g:
        n ^= g
        g >>= 1
    return n


def fields(cid):
    """config id -> dict of decoded field values."""
    f = lambda lo, n: (cid >> lo) & ((1 << n) - 1)  # noqa: E731
    return dict(source=f(0, 2), trigger=f(2, 1), level=ungray(f(3, 3)), fill=ungray(f(6, 2)),
                throttle=f(8, 1), confirm=f(9, 1), fallback=f(10, 1), insert=f(11, 1))


def pack(d):
    return (d["source"] | (d["trigger"] << 2) | (gray(d["level"]) << 3) | (gray(d["fill"]) << 6)
            | (d["throttle"] << 8) | (d["confirm"] << 9) | (d["fallback"] << 10) | (d["insert"] << 11))


def canonical(cid):
    """Zero every field that cannot change the simulation, so equal configs share an id."""
    d = fields(int(cid))
    if d["source"] == 0:  # prefetching off: nothing else matters
        return 0
    if d["source"] == 1:  # next-line has no stride/link to confirm and never falls back
        d["confirm"] = 0
        d["fallback"] = 0
    return pack(d)


def decode(cid):
    """config id -> (KNOBBED_HINT string, KNOBBED_INSERT value)."""
    d = fields(cid)
    hint = ",".join([
        "source=" + SOURCE[d["source"]],
        "trigger=" + TRIGGER[d["trigger"]],
        f"level={d['level']}",
        "fill=" + FILL[d["fill"]],
        f"throttle={d['throttle']}",
        f"confirm={1 + d['confirm']}",
        f"fallback={d['fallback']}",
    ])
    return hint, ("lru" if d["insert"] else "mru")


def all_canonical():
    """Every distinct config in the space (the exhaustive sweep set)."""
    return sorted({canonical(i) for i in range(1 << N_BITS)})


# The module's built-in default hint (what KNOBBED_HINT="" gives), as a config id.
DEFAULT_ID = pack(dict(source=2, trigger=0, level=1, fill=1, throttle=1, confirm=1, fallback=0, insert=0))


RECORD_BYTES = 64  # one ChampSim input_instr


def trace_records(path):
    """Number of instructions in a ChampSim trace (.xz is decompressed once to count)."""
    path = str(path)
    if path.endswith(".xz"):
        n = 0
        with lzma.open(path, "rb") as f:
            while True:
                chunk = f.read(1 << 24)
                if not chunk:
                    break
                n += len(chunk)
        return n // RECORD_BYTES
    return os.path.getsize(path) // RECORD_BYTES


def fit_to_trace(warmup, sim, records):
    """Scale warmup and sim down (same ratio) so that they fit in the trace."""
    if records <= 0 or warmup + sim <= records:
        return warmup, sim
    w = int(records * warmup / (warmup + sim))
    return w, max(1, records - w)


class Simulator:
    """Runs champsim_knobbed on one trace with a given hint; caches results on disk.

    Safe to call from several threads: results are cached under a lock, and a config
    that is already being simulated is waited for rather than simulated twice."""

    def __init__(self, binary, trace, warmup, sim, cache_path):
        self.binary, self.trace, self.warmup, self.sim = binary, trace, warmup, sim
        self.cache_path = Path(cache_path)
        self.cache = {}
        if self.cache_path.exists():
            self.cache = json.loads(self.cache_path.read_text()).get("cache", {})
        self.runs = 0
        self.lock = threading.Lock()
        self.inflight = {}  # key -> Event set when that simulation finishes

    def run(self, hint, key, insert="mru"):
        """Return cycles for `hint` (a KNOBBED_HINT string) or None on failure."""
        with self.lock:
            if key in self.cache:
                return self.cache[key]
            ev = self.inflight.get(key)
            owner = ev is None
            if owner:
                ev = self.inflight[key] = threading.Event()
        if not owner:
            ev.wait()
            with self.lock:
                return self.cache.get(key)
        try:
            env = dict(os.environ, KNOBBED_HINT=hint, KNOBBED_INSERT=insert)
            out = subprocess.run(
                [self.binary, "--warmup-instructions", str(self.warmup),
                 "--simulation-instructions", str(self.sim), self.trace],
                env=env, capture_output=True, text=True,
            ).stdout
            m = re.search(r"^CPU 0 cumulative IPC: [0-9.]+ instructions: \d+ cycles: (\d+)", out, re.M)
            cycles = float(m.group(1)) if m else None
            with self.lock:
                self.runs += 1
                self.cache[key] = cycles
            return cycles
        finally:
            with self.lock:
                del self.inflight[key]
            ev.set()


class KnobLUT:
    """The interface flagbench.tuners expects, backed by live simulation."""

    def __init__(self, sim):
        self.n = N_BITS
        self.o3 = DEFAULT_ID  # only used if a tuner bootstraps from it; we pass bootstrap=False
        self.sim = sim
        self.evals = 0

    def evaluate(self, cid):
        self.evals += 1
        cid = canonical(int(cid))  # equivalent configs share one simulation
        hint, insert = decode(cid)
        return self.sim.run(hint, str(cid), insert)

    def bits(self, cid):
        return np.array([(cid >> j) & 1 for j in range(self.n)], dtype=np.int8)

    @staticmethod
    def to_id(bits):
        return int(sum(int(b) << j for j, b in enumerate(bits)))


def run_kernel(args):
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    warmup, simn = args.warmup, args.sim
    records = trace_records(args.trace)
    if warmup + simn > records:
        warmup, simn = fit_to_trace(warmup, simn, records)
        print(f"{args.kernel}: trace has {records} instructions; using warmup {warmup} + sim {simn} "
              f"instead of {args.warmup} + {args.sim} so ChampSim does not replay it", flush=True)
    sim = Simulator(args.binary, args.trace, warmup, simn, out)
    with ThreadPoolExecutor(2) as ex:
        f_off = ex.submit(sim.run, "source=off", "off")
        f_def = ex.submit(sim.run, "", "default")
        ref = {"prefetch_off": f_off.result(), "default_hint": f_def.result()}
    tuner_fns = (("carbs", tuners.carbs, {"bootstrap": False}),
                 ("random", tuners.random_search, {}),
                 ("hillclimb", tuners.hill_climb, {}))
    # one slot per (tuner, seed), filled as runs finish; seed order is kept
    results = {name: [None] * args.seeds for name, _, _ in tuner_fns}
    save_lock = threading.Lock()

    def save():
        with save_lock, sim.lock:
            data = json.dumps({"kernel": args.kernel, "reference": ref,
                               "traces": {k: [t for t in v if t is not None] for k, v in results.items()},
                               "cache": dict(sim.cache), "budget": args.budget, "seeds": args.seeds,
                               "warmup": warmup, "sim": simn, "trace_records": records,
                               "n_bits": N_BITS, "encoding": "v2"})
        with save_lock:
            out.write_text(data)

    if args.exhaustive:  # ground truth: simulate every distinct config once
        todo = [c for c in all_canonical() if str(c) not in sim.cache]
        print(f"{args.kernel}: exhaustive sweep, {len(todo)} configs left of {len(all_canonical())}", flush=True)
        def one(cid):
            hint, insert = decode(cid)
            sim.run(hint, str(cid), insert)
        with ThreadPoolExecutor(args.jobs) as ex:
            for i, _ in enumerate(ex.map(one, todo), 1):
                if i % 50 == 0:
                    save()

    def tune(name, fn, kw, seed):
        results[name][seed] = fn(KnobLUT(sim), args.budget, seed, **kw)
        save()

    # Each tuner run is sequential (every step depends on the last), but the 3 x seeds
    # runs are independent, so they run side by side, up to --jobs at once.
    with ThreadPoolExecutor(max(1, args.jobs)) as ex:
        futs = [ex.submit(tune, name, fn, kw, seed)
                for name, fn, kw in tuner_fns for seed in range(args.seeds)]
        for f in futs:
            f.result()  # re-raise any error
    # also record which config was best, for reading the result
    valid = {k: v for k, v in sim.cache.items() if k.isdigit() and v is not None}
    best_id = int(min(valid, key=valid.get)) if valid else None
    data = json.loads(out.read_text())
    best = {"config_id": best_id, "cycles": valid.get(str(best_id)) if valid else None}
    if valid:
        best["hint"], best["insert"] = decode(best_id)
        best["hint"] += ",insert=" + best["insert"]
    data["best"] = best
    data["exhaustive"] = bool(args.exhaustive)
    out.write_text(json.dumps(data))
    print(f"{args.kernel}: {sim.runs} simulations, best {data['best']}")


def summarize(directory):
    files = sorted(Path(directory).glob("*.json"))
    hdr = f"{'kernel':20s} {'off':>9s} {'default':>9s} {'best':>9s} | speedup over off:  {'CARBS':>7s} {'random':>7s} {'hillcl':>7s} | {'best vs default':>15s}  best hint"
    print(hdr)
    rows = []
    for f in files:
        d = json.loads(f.read_text())
        if not isinstance(d, dict) or "reference" not in d:
            continue  # not a tuning result (e.g. a timing file in the same folder)
        off, dflt = d["reference"]["prefetch_off"], d["reference"]["default_hint"]
        if off is None or "best" not in d:
            continue
        sp = {}
        for name, traces in d["traces"].items():
            finals = [t[-1] for t in traces if t and math.isfinite(t[-1])]
            sp[name] = off / statistics.median(finals) if finals else float("nan")
        best = d["best"]["cycles"]
        rows.append((d["kernel"], off, dflt, best, sp, d["best"]["hint"]))
        print(f"{d['kernel']:20s} {off:9.0f} {dflt:9.0f} {best:9.0f} |                    "
              f"{sp['carbs']:7.3f} {sp['random']:7.3f} {sp['hillclimb']:7.3f} | {dflt / best:14.3f}x  {d['best']['hint']}")
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--kernel")
    ap.add_argument("--trace")
    ap.add_argument("--out")
    ap.add_argument("--binary", default=os.path.expanduser("~/champsim-knobbed-linux/bin/champsim_knobbed"))
    ap.add_argument("--budget", type=int, default=40, help="evaluations per tuner run")
    ap.add_argument("--seeds", type=int, default=2)
    ap.add_argument("--warmup", type=int, default=100_000)
    ap.add_argument("--sim", type=int, default=2_000_000, help="simulated instructions (must fit in the trace after warmup)")
    ap.add_argument("--exhaustive", action="store_true", help="first simulate every distinct config (ground truth)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4,
                    help="simulations at once: the exhaustive sweep, and tuner runs side by side "
                         "(default: all CPUs)")
    ap.add_argument("--summarize", metavar="DIR")
    args = ap.parse_args()
    if args.summarize:
        summarize(args.summarize)
        return
    if not (args.kernel and args.trace and args.out):
        ap.error("--kernel, --trace and --out are required")
    run_kernel(args)


if __name__ == "__main__":
    main()
