#!/usr/bin/env python3
"""Stage 2: simulate each UNIQUE binary (by code hash) once in gem5 and record region-of-interest stats.

For each kernel: reads <work>/<kernel>/build.jsonl, runs gem5 on <work>/<kernel>/bins/<hash>, parses the
FIRST stats block of stats.txt (m5_work_begin resets stats, m5_work_end dumps them => the ROI), and the
program's printed checksum. Results go to <work>/<kernel>/sim.jsonl (resumable); the raw ROI stats block
is kept gzipped in <work>/<kernel>/roi_stats/<hash>.txt.gz for audit.
Also builds the -O0 reference checksum (native run), and --determinism K re-runs K random binaries.
"""
import argparse, gzip, json, os, random, re, shutil, subprocess, sys, tempfile
from concurrent.futures import ProcessPoolExecutor
import flags as F

CYC = "board.processor.cores.core.numCycles"
INS = "board.processor.cores.core.commitStats0.numInsts"
IPC = "board.processor.cores.core.ipc"


def parse_roi(stats_path):
    """Return (dict of ROI stats, raw first-block text). First block == region of interest."""
    block, n = [], 0
    for line in open(stats_path):
        if "Begin Simulation Statistics" in line:
            n += 1
            continue
        if "End Simulation Statistics" in line:
            if n >= 1:
                break
            continue
        if n == 1:
            block.append(line)
    vals = {}
    for line in block:
        parts = line.split()
        if len(parts) >= 2 and parts[0] in (CYC, INS, IPC):
            vals[parts[0]] = parts[1]
    return vals, "".join(block)


def run_gem5(binary, outdir, timeout):
    os.makedirs(outdir, exist_ok=True)
    cmd = [F.GEM5_BIN, "-d", outdir, F.SIM_CONFIG, binary]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    m = re.search(r"^CHECKSUM=(\d+)\s*$", r.stdout, re.M)
    return r, (m.group(1) if m else None)


def sim_one(args):
    kernel, h, work, timeout = args
    kd = F.work_dir(work, kernel)
    binary = os.path.join(kd, "bins", h)
    out = tempfile.mkdtemp(prefix="g5_", dir=os.path.join(kd, "tmp"))
    rec = {"binary_hash": h, "sim_ok": False, "cycles": None, "instructions": None, "ipc": None,
           "checksum": None, "error": ""}
    try:
        try:
            r, chk = run_gem5(binary, out, timeout)
        except subprocess.TimeoutExpired:
            rec["error"] = "timeout"
            return kernel, rec
        sp = os.path.join(out, "stats.txt")
        if r.returncode != 0 or not os.path.exists(sp):
            rec["error"] = f"gem5 rc={r.returncode}: " + (r.stderr or r.stdout)[-300:]
            return kernel, rec
        vals, raw = parse_roi(sp)
        if CYC not in vals or INS not in vals:
            rec["error"] = "ROI stats missing (m5_work_begin/end not reached?)"
            return kernel, rec
        rec.update(sim_ok=True, cycles=int(float(vals[CYC])), instructions=int(float(vals[INS])),
                   ipc=float(vals.get(IPC, "nan")), checksum=chk)
        os.makedirs(os.path.join(kd, "roi_stats"), exist_ok=True)
        with gzip.open(os.path.join(kd, "roi_stats", h + ".txt.gz"), "wt") as f:
            f.write(raw)
        return kernel, rec
    finally:
        shutil.rmtree(out, ignore_errors=True)


def build_reference(kernel, work):
    """-O0 native reference checksum (no m5ops)."""
    kd = F.work_dir(work, kernel)
    path = os.path.join(kd, "reference.json")
    if os.path.exists(path):
        return json.load(open(path))["checksum"]
    exe = os.path.join(kd, "tmp", "ref.out")
    r = subprocess.run([F.CC, "-O0", "-DNO_M5OPS", f"-I{F.M5_INC}", os.path.join(F.KERNEL_DIR, kernel + ".c"),
                        "-o", exe], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f"reference build failed for {kernel}: {r.stderr}")
    out = subprocess.run([exe], capture_output=True, text=True).stdout
    m = re.search(r"CHECKSUM=(\d+)", out)
    os.remove(exe)
    json.dump({"checksum": m.group(1), "build": "gcc -O0 native, -DNO_M5OPS"}, open(path, "w"))
    return m.group(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--work", default=os.path.join(F.ROOT, "work"))
    ap.add_argument("--kernels", nargs="+", default=F.KERNELS)
    ap.add_argument("-j", "--workers", type=int, default=os.cpu_count())
    ap.add_argument("--timeout", type=int, default=1800, help="per-simulation timeout (s)")
    ap.add_argument("--determinism", type=int, default=5, help="re-run K random binaries per kernel")
    ap.add_argument("--seed", type=int, default=0)
    a = ap.parse_args()

    jobs = []
    for k in a.kernels:
        kd = F.work_dir(a.work, k)
        os.makedirs(os.path.join(kd, "tmp"), exist_ok=True)
        build_reference(k, a.work)
        hashes = []
        for line in open(os.path.join(kd, "build.jsonl")):
            r = json.loads(line)
            if r["compiled_ok"] and r["binary_hash"] not in hashes:
                hashes.append(r["binary_hash"])
        sj = os.path.join(kd, "sim.jsonl")
        done = set()
        if os.path.exists(sj):
            for line in open(sj):
                try:
                    r = json.loads(line)
                    if r["sim_ok"]: done.add(r["binary_hash"])   # failed sims are retried
                except Exception: pass
        todo = [h for h in hashes if h not in done]
        print(f"{k}: {len(hashes)} unique binaries, {len(done)} simulated, {len(todo)} to run", flush=True)
        jobs += [(k, h, a.work, a.timeout) for h in todo]

    outs = {k: open(os.path.join(F.work_dir(a.work, k), "sim.jsonl"), "a") for k in a.kernels}
    cnt = 0
    with ProcessPoolExecutor(a.workers) as ex:
        for k, rec in ex.map(sim_one, jobs):
            outs[k].write(json.dumps(rec) + "\n"); outs[k].flush()
            cnt += 1
            if cnt % 50 == 0: print(f"  simulated {cnt}/{len(jobs)}", flush=True)
    for o in outs.values(): o.close()

    # determinism check: re-run K random already-simulated binaries, require identical cycles
    rng = random.Random(a.seed)
    ok_all = True
    for k in a.kernels:
        kd = F.work_dir(a.work, k)
        sims = {}
        for line in open(os.path.join(kd, "sim.jsonl")):
            r = json.loads(line)
            if r["sim_ok"]: sims[r["binary_hash"]] = r
        pick = rng.sample(sorted(sims), min(a.determinism, len(sims)))
        res = []
        with ProcessPoolExecutor(a.workers) as ex:
            for _, rec in ex.map(sim_one, [(k, h, a.work, a.timeout) for h in pick]):
                res.append({"binary_hash": rec["binary_hash"], "first": sims[rec["binary_hash"]]["cycles"],
                            "rerun": rec["cycles"], "identical": rec["cycles"] == sims[rec["binary_hash"]]["cycles"]})
        json.dump(res, open(os.path.join(kd, "determinism.json"), "w"), indent=1)
        good = all(x["identical"] for x in res)
        ok_all &= good
        print(f"{k}: determinism {sum(x['identical'] for x in res)}/{len(res)} identical")
    if not ok_all:
        sys.exit("DETERMINISM CHECK FAILED - do not trust the LUT")


if __name__ == "__main__":
    main()
