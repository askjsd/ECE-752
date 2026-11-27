#!/usr/bin/env python3
"""Run all FlagBench benchmarks on native Windows using WSL MinGW + Intel PIN + native ChampSim.

Two modes:
  1. Default (compiler-flag tuning):
     Compiles every kernel in flagbench/kernels/*.c with WSL MinGW (x86_64-w64-mingw32-gcc)
     so all GCC flags work, traces with Windows Intel PIN (champsim_tracer.dll), and simulates
     with native Windows ChampSim (defaults to C:\\champsim-knobbed-win\\bin\\champsim_knobbed
     if present, else champsim/source-windows/bin/champsim.exe).

  2. --knob-tune (prefetcher knob tuning):
     Compiles each kernel at -O3 with WSL MinGW, traces it once with Windows Intel PIN, and
     runs champsim/knob_carbs.py on each trace with the native knobbed ChampSim binary.

Examples (PowerShell, from repo root):
  python champsim\\run_flagbench_windows.py
  python champsim\\run_flagbench_windows.py --knob-tune
  python champsim\\run_flagbench_windows.py --kernels k_stencil k_ptrchase --nflags 12
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHAMPSIM = Path(__file__).resolve().parent
UPSTREAM = CHAMPSIM / "source-windows"
TRACER = UPSTREAM / "tracer" / "pin" / "obj-intel64" / "champsim_tracer.dll"
KERNELS_DIR = ROOT / "flagbench" / "kernels"

if str(CHAMPSIM) not in sys.path:
    sys.path.insert(0, str(CHAMPSIM))
import wsl_mingw  # noqa: E402
from portable_flags import (  # noqa: E402
    DEFAULT_FLAGS,
    select,
)


def fail(message):
    raise SystemExit(message)


def all_flagbench_kernels():
    return sorted(p.stem for p in KERNELS_DIR.glob("k_*.c"))


def find_pin(explicit):
    if explicit:
        root = Path(explicit).expanduser().resolve()
        if (root / "pin.exe").is_file():
            return root
        fail(f"Intel PIN was not found at {root}")
    downloads = Path.home() / "Downloads"
    candidates = sorted(
        downloads.glob("pin-external-*-windows"),
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
    for cand in candidates:
        nested = cand / cand.name
        root = nested if (nested / "pin.exe").is_file() else cand
        if (root / "pin.exe").is_file():
            return root
    fail(
        "Intel PIN was not found in ~/Downloads. Pass --pin <dir-with-pin.exe>."
    )


def find_simulator(explicit):
    if explicit:
        p = Path(explicit).expanduser().resolve()
        if p.is_file():
            return p
        fail(f"ChampSim binary not found: {p}")
    candidates = [
        Path(r"C:\champsim-knobbed-win\bin\champsim_knobbed.exe"),
        Path(r"C:\champsim-knobbed-win\bin\champsim_knobbed"),
        UPSTREAM / "bin" / "champsim.exe",
    ]
    for cand in candidates:
        if cand.is_file():
            return cand
    fail(
        "No native ChampSim binary found. Build champsim_knobbed first or pass --simulator."
    )


def run_stage(label, args, cwd=None, env=None):
    print(f"\n== {label} ==", flush=True)
    subprocess.run([str(a) for a in args], cwd=cwd, env=env, check=True)


def compile_o3_kernel(kernel, out_exe, compiler):
    src = KERNELS_DIR / f"{kernel}.c"
    if not src.is_file():
        fail(f"Kernel source not found: {src}")
    out_exe.parent.mkdir(parents=True, exist_ok=True)
    if compiler == "wsl-mingw":
        if not wsl_mingw.available():
            fail(
                "wsl-mingw requested, but x86_64-w64-mingw32-gcc was not found in WSL."
            )
        cmd = [
            "x86_64-w64-mingw32-gcc",
            "-O3",
            "-march=x86-64",
            "-I",
            str(KERNELS_DIR),
            str(src),
            "-o",
            str(out_exe),
            "-lm",
        ]
        res = wsl_mingw.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            fail(f"Failed to compile {kernel}:\n{res.stderr}")
    else:
        clang = shutil.which("clang") or r"C:\Program Files\LLVM\bin\clang.exe"
        cmd = [
            clang,
            "-O3",
            "-march=x86-64",
            "-I",
            str(KERNELS_DIR),
            str(src),
            "-o",
            str(out_exe),
            "-lm",
        ]
        subprocess.run(cmd, check=True)


def trace_with_pin(
    pin_dir, tracer_dll, exe_path, trace_path, skip_insns, trace_insns, force
):
    if trace_path.is_file() and trace_path.stat().st_size > 0 and not force:
        print(f"Reusing PIN trace {trace_path}")
        return
    trace_path.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        str(pin_dir / "pin.exe"),
        "-t",
        str(tracer_dll),
        "-s",
        str(skip_insns),
        "-t",
        str(trace_insns),
        "-o",
        str(trace_path),
        "--",
        str(exe_path),
    ]
    run_stage(f"PIN trace {exe_path.stem}", cmd)


def main():
    if os.name != "nt":
        fail("run_flagbench_windows.py is for native Windows.")

    all_kernels = all_flagbench_kernels()
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--kernels",
        nargs="+",
        default=all_kernels,
        help=f"default: all {len(all_kernels)} kernels",
    )
    parser.add_argument(
        "--compiler", choices=("wsl-mingw", "clang"), default="wsl-mingw"
    )
    parser.add_argument("--pin", help="Intel PIN directory containing pin.exe")
    parser.add_argument(
        "--simulator",
        help="Native ChampSim binary (default: C:\\champsim-knobbed-win\\bin\\champsim_knobbed)",
    )
    parser.add_argument("--out", help="Output directory")
    parser.add_argument(
        "--knob-tune",
        action="store_true",
        help="Tune prefetcher knobs (knob_carbs.py) on -O3 PIN traces",
    )
    parser.add_argument(
        "--nflags",
        type=int,
        default=None,
        help="Number of leading flags for flag tuning",
    )
    parser.add_argument(
        "--flags", nargs="+", default=None, help="Explicit optimization flags"
    )
    parser.add_argument(
        "--budget",
        type=int,
        default=20,
        help="Evaluations per tuner for --knob-tune",
    )
    parser.add_argument(
        "--seeds",
        type=int,
        default=2,
        help="Tuner seeds (--knob-tune; flag-tune uses 100)",
    )
    parser.add_argument("--trace-instructions", type=int, default=2_000_000)
    parser.add_argument("--trace-skip-instructions", type=int, default=1_000)
    parser.add_argument("--warmup-instructions", type=int, default=100_000)
    parser.add_argument("--jobs", "-j", type=int, default=8)
    parser.add_argument("--force-trace", action="store_true")
    parser.add_argument("--skip-tests", action="store_true")
    parser.add_argument("--no-graphs", action="store_true")
    args = parser.parse_args()

    llvm = Path(r"C:\Program Files\LLVM\bin")
    runtime = UPSTREAM / "vcpkg_installed" / "x64-windows" / "bin"
    if not runtime.is_dir():
        fail(f"vcpkg runtime DLLs not found: {runtime}")

    pin = find_pin(args.pin)
    if not TRACER.is_file():
        fail(f"Windows PIN tracer DLL not found: {TRACER}")
    simulator = find_simulator(args.simulator)

    os.environ["PATH"] = os.pathsep.join(
        [
            str(llvm),
            str(simulator.parent),
            str(runtime),
            os.environ.get("PATH", ""),
        ]
    )
    print(f"PIN:       {pin}")
    print(f"Tracer:    {TRACER}")
    print(f"Simulator: {simulator}")
    print(f"Kernels ({len(args.kernels)}): {' '.join(args.kernels)}")

    if args.knob_tune:
        output = (
            Path(args.out).resolve()
            if args.out
            else CHAMPSIM / "results" / "flagbench-knob-windows"
        )
        output.mkdir(parents=True, exist_ok=True)
        bin_dir = output / "binaries"
        trace_dir = output / "traces"
        env = dict(os.environ, PYTHONPATH=str(ROOT))
        for kernel in args.kernels:
            exe = bin_dir / f"{kernel}.exe"
            trace = trace_dir / f"{kernel}.champsimtrace"
            if args.force_trace or not exe.is_file():
                compile_o3_kernel(kernel, exe, args.compiler)
            trace_with_pin(
                pin,
                TRACER,
                exe,
                trace,
                args.trace_skip_instructions,
                args.trace_instructions,
                args.force_trace,
            )
            run_stage(
                f"knob_carbs on {kernel}",
                [
                    sys.executable,
                    str(CHAMPSIM / "knob_carbs.py"),
                    "--kernel",
                    kernel,
                    "--trace",
                    str(trace),
                    "--out",
                    str(output / f"{kernel}.json"),
                    "--binary",
                    str(simulator),
                    "--budget",
                    str(args.budget),
                    "--seeds",
                    str(args.seeds),
                    "--warmup",
                    str(args.warmup_instructions),
                    "--sim",
                    str(
                        max(
                            1,
                            args.trace_instructions - args.warmup_instructions,
                        )
                    ),
                ],
                env=env,
            )
        if not args.skip_tests:
            run_stage(
                "tuner unit tests",
                [sys.executable, str(ROOT / "flagbench" / "test_tuners.py")],
                cwd=ROOT / "flagbench",
            )
        run_stage(
            "summary",
            [
                sys.executable,
                str(CHAMPSIM / "knob_carbs.py"),
                "--summarize",
                str(output),
            ],
            env=env,
        )
        print(f"\nDone. Results: {output}")
        return

    # Default: FlagBench compiler-flag LUT + CARBS + evaluate.py
    flags = select(args.nflags, args.flags)
    output = (
        Path(args.out).resolve()
        if args.out
        else CHAMPSIM / "results" / "flagbench-windows"
    )
    output.mkdir(parents=True, exist_ok=True)
    for kernel in args.kernels:
        kernel_out = output / kernel
        run_stage(
            f"FlagBench {kernel}",
            [
                sys.executable,
                str(CHAMPSIM / "run_carbs.py"),
                "--repo",
                str(ROOT),
                "--champsim-root",
                str(UPSTREAM),
                "--simulator",
                str(simulator),
                "--pin",
                str(pin),
                "--tracer",
                str(TRACER),
                "--out",
                str(kernel_out),
                "--kernel",
                kernel,
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
                *(("--force-trace",) if args.force_trace else ()),
            ],
        )
        shutil.copyfile(kernel_out / f"{kernel}.csv", output / f"{kernel}.csv")
        shutil.copyfile(
            kernel_out / f"{kernel}.json", output / f"{kernel}.json"
        )

    if not args.skip_tests:
        run_stage(
            "tuner unit tests",
            [sys.executable, str(ROOT / "flagbench" / "test_tuners.py")],
            cwd=ROOT / "flagbench",
        )

    if not args.no_graphs:
        graphs = output / "graphs"
        eval_seeds = args.seeds if args.seeds != 2 else 100
        run_stage(
            "evaluate.py",
            [
                sys.executable,
                str(ROOT / "flagbench" / "evaluate.py"),
                "--luts",
                str(output),
                "--out",
                str(graphs),
                "--kernels",
                *args.kernels,
                "--seeds",
                str(eval_seeds),
                "--min-headroom",
                "0",
                "--min-denom-frac",
                "0",
            ],
            cwd=ROOT / "flagbench",
        )
    print(f"\nDone. Results: {output}")


if __name__ == "__main__":
    main()
