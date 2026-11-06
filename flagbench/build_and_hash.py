#!/usr/bin/env python3
"""Stage 1: compile every flag configuration of each kernel, hash the executable code, keep one
binary per unique hash. Resumable: configs already present in <work>/<kernel>/build.jsonl are skipped.

Hash = SHA-256 of the raw .text section (objcopy -O binary -j .text). Static binaries, so the hash
covers kernel + libc + m5ops code; builds with identical code share a hash and are simulated once.
"""
import argparse, hashlib, json, os, shutil, subprocess, sys, tempfile
from concurrent.futures import ProcessPoolExecutor
import flags as F


def build_one(args):
    kernel, config_id, names, work = args
    kdir = F.work_dir(work, kernel)
    tmpd = tempfile.mkdtemp(prefix="fb_", dir=os.path.join(kdir, "tmp"))
    exe = os.path.join(tmpd, "a.out")
    cmd = [F.CC, *F.BASE_FLAGS, *F.config_flags(config_id, names), "-static", f"-I{F.M5_INC}",
           os.path.join(F.KERNEL_DIR, kernel + ".c"), "-o", exe, f"-L{F.M5_LIB}", "-lm5"]
    rec = {"config_id": config_id, "compiled_ok": False, "binary_hash": "", "error": ""}
    try:
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(exe):
            rec["error"] = (r.stderr or "compile failed")[-300:]
            return kernel, rec
        textbin = os.path.join(tmpd, "text.bin")
        r = subprocess.run(["objcopy", "-O", "binary", "-j", ".text", exe, textbin],
                           capture_output=True, text=True)
        if r.returncode != 0:
            rec["error"] = "objcopy: " + r.stderr[-200:]
            return kernel, rec
        h = hashlib.sha256(open(textbin, "rb").read()).hexdigest()
        rec.update(compiled_ok=True, binary_hash=h)
        dst = os.path.join(kdir, "bins", h)
        if not os.path.exists(dst):  # first config with this hash keeps its binary
            try:
                os.rename(exe, dst + f".{os.getpid()}.part")
                os.replace(dst + f".{os.getpid()}.part", dst)
            except OSError:
                pass
        return kernel, rec
    finally:
        shutil.rmtree(tmpd, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--work", default=os.path.join(F.ROOT, "work"))
    ap.add_argument("--kernels", nargs="+", default=F.KERNELS)
    ap.add_argument("-n", "--nflags", type=int, default=12)
    ap.add_argument("--flags", nargs="+", default=None, help="explicit flag names (overrides -n)")
    ap.add_argument("-j", "--workers", type=int, default=os.cpu_count())
    a = ap.parse_args()

    names, defaults = F.select_flags(a.nflags, a.flags)
    n = len(names)
    meta = {"gcc": F.gcc_version(), "flags": names, "o3_defaults": defaults, "base_flags": F.BASE_FLAGS,
            "N": n, "o3_config_id": F.o3_config_id(names, defaults)}
    os.makedirs(a.work, exist_ok=True)
    json.dump(meta, open(os.path.join(a.work, "flagspace.json"), "w"), indent=2)

    jobs = []
    for k in a.kernels:
        kd = F.work_dir(a.work, k)
        for sub in ("bins", "tmp"):
            os.makedirs(os.path.join(kd, sub), exist_ok=True)
        bj = os.path.join(kd, "build.jsonl")
        done = set()
        if os.path.exists(bj):
            for line in open(bj):
                try: done.add(json.loads(line)["config_id"])
                except Exception: pass
        todo = [c for c in range(1 << n) if c not in done]
        print(f"{k}: {len(done)} done, {len(todo)} to build", flush=True)
        jobs += [(k, c, names, a.work) for c in todo]

    outs = {k: open(os.path.join(F.work_dir(a.work, k), "build.jsonl"), "a") for k in a.kernels}
    cnt = 0
    with ProcessPoolExecutor(a.workers) as ex:
        for k, rec in ex.map(build_one, jobs, chunksize=4):
            outs[k].write(json.dumps(rec) + "\n")
            cnt += 1
            if cnt % 500 == 0:
                for o in outs.values(): o.flush()
                print(f"  built {cnt}/{len(jobs)}", flush=True)
    for o in outs.values(): o.close()
    for k in a.kernels:
        recs = [json.loads(l) for l in open(os.path.join(F.work_dir(a.work, k), "build.jsonl"))]
        ok = [r for r in recs if r["compiled_ok"]]
        print(f"{k}: {len(recs)} configs, {len(recs)-len(ok)} compile failures, "
              f"{len({r['binary_hash'] for r in ok})} unique binaries")


if __name__ == "__main__":
    main()
