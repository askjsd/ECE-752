#!/usr/bin/env python3
"""Run the native Windows ChampSim FlagBench workflow.

The native measurement stage compiles each flag configuration, validates it,
traces it with Intel PIN, simulates it with ChampSim, and writes the LUT.
The remaining stages replay CARBS, randomized search, and hill climbing from
that LUT and generate the same reports and plots as the gem5 workflow.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

from portable_flags import DEFAULT_FLAGS, select

ROOT = Path(__file__).resolve().parents[1]
CHAMPSIM = Path(__file__).resolve().parent
UPSTREAM = CHAMPSIM / ".upstream-windows"
TRACER = UPSTREAM / "tracer" / "pin" / "obj-intel64" / "champsim_tracer.dll"
SIMULATOR = UPSTREAM / "bin" / "champsim.exe"
DEFAULT_BUDGETS = (10, 20, 50, 100, 200)


def fail(message):
    raise SystemExit(message)


def command_path(name, description):
    path = shutil.which(name)
    if not path:
        fail(f"{description} was not found on PATH: {name}")
    return Path(path)


def find_pin(explicit):
    if explicit:
        root = Path(explicit).expanduser().resolve()
        if (root / "pin.exe").is_file():
            return root
        fail(f"Intel PIN was not found at {root}")

    downloads = Path.home() / "Downloads"
    candidates = sorted(downloads.glob("pin-external-*-windows"), key=lambda p: p.stat().st_mtime, reverse=True)
    for candidate in candidates:
        nested = candidate / candidate.name
        root = nested if (nested / "pin.exe").is_file() else candidate
        if (root / "pin.exe").is_file():
            return root
    fail("Intel PIN was not found. Pass --pin with the directory containing pin.exe.")


def build(pin, make):
    if not make:
        return
    make_path = command_path("make", "GNU Make")
    subprocess.run(
        [
            str(make_path),
            "CXX=clang++",
            "CC=clang",
            "CXXFLAGS=-Wno-c++11-narrowing -fms-runtime-lib=dll",
            "LDLIBS=-lshell32",
        ],
        cwd=UPSTREAM,
        check=True,
    )
    subprocess.run([str(make_path), f"PIN_ROOT={pin}"], cwd=UPSTREAM / "tracer" / "pin", check=True)


def run_stage(label, args, cwd=None, env=None):
    print(f"\n== {label} ==")
    subprocess.run(args, cwd=cwd, env=env, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "--kernels",
        nargs="+",
        default=["k_stencil", "k_ptrchase", "k_branchy", "k_reduce", "k_crc32", "k_helper"],
        help="kernel source names without .c",
    )
    parser.add_argument("--nflags", type=int, default=None,
                        help="number of leading gem5 flags to use (default: 12)")
    parser.add_argument("--flags", nargs="+", default=None,
                        help="explicit compiler optimization flags; overrides the default")
    parser.add_argument("--compiler", choices=("wsl-mingw", "clang"), default="wsl-mingw",
                        help="wsl-mingw is the portable GCC-equivalent backend")
    parser.add_argument("--pin", help="Intel PIN directory containing pin.exe")
    parser.add_argument("--out", help="output directory; defaults to champsim/results/windows")
    parser.add_argument("--trace-instructions", type=int, default=2_000_000)
    parser.add_argument("--warmup-instructions", type=int, default=100_000)
    parser.add_argument("--seeds", type=int, default=100, help="evaluation seeds for comparison plots")
    parser.add_argument("--budgets", type=int, nargs="+", default=DEFAULT_BUDGETS)
    parser.add_argument("--strict-o3", action="store_true",
                        help="fail unless plain Clang -O3 matches an explicit configuration")
    parser.add_argument("--build", action="store_true", help="rebuild ChampSim and the PIN tracer")
    parser.add_argument("--skip-tests", action="store_true", help="skip the shared tuner unit tests")
    parser.add_argument("--no-graphs", action="store_true", help="stop after the native LUT/CARBS stage")
    args = parser.parse_args()
    try:
        flags = select(args.nflags, args.flags)
    except ValueError as error:
        fail(str(error))

    if os.name != "nt":
        fail("run_windows.py is for native Windows. Use the Linux/WSL workflow on non-Windows hosts.")
    if not UPSTREAM.is_dir():
        fail(f"ChampSim source was not found: {UPSTREAM}")

    llvm = Path(r"C:\Program Files\LLVM\bin")
    if (llvm / "clang.exe").is_file():
        os.environ["PATH"] = str(llvm) + os.pathsep + os.environ.get("PATH", "")
    command_path("python", "Python")
    command_path("clang", "LLVM Clang")
    command_path("llvm-objcopy", "LLVM objcopy")

    pin = find_pin(args.pin)
    build(pin, args.build)
    if not SIMULATOR.is_file():
        fail(f"Native ChampSim executable was not found: {SIMULATOR}. Re-run with --build.")
    if not TRACER.is_file():
        fail(f"Windows PIN tracer was not found: {TRACER}. Re-run with --build.")

    runtime = UPSTREAM / "vcpkg_installed" / "x64-windows" / "bin"
    os.environ["PATH"] = str(llvm) + os.pathsep + str(runtime) + os.pathsep + os.environ.get("PATH", "")
    output = Path(args.out) if args.out else CHAMPSIM / "results" / "windows"
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)

    for kernel in args.kernels:
        kernel_output = output / kernel
        run_stage(
            f"native measurement and LUT for {kernel}",
            [
                sys.executable,
                str(CHAMPSIM / "run_carbs.py"),
                "--repo", str(ROOT),
                "--champsim-root", str(UPSTREAM),
                "--pin", str(pin),
                "--tracer", str(TRACER),
                "--out", str(kernel_output),
                "--kernel", kernel,
                "--flags", *flags,
                "--compiler", args.compiler,
                "--trace-instructions", str(args.trace_instructions),
                "--warmup-instructions", str(args.warmup_instructions),
                "--determinism", "5",
                *(("--strict-o3",) if args.strict_o3 else ()),
            ],
        )
        shutil.copyfile(kernel_output / f"{kernel}.csv", output / f"{kernel}.csv")
        shutil.copyfile(kernel_output / f"{kernel}.json", output / f"{kernel}.json")

    if not args.skip_tests:
        run_stage("tuner unit tests", [sys.executable, str(ROOT / "flagbench" / "test_tuners.py")], cwd=ROOT / "flagbench")

    if not args.no_graphs:
        graphs = output / "graphs"
        run_stage(
            "randomized, hill-climber, and CARBS evaluation plus graphs",
            [
                sys.executable,
                str(ROOT / "flagbench" / "evaluate.py"),
                "--luts", str(output),
                "--out", str(graphs),
                "--kernels", *args.kernels,
                "--seeds", str(args.seeds),
                "--budgets", *[str(b) for b in args.budgets],
                "--min-headroom", "0",
                "--min-denom-frac", "0",
            ],
            cwd=ROOT / "flagbench",
        )

    print("\nWorkflow complete.")
    print(f"Results: {output}")
    print(f"LUTs:    {output}/*/*.csv")
    print(f"CARBS:   {output}/*/carbs.json")
    if not args.no_graphs:
        print(f"Graphs:  {output / 'graphs'}")


if __name__ == "__main__":
    main()
