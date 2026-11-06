"""LUT replay + tuners. No simulation happens here: evaluate(config_id) reads the LUT.

Interface: tuner(lut, budget, seed, **opts) -> trace, a list of length <= budget where trace[i] is the
best cycles seen after i+1 evaluations (inf until the first valid evaluation). Each call to
LUT.evaluate counts one evaluation, valid or not.
"""
import csv, json, math, os, random
import numpy as np


class LUT:
    def __init__(self, csv_path):
        meta_path = csv_path[:-4] + ".json"
        self.meta = json.load(open(meta_path))
        self.names = self.meta["flags"]
        self.n = self.meta["N"]
        self.o3 = self.meta["o3_config_id"]
        self.cycles = np.full(1 << self.n, np.nan)
        for r in csv.DictReader(open(csv_path)):
            if int(r["compiled_ok"]) and int(r["correct"]) and r["cycles"] != "":
                self.cycles[int(r["config_id"])] = float(r["cycles"])
        self.valid = ~np.isnan(self.cycles)
        self.evals = 0

    def evaluate(self, config_id):
        """cycles, or None for a failed/incorrect config. Counts as one evaluation."""
        self.evals += 1
        v = self.cycles[config_id]
        return None if math.isnan(v) else float(v)

    def bits(self, config_id):
        return np.array([(config_id >> j) & 1 for j in range(self.n)], dtype=np.int8)

    @staticmethod
    def to_id(bits):
        return int(sum(int(b) << j for j, b in enumerate(bits)))

    # ground truth helpers (never used by tuners)
    def optimum(self):
        return float(np.nanmin(self.cycles))

    def o3_cycles(self):
        return float(self.cycles[self.o3])


def _trace_update(best, v):
    return best if v is None or v >= best else v


def random_search(lut, budget, seed, **_):
    """Uniform sampling without replacement over all 2^N configs (the O3 config is NOT given for free)."""
    rng = random.Random(seed)
    order = list(range(1 << lut.n)); rng.shuffle(order); order = order[:budget]  # prefix-consistent
    best, trace = math.inf, []
    for c in order:
        best = _trace_update(best, lut.evaluate(c)); trace.append(best)
    return trace


def expected_best_of_n(values, nmax, higher_is_better):
    """Exact E[best of n draws WITHOUT replacement] for n=1..nmax from `values` (analytic, no sampling).
    Sort best-first; P(best of n is the i-th) = C(m-1-i, n-1) / C(m, n)."""
    vals = np.sort(np.asarray(values, dtype=float))
    if higher_is_better:
        vals = vals[::-1]
    m = len(vals)
    i = np.arange(m)
    out = []
    for n in range(1, nmax + 1):
        n = min(n, m)
        k = m - 1 - i
        mask = k >= n - 1
        lg = (_lgamma(k[mask] + 1) - _lgamma(n) - _lgamma(k[mask] - n + 2)) - \
             (math.lgamma(m + 1) - math.lgamma(n + 1) - math.lgamma(m - n + 1))
        out.append(float((np.exp(lg) * vals[mask]).sum()))
    return out


def expected_random_best(lut, nmax, speedup=False):
    """Exact expected best-of-n for random search over the VALID configs: in cycles (min), or in
    speedup over -O3 (max of O3_cycles/cycles) when speedup=True."""
    v = lut.cycles[lut.valid]
    if speedup:
        return expected_best_of_n(lut.o3_cycles() / v, nmax, True)
    return expected_best_of_n(v, nmax, False)


def _lgamma(x):
    return np.array([math.lgamma(t) for t in np.atleast_1d(x)])


def hill_climb(lut, budget, seed, **_):
    """One-bit-flip greedy hill climber with random restarts from a random config when stuck."""
    rng = random.Random(seed)
    best, trace, evals = math.inf, [], 0
    cur = rng.randrange(1 << lut.n)
    cur_v = lut.evaluate(cur); evals += 1
    best = _trace_update(best, cur_v); trace.append(best)
    while evals < budget:
        neigh = list(range(lut.n)); rng.shuffle(neigh)
        improved = False
        for j in neigh:
            if evals >= budget: break
            c = cur ^ (1 << j)
            v = lut.evaluate(c); evals += 1
            best = _trace_update(best, v); trace.append(best)
            if v is not None and (cur_v is None or v < cur_v):
                cur, cur_v, improved = c, v, True
                break  # first-improvement
        if not improved and evals < budget:  # local optimum: random restart
            cur = rng.randrange(1 << lut.n)
            cur_v = lut.evaluate(cur); evals += 1
            best = _trace_update(best, cur_v); trace.append(best)
    return trace[:budget]


def carbs(lut, budget, seed, eta=0.9, h=5, gamma=0.9, eps=0.005, k=None, clip=(0.0, 1.0),
          return_state=False, **_):
    """CARBS (Algorithm 1) replayed on the LUT.

    * P[j] = probability flag j is ON, init 0.5. O_best/history start from the -O3 config, whose
      evaluation counts toward the budget.
    * Sampling: the k flags with highest P are forced on; the rest are Bernoulli(P_j).
      NOTE k = max(1, round(0.1*N)) by default. The paper's k=10 assumes ~260 flags; at N=12 it
      would force most flags on.
    * History: cur is compared against each of the last h evaluated configs; the i-th most recent
      (i=1..h) uses learning rate eta*gamma**(i-1).
    * Case 1 (relative difference > eps, replacing the paper's t-test since gem5 is deterministic):
        R = +T_prev/T_cur if cur faster else -T_cur/T_prev; only flags that DIFFER between cur and prev are
        updated, step lr/#differing, moving P toward the setting of the faster config (flag on in cur:
        P += step*R; flag off in cur: P -= step*R).
    * Case 2 (difference not significant): R = -T_cur/T_best; only flags that AGREE are updated, step
        lr/#agreeing: enabled-in-both (Eq. 4 form): P += step*R ; disabled-in-both (Eq. 5 form):
        P -= step*R. (R<0, so common flags of a config that is no better than the best are pushed away
        from their shared setting. Signs follow the reference implementation in carbs/optimizers.py.)
    * P is clipped to `clip` after each update.
    * Failed/incorrect configs consume budget and are not added to history or used for updates.
    * Repeated samples of an already-evaluated config count as evaluations (no memoisation).
    """
    rng = random.Random(seed)
    n = lut.n
    k = max(1, round(0.1 * n)) if k is None else k
    P = np.full(n, 0.5)
    best, trace = math.inf, []
    hist = []  # [(bits, T)] evaluated valid configs, oldest first
    # initial: -O3
    t = lut.evaluate(lut.o3)
    best = _trace_update(best, t); trace.append(best)
    best_t = t if t is not None else math.inf
    if t is not None:
        hist.append((lut.bits(lut.o3), t))
    while len(trace) < budget:
        top = set(np.argsort(-P, kind="stable")[:k].tolist())
        bits = np.array([1 if (j in top or rng.random() < P[j]) else 0 for j in range(n)], dtype=np.int8)
        t = lut.evaluate(LUT.to_id(bits))
        best = _trace_update(best, t); trace.append(best)
        if t is None:
            continue
        for i, (pb, pt) in enumerate(reversed(hist[-h:]), start=1):
            lr = eta * gamma ** (i - 1)
            if abs(t - pt) / pt > eps:                      # Case 1
                R = pt / t if t < pt else -t / pt
                diff = bits != pb
                nd = int(diff.sum())
                if nd:
                    step = lr / nd
                    for j in np.nonzero(diff)[0]:
                        P[j] += step * R if bits[j] == 1 else -step * R
            else:                                           # Case 2
                R = -t / (best_t if math.isfinite(best_t) else t)   # Eq. 7: O_best excludes the current config
                agree = bits == pb
                na = int(agree.sum())
                if na:
                    step = lr / na
                    for j in np.nonzero(agree)[0]:
                        P[j] += step * R if bits[j] == 1 else -step * R
            np.clip(P, clip[0], clip[1], out=P)
        hist.append((bits, t))
        best_t = min(best_t, t)
    return (trace, P.copy()) if return_state else trace


TUNERS = {"carbs": carbs, "random": random_search, "hillclimb": hill_climb}
