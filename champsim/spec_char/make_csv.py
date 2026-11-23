#!/usr/bin/env python3
"""Build trace_characterization.csv from raw/*.json.out and branches.jsonl.
MPKI = (LOAD + RFO demand accesses that missed, incl. MSHR merges) / ROI instructions * 1000."""
import csv, json, sys
from pathlib import Path
here = Path(__file__).parent
br = {}
for l in (here / "branches.jsonl").read_text().splitlines():
    d = json.loads(l); br[d["trace"]] = d
rows = []
for f in sorted((here / "raw").glob("*.json.out")):
    name = f.name[:-9]
    t = f.read_text()
    try:
        d = json.loads(t[t.index("\n[") + 1:])
    except Exception:
        print("no result:", name, file=sys.stderr); continue
    r = d[0]["roi"]; c = r["cores"][0]; ins = c["instructions"]
    def mpki(cache, types=("LOAD", "RFO")):
        x = r[cache]; return sum(x[k]["miss"][0] + x[k]["miss_merge"][0] for k in types) / ins * 1000
    mis = sum(c["mispredict"].values())
    nb = br[name]["roi_branches"]
    llc_all = sum(r["LLC"][k]["miss"][0] for k in r["LLC"] if isinstance(r["LLC"][k], dict) and "miss" in r["LLC"][k]) / ins * 1000
    llc = mpki("LLC")
    label = "KEEP" if llc >= 3 else "MAYBE" if llc >= 1 else "DROP"
    flag = []
    if nb == 0: flag.append("ZERO_BRANCHES")
    elif nb / ins < 0.02: flag.append("LOW_BRANCH_COUNT")
    if nb and mis > nb: flag.append("MISPRED_GT_BRANCHES")
    rows.append(dict(trace=name, instructions=ins, cycles=c["cycles"], ipc=round(ins / c["cycles"], 4),
                     branches=nb, branches_per_ki=round(nb / ins * 1000, 1), branch_mispredicts=mis,
                     branch_mpki=round(mis / ins * 1000, 2), l1d_mpki=round(mpki("cpu0_L1D"), 2),
                     l2c_mpki=round(mpki("cpu0_L2C"), 2), llc_mpki=round(llc, 2),
                     llc_mpki_all_types=round(llc_all, 2), label=label, flags=";".join(flag)))
with open(here / "trace_characterization.csv", "w", newline="") as fh:
    w = csv.DictWriter(fh, fieldnames=list(rows[0])); w.writeheader(); w.writerows(rows)
for r_ in rows: print(*[r_[k] for k in ("trace", "ipc", "branches_per_ki", "branch_mpki", "l1d_mpki", "l2c_mpki", "llc_mpki", "label", "flags")], sep="\t")
