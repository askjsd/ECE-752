#!/usr/bin/env python3
"""Compile x86 FlagBench binaries, trace/simulate them in ChampSim, and run CARBS."""

import argparse
import csv
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from concurrent.futures import (
    ThreadPoolExecutor,
    as_completed,
)

import numpy as np
from portable_flags import DEFAULT_FLAGS
from portable_flags import config_flags as make_config_flags
from portable_flags import select


def command(args, **kwargs):
    return subprocess.run(args, check=True, text=True, **kwargs)


def executable(root, name):
    candidates = [os.path.join(root, name)]
    if os.name == "nt":
        candidates.insert(0, os.path.join(root, name + ".exe"))
    for candidate in candidates:
        if os.path.isfile(candidate):
            return candidate
    return candidates[0]


def stat(output, name):
    matches = re.findall(rf"{re.escape(name)}:\s*([0-9.eE+-]+)", output)
    return float(matches[-1]) if matches else None


def canonicalize_windows_trace(trace_path):
    """Avoid ChampSim's Windows allocator fault for traced 0x1Axxxxxx addresses.

    The checked-in PIN tracer applies the same remap while recording, but an
    already-built tracer DLL may predate that fix. Normalize trace memory fields
    here as well so old and new DLLs produce simulator-safe traces. Windows may
    place the allocator range at either 0x0A000000 or 0x1A000000.
    """
    with open(trace_path, "r+b") as trace_file:
        raw = bytearray(trace_file.read())
        if len(raw) % 64:
            raise SystemExit(
                f"Invalid ChampSim trace size ({len(raw)} bytes): {trace_path}"
            )
        memory = np.ndarray(
            (len(raw) // 64, 6),
            dtype="<u8",
            buffer=raw,
            offset=16,
            strides=(64, 8),
        )
        had_full_addresses = bool(np.any(memory > 0xFFFFFFFF))
        # ChampSim's Windows build expects the experiment's legacy low-32-bit
        # address model. PIN can hand the tracer full-width process pointers.
        memory[:] &= 0xFFFFFFFF
        low_allocator_32 = (memory >= 0x0A000000) & (memory < 0x0B000000)
        high_allocator_32 = (memory >= 0x1A000000) & (memory < 0x1B000000)
        previous_allocator_32 = (memory >= 0x1B000000) & (memory < 0x1C000000)
        count = sum(
            int(np.count_nonzero(mask))
            for mask in (
                low_allocator_32,
                high_allocator_32,
                previous_allocator_32,
            )
        )
        if count or had_full_addresses:
            memory[low_allocator_32] = 0x40000000 + (
                memory[low_allocator_32] - 0x0A000000
            )
            memory[high_allocator_32] = 0x40000000 + (
                memory[high_allocator_32] - 0x1A000000
            )
            memory[previous_allocator_32] = 0x40000000 + (
                memory[previous_allocator_32] - 0x1B000000
            )
            trace_file.seek(0)
            trace_file.write(raw)
        return count


def trace_is_normalized(trace_path):
    """Check that a trace uses low-32-bit addresses and the current heap map."""
    with open(trace_path, "rb") as trace_file:
        raw = trace_file.read()
    if not raw or len(raw) % 64:
        return False
    records = len(raw) // 64
    ips = np.ndarray(
        (records,), dtype="<u8", buffer=raw, offset=0, strides=(64,)
    )
    memory = np.ndarray(
        (records, 6), dtype="<u8", buffer=raw, offset=16, strides=(64, 8)
    )
    allocator = ((memory >= 0x0A000000) & (memory < 0x0C000000)) | (
        (memory >= 0x1A000000) & (memory < 0x1C000000)
    )
    # Preserve high-bit PCs: folding them into another range can itself create
    # a ChampSim page-walk deadlock. Requiring one also invalidates traces from
    # an earlier experimental normalization that altered instruction PCs.
    return bool(np.any(ips & 0x80000000)) and not bool(
        np.any(memory > 0xFFFFFFFF) or np.any(allocator)
    )


def checksum(binary):
    result = subprocess.run(
        [binary], check=True, capture_output=True, text=True
    )
    match = re.search(r"^CHECKSUM=(\d+)$", result.stdout, re.MULTILINE)
    if not match:
        raise SystemExit("Binary did not print a checksum: " + binary)
    return match.group(1)


def text_hash(binary, objcopy, temp_dir):
    text = os.path.join(temp_dir, f"text_{threading.get_ident()}.bin")
    try:
        command(
            [objcopy, "-O", "binary", "-j", ".text", binary, text],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        with open(text, "rb") as stream:
            return hashlib.sha256(stream.read()).hexdigest()
    except Exception:
        # If objcopy fails (e.g., debug directory not found on Windows),
        # use a hash based on the binary's existence timestamp as fallback
        # This still allows deduplication to work, just may trace slightly more binaries
        import time

        return hashlib.sha256(
            f"{binary}:{os.path.getmtime(binary)}".encode()
        ).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--repo", required=True, help="ECE-752 repository root"
    )
    parser.add_argument(
        "--champsim-root", required=True, help="ChampSim checkout"
    )
    parser.add_argument("--pin", required=True, help="Intel PIN root")
    parser.add_argument(
        "--tracer",
        required=True,
        help="Windows .dll or Linux .so ChampSim tracer",
    )
    parser.add_argument(
        "--out", required=True, help="Dedicated results directory"
    )
    parser.add_argument("--kernel", default="k_stencil")
    parser.add_argument(
        "--nflags",
        type=int,
        default=None,
        help="number of leading gem5 flags to use (default: 12)",
    )
    parser.add_argument(
        "--flags",
        nargs="+",
        default=None,
        help="explicit compiler optimization flags; overrides the default",
    )
    parser.add_argument(
        "--compiler",
        choices=("wsl-mingw", "clang"),
        default="wsl-mingw",
        help="Compiler backend; wsl-mingw preserves the GCC flag space",
    )
    parser.add_argument(
        "--jobs",
        "-j",
        type=int,
        default=8,
        help="Parallel compilation jobs for flag combinations (default: 8)",
    )
    parser.add_argument("--trace-instructions", type=int, default=2_000_000)
    parser.add_argument(
        "--trace-skip-instructions",
        type=int,
        default=0,
        help="PIN instructions to skip before tracing starts",
    )
    parser.add_argument("--warmup-instructions", type=int, default=100_000)
    parser.add_argument(
        "--determinism",
        type=int,
        default=0,
        help="Repeat this many measured traces and require identical cycles",
    )
    parser.add_argument(
        "--strict-o3",
        action="store_true",
        help="fail if plain Clang -O3 has no exact explicit configuration match",
    )
    parser.add_argument(
        "--force-trace",
        action="store_true",
        help="regenerate every trace and simulation, even when cached results exist",
    )
    args = parser.parse_args()
    try:
        flags = select(args.nflags, args.flags)
    except ValueError as error:
        raise SystemExit(str(error))

    repo = os.path.abspath(args.repo)
    if repo not in sys.path:
        sys.path.insert(0, repo)
    from flagbench.tuners import (
        LUT,
        carbs,
    )

    champsim = os.path.abspath(args.champsim_root)
    pin = os.path.abspath(args.pin)
    tracer = os.path.abspath(args.tracer)
    out = os.path.abspath(args.out)
    source = os.path.join(repo, "flagbench", "kernels", args.kernel + ".c")
    os.makedirs(os.path.join(out, "binaries"), exist_ok=True)
    os.makedirs(os.path.join(out, "traces"), exist_ok=True)
    os.makedirs(os.path.join(out, "simulations"), exist_ok=True)
    os.makedirs(os.path.join(out, "compile-logs"), exist_ok=True)
    os.makedirs(os.path.join(out, "trace-logs"), exist_ok=True)
    hash_dir = tempfile.mkdtemp(prefix="champsim_hash_")

    llvm_candidates = [
        r"C:\Program Files\LLVM\bin",
        r"C:\Program Files (x86)\LLVM\bin",
    ]
    for cand in llvm_candidates:
        if os.path.isdir(cand) and cand not in os.environ.get("PATH", ""):
            os.environ["PATH"] = cand + os.pathsep + os.environ.get("PATH", "")
    runtime_bin = os.path.join(champsim, "bin")
    if os.path.isdir(runtime_bin) and runtime_bin not in os.environ.get(
        "PATH", ""
    ):
        os.environ["PATH"] = (
            runtime_bin + os.pathsep + os.environ.get("PATH", "")
        )

    compiler = shutil.which("clang") if args.compiler == "clang" else None
    if args.compiler == "wsl-mingw":
        from wsl_mingw import available
        from wsl_mingw import run as compiler_run

        if not available():
            raise SystemExit(
                "WSL MinGW GCC is required for the equivalent GCC flag space. "
                "Install it with: wsl.exe sudo apt-get install mingw-w64"
            )
        compiler = "x86_64-w64-mingw32-gcc"
    else:
        compiler_run = subprocess.run
    objcopy = shutil.which("objcopy") or shutil.which("llvm-objcopy")
    pin_bin = executable(pin, "pin")
    simulator = executable(os.path.join(champsim, "bin"), "champsim")
    for path in (source, tracer, pin_bin, simulator):
        if not os.path.exists(path):
            raise SystemExit("Required path does not exist: " + path)
    if not compiler or not objcopy:
        raise SystemExit(
            "The selected compiler and llvm-objcopy must be available"
        )

    def compile_binary(output, optimization, extra_flags, log_path):
        compile_args = [
            compiler,
            optimization,
            "-march=x86-64",
            "-DNO_M5OPS",
            "-static",
            "-I",
            os.path.join(repo, "include"),
            *extra_flags,
            source,
            "-o",
            output,
        ]
        with open(log_path, "w") as log:
            return compiler_run(
                compile_args, text=True, stdout=log, stderr=log
            )

    reference_source = os.path.join(
        repo, "flagbench", "kernels", args.kernel + ".c"
    )
    reference_binary = os.path.join(
        out, "reference.exe" if os.name == "nt" else "reference"
    )
    reference_log = os.path.join(out, "compile-logs", "reference.log")
    with open(reference_log, "w") as log:
        result = compiler_run(
            [
                compiler,
                "-O0",
                "-DNO_M5OPS",
                "-I",
                os.path.join(repo, "include"),
                "-static",
                reference_source,
                "-o",
                reference_binary,
            ],
            text=True,
            stdout=log,
            stderr=log,
        )
    if result.returncode:
        raise SystemExit("Reference compilation failed; see " + reference_log)
    reference_checksum = checksum(reference_binary)
    o3_binary = os.path.join(
        out, "plain-o3.exe" if os.name == "nt" else "plain-o3"
    )
    o3_log = os.path.join(out, "compile-logs", "plain-o3.log")
    result = compile_binary(o3_binary, "-O3", [], o3_log)
    if result.returncode:
        raise SystemExit("Plain -O3 compilation failed; see " + o3_log)
    o3_hash = text_hash(o3_binary, objcopy, hash_dir)

    total_configs = 1 << len(flags)
    print(
        f"=== Compiling {total_configs} flag configurations ({args.jobs} jobs) ===",
        flush=True,
    )

    def compile_one(cid):
        bname = str(cid) + (".exe" if os.name == "nt" else "")
        bpath = os.path.join(out, "binaries", bname)
        if os.path.isfile(bpath) and os.path.getsize(bpath) > 0:
            return cid
        cswitches = make_config_flags(cid, flags)
        clog = os.path.join(out, "compile-logs", f"compile-{cid}.log")
        res = compile_binary(bpath, "-O3", cswitches, clog)
        if res.returncode:
            print(
                f"  WARNING: Compilation failed for config {cid}; see {clog}",
                flush=True,
            )
            return None
        return cid

    compiled_configs = []
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {
            executor.submit(compile_one, cid): cid
            for cid in range(total_configs)
        }
        done_count = 0
        for f in as_completed(futures):
            result = f.result()
            done_count += 1
            if result is not None:
                compiled_configs.append(result)
            if done_count % 256 == 0 or done_count == total_configs:
                print(
                    f"  Compiled {done_count}/{total_configs} configurations... ({len(compiled_configs)} ok)",
                    flush=True,
                )

    print(
        f"=== Hashing {total_configs} binaries and finding unique variants ===",
        flush=True,
    )
    config_info = []
    unique_binaries = {}  # binary_hash -> (config_id, binary_path)

    for config_id in compiled_configs:
        binary_name = str(config_id) + (".exe" if os.name == "nt" else "")
        binary = os.path.join(out, "binaries", binary_name)
        if not os.path.isfile(binary) or os.path.getsize(binary) == 0:
            print(
                f"  WARNING: Binary missing for config {config_id}; skipping",
                flush=True,
            )
            continue
        binary_hash = text_hash(binary, objcopy, hash_dir)
        config_info.append((config_id, binary, binary_hash))
        if binary_hash not in unique_binaries:
            unique_binaries[binary_hash] = (config_id, binary)

    total_unique = len(unique_binaries)
    print(
        f"  Found {total_unique} unique binaries out of {total_configs} configurations.",
        flush=True,
    )
    print(
        f"=== Simulating {total_unique} unique binaries in parallel ({args.jobs} workers) ===",
        flush=True,
    )

    def simulate_one_unique(item):
        b_hash, (cid, b_path) = item
        sim_dir = os.path.join(out, "simulations", str(cid))
        os.makedirs(sim_dir, exist_ok=True)
        sim_log = os.path.join(sim_dir, "stdout.txt")
        trace = os.path.join(out, "traces", f"{cid}.champsim")
        has_valid_stats = False

        if not os.path.isfile(b_path):
            print(
                f"  WARNING: Binary missing for config {cid}; skipping simulation",
                flush=True,
            )
            return None

        # Resume results only when their trace format matches this Windows run.
        if os.path.isfile(sim_log):
            output = open(sim_log).read()
            cyc = stat(output, "cycles")
            ins = stat(output, "instructions")
            ipc_val = stat(output, "cumulative IPC")
            has_valid_stats = (
                cyc is not None and ins is not None and ipc_val is not None
            )
            trace_is_valid = (
                os.path.isfile(trace)
                and os.path.getsize(trace) > 0
                and os.path.getsize(trace) % 64 == 0
            )
            trace_is_current = trace_is_valid and trace_is_normalized(trace)
            if has_valid_stats and trace_is_current and not args.force_trace:
                return b_hash, cyc, ins, ipc_val

        trace_log = os.path.join(out, "trace-logs", f"trace-{cid}.log")

        existing_trace_is_current = (
            os.path.isfile(trace)
            and os.path.getsize(trace) > 0
            and os.path.getsize(trace) % 64 == 0
            and trace_is_normalized(trace)
        )
        valid_trace = (
            os.path.isfile(trace)
            and os.path.getsize(trace) > 0
            and os.path.getsize(trace) % 64 == 0
        )
        if (
            args.force_trace
            or not existing_trace_is_current
            or not valid_trace
        ):
            if not os.path.isfile(b_path):
                print(
                    f"  WARNING: Binary missing for config {cid}; skipping trace",
                    flush=True,
                )
                return None
            trace_attempts = 0
            while trace_attempts < 5:
                with open(trace_log, "w") as log:
                    result = subprocess.run(
                        [
                            pin_bin,
                            "-t",
                            tracer,
                            "-o",
                            trace,
                            "-s",
                            str(args.trace_skip_instructions),
                            "-t",
                            str(args.trace_instructions),
                            "--",
                            b_path,
                        ],
                        text=True,
                        stdout=log,
                        stderr=log,
                    )
                if result.returncode == 0:
                    break
                trace_attempts += 1
                if trace_attempts < 5:
                    print(
                        f"  WARNING: Tracing failed for config {cid} (attempt {trace_attempts}/5); retrying in 2s",
                        flush=True,
                    )
                    time.sleep(2)
            if trace_attempts >= 5:
                print(
                    f"  WARNING: Tracing failed for config {cid} after 5 attempts; skipping",
                    flush=True,
                )
                return None
            trace_size = os.path.getsize(trace)
            if trace_size == 0 or trace_size % 64:
                print(
                    f"  WARNING: Invalid trace for config {cid}; skipping",
                    flush=True,
                )
                return None

        if not os.path.isfile(b_path):
            print(
                f"  WARNING: Binary missing for config {cid}; skipping simulation",
                flush=True,
            )
            return None

        canonicalized = canonicalize_windows_trace(trace)
        if canonicalized:
            print(
                f"  Canonicalized {canonicalized} Windows allocator addresses "
                f"in config {cid}'s trace.",
                flush=True,
            )

        for attempt in range(5):
            if not os.path.isfile(b_path):
                print(
                    f"  WARNING: Binary missing for config {cid}; skipping simulation",
                    flush=True,
                )
                return None
            with open(sim_log, "w") as log:
                result = subprocess.run(
                    [
                        simulator,
                        "--warmup-instructions",
                        str(args.warmup_instructions),
                        "--simulation-instructions",
                        str(args.trace_instructions),
                        trace,
                    ],
                    text=True,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                )
            if result.returncode == 0:
                break
            if attempt == 4:
                print(
                    f"  WARNING: ChampSim failed for config {cid} after 5 attempts; skipping",
                    flush=True,
                )
                return None

            # ASLR can place the Windows heap/stack in address ranges that
            # trigger ChampSim's legacy translation-path fault. A new PIN
            # process gets a new layout, while retaining the same binary and
            # instruction count.
            if not os.path.isfile(b_path):
                print(
                    f"  WARNING: Binary missing for config {cid}; skipping trace retry",
                    flush=True,
                )
                return None
            with open(trace_log, "w") as log:
                result = subprocess.run(
                    [
                        pin_bin,
                        "-t",
                        tracer,
                        "-o",
                        trace,
                        "-s",
                        str(args.trace_skip_instructions),
                        "-t",
                        str(args.trace_instructions),
                        "--",
                        b_path,
                    ],
                    text=True,
                    stdout=log,
                    stderr=log,
                )
            if result.returncode:
                print(
                    f"  WARNING: Tracing retry failed for config {cid}; skipping",
                    flush=True,
                )
                return None
            canonicalize_windows_trace(trace)
        output = open(sim_log).read()
        cyc = stat(output, "cycles")
        ins = stat(output, "instructions")
        ipc_val = stat(output, "cumulative IPC")
        if cyc is None or ins is None or ipc_val is None:
            print(
                f"  WARNING: Could not parse stats for config {cid}; skipping",
                flush=True,
            )
            return None
        return b_hash, cyc, ins, ipc_val

    results_by_hash = {}
    completed_sims = 0
    skipped_sims = 0
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {
            executor.submit(simulate_one_unique, item): item
            for item in unique_binaries.items()
        }
        for f in as_completed(futures):
            result = f.result()
            if result is None:
                skipped_sims += 1
                continue
            b_hash, cyc, ins, ipc_val = result
            results_by_hash[b_hash] = (cyc, ins, ipc_val)
            completed_sims += 1
            if completed_sims % 25 == 0 or completed_sims == total_unique:
                print(
                    f"  [{completed_sims}/{total_unique}] Finished simulations... (latest: cycles={cyc:.0f}, IPC={ipc_val:.3f})",
                    flush=True,
                )
    if skipped_sims:
        print(
            f"  Skipped {skipped_sims} simulations due to missing binaries or failures",
            flush=True,
        )

    print("=== Assembling full LUT database ===", flush=True)
    records = []
    by_hash = {}
    skipped_configs = 0
    for config_id, binary, binary_hash in config_info:
        if binary_hash not in results_by_hash:
            print(
                f"  WARNING: No simulation result for config {config_id}; skipping",
                flush=True,
            )
            skipped_configs += 1
            continue
        if not os.path.isfile(binary):
            print(
                f"  WARNING: Binary missing for config {config_id}; skipping checksum",
                flush=True,
            )
            skipped_configs += 1
            continue
        try:
            current_checksum = checksum(binary)
        except SystemExit:
            print(
                f"  WARNING: Checksum failed for config {config_id}; marking incorrect",
                flush=True,
            )
            current_checksum = None
        cycles, instructions, ipc = results_by_hash[binary_hash]
        record = {
            "config_id": config_id,
            "binary_hash": binary_hash,
            "compiled_ok": 1,
            "correct": (
                int(current_checksum == reference_checksum)
                if current_checksum
                else 0
            ),
            "cycles": cycles,
            "instructions": instructions,
            "ipc": ipc,
        }
        records.append(record)
        by_hash[binary_hash] = record
    if skipped_configs:
        print(
            f"  Skipped {skipped_configs} configs without simulation results",
            flush=True,
        )

    try:
        shutil.rmtree(hash_dir, ignore_errors=True)
    except Exception:
        pass

    if args.determinism:
        checks = list(by_hash.values())[: args.determinism]
        for record in checks:
            trace = os.path.join(
                out, "traces", f"{record['config_id']}.champsim"
            )
            if not os.path.exists(trace):
                continue
            result = subprocess.run(
                [
                    simulator,
                    "--warmup-instructions",
                    str(args.warmup_instructions),
                    "--simulation-instructions",
                    str(args.trace_instructions),
                    trace,
                ],
                capture_output=True,
                text=True,
            )
            rerun = stat(result.stdout, "cycles")
            if result.returncode or rerun != record["cycles"]:
                raise SystemExit(
                    "DETERMINISM CHECK FAILED for " + record["binary_hash"]
                )

    o3_matches = [
        record["config_id"]
        for record in records
        if record["binary_hash"] == o3_hash
    ]
    o3_config_id = o3_matches[0] if o3_matches else None
    o3_identity_verified = o3_config_id is not None
    if o3_config_id is None:
        if args.strict_o3:
            raise SystemExit(
                "Plain -O3 does not match any explicit flag configuration; "
                "choose a flag space that represents Clang's -O3 defaults"
            )
        print(
            "WARNING: plain Clang -O3 has no exact explicit configuration match; using config 0 as baseline"
        )
        o3_config_id = 0
    o3_defaults = {
        name: bool(o3_config_id is not None and o3_config_id & (1 << bit))
        for bit, name in enumerate(flags)
    }
    lut_base = os.path.join(out, "champsim")
    with open(lut_base + ".csv", "w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(
            [
                "config_id",
                *(f"f_{name}" for name in flags),
                "binary_hash",
                "compiled_ok",
                "correct",
                "cycles",
                "instructions",
                "ipc",
            ]
        )
        for record in records:
            cid = record["config_id"]
            writer.writerow(
                [
                    cid,
                    *(
                        int(bool(cid & (1 << bit)))
                        for bit in range(len(flags))
                    ),
                    record["binary_hash"],
                    1,
                    record["correct"],
                    record["cycles"],
                    record["instructions"],
                    record["ipc"],
                ]
            )
    with open(lut_base + ".json", "w") as stream:
        json.dump(
            {
                "flags": list(flags),
                "N": len(flags),
                "o3_config_id": o3_config_id,
                "o3_defaults": o3_defaults,
                "o3_hash": o3_hash,
                "o3_identity_verified": o3_identity_verified,
                "unique_binaries": len(by_hash),
                "isa": "X86",
                "simulator": "ChampSim",
                "kernel": args.kernel,
            },
            stream,
            indent=2,
        )
    shutil.copyfile(lut_base + ".csv", os.path.join(out, args.kernel + ".csv"))
    shutil.copyfile(
        lut_base + ".json", os.path.join(out, args.kernel + ".json")
    )

    lut = LUT(lut_base + ".csv")
    trace, probabilities = carbs(
        lut, budget=len(records), seed=0, return_state=True
    )
    with open(os.path.join(out, "carbs.json"), "w") as stream:
        json.dump(
            {
                "trace_best_cycles": trace,
                "final_probabilities": probabilities.tolist(),
                "evaluations": lut.evals,
                "optimum_cycles": lut.optimum(),
                "o3_cycles": lut.o3_cycles(),
            },
            stream,
            indent=2,
        )
    print(json.dumps({"results": out, "configs": len(records)}))


if __name__ == "__main__":
    main()
