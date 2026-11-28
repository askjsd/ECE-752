#!/usr/bin/env python3
"""Generate ChampSim PIN traces for every pfbench configuration on native Windows.

Usage (PowerShell, from repo root):
  python pfbench\\trace_pin_windows.py
  python pfbench\\trace_pin_windows.py --only gather,jump
  python pfbench\\trace_pin_windows.py --jobs 8     # trace 8 benchmarks at a time

Each trace covers exactly the measured region (ROI): the PIN tracer starts at
pfb_roi_begin and stops at pfb_roi_end (-start_symbol / -stop_symbol), like
trace_dr.sh -S/-E on Linux. --trace-instructions is only a cap. Earlier versions
started a fixed 100K instructions into the program, which recorded mostly setup code
(allocation, filling memory, building lists): setup runs 0.25M-12M instructions.
pfbench.exe exports the two ROI functions so PIN can find them without debug info.

A trace is reused only if it was made by the same command from a pfbench.exe that
has not changed since; otherwise (for example, traces from the old fixed-offset
version) it is made again. pfbench.exe is rebuilt when a source file is newer.

Traces are independent, so they run in parallel (--jobs). Native timing runs are
not done here; those belong in run_all.py and must stay sequential.
"""

import argparse
import json
import os
from concurrent.futures import ThreadPoolExecutor, as_completed
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


HDRS = ["pfb.h"]


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
        newest_src = max((HERE / f).stat().st_mtime for f in SRC + HDRS)
        if out.is_file() and not rebuild and out.stat().st_mtime >= newest_src:
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
        "--trace-instructions",
        type=int,
        default=5_000_000,
        help="cap on trace length; traces normally end at pfb_roi_end first (default: 5000000)",
    )
    ap.add_argument(
        "--rebuild",
        action="store_true",
        help="Rebuild pfbench.exe and pfbench_noswpf.exe",
    )
    ap.add_argument(
        "--force", action="store_true", help="Regenerate existing traces"
    )
    ap.add_argument(
        "--jobs",
        "-j",
        type=int,
        default=max(1, (os.cpu_count() or 2) // 2),
        help="Benchmarks traced in parallel (default: half the logical CPUs)",
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

    jobs = []
    for name, kernel, kargs, which in suite:
        trace_path = out_dir / f"{name}.champsimtrace"
        meta_path = out_dir / f"{name}.champsimtrace.json"
        exe = HERE / (
            "pfbench_noswpf.exe" if which == "noswpf" else "pfbench.exe"
        )
        cmd = [
            str(pin_exe),
            "-t",
            str(TRACER),
            "-o",
            str(trace_path),
            "-start_symbol",
            "pfb_roi_begin",
            "-stop_symbol",
            "pfb_roi_end",
            "-t",
            str(args.trace_instructions),
            "--",
            str(exe),
            kernel,
            *kargs,
            "--quiet=1",
        ]
        if not args.force and trace_path.is_file() and meta_path.is_file():
            meta = json.loads(meta_path.read_text())
            if (
                meta.get("cmd") == cmd
                and trace_path.stat().st_size > 0
                and trace_path.stat().st_mtime >= exe.stat().st_mtime
            ):
                print(f"Reusing {trace_path.name} ({meta.get('records')} instructions)")
                continue
        jobs.append((name, cmd, trace_path, meta_path, out_dir / f"{name}.trace.log"))

    def run(job):
        name, cmd, trace, meta_path, log = job
        for stale in (trace, meta_path):
            if stale.exists():
                stale.unlink()
        with open(log, "w") as f:
            rc = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT).returncode
        text = log.read_text(errors="replace")
        ok = rc == 0 and "Found start symbol" in text and trace.is_file() and trace.stat().st_size > 0
        if not ok:
            if trace.exists():
                trace.unlink()  # never reuse a partial or wrong trace
            why = "start symbol pfb_roi_begin not found" if "Found start symbol" not in text else f"exit {rc}"
            raise RuntimeError(f"{why}, see {log}")
        records = trace.stat().st_size // 64  # one ChampSim instruction record = 64 bytes
        capped = "Stopped tracing" not in text
        meta_path.write_text(json.dumps({"cmd": cmd, "records": records, "capped": capped}))
        note = f"; ROI longer than --trace-instructions, trace holds its first {records}" if capped else ""
        return f"{name} ({records} instructions{note})"

    failed = []
    print(f"Tracing {len(jobs)} benchmarks, {args.jobs} at a time", flush=True)
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(run, job): job[0] for job in jobs}
        for i, fut in enumerate(as_completed(futs), 1):
            try:
                print(f"[{i}/{len(jobs)}] Traced {fut.result()}", flush=True)
            except Exception as e:
                failed.append(futs[fut])
                print(f"[{i}/{len(jobs)}] FAILED {futs[fut]}: {e}", flush=True)
    if failed:
        raise SystemExit(f"{len(failed)} trace(s) failed: {', '.join(failed)}")

    print(f"\nDone. Traces in {out_dir}")


if __name__ == "__main__":
    main()
