#!/usr/bin/env python3
"""Build a C kernel with and without FP-boost hints, trace both with DynamoRIO, simulate in ChampSim,
and report time / energy / EDP.

env: DR (DynamoRIO dir), PY (python with capstone), LD_LIBRARY_PATH (libsnappy), CHAMPSIM (binary).
usage: dvfs_eval.py kernel.c [--opt -O2] [--fp-min .25 --min-instrs 8 --mem-max .6 --level 3]
                             [--penalty-ns 2000] [--trace-skip N] [--trace-count N] [--sim N] [--json out.json]
"""
import argparse, json, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
from dvfs_energy import metrics, parse_time_at_period


def sh(cmd, **kw):
    return subprocess.run(cmd, check=True, text=True, capture_output=True, **kw)


def build(src, opt, workdir, knobs, extra=()):
    inc = ["-DNO_M5OPS", "-I", os.path.join(REPO, "include"), "-I", os.path.dirname(os.path.abspath(src))]
    base, hinted = os.path.join(workdir, "base"), os.path.join(workdir, "hinted")
    sh(["gcc", opt, "-march=x86-64", *inc, *extra, "-static", "-no-pie", src, "-o", base])
    asm, asm_h = hinted + ".s", hinted + "_h.s"
    sh(["gcc", opt, "-march=x86-64", *inc, *extra, "-S", src, "-o", asm])
    r = sh([sys.executable, os.path.join(HERE, "fp_boost_pass.py"), asm, "-o", asm_h, "--report",
            "--fp-min", str(knobs.fp_min), "--min-instrs", str(knobs.min_instrs),
            "--mem-max", str(knobs.mem_max), "--level", str(knobs.level),
            "--hoist-fp-min", str(knobs.hoist_fp_min)])
    sh(["gcc", "-static", "-no-pie", asm_h, "-o", hinted])
    a, b = sh([base]).stdout, sh([hinted]).stdout
    if a != b:
        raise SystemExit(f"hinted binary output differs from baseline:\n{a}\n{b}")
    return base, hinted, r.stderr


def trace(binary, out, skip, count):
    env = dict(os.environ)
    env.setdefault("PY", sys.executable)
    sh(["bash", os.path.join(REPO, "champsim/dynamorio/dr_trace/trace_dr.sh"), "-o", out,
        "-s", str(skip), "-t", str(count), "--", binary], env=env)


def simulate(champsim, trace_path, sim, warm, penalty_ns, dwell_ns=0):
    env = dict(os.environ, CHAMPSIM_DVFS_PENALTY_NS=str(penalty_ns), CHAMPSIM_DVFS_MIN_DWELL_NS=str(dwell_ns))
    out = sh([champsim, "--warmup-instructions", str(warm), "--simulation-instructions", str(sim), trace_path], env=env).stdout
    dv = [l for l in out.splitlines() if "DVFS transitions" in l][-1]
    tap, trans = parse_time_at_period(dv)
    return tap, trans


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("kernel")
    ap.add_argument("--opt", default="-O2")
    ap.add_argument("--fp-min", type=float, default=0.25)
    ap.add_argument("--min-instrs", type=int, default=8)
    ap.add_argument("--mem-max", type=float, default=0.60)
    ap.add_argument("--hoist-fp-min", type=float, default=0.05)
    ap.add_argument("--dwell-ns", type=int, default=0)
    ap.add_argument("--level", type=int, default=3)
    ap.add_argument("--penalty-ns", type=int, default=2000)
    ap.add_argument("--trace-skip", type=int, default=0)
    ap.add_argument("--trace-count", type=int, default=3_000_000)
    ap.add_argument("--sim", type=int, default=2_500_000)
    ap.add_argument("--warm", type=int, default=100_000)
    ap.add_argument("--static-frac", type=float, default=0.3)
    ap.add_argument("--base-period-ps", type=int, default=500)
    ap.add_argument("--champsim", default=os.environ.get("CHAMPSIM", os.path.expanduser("~/champsim-knobbed-linux/bin/champsim_knobbed")))
    ap.add_argument("--json")
    ap.add_argument("--keep", help="keep build/trace files in this directory")
    a = ap.parse_args()

    ctx = tempfile.TemporaryDirectory() if not a.keep else None
    wd = a.keep or ctx.name
    os.makedirs(wd, exist_ok=True)
    base, hinted, rep = build(a.kernel, a.opt, wd, a)
    print(rep, file=sys.stderr)
    res = {}
    for name, binary in (("baseline", base), ("hinted", hinted)):
        tp = os.path.join(wd, name + ".champsim")
        trace(binary, tp, a.trace_skip, a.trace_count)
        tap, trans = simulate(a.champsim, tp, a.sim, a.warm, a.penalty_ns, a.dwell_ns)
        m = metrics(tap, a.base_period_ps, static_frac=a.static_frac)
        m["transitions"] = trans
        res[name] = m
    b, h = res["baseline"], res["hinted"]
    res["ratio"] = {k: h[k] / b[k] for k in ("time_ps", "energy", "edp", "ed2p")}
    print(f"{'':10}{'time(us)':>11}{'energy':>14}{'trans':>7}")
    for n in ("baseline", "hinted"):
        print(f"{n:10}{res[n]['time_ps']/1e6:>11.1f}{res[n]['energy']:>14.4g}{res[n]['transitions']:>7}")
    r = res["ratio"]
    print(f"hinted/baseline: time {r['time_ps']:.3f}  energy {r['energy']:.3f}  EDP {r['edp']:.3f}  ED2P {r['ed2p']:.3f}")
    if a.json:
        json.dump(res, open(a.json, "w"), indent=1)


if __name__ == "__main__":
    main()
