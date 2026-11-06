#!/usr/bin/env python3
"""One-at-a-time flag screening: for each kernel build plain -O3 and every single-flag flip (relative to the
-O3 default), simulate unique binaries in gem5, print the % change in ROI cycles of each flip.
Used to design kernels in which specific flags matter. Real gem5 runs only.
usage: screen.py kernels/screen/p_unswitch.c ... [-n 12] [-j 22]
"""
import argparse, hashlib, json, os, re, shutil, subprocess, sys, tempfile
from concurrent.futures import ProcessPoolExecutor
import flags as F
import simulate as S


def build(src, config_id, names, tmp):
    exe = os.path.join(tmp, f"{os.path.basename(src)}.{config_id}")
    cmd = [F.CC, *F.BASE_FLAGS, *F.config_flags(config_id, names), "-static", f"-I{F.M5_INC}", src, "-o", exe,
           f"-L{F.M5_LIB}", "-lm5"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode: return None, r.stderr[-200:]
    t = exe + ".text"
    subprocess.run(["objcopy", "-O", "binary", "-j", ".text", exe, t], check=True)
    return (exe, hashlib.sha256(open(t, "rb").read()).hexdigest()), ""


def sim(args):
    exe, h, tmp = args
    out = tempfile.mkdtemp(prefix="g5_", dir=tmp)
    try:
        r, chk = S.run_gem5(exe, out, 1800)
        vals, _ = S.parse_roi(os.path.join(out, "stats.txt"))
        return h, int(float(vals[S.CYC])), int(float(vals[S.INS])), chk
    except Exception as e:
        return h, None, None, None
    finally:
        shutil.rmtree(out, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("srcs", nargs="+"); ap.add_argument("-n", type=int, default=12)
    ap.add_argument("-j", type=int, default=os.cpu_count()); ap.add_argument("--flags", nargs="+", default=None)
    a = ap.parse_args()
    names, defaults = F.select_flags(a.n, a.flags)
    o3 = F.o3_config_id(names, defaults)
    tmp = tempfile.mkdtemp(prefix="screen_", dir="/tmp")
    try:
        jobs, meta = {}, {}
        for src in a.srcs:
            src = os.path.abspath(src)
            for cid in [o3] + [o3 ^ (1 << j) for j in range(len(names))]:
                res, err = build(src, cid, names, tmp)
                if res is None: print("BUILD FAIL", src, cid, err); continue
                meta[(src, cid)] = res[1]; jobs.setdefault(res[1], res[0])
        todo = [(exe, h, tmp) for h, exe in jobs.items()]
        with ProcessPoolExecutor(a.j) as ex:
            results = {h: (c, i, k) for h, c, i, k in ex.map(sim, todo)}
        for src in a.srcs:
            src = os.path.abspath(src)
            h0 = meta[(src, o3)]; c0, i0, k0 = results[h0]
            ref = subprocess.run([F.CC, "-O0", "-DNO_M5OPS", f"-I{F.M5_INC}", src, "-o", tmp + "/ref"], capture_output=True)
            refchk = re.search(r"CHECKSUM=(\d+)", subprocess.run([tmp + "/ref"], capture_output=True, text=True).stdout).group(1)
            print(f"\n{os.path.basename(src)}: O3 cycles={c0} insts={i0} ipc={i0/c0:.2f} checksum_ok={k0==refchk}")
            row = []
            for j, f in enumerate(names):
                h = meta.get((src, o3 ^ (1 << j)))
                c, i, k = results.get(h, (None, None, None))
                if c is None: row.append(f"{f}=FAIL"); continue
                flip = "off" if defaults[f] else "on"
                row.append(f"{f}({'O3on' if defaults[f] else 'O3off'}->{flip}):{(c/c0-1)*100:+.1f}%" + ("" if k == refchk else "!WRONG"))
            print("  " + "\n  ".join(row))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
