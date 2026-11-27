#!/usr/bin/env python3
"""Generate ChampSim PIN traces for all 56 pfbench configurations on native Windows.

Usage (PowerShell, from repo root):
  python pfbench\\trace_pin_windows.py
  python pfbench\\trace_pin_windows.py --only gather,jump
  python pfbench\\trace_pin_windows.py --skip-instructions 100000 --trace-instructions 2000000
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
TRACER = (
    ROOT
    / "champsim"
    / "source-windows"
    / "tracer"
    / "pin"
    / "obj-intel64"
    / "champsim_tracer.dll"
)
SRC = [
    "main.c",
    "pfb_common.c",
    "k_pointer.c",
    "k_indirect.c",
    "k_stride.c",
    "k_stream.c",
    "k_mix.c",
]

sys.path.insert(0, str(HERE))
from run_all import SUITE  # noqa: E402


def find_pin(explicit):
    if explicit:
        root = Path(explicit).expanduser().resolve()
        if (root / "pin.exe").is_file():
            return root
        raise SystemExit(f"Intel PIN was not found at {root}")
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
    raise SystemExit(
        "Intel PIN was not found in ~/Downloads. Pass --pin <dir-with-pin.exe>."
    )


def build_pfbench(rebuild):
    clang = shutil.which("clang") or r"C:\Program Files\LLVM\bin\clang.exe"
    cflags = [
        "-O2",
        "-std=gnu11",
        "-Wall",
        "-Wextra",
        "-Wno-unused-parameter",
        "-fno-tree-vectorize",
        "-fno-unroll-loops",
        "-fno-omit-frame-pointer",
    ]
    for name, extra in (
        ("pfbench.exe", []),
        ("pfbench_noswpf.exe", ["-DPFB_NO_SWPF"]),
    ):
        out = HERE / name
        if out.is_file() and not rebuild:
            continue
        print(f"Building {out} ...", flush=True)
        subprocess.run(
            [
                clang,
                *cflags,
                *extra,
                "-o",
                str(out),
                *[str(HERE / s) for s in SRC],
            ],
            check=True,
        )


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--pin", help="Intel PIN directory containing pin.exe")
    ap.add_argument(
        "--out",
        default=str(HERE / "traces"),
        help="Trace output directory (default: pfbench/traces)",
    )
    ap.add_argument(
        "--only",
        default="",
        help="Comma-separated benchmark names or kernel names",
    )
    ap.add_argument(
        "--skip-instructions",
        type=int,
        default=100_000,
        help="PIN -s instructions to skip (default: 100000)",
    )
    ap.add_argument(
        "--trace-instructions",
        type=int,
        default=2_000_000,
        help="PIN -t instructions to trace (default: 2000000)",
    )
    ap.add_argument(
        "--rebuild",
        action="store_true",
        help="Rebuild pfbench.exe and pfbench_noswpf.exe",
    )
    ap.add_argument(
        "--force", action="store_true", help="Regenerate existing traces"
    )
    args = ap.parse_args()

    if not TRACER.is_file():
        raise SystemExit(f"Windows PIN tracer DLL not found: {TRACER}")
    pin_dir = find_pin(args.pin)
    build_pfbench(args.rebuild)

    only = {s for s in args.only.split(",") if s}
    suite = [s for s in SUITE if not only or s[0] in only or s[1] in only]
    if not suite:
        raise SystemExit("No benchmark matches --only")

    out_dir = Path(args.out).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    pin_exe = pin_dir / "pin.exe"

    for i, (name, kernel, kargs, which) in enumerate(suite, 1):
        trace_path = out_dir / f"{name}.champsimtrace"
        if (
            trace_path.is_file()
            and trace_path.stat().st_size > 0
            and not args.force
        ):
            print(f"[{i}/{len(suite)}] Reusing {trace_path.name}")
            continue
        exe = HERE / (
            "pfbench_noswpf.exe" if which == "noswpf" else "pfbench.exe"
        )
        cmd = [
            str(pin_exe),
            "-t",
            str(TRACER),
            "-o",
            str(trace_path),
            "-s",
            str(args.skip_instructions),
            "-t",
            str(args.trace_instructions),
            "--",
            str(exe),
            kernel,
            *kargs,
        ]
        print(f"[{i}/{len(suite)}] Tracing {name} ...", flush=True)
        subprocess.run(cmd, check=True)

    print(f"\nDone. Traces in {out_dir}")


if __name__ == "__main__":
    main()
