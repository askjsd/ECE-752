#!/usr/bin/env python3
"""Compile and simulate only configurations requested by a tuner.

Each measurement uses run_carbs.py's selective mode, so the exhaustive legacy
workflow remains available while this runner spends resources on observed asks.
"""

import argparse
import csv
import hashlib
import json
import math
import os
import random
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
        self.cycles = {}
        self.evals = 0
        self._evaluate_config = evaluate_config

    def evaluate(self, cid):
        self.evals += 1
        value = self._evaluate_config(int(cid))
        self.cycles[int(cid)] = value
        return value

    def bits(self, cid):
        return np.array([(cid >> j) & 1 for j in range(self.n)], dtype=np.int8)

    @staticmethod
    def to_id(bits):
        return int(sum(int(b) << j for j, b in enumerate(bits)))

    def o3_cycles(self):
        value = self.cycles.get(self.o3)
        return float(value) if value is not None else math.inf

    def optimum(self):
        values = [value for value in self.cycles.values() if value is not None]
        return min(values) if values else math.inf


def random_search_online(lut, budget, seed):
    """Sample without replacement without enumerating the exponential space."""
    count = min(budget, 1 << lut.n)
    rng = random.Random(seed)
    selected = set()
    total = 1 << lut.n
    for value in range(total - count, total):
        candidate = rng.randrange(value + 1)
        selected.add(value if candidate in selected else candidate)
    order = list(selected)
    rng.shuffle(order)
    best = math.inf
    trace = []
    for cid in order:
        value = lut.evaluate(cid)
        if value is not None:
            best = min(best, value)
        trace.append(best)
    return trace


def file_fingerprint(path):
    path = Path(path)
    if not path.is_file():
        return None
    stat = path.stat()
    return {"size": stat.st_size, "mtime_ns": stat.st_mtime_ns}


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
    ap.add_argument("--trace-skip-instructions", type=int, default=1_000)
    ap.add_argument("--warmup-instructions", type=int, default=100_000)
    ap.add_argument(
        "--simulation-timeout",
        type=int,
        default=900,
        help="per-configuration ChampSim timeout in seconds (default: 900)",
    )
    ap.add_argument(
        "--budget",
        type=int,
        default=None,
        help="evaluations per tuner (default: 20 when comparing, otherwise 100)",
    )
    ap.add_argument(
        "--compare",
        action="store_true",
        help="compare CARBS, random search, and hill climbing online",
    )
    ap.add_argument("--seeds", type=int, default=3)
    ap.add_argument("--determinism", type=int, default=0)
    ap.add_argument(
        "--algorithm",
        choices=("carbs", "random", "hillclimb"),
        default="carbs",
    )
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--force-trace", action="store_true")
    args = ap.parse_args()
    if args.budget is None:
        args.budget = 20 if args.compare else 100
    try:
        flags = select(args.nflags, args.flags)
    except ValueError as error:
        ap.error(str(error))
    if (
        args.budget < 1
        or args.seeds < 1
        or args.jobs < 1
        or args.determinism < 0
        or args.simulation_timeout < 1
        or args.trace_instructions < 1
        or args.trace_skip_instructions < 0
        or args.warmup_instructions < 0
    ):
        ap.error(
            "--budget, --jobs, --seeds, and --simulation-timeout must be positive; "
            "trace instructions must be positive, and skip, warmup, and "
            "determinism counts cannot be negative"
        )
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    repo = Path(args.repo).resolve()
    champsim_root = Path(args.champsim_root).resolve()
    source = repo / "flagbench" / "kernels" / f"{args.kernel}.c"
    pin_root = Path(args.pin).resolve()
    pin_executable = pin_root / ("pin.exe" if os.name == "nt" else "pin")
    legacy_runner = Path(__file__).with_name("run_carbs.py")
    simulator = (
        champsim_root
        / "bin"
        / ("champsim.exe" if os.name == "nt" else "champsim")
    )
    tracer = Path(args.tracer).resolve()
    measurements_path = out / "measurements.json"
    measurements = (
        json.loads(measurements_path.read_text())
        if measurements_path.exists()
        else {}
    )
    run_settings = {
        "repo": str(repo),
        "champsim_root": str(champsim_root),
        "pin": str(pin_root),
        "tracer": str(tracer),
        "kernel": args.kernel,
        "flags": list(flags),
        "compiler": args.compiler,
        "trace_instructions": args.trace_instructions,
        "trace_skip_instructions": args.trace_skip_instructions,
        "warmup_instructions": args.warmup_instructions,
        "simulation_timeout": args.simulation_timeout,
        "determinism": args.determinism,
        "cache_version": 4,
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "portable_flags_sha256": hashlib.sha256(
            Path(__file__).with_name("portable_flags.py").read_bytes()
        ).hexdigest(),
        "runner_sha256": hashlib.sha256(
            legacy_runner.read_bytes()
        ).hexdigest(),
        "adaptive_sha256": hashlib.sha256(
            Path(__file__).read_bytes()
        ).hexdigest(),
        "pin_fingerprint": file_fingerprint(pin_executable),
        "tracer_fingerprint": file_fingerprint(tracer),
        "simulator_fingerprint": file_fingerprint(simulator),
    }
    if measurements and measurements.get("settings") != run_settings:
        raise SystemExit(
            "The existing measurements.json uses different run settings. "
            "Choose another --out directory or remove that cache."
        )
    cache = {int(k): v for k, v in measurements.get("configs", {}).items()}
    hash_results = measurements.get("hash_results", {})
    meta = measurements.get("meta")
    baseline_meta = measurements.get("baseline_meta", {})
    total = 1 << len(flags)
    runner = legacy_runner
    settings_hash = hashlib.sha256(
        json.dumps(run_settings, sort_keys=True).encode()
    ).hexdigest()[:16]
    backend = out / "backend" / settings_hash
    backend.mkdir(parents=True, exist_ok=True)
    hash_results_path = backend / "hash-results.json"
    attempted = set()

    def measure(cid):
        nonlocal baseline_meta, meta
        if cid < 0 or cid >= total:
            return None
        if cid in attempted:
            return cache[cid].get("cycles")
        if (
            cid in cache
            and cache[cid].get("status") == "valid"
            and not args.force_trace
        ):
            return cache[cid].get("cycles")
        attempted.add(cid)
        one = backend
        cmd = [
            sys.executable,
            str(runner),
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
            "--determinism",
            str(args.determinism),
            "--simulation-timeout",
            str(args.simulation_timeout),
            "--config-ids",
            str(cid),
            "--reuse-baseline",
        ]
        if args.force_trace:
            cmd.append("--force-trace")
        hash_results_path.write_text(
            json.dumps({} if args.force_trace else hash_results)
        )
        cmd.extend(["--known-hash-results", str(hash_results_path)])
        try:
            subprocess.run(cmd, check=True)
            meta = json.loads((one / "champsim.json").read_text())
            if cid == 0:
                baseline_meta = dict(meta)
            with (one / "champsim.csv").open(encoding="utf-8") as stream:
                row = next(csv.DictReader(stream), None)
            result = None
            if row and row["cycles"] and row["correct"] == "1":
                result = float(row["cycles"])
                hash_results[row["binary_hash"]] = {
                    "cycles": result,
                    "instructions": float(row["instructions"]),
                    "ipc": float(row["ipc"]),
                }
        finally:
            # Traces grow by 64 bytes per instruction; discard one-use artifacts
            # after the measurement is durable in measurements.json.
            suffix = ".exe" if os.name == "nt" else ""
            for path in (
                one / "binaries" / f"{cid}{suffix}",
                one / "traces" / f"{cid}.champsim",
            ):
                path.unlink(missing_ok=True)
        cache[cid] = {
            "cycles": result,
            "compiled_ok": bool(int(row["compiled_ok"])) if row else None,
            "correct": bool(int(row["correct"])) if row else None,
            "status": (
                "valid"
                if result is not None
                else (
                    "incorrect"
                    if row and row["correct"] != "1"
                    else "no_simulation_result"
                )
            ),
        }
        temporary_measurements = measurements_path.with_suffix(".json.tmp")
        temporary_measurements.write_text(
            json.dumps(
                {
                    "settings": run_settings,
                    "meta": meta,
                    "baseline_meta": baseline_meta,
                    "configs": {str(k): v for k, v in cache.items()},
                    "hash_results": hash_results,
                },
                indent=2,
            )
        )
        temporary_measurements.replace(measurements_path)
        print(
            f"Measured config {cid}: {result if result is not None else 'invalid'} "
            f"({len(cache)} unique configurations cached)",
            flush=True,
        )
        return result

    def write_sparse_csv():
        with (out / f"{args.kernel}.csv").open("w", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(
                [
                    "config_id",
                    *(f"f_{name}" for name in flags),
                    "cycles",
                    "correct",
                    "compiled_ok",
                    "status",
                ]
            )
            for cid in sorted(cache):
                record = cache[cid]
                writer.writerow(
                    [
                        cid,
                        *[(cid >> bit) & 1 for bit in range(len(flags))],
                        record["cycles"] or "",
                        (
                            ""
                            if record["correct"] is None
                            else int(record["correct"])
                        ),
                        (
                            ""
                            if record["compiled_ok"] is None
                            else int(record["compiled_ok"])
                        ),
                        record["status"],
                    ]
                )

    # Config 0 is the shared reference and CARBS bootstrap configuration.
    baseline_id = 0
    if args.compare or args.algorithm == "carbs":
        measure(0)
    lut = OnlineLUT(len(flags), baseline_id, measure)
    lut.names = flags
    sys.path.insert(0, str(Path(args.repo).resolve()))
    from flagbench import tuners

    if args.compare:
        algorithms = ("carbs", "random", "hillclimb")
        all_traces = {name: [] for name in algorithms}
        for name in algorithms:
            for seed in range(args.seed, args.seed + args.seeds):
                if name == "carbs":
                    trace, _ = tuners.carbs(
                        lut, args.budget, seed, return_state=True
                    )
                elif name == "random":
                    trace = random_search_online(lut, args.budget, seed)
                else:
                    trace = tuners.hill_climb(lut, args.budget, seed)
                trace = list(trace)
                if trace:
                    trace.extend([trace[-1]] * (args.budget - len(trace)))
                all_traces[name].append(trace[: args.budget])
                print(
                    f"Completed {name} seed {seed + 1}/{args.seeds}; "
                    f"{len(cache)} unique configurations measured.",
                    flush=True,
                )

        write_sparse_csv()
        evaluation_axis = np.arange(1, args.budget + 1)
        report = {
            "kernel": args.kernel,
            "flags": list(flags),
            "budget": args.budget,
            "seeds": args.seeds,
            "seed_start": args.seed,
            "measured_configurations": len(cache),
            "total_configurations": total,
            "reference_config_id": baseline_id,
            "o3_identity_verified": baseline_meta.get(
                "o3_identity_verified", False
            ),
            "algorithms": {},
        }
        fig, ax = plt.subplots(figsize=(7, 4.5))
        rng = np.random.default_rng(17)
        styles = {
            "carbs": ("o", "-"),
            "random": ("s", "--"),
            "hillclimb": ("^", ":"),
        }
        for name, traces in all_traces.items():
            values = np.asarray(
                [
                    [
                        (
                            float(x)
                            if x is not None and math.isfinite(x)
                            else np.nan
                        )
                        for x in trace
                    ]
                    for trace in traces
                ],
                dtype=float,
            )
            medians, lows, highs = [], [], []
            for column in values.T:
                finite = column[np.isfinite(column)]
                if not len(finite):
                    medians.append(np.nan)
                    lows.append(np.nan)
                    highs.append(np.nan)
                    continue
                medians.append(float(np.median(finite)))
                boot = np.median(
                    rng.choice(finite, size=(1000, len(finite)), replace=True),
                    axis=1,
                )
                lows.append(float(np.percentile(boot, 2.5)))
                highs.append(float(np.percentile(boot, 97.5)))
            marker, linestyle = styles[name]
            ax.plot(
                evaluation_axis,
                medians,
                label=name,
                marker=marker,
                linestyle=linestyle,
                markevery=max(1, args.budget // 10),
                markersize=4,
            )
            ax.fill_between(evaluation_axis, lows, highs, alpha=0.18)
            final = values[:, -1]
            final = final[np.isfinite(final)]
            report["algorithms"][name] = {
                "median_best_cycles_by_evaluation": [
                    None if not np.isfinite(value) else float(value)
                    for value in medians
                ],
                "ci95_best_cycles_by_evaluation": [
                    (
                        None
                        if not np.isfinite(low) or not np.isfinite(high)
                        else [low, high]
                    )
                    for low, high in zip(lows, highs)
                ],
                "final_median_best_cycles": (
                    float(np.median(final)) if len(final) else None
                ),
                "seeds_with_valid_measurements": int(len(final)),
                "seed_traces": [
                    [None if not math.isfinite(x) else x for x in trace]
                    for trace in traces
                ],
            }
        ax.set(
            xlabel="tuner evaluations",
            ylabel="best cycles (lower is better)",
            title=f"{args.kernel}: online tuner comparison ({args.seeds} seeds)",
        )
        ax.grid(True, alpha=0.25)
        ax.legend()
        fig.tight_layout()
        fig.savefig(out / f"score_compare_{args.kernel}.png", dpi=140)
        plt.close(fig)
        (out / "comparison.json").write_text(json.dumps(report, indent=2))
        print(
            f"Comparison complete: {len(cache)} unique configurations measured "
            f"out of {total}. Results: {out}",
            flush=True,
        )
        return

    tuner = (
        random_search_online
        if args.algorithm == "random"
        else tuners.TUNERS[args.algorithm]
    )
    result = tuner(
        lut,
        args.budget,
        args.seed,
        **({"return_state": True} if args.algorithm == "carbs" else {}),
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
                "baseline_config_id": lut.o3,
                "o3_identity_verified": baseline_meta.get(
                    "o3_identity_verified", False
                ),
            },
            indent=2,
        )
    )
    write_sparse_csv()
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
