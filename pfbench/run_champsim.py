#!/usr/bin/env python3
"""Trace every pfbench configuration and simulate it in ChampSim, in parallel.

Native runs stay in run_all.py and run one at a time, because benchmarks running
together would share L3 and memory bandwidth and measure each other. Tracing and
simulation have no such problem: each job is an independent process with a
deterministic result, so this script runs them concurrently.

  ./run_champsim.py                         # trace all 56 configs, simulate with 2 hints
  ./run_champsim.py --only chase,jump -j 16
  ./run_champsim.py --hint off=source=off --hint m8=source=markov,level=3,throttle=0
  ./run_champsim.py --trace-only            # just (re)build the traces

Steps:
  1. Trace (DynamoRIO, trace_dr.sh). Each trace covers exactly the ROI: it starts at
     pfb_roi_begin and stops at pfb_roi_end (capped by --trace-instructions). Traces
     are cached in <traces>/ and redone only when the command changes or with
     --force-trace.
  2. Simulate (ChampSim). Every kernel uses the L1-only build except filllevel, which
     needs the full hierarchy. The first --warmup-frac of the trace is warm-up (half
     for chase and jump, whose first traversal is what a Markov prefetcher learns
     from), and the rest is measured, so each trace is simulated exactly once.
     Each --hint NAME=HINT is one simulation per benchmark, with KNOBBED_HINT=HINT.

Output: <out>/results.csv (one row per benchmark x hint) and <out>/logs/.
Needs DR (DynamoRIO tree) and PY (python with capstone) in the environment, plus
LD_LIBRARY_PATH for DynamoRIO's libraries, as in README.md.
"""
import argparse, csv, json, os, re, shlex, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor, as_completed

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
TRACE_DR = os.path.join(REPO, "champsim", "dr_trace", "trace_dr.sh")
sys.path.insert(0, HERE)
from run_all import SUITE  # noqa: E402

# Fraction of each trace used as warm-up when it differs from --warmup-frac. chase and
# jump run two full traversals in the ROI; the first is for a Markov prefetcher to learn.
WARMUP_FRAC = {"chase": 0.5, "jump": 0.5}
FULL_HIERARCHY = {"filllevel"}


def trace_one(name, kernel, args, which, a):
    binary = a.bin_noswpf if which == "noswpf" else a.bin
    out = os.path.join(a.traces, f"{name}.champsim.xz")
    meta_path = os.path.join(a.traces, f"{name}.json")
    cmd = ["bash", TRACE_DR, "-o", out, "-S", "pfb_roi_begin", "-E", "pfb_roi_end",
           "-t", str(a.trace_instructions), "--",
           binary, kernel, *args, "--quiet=1", "--perf=off"]
    key = shlex.join(cmd)
    if not a.force_trace and os.path.exists(out) and os.path.exists(meta_path):
        meta = json.load(open(meta_path))
        if meta.get("cmd") == key and os.path.getmtime(out) >= os.path.getmtime(binary):
            return name, meta["records"], "cached"
    if a.dry_run:
        print("$", key)
        return name, 0, "dry-run"
    p = subprocess.run(cmd, capture_output=True, text=True)
    log = os.path.join(a.out, "logs", f"{name}.trace.log")
    with open(log, "w") as f:
        f.write(p.stdout + p.stderr)
    m = re.search(r"wrote (\d+) records", p.stderr)
    if p.returncode or not m:
        raise RuntimeError(f"tracing {name} failed (exit {p.returncode}), see {log}")
    records = int(m.group(1))
    if "stopped at --stop-pc" not in p.stderr:
        print(f"  note: {name}: ROI longer than --trace-instructions; trace holds its first {records}",
              file=sys.stderr)
    with open(meta_path, "w") as f:
        json.dump({"cmd": key, "records": records}, f)
    return name, records, "traced"


def roi_stats(text):
    """Pull the ROI numbers out of a ChampSim log."""
    roi = text.split("Region of Interest Statistics", 1)[-1]
    r = {}
    m = re.search(r"CPU 0 cumulative IPC: ([\d.]+) instructions: (\d+) cycles: (\d+)", roi)
    if m:
        r["ipc"], r["instructions"], r["cycles"] = m.group(1), m.group(2), m.group(3)
    for lvl in ("L1D", "L2C", "LLC"):
        tag = lvl.lower()
        m = re.search(rf"cpu0_{lvl} LOAD\s+ACCESS:\s+(\d+) HIT:\s+(\d+) MISS:\s+(\d+)", roi) or \
            re.search(rf"\b{lvl} LOAD\s+ACCESS:\s+(\d+) HIT:\s+(\d+) MISS:\s+(\d+)", roi)
        if m:
            r[f"{tag}_load_access"], r[f"{tag}_load_hit"], r[f"{tag}_load_miss"] = m.groups()
    m = re.search(r"cpu0_L1D PREFETCH REQUESTED:\s+(\d+) ISSUED:\s+(\d+) USEFUL:\s+(\d+) USELESS:\s+(\d+)", roi)
    if m:
        r["l1d_pf_requested"], r["l1d_pf_issued"], r["l1d_pf_useful"], r["l1d_pf_useless"] = m.groups()
    m = re.search(r"cpu0_L1D AVERAGE MISS LATENCY: ([\d.]+|-nan|nan)", roi)
    if m:
        r["l1d_miss_latency"] = m.group(1)
    # knobbed's own telemetry (region 0 = the default hint)
    m = re.search(r"\[knobbed cpu0_L1D\] region 0 .*?final level=(\d+).*?accuracy=([\d.]+) coverage=([\d.]+) timeliness=([\d.]+)", text)
    if m:
        r["final_level"], r["accuracy"], r["coverage"], r["timeliness"] = m.groups()
    return r


def simulate_one(name, kernel, records, hint_name, hint, a):
    full = kernel in FULL_HIERARCHY
    binary = a.full_bin if full else a.l1only_bin
    frac = WARMUP_FRAC.get(kernel, a.warmup_frac)
    warm = int(records * frac)
    sim = max(1, records - warm)
    trace = os.path.join(a.traces, f"{name}.champsim.xz")
    cmd = [binary, "--warmup-instructions", str(warm), "--simulation-instructions", str(sim), trace]
    env = dict(os.environ)
    env.pop("KNOBBED_HINT", None)
    if hint:
        env["KNOBBED_HINT"] = hint
    if a.dry_run:
        print(f"$ KNOBBED_HINT={shlex.quote(hint)} {shlex.join(cmd)}")
        return None
    p = subprocess.run(cmd, capture_output=True, text=True, env=env)
    log = os.path.join(a.out, "logs", f"{name}.{hint_name}.log")
    with open(log, "w") as f:
        f.write(p.stdout + p.stderr)
    if p.returncode:
        raise RuntimeError(f"simulating {name} [{hint_name}] failed (exit {p.returncode}), see {log}")
    return {"name": name, "kernel": kernel, "hint_name": hint_name, "hint": hint,
            "config": "full" if full else "l1only", "trace_records": records,
            "warmup": warm, "sim": sim, **roi_stats(p.stdout + p.stderr)}


def run_pool(jobs, tasks, label):
    """Run (fn, args) tasks on a thread pool (each task is a subprocess). Returns results."""
    out, failed = [], []
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        futs = {ex.submit(fn, *args): args[0] for fn, args in tasks}
        for i, f in enumerate(as_completed(futs), 1):
            try:
                res = f.result()
                out.append(res)
                print(f"[{label} {i}/{len(futs)}] {futs[f]}", flush=True)
            except Exception as e:  # keep going; report at the end
                failed.append(futs[f])
                print(f"[{label} {i}/{len(futs)}] {e}", file=sys.stderr, flush=True)
    return out, failed


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ncpu = os.cpu_count() or 4
    ap.add_argument("-j", "--jobs", type=int, default=ncpu, help=f"parallel simulations (default {ncpu})")
    ap.add_argument("--trace-jobs", type=int, default=max(1, ncpu // 2),
                    help="parallel traces; each uses about 2 cores (default %(default)s)")
    ap.add_argument("--only", default="", help="comma-separated benchmark or kernel names")
    ap.add_argument("--hint", action="append", default=None, metavar="NAME=HINT",
                    help="knobbed hint to simulate, repeatable (default: off=source=off and "
                         "default= , the module's built-in default)")
    ap.add_argument("--trace-instructions", type=int, default=5_000_000,
                    help="cap on trace length; traces normally end at pfb_roi_end first")
    ap.add_argument("--warmup-frac", type=float, default=0.2,
                    help="fraction of each trace used as warm-up (chase and jump: 0.5)")
    ap.add_argument("--bin", default=os.path.join(HERE, "pfbench"))
    ap.add_argument("--bin-noswpf", default=os.path.join(HERE, "pfbench_noswpf"))
    ap.add_argument("--l1only-bin", default=os.path.expanduser("~/champsim-knobbed-l1only/bin/champsim_knobbed_l1only"))
    ap.add_argument("--full-bin", default=os.path.expanduser("~/champsim-knobbed-linux/bin/champsim_knobbed"))
    ap.add_argument("--traces", default=os.path.join(HERE, "traces"))
    ap.add_argument("--out", default=os.path.join(HERE, "results", "champsim"))
    ap.add_argument("--force-trace", action="store_true", help="retrace even if a cached trace matches")
    ap.add_argument("--trace-only", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    hints = []
    for h in a.hint or ["off=source=off", "default="]:
        name, eq, val = h.partition("=")
        if not eq or not re.fullmatch(r"[\w.-]+", name):
            sys.exit(f"--hint expects NAME=HINT, got {h!r}")
        hints.append((name, val))
    only = {s for s in a.only.split(",") if s}
    suite = [s for s in SUITE if not only or s[0] in only or s[1] in only]
    if not suite:
        sys.exit("no benchmark matches --only")
    if not a.dry_run:
        if not os.environ.get("DR"):
            sys.exit("set DR (DynamoRIO tree), PY (python with capstone) and LD_LIBRARY_PATH; see README.md")
        needed = [a.bin, a.bin_noswpf] + ([] if a.trace_only else [a.l1only_bin] +
                  ([a.full_bin] if any(s[1] in FULL_HIERARCHY for s in suite) else []))
        for b in needed:
            if not os.path.exists(b):
                sys.exit(f"{b} not found (run make here, or build ChampSim with champsim/build_knobbed.sh)")
    os.makedirs(a.traces, exist_ok=True)
    os.makedirs(os.path.join(a.out, "logs"), exist_ok=True)

    print(f"tracing {len(suite)} benchmarks, {a.trace_jobs} at a time", flush=True)
    traced, tfail = run_pool(a.trace_jobs, [(trace_one, (n, k, args, w, a)) for n, k, args, w in suite], "trace")
    records = {n: r for n, r, _ in traced}
    if a.trace_only:
        if tfail:
            sys.exit(f"{len(tfail)} trace(s) failed: {', '.join(tfail)}")
        return

    tasks = [(simulate_one, (n, k, records[n], hn, h, a)) for n, k, _, _ in suite if n in records for hn, h in hints]
    print(f"simulating {len(tasks)} runs ({len(records)} traces x {len(hints)} hints), {a.jobs} at a time", flush=True)
    rows, sfail = run_pool(a.jobs, tasks, "sim")
    rows = [r for r in rows if r]
    if rows:
        order = {s[0]: i for i, s in enumerate(suite)}
        hord = {h: i for i, (h, _) in enumerate(hints)}
        rows.sort(key=lambda r: (order[r["name"]], hord[r["hint_name"]]))
        keys = []
        for r in rows:
            keys += [k for k in r if k not in keys]
        path = os.path.join(a.out, "results.csv")
        with open(path, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=keys)
            w.writeheader()
            w.writerows(rows)
        print(f"wrote {path} ({len(rows)} rows)")
    if tfail or sfail:
        sys.exit(f"failed: traces {tfail or 'none'}; simulations {sfail or 'none'}")


if __name__ == "__main__":
    main()
