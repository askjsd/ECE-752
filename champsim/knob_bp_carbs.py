#!/usr/bin/env python3
"""Run the flagbench tuners (CARBS, random search, hill climbing) over the knobbed
BRANCH PREDICTOR's knobs (champsim/modules/branch/knobbed_bp). Sibling of knob_carbs.py,
which does the same for the prefetcher.

A config id is an 11-bit pattern decoded into a KNOBBED_BP_HINT string; evaluate()
runs champsim_knobbed_bp on one trace and returns cycles (lower is better).

Bit layout (least significant bit first). Every field uses all its codes. Ordered or
related fields use Gray code so neighbouring values differ by one bit.

  bits 0-1   engine    0 bimodal, 1 gshare, 2 perceptron, 3 tage      (Gray coded)
  bits 2-3   history   0 global, 1 path, 2 local, 3 imli              (Gray coded)
  bits 4-6   level     history length ladder 0-7                      (Gray coded)
  bit  7     counter   0 -> 2-bit counters, 1 -> 1-bit counters
  bit  8     loop      loop-count predictor overrides
  bit  9     hybrid    tournament selector against a plain bimodal
  bit  10    fold      0 truncate long history, 1 XOR-fold it

canonical(cid) zeroes fields that cannot change the simulation (bimodal ignores
history, level, fold and hybrid; perceptron ignores counter and fold; gshare never
needs folding at level <= 5; TAGE with local history clamps at 64 bits), so
equivalent configs share one simulation.

Usage (one trace):
  knob_bp_carbs.py --kernel gcc --trace t.xz --out results/knob-bp/gcc.json
  knob_bp_carbs.py ... --exhaustive --jobs 8       # every distinct config (a few hundred)
  knob_bp_carbs.py --summarize results/knob-bp
"""
import argparse
import json
import math
import os
import re
import statistics
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))  # repo root, for flagbench.tuners
from flagbench import tuners  # noqa: E402

N_BITS = 11
ENGINE = ["bimodal", "gshare", "perceptron", "tage"]
HISTORY = ["global", "path", "local", "imli"]


def gray(n):
    return n ^ (n >> 1)


def ungray(g):
    n = 0
    while g:
        n ^= g
        g >>= 1
    return n


def fields(cid):
    f = lambda lo, n: (cid >> lo) & ((1 << n) - 1)  # noqa: E731
    return dict(engine=ungray(f(0, 2)), history=ungray(f(2, 2)), level=ungray(f(4, 3)),
                counter=f(7, 1), loop=f(8, 1), hybrid=f(9, 1), fold=f(10, 1))


def pack(d):
    return (gray(d["engine"]) | (gray(d["history"]) << 2) | (gray(d["level"]) << 4)
            | (d["counter"] << 7) | (d["loop"] << 8) | (d["hybrid"] << 9) | (d["fold"] << 10))


def canonical(cid):
    d = fields(int(cid))
    if d["engine"] == 0:  # bimodal: no history; hybrid of bimodal with bimodal is itself
        d.update(history=0, level=0, fold=0, hybrid=0)
    elif d["engine"] == 2:  # perceptron: no counters, no folding
        d.update(counter=0, fold=0)
    elif d["engine"] == 1 and d["level"] <= 5:  # gshare history <= 12 bits fits the index
        d["fold"] = 1
    elif d["engine"] == 3 and d["history"] == 2 and d["level"] == 7:  # local history is 64 bits max
        d["level"] = 6
    return pack(d)


def decode(cid):
    d = fields(cid)
    return ",".join([
        "engine=" + ENGINE[d["engine"]],
        "history=" + HISTORY[d["history"]],
        f"level={d['level']}",
        f"counter={2 - d['counter']}",
        f"loop={d['loop']}",
        f"hybrid={d['hybrid']}",
        f"fold={d['fold']}",
    ])


def all_canonical():
    return sorted({canonical(i) for i in range(1 << N_BITS)})


# The module's built-in default hint (what KNOBBED_BP_HINT="" gives), as a config id.
DEFAULT_ID = pack(dict(engine=1, history=0, level=4, counter=0, loop=0, hybrid=0, fold=1))
BASELINE_HINT = "engine=bimodal"  # the simplest predictor, reference for speedup


class Simulator:
    """Runs champsim_knobbed_bp on one trace with a given hint; caches results on disk."""

    def __init__(self, binary, trace, warmup, sim, cache_path):
        self.binary, self.trace, self.warmup, self.sim = binary, trace, warmup, sim
        self.cache_path = Path(cache_path)
        self.cache = {}
        if self.cache_path.exists():
            self.cache = json.loads(self.cache_path.read_text()).get("cache", {})
        self.runs = 0

    def run(self, hint, key):
        if key in self.cache:
            return self.cache[key]
        env = dict(os.environ, KNOBBED_BP_HINT=hint)
        out = subprocess.run(
            [self.binary, "--warmup-instructions", str(self.warmup),
             "--simulation-instructions", str(self.sim), self.trace],
            env=env, capture_output=True, text=True,
        ).stdout
        m = re.search(r"^CPU 0 cumulative IPC: [0-9.]+ instructions: \d+ cycles: (\d+)", out, re.M)
        cycles = float(m.group(1)) if m else None
        self.runs += 1
        self.cache[key] = cycles
        return cycles


class KnobLUT:
    """The interface flagbench.tuners expects, backed by live simulation."""

    def __init__(self, sim):
        self.n = N_BITS
        self.o3 = DEFAULT_ID
        self.sim = sim
        self.evals = 0

    def evaluate(self, cid):
        self.evals += 1
        cid = canonical(int(cid))
        return self.sim.run(decode(cid), str(cid))

    def bits(self, cid):
        return np.array([(cid >> j) & 1 for j in range(self.n)], dtype=np.int8)

    @staticmethod
    def to_id(bits):
        return int(sum(int(b) << j for j, b in enumerate(bits)))


def run_kernel(args):
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    sim = Simulator(args.binary, args.trace, args.warmup, args.sim, out)
    ref = {"baseline_bimodal": sim.run(BASELINE_HINT, "baseline"), "default_hint": sim.run("", "default")}
    results = {"carbs": [], "random": [], "hillclimb": []}

    def save():
        out.write_text(json.dumps({"kernel": args.kernel, "reference": ref, "traces": results,
                                   "cache": sim.cache, "budget": args.budget, "seeds": args.seeds,
                                   "n_bits": N_BITS, "encoding": "bp-v1"}))

    if args.exhaustive:
        todo = [c for c in all_canonical() if str(c) not in sim.cache]
        print(f"{args.kernel}: exhaustive sweep, {len(todo)} configs left of {len(all_canonical())}", flush=True)

        def one(cid):
            sim.run(decode(cid), str(cid))
        with ThreadPoolExecutor(args.jobs) as ex:
            for i, _ in enumerate(ex.map(one, todo), 1):
                if i % 50 == 0:
                    save()
    for name, fn, kw in (("carbs", tuners.carbs, {"bootstrap": False}),
                         ("random", tuners.random_search, {}),
                         ("hillclimb", tuners.hill_climb, {})):
        for seed in range(args.seeds):
            results[name].append(fn(KnobLUT(sim), args.budget, seed, **kw))
            save()
    valid = {k: v for k, v in sim.cache.items() if k.isdigit() and v is not None}
    best_id = int(min(valid, key=valid.get)) if valid else None
    save()
    data = json.loads(out.read_text())
    data["best"] = {"config_id": best_id, "cycles": valid.get(str(best_id)) if valid else None,
                    "hint": decode(best_id) if valid else None}
    data["exhaustive"] = bool(args.exhaustive)
    out.write_text(json.dumps(data))
    print(f"{args.kernel}: {sim.runs} simulations, best {data['best']}")


def summarize(directory):
    print(f"{'kernel':20s} {'bimodal':>10s} {'default':>10s} {'best':>10s} | speedup over bimodal: {'CARBS':>7s} {'random':>7s} {'hillcl':>7s} | {'best/default':>12s}  best hint")
    for f in sorted(Path(directory).glob("*.json")):
        d = json.loads(f.read_text())
        base, dflt = d["reference"]["baseline_bimodal"], d["reference"]["default_hint"]
        if base is None or "best" not in d:
            continue
        sp = {}
        for name, traces in d["traces"].items():
            finals = [t[-1] for t in traces if t and math.isfinite(t[-1])]
            sp[name] = base / statistics.median(finals) if finals else float("nan")
        best = d["best"]["cycles"]
        print(f"{d['kernel']:20s} {base:10.0f} {dflt:10.0f} {best:10.0f} |                       "
              f"{sp['carbs']:7.3f} {sp['random']:7.3f} {sp['hillclimb']:7.3f} | {dflt / best:11.3f}x  {d['best']['hint']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--kernel")
    ap.add_argument("--trace")
    ap.add_argument("--out")
    ap.add_argument("--binary", default=os.path.expanduser("~/champsim-knobbed-bp/bin/champsim_knobbed_bp"))
    ap.add_argument("--budget", type=int, default=40)
    ap.add_argument("--seeds", type=int, default=2)
    ap.add_argument("--warmup", type=int, default=100_000)
    ap.add_argument("--sim", type=int, default=2_000_000)
    ap.add_argument("--exhaustive", action="store_true")
    ap.add_argument("--jobs", type=int, default=4)
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
