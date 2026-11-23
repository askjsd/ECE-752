#!/usr/bin/env python3
"""Tune the FP-boost DVFS policy with the flagbench tuners (CARBS, random search, hill climbing).

Same shape as champsim/knob_carbs.py: a config id is a bit pattern, decoded into policy knobs;
evaluate(config_id) builds + simulates and returns a number to MINIMISE (here: the hinted run's
ED2P / EDP / time divided by the unhinted baseline's, so 1.0 = no gain and < 1.0 = better).

Bit layout (least significant bit first; ordered fields are Gray coded):
  bits 0-1   boost     0 no boost (== baseline), 1 1.25x, 2 1.5x, 3 2x core frequency
  bits 2-4   fp_min    minimum FP-arithmetic fraction of a loop to boost it  (8 values, .05 .. .70)
  bits 5-6   mem_max   maximum fraction of loop instructions touching memory (.30 .45 .60 .90)
  bits 7-8   hoist     climb to enclosing loops with FP density >= (.02 .05 .15 never)
  bits 9-10  min_size  minimum static loop size in instructions               (4 8 16 32)
  bits 11-12 dwell     governor: ignore clock requests sooner than this after a change (0 5 20 50 us)

The pass always emits hint level 1; the boost frequency is applied at simulation time through
CHAMPSIM_DVFS_PERIODS_PS, so `boost` and `dwell` never need a new trace. Traces are cached by the hash
of the hinted assembly, so pass-knob combinations that produce the same code share one trace.

env: DR, PY, LD_LIBRARY_PATH as for dvfs_eval.py.
usage: dvfs_carbs.py kernel.c --out results/k_mix.json [--metric ed2p|edp|time] [--budget 30] [--seeds 2]
"""
import argparse, hashlib, json, os, shutil, subprocess, sys, tempfile
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPO))
from dvfs_energy import metrics, parse_time_at_period  # noqa: E402
from flagbench import tuners  # noqa: E402

BASE_PERIOD = 500
THRASH_PENALTY = 100.0  # value assigned to a config that makes ChampSim's livelock detector abort
FIELDS = [  # (name, bits, values)
    ("boost", 2, [BASE_PERIOD, 400, 333, 250]),                # core period in ps; 500 = off
    ("fp_min", 3, [0.05, 0.10, 0.15, 0.20, 0.30, 0.40, 0.50, 0.70]),
    ("mem_max", 2, [0.30, 0.45, 0.60, 0.90]),
    ("hoist", 2, [0.02, 0.05, 0.15, 1.0]),
    ("min_size", 2, [4, 8, 16, 32]),
    ("dwell", 2, [0, 5000, 20000, 50000]),                      # ns
]
N_BITS = sum(b for _, b, _ in FIELDS)
DEFAULT_ID = 0


def gray(n):
    return n ^ (n >> 1)


def ungray(g):
    n = 0
    while g:
        n ^= g
        g >>= 1
    return n


def decode(cid):
    knobs, shift = {}, 0
    for name, bits, values in FIELDS:
        code = (cid >> shift) & ((1 << bits) - 1)
        knobs[name] = values[ungray(code)]
        shift += bits
    return knobs


def sh(cmd, **kw):
    return subprocess.run(cmd, check=True, text=True, capture_output=True, **kw)


class Evaluator:
    def __init__(self, a, cache_path):
        self.a = a
        self.work = Path(a.workdir or tempfile.mkdtemp(prefix="dvfs_carbs_"))
        self.work.mkdir(parents=True, exist_ok=True)
        self.cache_path = Path(cache_path)
        self.cache = json.loads(self.cache_path.read_text()).get("cache", {}) if self.cache_path.exists() else {}
        self.src = str(Path(a.kernel).resolve())
        self.inc = ["-DNO_M5OPS", "-I", str(REPO / "include"), "-I", str(Path(self.src).parent)]
        self.traces = {}   # asm hash -> trace path
        self.sims = 0
        self.thrash_aborts = 0
        # baseline
        self.base_bin = self.work / "base"
        sh(["gcc", a.opt, "-march=x86-64", *self.inc, "-static", "-no-pie", self.src, "-o", str(self.base_bin)])
        self.base_out = sh([str(self.base_bin)]).stdout
        self.asm = self.work / "kernel.s"
        sh(["gcc", a.opt, "-march=x86-64", *self.inc, "-S", self.src, "-o", str(self.asm)])
        self.base_trace = self.trace(self.base_bin, "baseline")
        tap, _ = self.simulate(self.base_trace, BASE_PERIOD, 0)
        self.base_metrics = metrics(tap, BASE_PERIOD, static_frac=a.static_frac)

    def trace(self, binary, tag):
        out = self.work / f"{tag}.champsim"
        if not out.exists():
            env = dict(os.environ)
            env.setdefault("PY", sys.executable)
            sh(["bash", str(REPO / "champsim/dynamorio/dr_trace/trace_dr.sh"), "-o", str(out),
                "-s", str(self.a.trace_skip), "-t", str(self.a.trace_count), "--", str(binary)], env=env)
        return out

    def hinted_trace(self, k):
        """Run the pass with these knobs; return a trace path (cached by assembly hash)."""
        tmp = self.work / "tmp_h.s"
        sh([sys.executable, str(HERE / "fp_boost_pass.py"), str(self.asm), "-o", str(tmp), "--level", "1",
            "--fp-min", str(k["fp_min"]), "--mem-max", str(k["mem_max"]), "--hoist-fp-min", str(k["hoist"]),
            "--min-instrs", str(k["min_size"])])
        h = hashlib.sha1(tmp.read_bytes()).hexdigest()[:12]
        if h not in self.traces:
            hbin = self.work / f"h_{h}"
            sh(["gcc", "-static", "-no-pie", str(tmp), "-o", str(hbin)])
            if sh([str(hbin)]).stdout != self.base_out:
                raise RuntimeError("hinted binary output differs from baseline")
            self.traces[h] = self.trace(hbin, f"h_{h}")
        return h, self.traces[h]

    def simulate(self, trace, boost_period, dwell_ns):
        env = dict(os.environ, CHAMPSIM_DVFS_PENALTY_NS=str(self.a.penalty_ns),
                   CHAMPSIM_DVFS_MIN_DWELL_NS=str(dwell_ns),
                   CHAMPSIM_DVFS_PERIODS_PS=f"{BASE_PERIOD},{boost_period}")
        out = sh([self.a.binary, "--warmup-instructions", str(self.a.warmup),
                  "--simulation-instructions", str(self.a.sim), str(trace)], env=env).stdout
        self.sims += 1
        line = [l for l in out.splitlines() if "DVFS transitions" in l][-1]
        return parse_time_at_period(line)

    def value(self, cid):
        """Hinted metric / baseline metric (lower is better), or None on failure."""
        k = decode(cid)
        if k["boost"] == BASE_PERIOD:
            return 1.0   # boost off == baseline
        key = f"{k['boost']}|{k['dwell']}|"
        try:
            h, trace = self.hinted_trace(k)
            key += h
            if key in self.cache:
                return self.cache[key]
            tap, trans = self.simulate(trace, k["boost"], k["dwell"])
            m = metrics(tap, BASE_PERIOD, static_frac=self.a.static_frac)
        except subprocess.CalledProcessError as e:
            if "panic: IPC" in (e.stdout or ""):
                # ChampSim's livelock detector fired: the policy is switching clocks so often that the core
                # makes no progress. That is a real (terrible) outcome, not a tool failure.
                self.thrash_aborts += 1
                key += h
                self.cache[key] = THRASH_PENALTY
                self.detail[key] = {"knobs": k, "thrash_abort": True, "ratio": THRASH_PENALTY}
                return THRASH_PENALTY
            print(f"config {cid} failed: {e}", file=sys.stderr)
            return None
        except Exception as e:  # noqa: BLE001
            print(f"config {cid} failed: {e}", file=sys.stderr)
            return None
        v = m[self.a.metric + ("_ps" if self.a.metric == "time" else "")] / self.base_metrics[self.a.metric + ("_ps" if self.a.metric == "time" else "")]
        self.cache[key] = v
        self.detail[key] = {"knobs": k, "transitions": trans, "ratio": v,
                            "time_ratio": m["time_ps"] / self.base_metrics["time_ps"],
                            "energy_ratio": m["energy"] / self.base_metrics["energy"]}
        return v

    detail = {}


class KnobLUT:
    """The interface flagbench.tuners expects."""

    def __init__(self, ev):
        self.n, self.o3, self.ev, self.evals = N_BITS, DEFAULT_ID, ev, 0

    def evaluate(self, cid):
        self.evals += 1
        return self.ev.value(int(cid))

    def bits(self, cid):
        return np.array([(cid >> j) & 1 for j in range(self.n)], dtype=np.int8)

    @staticmethod
    def to_id(bits):
        return int(sum(int(b) << j for j, b in enumerate(bits)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("kernel")
    ap.add_argument("--out", required=True)
    ap.add_argument("--opt", default="-O2")
    ap.add_argument("--metric", choices=["ed2p", "edp", "time"], default="ed2p")
    ap.add_argument("--budget", type=int, default=30)
    ap.add_argument("--seeds", type=int, default=2)
    ap.add_argument("--penalty-ns", type=int, default=2000)
    ap.add_argument("--static-frac", type=float, default=0.3)
    ap.add_argument("--trace-skip", type=int, default=0)
    ap.add_argument("--trace-count", type=int, default=6_000_000)
    ap.add_argument("--warmup", type=int, default=100_000)
    ap.add_argument("--sim", type=int, default=5_000_000)
    ap.add_argument("--workdir")
    ap.add_argument("--binary", default=os.path.expanduser("~/champsim-knobbed-linux/bin/champsim_knobbed"))
    a = ap.parse_args()

    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    ev = Evaluator(a, out)
    results = {"carbs": [], "random": [], "hillclimb": []}

    def save():
        out.write_text(json.dumps({"kernel": a.kernel, "metric": a.metric, "budget": a.budget, "seeds": a.seeds,
                                   "penalty_ns": a.penalty_ns, "static_frac": a.static_frac,
                                   "baseline": ev.base_metrics, "traces": results, "cache": ev.cache,
                                   "detail": ev.detail, "n_bits": N_BITS}, indent=1))

    for name, fn, kw in (("carbs", tuners.carbs, {"bootstrap": False}),
                         ("random", tuners.random_search, {}),
                         ("hillclimb", tuners.hill_climb, {})):
        for seed in range(a.seeds):
            results[name].append(fn(KnobLUT(ev), a.budget, seed, **kw))
            save()
    best = min(((v, k) for k, v in ev.cache.items() if v is not None), default=None)
    print(f"{a.kernel}: {ev.sims} simulations, {len(ev.traces)} distinct hinted traces, {ev.thrash_aborts} thrash aborts")
    if best:
        d = ev.detail.get(best[1], {})
        print(f"best {a.metric} ratio {best[0]:.3f}  {d}")
    save()


if __name__ == "__main__":
    main()
