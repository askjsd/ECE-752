#!/usr/bin/env python3
"""Run the pfbench suite natively and collect RESULT lines into a CSV.

Runs are sequential on purpose: benchmarks running together would share L3 and
memory bandwidth and measure each other. For ChampSim (tracing and simulation,
which do run in parallel) use run_champsim.py.

  ./run_all.py                          # target geometry (2^9/2^12/2^14 lines), all kernels
                                        # (all but filllevel are L1-only studies: simulate them
                                        #  with champsim_config_knobbed_l1only.json)
  ./run_all.py --geometry host --cpu 2  # size working sets for this machine, pin to CPU 2
  ./run_all.py --only gather,jump --repeat 5
  ./run_all.py --dry-run                # print the commands only

Per-run stdout/stderr (including REGION and PHASE lines) is kept in <out>/logs/.
"""
import argparse, csv, os, shlex, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))

# (name, kernel, args, binary) -- binary "noswpf" = built without any prefetch instructions
SUITE = [
    # pointer chasing
    ("chase",               "chase", [], None),                    # sweep the HW prefetcher's degree in the simulator
    ("jump_hw",             "jump",  ["--swpf=0"], "noswpf"),      # same nodes, no SW prefetch
    *[(f"jump_d8_k{k}",     "jump",  ["--dist=8", f"--k={k}"], None) for k in (1, 2, 4, 6)],   # step size k
    *[(f"jump_d{d}_k1",     "jump",  [f"--dist={d}", "--k=1"], None) for d in (2, 4, 16, 32)], # distance
    # indirect
    ("gather_hw",           "gather", [], "noswpf"),
    *[(f"gather_sw_d{d}",   "gather", [f"--dist={d}"], None) for d in (4, 16, 64, 256)],
    *[(f"gather_sw_d64_loc{l}", "gather", ["--dist=64", f"--locality={l}"], None) for l in (0, 1, 2)],
    ("gather_sw_d64_pfidx", "gather", ["--dist=64", "--pf-index=1"], None),
    ("scatter_hw",          "scatter", [], "noswpf"),
    ("scatter_sw_d64",      "scatter", ["--dist=64"], None),
    ("gather2_hw",          "gather2", [], "noswpf"),
    *[(f"gather2_sw_d{d}",  "gather2", [f"--dist={d}"], None) for d in (16, 64)],
    # stride pathologies
    ("stride_1line",        "stride", ["--stride-bytes=64"], None),             # control
    ("stride_4lines",       "stride", [], None),
    ("stride_overflow",     "stride_overflow", [], None),
    ("stride_overflow_s12", "stride_overflow", ["--bits=12", "--signed=1"], None),
    ("stride_alt_4_8",      "stride_alt", [], None),
    ("stride_zigzag_3_1",   "stride_zigzag", [], None),
    ("stride_zigzag_4_2",   "stride_zigzag", ["--fwd-lines=4", "--back-lines=2"], None),
    *[(f"bursts_b{b}",      "bursts", [f"--burst-lines={b}"], None) for b in (2, 4, 8, 16)],
    # streams
    ("stream_fwd",          "stream", [], None),
    ("stream_rev",          "stream_rev", [], None),
    *[(f"stream_holes_gap{g}", "stream_holes", [f"--gap={g}"], None) for g in (32, 128, 500)],
    ("stream_holes_gap128_spatial", "stream_holes", ["--run-lines=3", "--hole-lines=1"], None),
    # pollution / bandwidth
    ("phase",               "phase", [], None),
    ("phase_isolated",      "phase", ["--rand-run-lines=1"], None),
    ("pollute_none",        "pollute", ["--cold-every=0"], None),
    ("pollute_run1",        "pollute", ["--cold-run-lines=1"], None),
    ("pollute_run2",        "pollute", [], None),
    ("pollute_run4",        "pollute", ["--cold-run-lines=4", "--cold-every=8"], None),
    # timeliness
    *[(f"late_w{w}",        "late", [f"--work={w}"], None) for w in (0, 2, 8, 32)],
    # fill level
    ("fill_seq",            "filllevel", [], None),
    ("fill_seq_oneshot_rand", "filllevel", ["--one-shot=random"], None),
    ("fill_random",         "filllevel", ["--order=random", "--one-shot=random"], None),
    ("fill_no_oneshot",     "filllevel", ["--ps=0"], None),
    ("fill_r1_only",        "filllevel", ["--p2=0", "--p3=0", "--ps=0"], None),
]


def host_geometry(cpu):
    """Line size and L1D/L2/L3 capacities (in lines) from sysfs."""
    base = f"/sys/devices/system/cpu/cpu{cpu}/cache"
    geo, line = {}, 64
    for idx in sorted(os.listdir(base)):
        if not idx.startswith("index"):
            continue
        rd = lambda f: open(os.path.join(base, idx, f)).read().strip()
        if rd("type") not in ("Data", "Unified"):
            continue
        size = rd("size")
        mult = {"K": 1 << 10, "M": 1 << 20, "G": 1 << 30}.get(size[-1], 1)
        nbytes = int(size.rstrip("KMG")) * mult
        line = int(rd("coherency_line_size"))
        geo[int(rd("level"))] = nbytes // line
    if not all(l in geo for l in (1, 2, 3)):
        sys.exit(f"could not read L1/L2/L3 sizes from {base}")
    return [f"--line={line}", f"--l1-lines={geo[1]}", f"--l2-lines={geo[2]}", f"--l3-lines={geo[3]}"]


def parse_kv(line):
    out = {}
    for tok in line.split()[1:]:
        k, _, v = tok.partition("=")
        out[k] = v
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", default=os.path.join(HERE, "pfbench"))
    ap.add_argument("--bin-noswpf", default=os.path.join(HERE, "pfbench_noswpf"))
    ap.add_argument("--out", default=os.path.join(HERE, "results"))
    ap.add_argument("--geometry", choices=["target", "host"], default="target",
                    help="target = 2^9/2^12/2^14 lines; host = this machine's caches (for native runs)")
    ap.add_argument("--cpu", type=int, default=None, help="pin with taskset (use a P-core on hybrid CPUs)")
    ap.add_argument("--only", default="", help="comma-separated benchmark names or kernel names")
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--wrap", default="", help="command prefix; {name} is replaced by the benchmark name")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("extra", nargs="*", help="extra options passed to every run (after --)")
    a = ap.parse_args()

    for b in (a.bin, a.bin_noswpf):
        if not os.path.exists(b) and not a.dry_run:
            sys.exit(f"{b} not found: run make in {HERE}")
    common = host_geometry(a.cpu or 0) if a.geometry == "host" else []
    only = {s for s in a.only.split(",") if s}
    suite = [s for s in SUITE if not only or s[0] in only or s[1] in only]
    if not suite:
        sys.exit("no benchmark matches --only")

    os.makedirs(os.path.join(a.out, "logs"), exist_ok=True)
    rows, failed = [], []
    for name, kernel, args, which in suite:
        binary = a.bin_noswpf if which == "noswpf" else a.bin
        cmd = [binary, kernel, *common, *args, *a.extra]
        if a.cpu is not None:
            cmd = ["taskset", "-c", str(a.cpu), *cmd]
        if a.wrap:
            cmd = shlex.split(a.wrap.replace("{name}", name)) + cmd
        for rep in range(a.repeat):
            print("$", shlex.join(cmd), flush=True)
            if a.dry_run:
                continue
            p = subprocess.run(cmd, capture_output=True, text=True)
            log = os.path.join(a.out, "logs", f"{name}.{rep}.log")
            with open(log, "w") as f:
                f.write(p.stdout + p.stderr)
            res = [l for l in p.stdout.splitlines() if l.startswith("RESULT ")]
            if p.returncode or not res:
                failed.append(name)
                print(f"  FAILED (exit {p.returncode}), see {log}", file=sys.stderr)
                continue
            row = {"name": name, "rep": rep, "binary": os.path.basename(binary), **parse_kv(res[-1])}
            rows.append(row)
            print(f"  {row.get('ns_per_access', '?')} ns/access", flush=True)

    if rows:
        keys = []
        for r in rows:
            keys += [k for k in r if k not in keys]
        path = os.path.join(a.out, "results.csv")
        with open(path, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=keys)
            w.writeheader()
            w.writerows(rows)
        print(f"wrote {path} ({len(rows)} rows)")
    if failed:
        sys.exit(f"{len(failed)} run(s) failed: {', '.join(failed)}")


if __name__ == "__main__":
    main()
