#!/usr/bin/env python3
"""Count branch records in the ROI window (instructions 10M..60M) of a ChampSim trace.
Record layout (64 B): ip u64, is_branch u8, branch_taken u8, dest_regs[2], src_regs[4], dest_mem[2] u64, src_mem[4] u64."""
import json, subprocess, sys
import numpy as np
name, skip, n = sys.argv[1], 10_000_000, 50_000_000
path = f"{__import__('os').path.expanduser('~')}/spec-traces/{name}.champsimtrace.xz"
p = subprocess.Popen(["xz", "-dc", path], stdout=subprocess.PIPE)
dt = np.dtype([("ip", "<u8"), ("br", "u1"), ("tk", "u1"), ("rest", "u1", 54)])
assert dt.itemsize == 64
done = br = tk = loads = 0
CH = 1 << 20
while done < skip + n:
    buf = p.stdout.read(min(CH, skip + n - done) * 64)
    if not buf: break
    a = np.frombuffer(buf[: len(buf) // 64 * 64], dtype=dt)
    lo = max(0, skip - done)
    sel = a[lo:]
    br += int(sel["br"].sum()); tk += int(((sel["br"] == 1) & (sel["tk"] == 1)).sum())
    done += len(a)
p.kill()
print(json.dumps({"trace": name, "records_read": done, "roi_branches": br, "roi_taken": tk}))
