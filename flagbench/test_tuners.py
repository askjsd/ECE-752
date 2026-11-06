"""Unit tests: analytic random baseline vs brute force, CARBS invariants, tuner prefix property."""
import itertools, math, random
import numpy as np
import tuners as T


class FakeLUT:
    def __init__(self, cycles, o3=0, n=None):
        self.cycles = np.array(cycles, float); self.valid = ~np.isnan(self.cycles)
        self.n = n or int(math.log2(len(cycles))); self.o3 = o3; self.evals = 0
    evaluate = T.LUT.evaluate; bits = T.LUT.bits; to_id = staticmethod(T.LUT.to_id)
    def o3_cycles(self): return float(self.cycles[self.o3])
    def optimum(self): return float(np.nanmin(self.cycles))


def test_expected_best_bruteforce():
    rng = random.Random(1)
    vals = [rng.uniform(1, 100) for _ in range(9)]
    for n in range(1, 6):
        combos = list(itertools.combinations(vals, n))
        assert abs(np.mean([min(c) for c in combos]) - T.expected_best_of_n(vals, 5, False)[n - 1]) < 1e-9
        assert abs(np.mean([max(c) for c in combos]) - T.expected_best_of_n(vals, 5, True)[n - 1]) < 1e-9


def test_budget_and_prefix():
    rng = np.random.default_rng(0)
    lut = FakeLUT(rng.uniform(1e6, 2e6, 256), o3=37)
    for name, f in T.TUNERS.items():
        t200 = f(lut, 60, 7); t20 = f(lut, 20, 7)
        assert len(t200) == 60 and len(t20) == 20, name
        assert t200[:20] == t20, f"{name}: prefix property"
        assert all(a >= b for a, b in zip(t200, t200[1:])), f"{name}: trace must be non-increasing"
    # evaluation counting: CARBS counts the initial O3 eval
    lut.evals = 0; T.carbs(lut, 25, 3); assert lut.evals == 25


def test_carbs_first_eval_is_o3_and_finds_planted_optimum():
    n = 6; cyc = np.full(1 << n, 1000.0)
    # flag 2 ON is good (-30%), flag 4 ON is bad (+40%)
    for c in range(1 << n):
        cyc[c] = 1000 * (0.7 if c >> 2 & 1 else 1) * (1.4 if c >> 4 & 1 else 1)
    lut = FakeLUT(cyc, o3=0, n=n)
    wins = 0
    for s in range(30):
        tr, P = T.carbs(lut, 40, s, return_state=True)
        assert tr[0] == 1000.0
        wins += P[2] > 0.5 and P[4] < 0.5
    assert wins >= 20, wins


def test_random_without_replacement():
    lut = FakeLUT(np.arange(1, 65, dtype=float), o3=0, n=6)
    tr = T.random_search(lut, 64, 5)
    assert tr[-1] == 1.0 and len(tr) == 64  # all 64 distinct configs evaluated => optimum found


if __name__ == "__main__":
    for k, v in list(globals().items()):
        if k.startswith("test_"): v(); print("ok", k)
