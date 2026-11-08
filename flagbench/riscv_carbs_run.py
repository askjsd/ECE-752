#!/usr/bin/env python3
"""Build, simulate, and run CARBS on a small RISC-V FlagBench experiment."""

import argparse
import csv
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile

from tuners import LUT, carbs


FLAGS = ("unroll-loops", "peel-loops")


def run(cmd, **kwargs):
    """Run a command and fail immediately if it does not succeed."""
    return subprocess.run(cmd, check=True, text=True, **kwargs)


def read_stat(stats, name):
    """Read one numeric gem5 statistic from a stats.txt file."""
    match = re.search(
        r"^" + re.escape(name) + r"\s+([0-9.eE+-]+)", stats, re.MULTILINE
    )
    return float(match.group(1)) if match else None


def main():
    # Keep all generated binaries, logs, LUT data, and CARBS output together.
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True, help="Linux-native ECE-752 checkout")
    ap.add_argument("--out", required=True, help="Dedicated results directory")
    ap.add_argument("--kernel", default="k_stencil")
    args = ap.parse_args()

    repo = os.path.abspath(args.repo)
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    bins = os.path.join(out, "binaries")
    sims = os.path.join(out, "simulations")
    os.makedirs(bins, exist_ok=True)
    os.makedirs(sims, exist_ok=True)

    source = os.path.join(repo, "flagbench", "kernels", args.kernel + ".c")
    include = os.path.join(repo, "include")
    m5ops = os.path.join(repo, "util", "m5", "src", "abi", "riscv", "m5op.S")
    config = os.path.join(repo, "flagbench", "se_riscv_ooo.py")
    gem5 = os.path.join(repo, "build", "RISCV", "gem5.opt")
    compiler = shutil.which("riscv64-linux-gnu-gcc")
    objcopy = shutil.which("riscv64-linux-gnu-objcopy")
    if not compiler or not objcopy:
        raise SystemExit("Install gcc-riscv64-linux-gnu and binutils-riscv64-linux-gnu first")

    records = []
    reference_checksum = None
    for config_id in range(1 << len(FLAGS)):
        # Compile one flag configuration and hash its text section for identity.
        work = tempfile.mkdtemp(prefix="compile-", dir=out)
        binary = os.path.join(bins, str(config_id))
        flags = [f"-f{name}" if config_id & (1 << bit) else f"-fno-{name}"
                 for bit, name in enumerate(FLAGS)]
        compile_cmd = [
            compiler, "-static", "-O3", "-march=rv64gc", "-mabi=lp64d",
            *flags, "-I" + include, source, m5ops, "-o", binary,
        ]
        compile_log = os.path.join(out, "compile-" + str(config_id) + ".log")
        with open(compile_log, "w") as log:
            result = subprocess.run(compile_cmd, text=True, stdout=log, stderr=log)
        if result.returncode:
            shutil.rmtree(work, ignore_errors=True)
            raise SystemExit("RISC-V compilation failed; see " + compile_log)

        text_bin = os.path.join(work, "text.bin")
        run([objcopy, "-O", "binary", "-j", ".text", binary, text_bin],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        binary_hash = hashlib.sha256(open(text_bin, "rb").read()).hexdigest()
        sim_dir = os.path.join(sims, str(config_id))
        os.makedirs(sim_dir, exist_ok=True)
        stdout_path = os.path.join(sim_dir, "stdout.txt")
        stderr_path = os.path.join(sim_dir, "stderr.txt")
        with open(stdout_path, "w") as stdout, open(stderr_path, "w") as stderr:
            result = subprocess.run(
                [gem5, "-d", sim_dir, config, binary],
                text=True, stdout=stdout, stderr=stderr,
            )
        if result.returncode:
            raise SystemExit("gem5 failed; see " + stderr_path)
        stdout_text = open(stdout_path).read()
        checksum_match = re.search(r"^CHECKSUM=(\d+)$", stdout_text, re.MULTILINE)
        if not checksum_match:
            raise SystemExit("Missing checksum; see " + stdout_path)
        checksum = checksum_match.group(1)
        if reference_checksum is None:
            reference_checksum = checksum
        # Extract the measurements CARBS will use from gem5's statistics.
        stats = open(os.path.join(sim_dir, "stats.txt")).read()
        cycles = read_stat(stats, "system.cpu.numCycles")
        instructions = read_stat(stats, "system.cpu.commitStats0.numInsts")
        ipc = read_stat(stats, "system.cpu.ipc")
        if cycles is None or instructions is None:
            raise SystemExit("Required RISCV stats missing; see " + os.path.join(sim_dir, "stats.txt"))
        records.append({
            "config_id": config_id, "binary_hash": binary_hash,
            "compiled_ok": 1, "correct": int(checksum == reference_checksum),
            "cycles": cycles, "instructions": instructions, "ipc": ipc,
            "checksum": checksum,
        })
        shutil.rmtree(work, ignore_errors=True)

    # The LUT is the bridge between expensive gem5 runs and offline CARBS replay.
    lut_base = os.path.join(out, "riscv")
    with open(lut_base + ".csv", "w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["config_id", *(f"f_{name}" for name in FLAGS),
                         "binary_hash", "compiled_ok", "correct", "cycles",
                         "instructions", "ipc"])
        for record in records:
            cid = record["config_id"]
            writer.writerow([cid, *(int(bool(cid & (1 << bit))) for bit in range(len(FLAGS))),
                             record["binary_hash"], 1, record["correct"],
                             record["cycles"], record["instructions"], record["ipc"]])
    with open(lut_base + ".json", "w") as stream:
        json.dump({"flags": list(FLAGS), "N": len(FLAGS), "o3_config_id": 0,
                   "reference_checksum": reference_checksum,
                   "isa": "RISCV", "kernel": args.kernel}, stream, indent=2)

    # CARBS searches the measured LUT; it does not launch gem5 again here.
    lut = LUT(lut_base + ".csv")
    trace, probabilities = carbs(lut, budget=len(records), seed=0, return_state=True)
    with open(os.path.join(out, "carbs.json"), "w") as stream:
        json.dump({"trace_best_cycles": trace, "final_probabilities": probabilities.tolist(),
                   "evaluations": lut.evals, "optimum_cycles": lut.optimum(),
                   "o3_cycles": lut.o3_cycles()}, stream, indent=2)
    print(json.dumps({"results": out, "configs": len(records), "carbs_evaluations": lut.evals}))


if __name__ == "__main__":
    main()
