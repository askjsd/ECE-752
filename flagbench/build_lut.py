#!/usr/bin/env python3
"""Stage 3: join build.jsonl + sim.jsonl + reference checksum into per-kernel LUT CSV + metadata JSON.

CSV columns: config_id, <one 0/1 column per flag>, binary_hash, compiled_ok, correct, cycles,
instructions, ipc. cycles/instructions/ipc are empty unless the binary was simulated successfully.
`correct` = simulated checksum == -O0 reference checksum. Nothing is imputed.
"""
import argparse, csv, json, os, subprocess, sys
import flags as F


def gem5_info():
    info = {"binary": F.GEM5_BIN}
    try:
        info["binary_sha256"] = subprocess.run(["sha256sum", F.GEM5_BIN], capture_output=True, text=True).stdout.split()[0]
        info["binary_mtime"] = os.path.getmtime(F.GEM5_BIN)
        info["repo_head"] = subprocess.run(["git", "-C", F.GEM5_ROOT, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
        info["commit_at_binary_build"] = subprocess.run(
            ["git", "-C", F.GEM5_ROOT, "log", "-1", "--format=%H", f"--before=@{int(info['binary_mtime'])}"],
            capture_output=True, text=True).stdout.strip()
        info["src_dirty"] = bool(subprocess.run(["git", "-C", F.GEM5_ROOT, "status", "--porcelain", "src"], capture_output=True, text=True).stdout.strip())
        v = subprocess.run([F.GEM5_BIN, "--help"], capture_output=True, text=True)
        info["banner"] = [l for l in (v.stdout + v.stderr).splitlines() if "gem5" in l.lower() and "version" in l.lower()][:2]
    except Exception as e:
        info["error"] = str(e)
    return info


def plain_o3_hash(kernel):
    """.text hash of a plain `gcc -O3 -march=x86-64` build (no explicit -f flags)."""
    import hashlib, tempfile
    d = tempfile.mkdtemp()
    exe = os.path.join(d, "a.out")
    r = subprocess.run([F.CC, *F.BASE_FLAGS, "-static", f"-I{F.M5_INC}", os.path.join(F.KERNEL_DIR, kernel + ".c"),
                        "-o", exe, f"-L{F.M5_LIB}", "-lm5"], capture_output=True, text=True)
    if r.returncode: raise SystemExit("plain -O3 build failed: " + r.stderr)
    subprocess.run(["objcopy", "-O", "binary", "-j", ".text", exe, exe + ".t"], check=True)
    h = hashlib.sha256(open(exe + ".t", "rb").read()).hexdigest()
    import shutil; shutil.rmtree(d, ignore_errors=True)
    return h


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--work", default=os.path.join(F.ROOT, "work"))
    ap.add_argument("--out", default=os.path.join(F.ROOT, "luts"))
    ap.add_argument("--kernels", nargs="+", default=F.KERNELS)
    a = ap.parse_args()
    fs = json.load(open(os.path.join(a.work, "flagspace.json")))
    names, n = fs["flags"], fs["N"]
    os.makedirs(a.out, exist_ok=True)
    g5 = gem5_info()
    for k in a.kernels:
        kd = F.work_dir(a.work, k)
        builds = {}
        for line in open(os.path.join(kd, "build.jsonl")):
            r = json.loads(line); builds[r["config_id"]] = r
        sims = {}
        for line in open(os.path.join(kd, "sim.jsonl")):
            r = json.loads(line)
            if r["sim_ok"]: sims[r["binary_hash"]] = r
        ref = json.load(open(os.path.join(kd, "reference.json")))["checksum"]
        miss = [c for c in range(1 << n) if c not in builds]
        if miss:
            sys.exit(f"{k}: {len(miss)} configs not built yet; run build_and_hash.py")
        nsim_missing = {b["binary_hash"] for b in builds.values() if b["compiled_ok"]} - set(sims)
        if nsim_missing:
            print(f"WARNING {k}: {len(nsim_missing)} unique binaries not simulated (rows left empty)")
        with open(os.path.join(a.out, k + ".csv"), "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["config_id", *[f"f_{x}" for x in names], "binary_hash", "compiled_ok", "correct",
                        "cycles", "instructions", "ipc"])
            for c in range(1 << n):
                b = builds[c]; s = sims.get(b["binary_hash"]) if b["compiled_ok"] else None
                correct = (s is not None and s["checksum"] == ref)
                w.writerow([c, *[(c >> j) & 1 for j in range(n)], b["binary_hash"], int(b["compiled_ok"]),
                            int(correct) if s is not None else 0,
                            s["cycles"] if s else "", s["instructions"] if s else "",
                            f'{s["ipc"]:.6f}' if s else ""])
        plain = plain_o3_hash(k)
        o3_ok = builds[fs["o3_config_id"]]["binary_hash"] == plain
        if not o3_ok:
            sys.exit(f"{k}: recorded -O3 config (id {fs['o3_config_id']}) is NOT identical to plain gcc -O3 "
                     f"(check_o3_defaults.py / flags.O3_DEFAULT_OVERRIDES)")
        uniq = len({b["binary_hash"] for b in builds.values() if b["compiled_ok"]})
        meta = {"kernel": k, **fs, "unique_binaries": uniq, "o3_config_text_identical_to_plain_gcc_O3": o3_ok, "reference_checksum": ref,
                "compile_failures": sum(not b["compiled_ok"] for b in builds.values()),
                "incorrect_binaries": len({b["binary_hash"] for b in builds.values() if b["compiled_ok"]
                                           and b["binary_hash"] in sims and sims[b["binary_hash"]]["checksum"] != ref}),
                "gem5": g5, "gem5_config": "flagbench/sim_config.py (see docstring)",
                "gem5_config_source": open(F.SIM_CONFIG).read(),
                "kernel_source": open(os.path.join(F.KERNEL_DIR, k + ".c")).read()}
        det = os.path.join(kd, "determinism.json")
        if os.path.exists(det): meta["determinism"] = json.load(open(det))
        json.dump(meta, open(os.path.join(a.out, k + ".json"), "w"), indent=1)
        print(f"{k}: wrote {k}.csv ({1<<n} rows, {uniq} unique binaries)")


if __name__ == "__main__":
    main()
