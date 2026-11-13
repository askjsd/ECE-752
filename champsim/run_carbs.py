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

FLAGS = ("unroll-loops", "peel-loops")


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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True, help="ECE-752 repository root")
    parser.add_argument("--champsim-root", required=True, help="ChampSim checkout")
    parser.add_argument("--pin", required=True, help="Intel PIN root")
    parser.add_argument("--tracer", required=True, help="Windows .dll or Linux .so ChampSim tracer")
    parser.add_argument("--out", required=True, help="Dedicated results directory")
    parser.add_argument("--kernel", default="k_stencil")
    parser.add_argument("--trace-instructions", type=int, default=2_000_000)
    parser.add_argument("--warmup-instructions", type=int, default=100_000)
    args = parser.parse_args()

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

    compiler = shutil.which("gcc") or shutil.which("clang")
    objcopy = shutil.which("objcopy") or shutil.which("llvm-objcopy")
    pin_bin = executable(pin, "pin")
    simulator = executable(os.path.join(champsim, "bin"), "champsim")
    for path in (source, tracer, pin_bin, simulator):
        if not os.path.exists(path):
            raise SystemExit("Required path does not exist: " + path)
    if not compiler or not objcopy:
        raise SystemExit("gcc and objcopy must be available on PATH")

    records = []
    reference_checksum = None
    for config_id in range(1 << len(FLAGS)):
        binary_name = str(config_id) + (".exe" if os.name == "nt" else "")
        binary = os.path.join(out, "binaries", binary_name)
        flags = [f"-f{name}" if config_id & (1 << bit) else f"-fno-{name}"
                 for bit, name in enumerate(FLAGS)]
        compile_log = os.path.join(out, f"compile-{config_id}.log")
        with open(compile_log, "w") as log:
            result = subprocess.run(
                [compiler, *([] if os.name == "nt" else ["-static"]),
                 "-O3", "-march=x86-64", "-DNO_M5OPS",
                 "-I", os.path.join(repo, "include"),
                 *flags, source, "-o", binary],
                text=True, stdout=log, stderr=log,
            )
        if result.returncode:
            raise SystemExit("Compilation failed; see " + compile_log)

        with tempfile.TemporaryDirectory(prefix="hash-", dir=out) as work:
            text = os.path.join(work, "text.bin")
            command([objcopy, "-O", "binary", "-j", ".text", binary, text],
                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            binary_hash = hashlib.sha256(open(text, "rb").read()).hexdigest()

        current_checksum = checksum(binary)
        reference_checksum = reference_checksum or current_checksum
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
        records.append({
            "config_id": config_id, "binary_hash": binary_hash,
            "compiled_ok": 1, "correct": int(current_checksum == reference_checksum),
            "cycles": cycles, "instructions": instructions, "ipc": ipc,
        })

    lut_base = os.path.join(out, "champsim")
    with open(lut_base + ".csv", "w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["config_id", *(f"f_{name}" for name in FLAGS),
                         "binary_hash", "compiled_ok", "correct",
                         "cycles", "instructions", "ipc"])
        for record in records:
            cid = record["config_id"]
            writer.writerow([cid, *(int(bool(cid & (1 << bit))) for bit in range(len(FLAGS))),
                             record["binary_hash"], 1, record["correct"],
                             record["cycles"], record["instructions"], record["ipc"]])
    with open(lut_base + ".json", "w") as stream:
        json.dump({"flags": list(FLAGS), "N": len(FLAGS), "o3_config_id": 0,
                   "o3_defaults": {name: False for name in FLAGS},
                   "unique_binaries": len(records), "isa": "X86",
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
