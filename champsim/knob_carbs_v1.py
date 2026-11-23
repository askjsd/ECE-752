#!/usr/bin/env python3
"""ARCHIVED (v1 knob space, 16 bits). It only works with the v1 prefetcher module, whose knobs
(distance, degree, drop, fill thresholds, maxinflight, pagecross) were removed; it is kept so the v1
results in champsim/results/knob-carbs*/ can be understood. Use knob_carbs.py (v2) instead.

Run the existing flagbench tuners (CARBS, random search, hill climbing) over the
knobbed prefetcher's knobs instead of compiler flags.

The tuners in flagbench/tuners.py search a space of N on/off "flags" and only need an
object with .n, .o3, .evaluate(config_id), .bits(config_id) and .to_id(bits). Here a
config id is a 16-bit pattern that is decoded into a KNOBBED_HINT string, and
evaluate() runs the champsim_knobbed simulator on one trace and returns cycles
(lower is better).

Bit layout (least significant bit first). Multi-bit fields are plain binary numbers, so
the tuners see them as independent on/off bits; that is a limitation, not a feature.

  bit  0     source    0 next-line, 1 per-PC stride
  bits 1-2   trigger   0 miss, 1 tagged, 2 access, 3 tagged
  bits 3-6   distance  1 + value (1..16)
  bits 7-9   degree    1 + value (1..8)
  bits 10-11 fill      threshold 0.0, 0.34, 0.67, 1.0
  bit  12    drop      threshold 0.0 or 0.25
  bit  13    throttle  0 off, 1 on
  bits 14-15 confirm   1 + value (1..4)

Usage (one trace; run several in parallel for several kernels):
  knob_carbs.py --kernel k_mix --trace k_mix.trace --out results/knob-carbs/k_mix.json
  knob_carbs.py --summarize results/knob-carbs        # table over every *.json there
"""
import argparse
import json
import math
import os
import re
import statistics
import subprocess
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))  # repo root, for flagbench.tuners
from flagbench import tuners  # noqa: E402

N_BITS = 16
FILL = [0.0, 0.34, 0.67, 1.0]
TRIGGER = ["miss", "tagged", "access", "tagged"]


def decode(cid):
    """config id -> KNOBBED_HINT string."""
    f = lambda lo, n: (cid >> lo) & ((1 << n) - 1)  # noqa: E731
    return ",".join(
        [
            "source=" + ("stride" if f(0, 1) else "nextline"),
            "trigger=" + TRIGGER[f(1, 2)],
            f"distance={1 + f(3, 4)}",
            f"degree={1 + f(7, 3)}",
            f"fill={FILL[f(10, 2)]}",
            f"drop={0.25 if f(12, 1) else 0.0}",
            f"throttle={f(13, 1)}",
            f"confirm={1 + f(14, 2)}",
        ]
    )


def encode(source, trigger, distance, degree, fill, drop, throttle, confirm):
    """Inverse of decode() for the values it can express."""
    return (
        (1 if source == "stride" else 0)
        | (TRIGGER.index(trigger) << 1)
        | ((distance - 1) << 3)
        | ((degree - 1) << 7)
        | (FILL.index(fill) << 10)
        | ((1 if drop else 0) << 12)
        | ((1 if throttle else 0) << 13)
        | ((confirm - 1) << 14)
    )


# The simulator's built-in default hint (what KNOBBED_HINT="" gives), as a config id.
DEFAULT_ID = encode("stride", "tagged", 1, 2, 0.67, 0.0, 1, 2)  # fill 0.5 rounds near 0.67


class Simulator:
    """Runs champsim_knobbed on one trace with a given hint; caches results on disk."""

    def __init__(self, binary, trace, warmup, sim, cache_path):
        self.binary, self.trace, self.warmup, self.sim = binary, trace, warmup, sim
        self.cache_path = Path(cache_path)
        self.cache = {}
        if self.cache_path.exists():
            self.cache = json.loads(self.cache_path.read_text()).get("cache", {})
        self.runs = 0

    def run(self, hint, key):
        """Return cycles for `hint` (a KNOBBED_HINT string) or None on failure."""
        if key in self.cache:
            return self.cache[key]
        env = dict(os.environ, KNOBBED_HINT=hint)
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
        self.o3 = DEFAULT_ID  # only used if a tuner bootstraps from it; we pass bootstrap=False
        self.sim = sim
        self.evals = 0

    def evaluate(self, cid):
        self.evals += 1
        return self.sim.run(decode(int(cid)), str(int(cid)))

    def bits(self, cid):
        return np.array([(cid >> j) & 1 for j in range(self.n)], dtype=np.int8)

    @staticmethod
    def to_id(bits):
        return int(sum(int(b) << j for j, b in enumerate(bits)))


def run_kernel(args):
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    sim = Simulator(args.binary, args.trace, args.warmup, args.sim, out)
    ref = {
        "prefetch_off": sim.run("enable=0", "off"),
        "default_hint": sim.run("", "default"),
    }
    results = {"carbs": [], "random": [], "hillclimb": []}
    for name, fn, kw in (("carbs", tuners.carbs, {"bootstrap": False}),
                         ("random", tuners.random_search, {}),
                         ("hillclimb", tuners.hill_climb, {})):
        for seed in range(args.seeds):
            lut = KnobLUT(sim)
            results[name].append(fn(lut, args.budget, seed, **kw))
            out.write_text(json.dumps({"kernel": args.kernel, "reference": ref, "traces": results,
                                       "cache": sim.cache, "budget": args.budget, "seeds": args.seeds,
                                       "n_bits": N_BITS}))
    # also record which config was best, for reading the result
    valid = {k: v for k, v in sim.cache.items() if k.isdigit() and v is not None}
    best_id = int(min(valid, key=valid.get)) if valid else None
    data = json.loads(out.read_text())
    data["best"] = {"config_id": best_id, "cycles": valid.get(str(best_id)) if valid else None,
                    "hint": decode(best_id) if valid else None}
    out.write_text(json.dumps(data))
    print(f"{args.kernel}: {sim.runs} simulations, best {data['best']}")


def summarize(directory):
    files = sorted(Path(directory).glob("*.json"))
    hdr = f"{'kernel':20s} {'off':>9s} {'default':>9s} {'best':>9s} | speedup over off:  {'CARBS':>7s} {'random':>7s} {'hillcl':>7s} | {'best vs default':>15s}  best hint"
    print(hdr)
    rows = []
    for f in files:
        d = json.loads(f.read_text())
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
    ap.add_argument("--budget", type=int, default=40, help="simulations per tuner run")
    ap.add_argument("--seeds", type=int, default=2)
    ap.add_argument("--warmup", type=int, default=100_000)
    ap.add_argument("--sim", type=int, default=2_000_000, help="simulated instructions (must fit in the trace after warmup)")
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
