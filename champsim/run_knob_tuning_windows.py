#!/usr/bin/env python3
"""Run the knobbed-prefetcher tuning workflow on native Windows.

*** UNTESTED on Windows. *** Written on Linux in the same shape as run_windows.py
(preflight checks, optional build, one run per input, tests, report), reusing the
build commands from run_windows.py / run_windows.ps1. The Linux version of every
step is tested; the --report-only mode below works on any OS and was tested.

Stages, per run:
  1. preflight   python + numpy, clang, make, xz (for .xz traces), vcpkg DLLs
  2. build       copy source-windows + champsim/modules to a path WITHOUT spaces
                 (ChampSim's Makefile breaks on spaces; "ece 752" has one) and build
                 champsim_knobbed.exe from champsim_config_knobbed.json
  3. tune        champsim/knob_carbs.py on every trace: CARBS, random search and hill
                 climbing over the prefetcher knobs (optionally an exhaustive sweep first)
  4. tests       flagbench/test_tuners.py (the tuners themselves are unchanged)
  5. report      results table, plus a speedup bar chart if matplotlib is installed

Unlike run_windows.py this does not compile benchmarks or trace them with PIN: it
takes ChampSim traces you already have (SPEC .xz traces, or traces made earlier).

Examples (PowerShell, from the repo root):
  python champsim\\run_knob_tuning_windows.py --trace C:\\traces\\605.mcf_s-1554B.champsimtrace.xz
  python champsim\\run_knob_tuning_windows.py --trace a.trace b.trace --budget 30 --seeds 3
  python champsim\\run_knob_tuning_windows.py --trace k_stencil.trace --exhaustive --jobs 8
  python champsim\\run_knob_tuning_windows.py --report-only champsim\\results\\knob-carbs-spec-v2
"""

import argparse
import json
import math
import os
import re
import shutil
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHAMPSIM = Path(__file__).resolve().parent
SOURCE_WINDOWS = CHAMPSIM / "source-windows"
MODULES = CHAMPSIM / "modules"
CONFIG = CHAMPSIM / "champsim_config_knobbed.json"
TUNER = CHAMPSIM / "knob_carbs.py"
LLVM_BIN = Path(r"C:\Program Files\LLVM\bin")


def fail(message):
    raise SystemExit(message)


def command_path(name, hint):
    path = shutil.which(name)
    if not path:
        fail(f"{name} was not found on PATH. {hint}")
    return Path(path)


def run_stage(label, args, cwd=None, env=None):
    print(f"\n== {label}", flush=True)
    subprocess.run([str(a) for a in args], cwd=cwd, env=env, check=True)


def kernel_name(trace):
    """File name without the trace suffixes: 605.mcf_s-1554B.champsimtrace.xz -> 605.mcf_s-1554B."""
    return re.sub(r"\.(champsimtrace|trace|champsim)?(\.xz)?$", "", Path(trace).name)


# ---- 1. preflight ---------------------------------------------------------------------
def preflight(traces):
    if os.name != "nt":
        fail("This script is for native Windows. On Linux use champsim/build_knobbed.sh and "
             "champsim/knob_carbs.py (see KNOB_TUNING_README.md), or run it inside WSL2.")
    if not SOURCE_WINDOWS.is_dir():
        fail(f"ChampSim Windows source was not found: {SOURCE_WINDOWS}")
    if (LLVM_BIN / "clang.exe").is_file():
        os.environ["PATH"] = str(LLVM_BIN) + os.pathsep + os.environ.get("PATH", "")
    command_path("python", "Install Python 3.")
    clang = command_path("clang", "Install LLVM (https://releases.llvm.org).")
    command_path("make", "Install GNU Make (for example: winget install GnuWin32.Make).")
    try:
        import numpy  # noqa: F401  (knob_carbs.py needs it)
    except ImportError:
        fail("numpy is missing. Run: python -m pip install numpy")
    if any(str(t).endswith(".xz") for t in traces):
        command_path("xz", "ChampSim reads .xz traces through xz. Install xz, or decompress the trace with 7-Zip first.")
    runtime = SOURCE_WINDOWS / "vcpkg_installed" / "x64-windows" / "bin"
    if not runtime.is_dir():
        fail(f"vcpkg runtime DLLs not found: {runtime}. Build ChampSim's vcpkg dependencies first "
             "(see champsim/README.md, Windows prerequisites).")
    # same PATH layout as run_windows.py: LLVM first, then the vcpkg DLLs
    os.environ["PATH"] = os.pathsep.join([str(clang.parent), str(runtime), os.environ.get("PATH", "")])


# ---- 2. build -------------------------------------------------------------------------
def build(build_dir, rebuild):
    exe = build_dir / "bin" / "champsim_knobbed.exe"
    if exe.is_file() and not rebuild:
        print(f"Reusing {exe} (pass --rebuild to rebuild)")
        return exe
    print(f"Building knobbed ChampSim in {build_dir} (a few minutes)", flush=True)
    build_dir.mkdir(parents=True, exist_ok=True)
    skip = shutil.ignore_patterns(".csconfig", "bin", "tracer", "absolute.options")
    shutil.copytree(SOURCE_WINDOWS, build_dir, ignore=skip, dirs_exist_ok=True)
    modules = build_dir / "knobbed_modules"
    shutil.rmtree(modules, ignore_errors=True)
    shutil.copytree(MODULES, modules)
    for stale in (".csconfig", "inc/cache_modules.h", "inc/ooo_cpu_modules.h"):
        path = build_dir / stale
        if path.is_dir():
            shutil.rmtree(path)
        else:
            path.unlink(missing_ok=True)
    subprocess.run([sys.executable, str(build_dir / "config.sh"), "--module-dir", str(modules), str(CONFIG)],
                   cwd=build_dir, check=True)
    # config.sh can leave absolute paths here; run_windows.py deletes it before make too
    (build_dir / "absolute.options").unlink(missing_ok=True)
    subprocess.run(
        [str(command_path("make", "GNU Make")), "CXX=clang++", "CC=clang",
         "CXXFLAGS=-Wno-c++11-narrowing -fms-runtime-lib=dll", "LDLIBS=-lshell32"],
        cwd=build_dir, check=True)
    if not exe.is_file():
        fail(f"Build finished but {exe} does not exist; the name comes from executable_name in {CONFIG}.")
    return exe


# ---- 5. report ------------------------------------------------------------------------
def load_results(directory):
    rows = []
    for path in sorted(Path(directory).glob("*.json")):
        data = json.loads(path.read_text())
        ref = data.get("reference", {})
        off, default = ref.get("prefetch_off"), ref.get("default_hint")
        best = data.get("best", {}).get("cycles")
        if not off or not best or "traces" not in data:
            continue
        speed = {}
        for name, runs in data["traces"].items():
            finals = [t[-1] for t in runs if t and math.isfinite(t[-1])]
            speed[name] = off / statistics.median(finals) if finals else float("nan")
        rows.append({"kernel": data["kernel"], "default": off / default if default else float("nan"),
                     "best": off / best, **speed})
    return rows


def plot(rows, out_png):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not installed; skipping the plot (python -m pip install matplotlib)")
        return
    series = [("default", "default hint"), ("carbs", "CARBS"), ("random", "random"),
              ("hillclimb", "hill-climb"), ("best", "best found")]
    width = 0.16
    fig, ax = plt.subplots(figsize=(max(6, 1.3 * len(rows)), 4.5))
    for i, (key, label) in enumerate(series):
        ax.bar([x + (i - 2) * width for x in range(len(rows))], [r[key] for r in rows], width, label=label)
    ax.axhline(1.0, color="gray", linewidth=0.8)
    ax.set_xticks(range(len(rows)))
    ax.set_xticklabels([r["kernel"] for r in rows], rotation=35, ha="right", fontsize=8)
    ax.set_ylabel("speedup over prefetch off")
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(out_png, dpi=140)
    print(f"Plot: {out_png}")


def report(directory, no_plots):
    subprocess.run([sys.executable, str(TUNER), "--summarize", str(directory)], check=True,
                   env=dict(os.environ, PYTHONPATH=str(ROOT)))
    rows = load_results(directory)
    if rows and not no_plots:
        plot(rows, Path(directory) / "speedup.png")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--trace", nargs="+", help="one or more ChampSim traces (.xz or plain)")
    parser.add_argument("--out", help="results directory (default champsim/results/knob-carbs-windows)")
    parser.add_argument("--budget", type=int, default=20, help="evaluations per tuner run")
    parser.add_argument("--seeds", type=int, default=2)
    parser.add_argument("--warmup-instructions", type=int, default=1_000_000)
    parser.add_argument("--sim-instructions", type=int, default=3_000_000,
                        help="must fit in the trace after the warmup")
    parser.add_argument("--exhaustive", action="store_true",
                        help="simulate every distinct config first (2,305 per trace; ground truth)")
    parser.add_argument("--jobs", type=int, default=4, help="parallel simulations for --exhaustive")
    parser.add_argument("--build-dir", default=r"C:\champsim-knobbed-win", help="must not contain spaces")
    parser.add_argument("--rebuild", action="store_true", help="rebuild even if the .exe exists")
    parser.add_argument("--skip-tests", action="store_true", help="skip flagbench/test_tuners.py")
    parser.add_argument("--no-plots", action="store_true")
    parser.add_argument("--report-only", metavar="DIR",
                        help="only print the table and plot for an existing results directory (works on any OS)")
    args = parser.parse_args()

    if args.report_only:
        report(args.report_only, args.no_plots)
        return
    if not args.trace:
        parser.error("--trace is required (or use --report-only DIR)")
    traces = [Path(t).expanduser().resolve() for t in args.trace]
    for trace in traces:
        if not trace.is_file():
            fail(f"Trace not found: {trace}")
    build_dir = Path(args.build_dir)
    if " " in str(build_dir):
        fail(f"--build-dir must not contain spaces: {build_dir}")

    preflight(traces)
    exe = build(build_dir, args.rebuild)

    output = Path(args.out).resolve() if args.out else CHAMPSIM / "results" / "knob-carbs-windows"
    output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, PYTHONPATH=str(ROOT))  # knob_carbs.py imports flagbench.tuners from the repo root
    for trace in traces:
        kernel = kernel_name(trace)
        run_stage(
            f"tuning knobs on {kernel}",
            [sys.executable, TUNER, "--kernel", kernel, "--trace", trace,
             "--out", output / f"{kernel}.json", "--binary", exe,
             "--budget", args.budget, "--seeds", args.seeds,
             "--warmup", args.warmup_instructions, "--sim", args.sim_instructions,
             *(("--exhaustive", "--jobs", args.jobs) if args.exhaustive else ())],
            env=env)

    if not args.skip_tests:
        run_stage("tuner unit tests", [sys.executable, ROOT / "flagbench" / "test_tuners.py"],
                  cwd=ROOT / "flagbench")

    print("\n== report")
    report(output, args.no_plots)
    print("\nWorkflow complete.")
    print(f"Results:    {output}")
    print(f"Simulator:  {exe}")


if __name__ == "__main__":
    main()
