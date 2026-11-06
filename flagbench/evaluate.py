#!/usr/bin/env python3
"""Evaluate tuners by replaying against the LUTs (no gem5 runs).

Per kernel: ground truth (optimum, -O3, distribution), tuners x seeds x budgets, normalized score,
headroom captured, evaluations-to-target, plots, and full-factorial main/pairwise effects.
Aggregates across kernels + paired Wilcoxon (CARBS vs random) on per-kernel AUC.

Score(B) = (S_tuner(B) - E[S_random(B)]) / (S_opt - E[S_random(B)]),  S = speedup over -O3 = T_O3/T,
E[S_random(B)] exact (analytic, sampling without replacement). Headroom(B) = (S_tuner(B)-1)/(S_opt-1).
"""
import argparse, json, os, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from scipy import stats
import tuners as T
import flags as F

BUDGETS = [10, 20, 50, 100, 200]
TARGET = 0.9


def speedup(lut, cyc):
    return 0.0 if not np.isfinite(cyc) else lut.o3_cycles() / cyc


def run_traces(lut, name, seeds, bmax, **opts):
    f = T.TUNERS[name]
    out = np.full((seeds, bmax), np.inf)
    P = np.full((seeds, lut.n), np.nan)
    for s in range(seeds):
        if name == "carbs":
            tr, p = f(lut, bmax, s, return_state=True, **opts); P[s] = p
        else:
            tr = f(lut, bmax, s)
        tr = list(tr) + [tr[-1]] * (bmax - len(tr))  # budget beyond the space: nothing more to find
        out[s] = tr
    return out, P


def bootstrap_ci(x, stat=np.nanmedian, nb=1000, seed=0):
    rng = np.random.default_rng(seed)
    x = np.asarray(x, float)
    xs = x[~np.isnan(x)]
    if len(xs) == 0: return (np.nan, np.nan)
    bs = [stat(rng.choice(xs, len(xs))) for _ in range(nb)]
    return tuple(np.percentile(bs, [2.5, 97.5]))


def _resample(col, size, _rng=np.random.default_rng(2)):
    col = col[~np.isnan(col)]
    return _rng.choice(col, size) if len(col) else np.array([np.nan])


def factorial_effects(lut, thr):
    """Exact main effects / pairwise interactions on log(cycles) via OLS on +-1 coding (full factorial)."""
    n = lut.n
    ids = np.nonzero(lut.valid)[0]
    X = np.array([[(c >> j) & 1 for j in range(n)] for c in ids]) * 2 - 1
    cols = [np.ones(len(ids))] + [X[:, j] for j in range(n)]
    pairs = [(j, k) for j in range(n) for k in range(j + 1, n)]
    cols += [X[:, j] * X[:, k] for j, k in pairs]
    A = np.stack(cols, 1)
    y = np.log(lut.cycles[ids])
    beta, *_ = np.linalg.lstsq(A, y, rcond=None)
    main = 2 * beta[1:1 + n]                       # change in log(cycles) when flag goes off -> on
    inter = {f"{lut.names[j]}*{lut.names[k]}": 4 * beta[1 + n + i] for i, (j, k) in enumerate(pairs)}
    pct = np.expm1(main)                            # relative cycle change from turning the flag on
    cls = ["good" if p < -thr else "bad" if p > thr else "neutral" for p in pct]
    return main, inter, cls


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--luts", default=os.path.join(F.ROOT, "luts"))
    ap.add_argument("--out", default=os.path.join(F.ROOT, "results"))
    ap.add_argument("--kernels", nargs="+", default=None)
    ap.add_argument("--seeds", type=int, default=100)
    ap.add_argument("--budgets", type=int, nargs="+", default=BUDGETS)
    ap.add_argument("--eps", type=float, default=0.005, help="CARBS significance threshold (relative)")
    ap.add_argument("--eta", type=float, default=0.9)
    ap.add_argument("--h", type=int, default=5)
    ap.add_argument("--gamma", type=float, default=0.9)
    ap.add_argument("--k", type=int, default=None, help="top-k forced on (default max(1, round(0.1 N)))")
    ap.add_argument("--effect-thr", type=float, default=0.01)
    ap.add_argument("--min-headroom", type=float, default=0.001,
                    help="kernels whose best speedup over -O3 is below 1+this are controls: scores are not computed")
    ap.add_argument("--min-denom-frac", type=float, default=0.01,
                    help="score is undefined (NaN) at budgets where E[random] already captures > 1-this of the headroom")
    ap.add_argument("--no-hillclimb", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    kernels = a.kernels or sorted(f[:-4] for f in os.listdir(a.luts) if f.endswith(".csv"))
    bmax = max(a.budgets)
    names = ["carbs", "random"] + ([] if a.no_hillclimb else ["hillclimb"])
    copts = dict(eta=a.eta, h=a.h, gamma=a.gamma, eps=a.eps, k=a.k)
    report = {"params": {**copts, "seeds": a.seeds, "budgets": a.budgets, "effect_thr": a.effect_thr, "min_headroom": a.min_headroom, "min_denom_frac": a.min_denom_frac}, "kernels": {}}
    score_by = {}  # kernel -> tuner -> (seeds, nbudgets)

    for k in kernels:
        lut = T.LUT(os.path.join(a.luts, k + ".csv"))
        v = lut.cycles[lut.valid]
        s_opt = lut.o3_cycles() / v.min()
        erand = np.array(T.expected_random_best(lut, bmax, speedup=True))      # E[S_random(n)], n=1..bmax
        gt = {"n_configs": 1 << lut.n, "valid_configs": int(lut.valid.sum()), "unique_binaries": lut.meta["unique_binaries"],
              "o3_cycles": lut.o3_cycles(), "optimum_cycles": float(v.min()), "optimum_speedup": s_opt, "control_no_headroom": bool((s_opt - 1.0) < a.min_headroom),
              "worst_cycles": float(v.max()), "median_cycles": float(np.median(v)),
              "optimum_config_ids": [int(c) for c in np.nonzero(lut.cycles == v.min())[0][:10]],
              "o3_percentile_rank": float((v < lut.o3_cycles()).mean()),
              "expected_random_speedup": {str(b): erand[b - 1] for b in a.budgets}}
        rk = {"ground_truth": gt, "tuners": {}}
        score_by[k] = {}
        degenerate = (s_opt - 1.0) < a.min_headroom
        for t in names:
            tr, P = run_traces(lut, t, a.seeds, bmax, **(copts if t == "carbs" else {}))
            sp = np.vectorize(lambda c: speedup(lut, c))(tr)                   # (seeds, bmax) best-so-far speedup
            denom = s_opt - erand                                              # (bmax,)
            with np.errstate(invalid="ignore", divide="ignore"):
                score = np.where((denom[None, :] > a.min_denom_frac * (s_opt - 1.0)) & (not degenerate), (sp - erand[None, :]) / denom[None, :], np.nan)
                head = (sp - 1.0) / (s_opt - 1.0) if not degenerate else np.full_like(sp, np.nan)
            cols = [b - 1 for b in a.budgets]
            score_by[k][t] = score[:, cols]
            etarget = []
            for s in range(a.seeds):
                hit = np.nonzero(score[s] >= TARGET)[0]
                etarget.append(int(hit[0]) + 1 if len(hit) else None)
            reached = [e for e in etarget if e is not None]
            rk["tuners"][t] = {
                "score_median": {str(b): float(np.nanmedian(score[:, b - 1])) for b in a.budgets},
                "score_ci95": {str(b): [float(x) for x in bootstrap_ci(score[:, b - 1])] for b in a.budgets},
                "headroom_median": {str(b): float(np.nanmedian(head[:, b - 1])) for b in a.budgets},
                "speedup_median": {str(b): float(np.median(sp[:, b - 1])) for b in a.budgets},
                "evals_to_0.9": {"fraction_reached_within_%d" % bmax: len(reached) / a.seeds,
                                 "median_among_reached": float(np.median(reached)) if reached else None},
            }
            if t == "carbs":
                rk["_carbs_P"] = P
        # ground-truth factorial structure
        main_e, inter, cls = factorial_effects(lut, a.effect_thr)
        top_int = sorted(inter.items(), key=lambda kv: -abs(kv[1]))[:5]
        rk["flag_effects"] = {lut.names[j]: {"main_effect_pct": float(np.expm1(main_e[j]) * 100), "class": cls[j],
                                             "o3_default_on": bool(lut.meta["o3_defaults"][lut.names[j]])}
                              for j in range(lut.n)}
        rk["critical_flags"] = sum(c != "neutral" for c in cls)
        rk["top_pairwise_interactions_pct"] = [(n_, float(np.expm1(e) * 100)) for n_, e in top_int]
        rk["interaction_critical_pairs"] = int(sum(abs(np.expm1(e)) > a.effect_thr for e in inter.values()))
        P = rk.pop("_carbs_P")
        agree = {}
        for j, c in enumerate(cls):
            meanP = float(np.nanmean(P[:, j]))
            agree[lut.names[j]] = {"class": c, "carbs_final_P_mean@%d" % bmax: meanP,
                                   "agrees": (None if c == "neutral" else bool((meanP > 0.5) == (c == "good")))}
        rk["carbs_P_vs_ground_truth"] = agree
        rk["carbs_agreement"] = {"non_neutral_flags": rk["critical_flags"],
                                 "agree": sum(1 for x in agree.values() if x["agrees"] is True)}
        report["kernels"][k] = rk

        # per-kernel plot
        fig, ax = plt.subplots(figsize=(5.5, 3.8))
        for t in names:
            med = [report["kernels"][k]["tuners"][t]["score_median"][str(b)] for b in a.budgets]
            ci = np.array([report["kernels"][k]["tuners"][t]["score_ci95"][str(b)] for b in a.budgets])
            if np.all(np.isnan(med)): continue
            ax.plot(a.budgets, med, marker="o", label=t); ax.fill_between(a.budgets, ci[:, 0], ci[:, 1], alpha=0.2)
        if degenerate: ax.text(0.5, 0.5, "no headroom: score undefined", transform=ax.transAxes, ha="center")
        ax.axhline(0, color="gray", lw=0.7, ls=":"); ax.axhline(1, color="gray", lw=0.7, ls=":")
        ax.set_xscale("log"); ax.set_xlabel("budget (evaluations)"); ax.set_ylabel("normalized score (median)")
        ax.set_title(f"{k}  (opt speedup {s_opt:.3f}x)")
        if ax.get_legend_handles_labels()[0]: ax.legend(fontsize=8)
        fig.tight_layout()
        fig.savefig(os.path.join(a.out, f"score_{k}.png"), dpi=130); plt.close(fig)

    # ---- aggregate across kernels
    valid_k = [k for k in kernels if not np.all(np.isnan(score_by[k]["carbs"]))]
    skipped = [k for k in kernels if k not in valid_k]
    agg = {"kernels_used": valid_k, "kernels_skipped_no_headroom": skipped, "tuners": {}}
    rng = np.random.default_rng(1)
    fig, ax = plt.subplots(figsize=(5.8, 4))
    if not valid_k: ax.text(0.5, 0.5, "no kernel with headroom", transform=ax.transAxes, ha="center")
    for t in (names if valid_k else []):
        med = np.array([[np.nanmedian(score_by[k][t][:, i]) for i in range(len(a.budgets))] for k in valid_k])
        mean_med = med.mean(0)
        boots = []
        for _ in range(1000):
            boots.append(np.mean([[np.nanmedian(_resample(score_by[k][t][:, i], a.seeds))
                                   for i in range(len(a.budgets))] for k in valid_k], axis=0))
        ci = np.percentile(boots, [2.5, 97.5], axis=0)
        agg["tuners"][t] = {"mean_over_kernels_of_median_score": {str(b): float(m) for b, m in zip(a.budgets, mean_med)},
                            "ci95_over_seeds": {str(b): [float(ci[0, i]), float(ci[1, i])] for i, b in enumerate(a.budgets)}}
        ax.plot(a.budgets, mean_med, marker="o", label=t); ax.fill_between(a.budgets, ci[0], ci[1], alpha=0.2)
    ax.axhline(0, color="gray", lw=0.7, ls=":"); ax.set_xscale("log")
    ax.set_xlabel("budget (evaluations)"); ax.set_ylabel("score (mean over kernels of median over seeds)")
    ax.set_title("Aggregate")
    if valid_k: ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(os.path.join(a.out, "score_aggregate.png"), dpi=130); plt.close(fig)

    # paired Wilcoxon on per-kernel AUC (mean over budgets of median-over-seeds score)
    if not valid_k:
        report["aggregate"] = agg
        json.dump(report, open(os.path.join(a.out, "report.json"), "w"), indent=1, default=float)
        print("no kernel has headroom over -O3; nothing to aggregate"); return
    auc = {t: np.array([np.nanmean([np.nanmedian(score_by[k][t][:, i]) for i in range(len(a.budgets))]) for k in valid_k])
           for t in names}
    diff = auc["carbs"] - auc["random"]
    w = {"n_kernels": len(valid_k), "auc_carbs": auc["carbs"].tolist(), "auc_random": auc["random"].tolist(),
         "mean_auc_diff_carbs_minus_random": float(diff.mean()),
         "bootstrap_ci95_of_mean_diff_over_kernels": [float(x) for x in np.percentile(
             [rng.choice(diff, len(diff)).mean() for _ in range(5000)], [2.5, 97.5])] if len(diff) > 1 else None,
         "note": "With only %d kernels the Wilcoxon test has very low power (smallest attainable two-sided p = %.4f); "
                 "effect sizes and CIs are the primary result." % (len(valid_k), 2 / 2 ** max(len(valid_k), 1))}
    if len(valid_k) >= 2 and np.any(diff != 0):
        r = stats.wilcoxon(auc["carbs"], auc["random"])
        w["wilcoxon_statistic"], w["wilcoxon_p_two_sided"] = float(r.statistic), float(r.pvalue)
    agg["wilcoxon_carbs_vs_random_auc"] = w
    report["aggregate"] = agg
    json.dump(report, open(os.path.join(a.out, "report.json"), "w"), indent=1, default=float)

    # ---- markdown summary
    L = ["# flagbench results", "", f"params: {report['params']}", ""]
    L += ["| kernel | configs | unique bins | O3 cycles | opt cycles | opt speedup | O3 pct-rank | critical flags | CARBS P agrees |", "|---|---|---|---|---|---|---|---|---|"]
    for k in kernels:
        g = report["kernels"][k]["ground_truth"]; ag = report["kernels"][k]["carbs_agreement"]
        L.append(f"| {k} | {g['valid_configs']} | {g['unique_binaries']} | {g['o3_cycles']:.0f} | {g['optimum_cycles']:.0f} | "
                 f"{g['optimum_speedup']:.4f}x | {g['o3_percentile_rank']:.2f} | {ag['non_neutral_flags']} | {ag['agree']}/{ag['non_neutral_flags']} |")
    L += ["", "Median score (95% bootstrap CI over seeds) per budget:", ""]
    for k in kernels:
        L.append(f"**{k}**")
        for t in names:
            tt = report["kernels"][k]["tuners"][t]
            L.append(f"- {t}: " + ", ".join(f"B={b}: {tt['score_median'][str(b)]:.2f} [{tt['score_ci95'][str(b)][0]:.2f},{tt['score_ci95'][str(b)][1]:.2f}]" for b in a.budgets)
                     + f"; headroom@{a.budgets[-1]}={tt['headroom_median'][str(a.budgets[-1])]:.2f}; reached 0.9: {tt['evals_to_0.9']}")
        L.append("")
    L += ["Aggregate / Wilcoxon:", "", "```", json.dumps(agg, indent=1), "```"]
    open(os.path.join(a.out, "summary.md"), "w").write("\n".join(L))
    print("\n".join(L[:12])); print("wrote", a.out)


if __name__ == "__main__":
    main()
