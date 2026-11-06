#!/usr/bin/env python3
"""Empirically verify which setting of each flag equals plain `gcc -O3`, by comparing .text hashes of builds
that pass only that one flag explicitly against a plain -O3 build, across all kernels.
(`gcc -Q --help=optimizers -O3` is NOT reliable: it reports -funroll-loops as enabled although plain -O3
does not generate the code an explicit -funroll-loops does.)"""
import subprocess, hashlib, os, sys, tempfile, glob
from concurrent.futures import ThreadPoolExecutor
import flags as F

def run(names=None, n=16):
    names, defaults = F.select_flags(n, names)
    tmp = tempfile.mkdtemp()
    def h(src, extra, tag):
        exe = os.path.join(tmp, tag)
        subprocess.run([F.CC, *F.BASE_FLAGS, *extra, "-static", f"-I{F.M5_INC}", src, "-o", exe, f"-L{F.M5_LIB}", "-lm5"],
                       check=True, capture_output=True)
        subprocess.run(["objcopy", "-O", "binary", "-j", ".text", exe, exe + ".t"], check=True)
        return hashlib.sha256(open(exe + ".t", "rb").read()).hexdigest()
    srcs = sorted(glob.glob(os.path.join(F.KERNEL_DIR, "*.c")) + glob.glob(os.path.join(F.KERNEL_DIR, "screen", "*.c")))
    jobs = []
    for si, s in enumerate(srcs):
        jobs.append((s, [], f"plain{si}"))
        for f in names:
            jobs += [(s, [f"-f{f}"], f"on_{f}_{si}"), (s, [f"-fno-{f}"], f"off_{f}_{si}")]
    with ThreadPoolExecutor(os.cpu_count()) as ex:
        res = list(ex.map(lambda j: h(*j), jobs))
    H = {j[2]: r for j, r in zip(jobs, res)}
    out = {}
    for f in names:
        mat = [i for i in range(len(srcs)) if H[f"on_{f}_{i}"] != H[f"off_{f}_{i}"]]
        onm = sum(H[f"on_{f}_{i}"] == H[f"plain{i}"] for i in mat); offm = sum(H[f"off_{f}_{i}"] == H[f"plain{i}"] for i in mat)
        if not mat: v = None
        elif onm == len(mat) and offm == 0: v = True
        elif offm == len(mat) and onm == 0: v = False
        else: v = "mixed"
        out[f] = {"gcc_Q_says_enabled": defaults[f], "kernels_where_it_matters": len(mat), "empirical_default": v}
    return out

if __name__ == "__main__":
    for f, r in run().items(): print(f"{f:26s} {r}")
