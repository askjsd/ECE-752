#!/usr/bin/env python3
"""Compile and simulate only configurations requested by a tuner.

Each measurement uses run_carbs.py's selective mode, so the exhaustive legacy
workflow remains available while this runner spends resources on observed asks.
"""

import argparse
import csv
import json
import math
import os
import subprocess
import sys
from pathlib import Path

import matplotlib
import numpy as np

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from portable_flags import select


class OnlineLUT:
    def __init__(self, n, o3, evaluate_config):
        self.n = n
        self.o3 = o3
        self.names = []
        self.cycles = np.full(1 << n, np.nan)
        self.evals = 0
        self._evaluate_config = evaluate_config

    def evaluate(self, cid):
        self.evals += 1
        value = self._evaluate_config(int(cid))
        self.cycles[int(cid)] = np.nan if value is None else value
        return value

    def bits(self, cid):
        return np.array([(cid >> j) & 1 for j in range(self.n)], dtype=np.int8)

    @staticmethod
    def to_id(bits):
        return int(sum(int(b) << j for j, b in enumerate(bits)))

    def o3_cycles(self):
        value = self.cycles[self.o3]
        return float(value) if np.isfinite(value) else math.inf

    def optimum(self):
        return float(np.nanmin(self.cycles))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo", required=True)
    ap.add_argument("--champsim-root", required=True)
    ap.add_argument("--pin", required=True)
    ap.add_argument("--tracer", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--kernel", default="k_stencil")
    ap.add_argument("--nflags", type=int, default=None)
    ap.add_argument("--flags", nargs="+", default=None)
    ap.add_argument(
        "--compiler", choices=("wsl-mingw", "clang"), default="wsl-mingw"
    )
    ap.add_argument("--jobs", "-j", type=int, default=1)
    ap.add_argument("--trace-instructions", type=int, default=2_000_000)
    ap.add_argument("--trace-skip-instructions", type=int, default=0)
    ap.add_argument("--warmup-instructions", type=int, default=100_000)
    ap.add_argument("--budget", type=int, default=100)
    ap.add_argument(
        "--algorithm",
        choices=("carbs", "random", "hillclimb"),
        default="carbs",
    )
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--strict-o3", action="store_true")
    ap.add_argument("--force-trace", action="store_true")
    args = ap.parse_args()
    flags = select(args.nflags, args.flags)
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    measurements_path = out / "measurements.json"
    measurements = (
        json.loads(measurements_path.read_text())
        if measurements_path.exists()
        else {}
    )
    run_settings = {
        "repo": str(Path(args.repo).resolve()),
        "champsim_root": str(Path(args.champsim_root).resolve()),
        "pin": str(Path(args.pin).resolve()),
        "tracer": str(Path(args.tracer).resolve()),
        "kernel": args.kernel,
        "flags": list(flags),
        "compiler": args.compiler,
        "trace_instructions": args.trace_instructions,
        "trace_skip_instructions": args.trace_skip_instructions,
        "warmup_instructions": args.warmup_instructions,
    }
    if measurements and measurements.get("settings") != run_settings:
        raise SystemExit(
            "The existing measurements.json uses different run settings. "
            "Choose another --out directory or remove that cache."
        )
    cache = {int(k): v for k, v in measurements.get("configs", {}).items()}
    meta = measurements.get("meta")
    total = 1 << len(flags)
    backend = Path(__file__).with_name("run_carbs.py")

    def measure(cid):
        nonlocal meta
        if cid < 0 or cid >= total:
            return None
        if cid in cache and not args.force_trace:
            return cache[cid].get("cycles")
        one = out / "requested" / str(cid)
        cmd = [
            sys.executable,
            str(backend),
            "--repo",
            args.repo,
            "--champsim-root",
            args.champsim_root,
            "--pin",
            args.pin,
            "--tracer",
            args.tracer,
            "--out",
            str(one),
            "--kernel",
            args.kernel,
            "--flags",
            *flags,
            "--compiler",
            args.compiler,
            "--jobs",
            str(args.jobs),
            "--trace-instructions",
            str(args.trace_instructions),
            "--trace-skip-instructions",
            str(args.trace_skip_instructions),
            "--warmup-instructions",
            str(args.warmup_instructions),
            "--config-ids",
            str(cid),
        ]
        if args.strict_o3:
            cmd.append("--strict-o3")
        if args.force_trace:
            cmd.append("--force-trace")
        subprocess.run(cmd, check=True)
        data = json.loads((one / "champsim.json").read_text())
        meta = data
        row = next(
            csv.DictReader((one / "champsim.csv").open(encoding="utf-8")), None
        )
        result = None
        if row and row["cycles"] and row["correct"] == "1":
            result = float(row["cycles"])
        cache[cid] = {
            "cycles": result,
            "compiled_ok": bool(row),
            "correct": bool(row and row["correct"] == "1"),
        }
        measurements_path.write_text(
            json.dumps(
                {
                    "settings": run_settings,
                    "meta": meta,
                    "configs": {str(k): v for k, v in cache.items()},
                },
                indent=2,
            )
        )
        print(
            f"Measured config {cid}: {result if result is not None else 'invalid'} ({len(cache)}/{args.budget} unique)",
            flush=True,
        )
        return result

    # The selective backend also identifies the explicit configuration matching plain -O3.
    measure(0)
    if not meta:
        meta = json.loads(
            (out / "requested" / "0" / "champsim.json").read_text()
        )
    lut = OnlineLUT(len(flags), int(meta["o3_config_id"]), measure)
    lut.names = flags
    sys.path.insert(0, str(Path(args.repo).resolve()))
    from flagbench import tuners

    result = tuners.TUNERS[args.algorithm](
        lut, args.budget, args.seed, return_state=(args.algorithm == "carbs")
    )
    if args.algorithm == "carbs":
        trace, probabilities = result
    else:
        trace, probabilities = result, None
    # Include the forced bootstrap measurement in history and write the current sparse LUT.
    (out / "carbs.json").write_text(
        json.dumps(
            {
                "algorithm": args.algorithm,
                "seed": args.seed,
                "trace_best_cycles": [
                    None if not math.isfinite(x) else x for x in trace
                ],
                "final_probabilities": (
                    probabilities.tolist()
                    if probabilities is not None
                    else None
                ),
                "evaluations": lut.evals,
                "measured_config_ids": sorted(cache),
            },
            indent=2,
        )
    )
    ids = sorted(cache)
    with (out / f"{args.kernel}.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(
            ["config_id", *(f"f_{x}" for x in flags), "cycles", "correct"]
        )
        for cid in ids:
            rec = cache[cid]
            w.writerow(
                [
                    cid,
                    *[(cid >> j) & 1 for j in range(len(flags))],
                    rec["cycles"] or "",
                    int(rec["correct"]),
                ]
            )
    # Best-so-far progress plot; sparse measurements cannot support full-factorial analyses.
    best = np.minimum.accumulate(
        [x if x is not None else math.inf for x in trace]
    )
    fig, ax = plt.subplots(figsize=(6, 4))
    ax.plot(
        np.arange(1, len(best) + 1), best, marker=".", label=args.algorithm
    )
    ax.set(
        xlabel="tuner evaluations",
        ylabel="best cycles",
        title=f"{args.kernel}: adaptive {args.algorithm}",
    )
    ax.grid(True, alpha=0.25)
    ax.legend()
    fig.tight_layout()
    fig.savefig(out / f"score_{args.kernel}.png", dpi=140)
    plt.close(fig)
    print(
        f"Adaptive run complete. Unique configurations measured: {len(cache)} / {total}. Results: {out}"
    )


if __name__ == "__main__":
    main()
