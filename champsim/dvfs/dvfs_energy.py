"""Simple DVFS energy model for ChampSim results.

Assumptions (all overridable): voltage scales linearly with frequency across the DVFS range, so
    dynamic power ~ f * V^2 ~ f^3        static (leakage) power ~ V ~ f
    P_rel(f) = s * f_rel + (1 - s) * f_rel^3        f_rel = f / f_base,  s = static fraction at base
Energy is in units of (base power x ps); only ratios between runs are meaningful.
Transition stalls are charged at the power of the period the core was running at.
"""
import re


def parse_time_at_period(line):
    """'CPU 0 DVFS transitions: 3 time_at_period_ps: 250=4075 500=10213' -> ({250: 4075, 500: 10213}, 3)"""
    trans = int(re.search(r"transitions: (\d+)", line).group(1))
    pairs = re.findall(r"(\d+)=(\d+)", line.split("time_at_period_ps:")[1])
    return {int(p): int(t) for p, t in pairs}, trans


def energy(time_at_period, base_period_ps, static_frac=0.3, vf_exp=1.0):
    e = 0.0
    for period, t in time_at_period.items():
        f = base_period_ps / period
        e += (static_frac * f ** vf_exp + (1 - static_frac) * f ** (2 * vf_exp + 1)) * t
    return e


def metrics(time_at_period, base_period_ps, **kw):
    t = sum(time_at_period.values())
    e = energy(time_at_period, base_period_ps, **kw)
    return {"time_ps": t, "energy": e, "edp": e * t, "ed2p": e * t * t}
