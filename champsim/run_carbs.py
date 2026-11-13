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

from portable_flags import DEFAULT_FLAGS, config_flags as make_config_flags, select


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


def checksum(binary):
    result = subprocess.run([binary], check=True, capture_output=True, text=True)
    match = re.search(r"^CHECKSUM=(\d+)$", result.stdout, re.MULTILINE)
    if not match:
        raise SystemExit("Binary did not print a checksum: " + binary)
    return match.group(1)


def text_hash(binary, objcopy, out):
    with tempfile.TemporaryDirectory(prefix="hash-", dir=out) as work:
        text = os.path.join(work, "text.bin")
        command([objcopy, "-O", "binary", "-j", ".text", binary, text],
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        with open(text, "rb") as stream:
            return hashlib.sha256(stream.read()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True, help="ECE-752 repository root")
    parser.add_argument("--champsim-root", required=True, help="ChampSim checkout")
    parser.add_argument("--pin", required=True, help="Intel PIN root")
    parser.add_argument("--tracer", required=True, help="Windows .dll or Linux .so ChampSim tracer")
    parser.add_argument("--out", required=True, help="Dedicated results directory")
    parser.add_argument("--kernel", default="k_stencil")
    parser.add_argument("--nflags", type=int, default=None,
                        help="number of leading gem5 flags to use (default: 12)")
    parser.add_argument("--flags", nargs="+", default=None,
                        help="explicit compiler optimization flags; overrides the default")
    parser.add_argument("--compiler", choices=("wsl-mingw", "clang"), default="wsl-mingw",
                        help="Compiler backend; wsl-mingw preserves the GCC flag space")
    parser.add_argument("--trace-instructions", type=int, default=2_000_000)
    parser.add_argument("--warmup-instructions", type=int, default=100_000)
    parser.add_argument("--determinism", type=int, default=0,
                        help="Repeat this many measured traces and require identical cycles")
    parser.add_argument("--strict-o3", action="store_true",
                        help="fail if plain Clang -O3 has no exact explicit configuration match")
    args = parser.parse_args()
    try:
        flags = select(args.nflags, args.flags)
    except ValueError as error:
        raise SystemExit(str(error))

    repo = os.path.abspath(args.repo)
    if repo not in sys.path:
        sys.path.insert(0, repo)
    from flagbench.tuners import LUT, carbs

    champsim = os.path.abspath(args.champsim_root)
    pin = os.path.abspath(args.pin)
    tracer = os.path.abspath(args.tracer)
    out = os.path.abspath(args.out)
    source = os.path.join(repo, "flagbench", "kernels", args.kernel + ".c")
    os.makedirs(os.path.join(out, "binaries"), exist_ok=True)
    os.makedirs(os.path.join(out, "traces"), exist_ok=True)
    os.makedirs(os.path.join(out, "simulations"), exist_ok=True)

    compiler = shutil.which("clang") if args.compiler == "clang" else None
    if args.compiler == "wsl-mingw":
        from wsl_mingw import available, run as compiler_run
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
        raise SystemExit("The selected compiler and llvm-objcopy must be available")

    def compile_binary(output, optimization, extra_flags, log_path):
        compile_args = [
            compiler, optimization, "-march=x86-64", "-DNO_M5OPS",
            "-static",
            "-I", os.path.join(repo, "include"), *extra_flags, source, "-o", output,
        ]
        with open(log_path, "w") as log:
            return compiler_run(compile_args, text=True, stdout=log, stderr=log)

    records = []
    reference_source = os.path.join(repo, "flagbench", "kernels", args.kernel + ".c")
    reference_binary = os.path.join(out, "reference.exe" if os.name == "nt" else "reference")
    reference_log = os.path.join(out, "reference.log")
    with open(reference_log, "w") as log:
        result = compiler_run(
            [compiler, "-O0", "-DNO_M5OPS", "-I", os.path.join(repo, "include"),
             "-static", reference_source, "-o", reference_binary],
            text=True, stdout=log, stderr=log,
        )
    if result.returncode:
        raise SystemExit("Reference compilation failed; see " + reference_log)
    reference_checksum = checksum(reference_binary)
    o3_binary = os.path.join(out, "plain-o3.exe" if os.name == "nt" else "plain-o3")
    o3_log = os.path.join(out, "plain-o3.log")
    result = compile_binary(o3_binary, "-O3", [], o3_log)
    if result.returncode:
        raise SystemExit("Plain -O3 compilation failed; see " + o3_log)
    o3_hash = text_hash(o3_binary, objcopy, out)
    records = []
    by_hash = {}
    for config_id in range(1 << len(flags)):
        binary_name = str(config_id) + (".exe" if os.name == "nt" else "")
        binary = os.path.join(out, "binaries", binary_name)
        config_switches = make_config_flags(config_id, flags)
        compile_log = os.path.join(out, f"compile-{config_id}.log")
        result = compile_binary(binary, "-O3", config_switches, compile_log)
        if result.returncode:
            raise SystemExit("Compilation failed; see " + compile_log)

        binary_hash = text_hash(binary, objcopy, out)

        current_checksum = checksum(binary)
        if binary_hash in by_hash:
            previous = by_hash[binary_hash]
            records.append({
                "config_id": config_id, "binary_hash": binary_hash,
                "compiled_ok": 1, "correct": int(current_checksum == reference_checksum),
                "cycles": previous["cycles"], "instructions": previous["instructions"],
                "ipc": previous["ipc"],
            })
            continue
        trace = os.path.join(out, "traces", f"{config_id}.champsim")
        trace_log = os.path.join(out, f"trace-{config_id}.log")
        with open(trace_log, "w") as log:
            result = subprocess.run(
                [pin_bin, "-t", tracer, "-o", trace, "-t", str(args.trace_instructions),
                 "--", binary],
                text=True, stdout=log, stderr=log,
            )
        if result.returncode:
            raise SystemExit("Tracing failed; see " + trace_log)

        sim_dir = os.path.join(out, "simulations", str(config_id))
        os.makedirs(sim_dir, exist_ok=True)
        sim_log = os.path.join(sim_dir, "stdout.txt")
        with open(sim_log, "w") as log:
            result = subprocess.run(
                [simulator, "--warmup-instructions", str(args.warmup_instructions),
                 "--simulation-instructions", str(args.trace_instructions), trace],
                text=True, stdout=log, stderr=subprocess.STDOUT,
            )
        if result.returncode:
            raise SystemExit("ChampSim failed; see " + sim_log)
        output = open(sim_log).read()
        cycles = stat(output, "cycles")
        instructions = stat(output, "instructions")
        ipc = stat(output, "cumulative IPC")
        if cycles is None or instructions is None or ipc is None:
            raise SystemExit("Could not parse ChampSim statistics; see " + sim_log)
        record = {
            "config_id": config_id, "binary_hash": binary_hash,
            "compiled_ok": 1, "correct": int(current_checksum == reference_checksum),
            "cycles": cycles, "instructions": instructions, "ipc": ipc,
        }
        by_hash[binary_hash] = record
        records.append(record)

    if args.determinism:
        checks = list(by_hash.values())[:args.determinism]
        for record in checks:
            trace = os.path.join(out, "traces", f"{record['config_id']}.champsim")
            if not os.path.exists(trace):
                continue
            result = subprocess.run(
                [simulator, "--warmup-instructions", str(args.warmup_instructions),
                 "--simulation-instructions", str(args.trace_instructions), trace],
                capture_output=True, text=True,
            )
            rerun = stat(result.stdout, "cycles")
            if result.returncode or rerun != record["cycles"]:
                raise SystemExit("DETERMINISM CHECK FAILED for " + record["binary_hash"])

    o3_matches = [record["config_id"] for record in records if record["binary_hash"] == o3_hash]
    o3_config_id = o3_matches[0] if o3_matches else None
    o3_identity_verified = o3_config_id is not None
    if o3_config_id is None:
        if args.strict_o3:
            raise SystemExit(
                "Plain -O3 does not match any explicit flag configuration; "
                "choose a flag space that represents Clang's -O3 defaults"
            )
        print("WARNING: plain Clang -O3 has no exact explicit configuration match; using config 0 as baseline")
        o3_config_id = 0
    o3_defaults = {
        name: bool(o3_config_id is not None and o3_config_id & (1 << bit))
        for bit, name in enumerate(flags)
    }
    lut_base = os.path.join(out, "champsim")
    with open(lut_base + ".csv", "w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["config_id", *(f"f_{name}" for name in flags),
                         "binary_hash", "compiled_ok", "correct",
                         "cycles", "instructions", "ipc"])
        for record in records:
            cid = record["config_id"]
            writer.writerow([cid, *(int(bool(cid & (1 << bit))) for bit in range(len(flags))),
                             record["binary_hash"], 1, record["correct"],
                             record["cycles"], record["instructions"], record["ipc"]])
    with open(lut_base + ".json", "w") as stream:
        json.dump({"flags": list(flags), "N": len(flags), "o3_config_id": o3_config_id,
                   "o3_defaults": o3_defaults, "o3_hash": o3_hash,
                   "o3_identity_verified": o3_identity_verified,
                   "unique_binaries": len(by_hash), "isa": "X86",
                   "simulator": "ChampSim", "kernel": args.kernel}, stream, indent=2)
    shutil.copyfile(lut_base + ".csv", os.path.join(out, args.kernel + ".csv"))
    shutil.copyfile(lut_base + ".json", os.path.join(out, args.kernel + ".json"))

    lut = LUT(lut_base + ".csv")
    trace, probabilities = carbs(lut, budget=len(records), seed=0, return_state=True)
    with open(os.path.join(out, "carbs.json"), "w") as stream:
        json.dump({"trace_best_cycles": trace,
                   "final_probabilities": probabilities.tolist(),
                   "evaluations": lut.evals, "optimum_cycles": lut.optimum(),
                   "o3_cycles": lut.o3_cycles()}, stream, indent=2)
    print(json.dumps({"results": out, "configs": len(records)}))


if __name__ == "__main__":
    main()
