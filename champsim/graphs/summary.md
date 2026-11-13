# flagbench results

params: {'eta': 0.9, 'h': 5, 'gamma': 0.9, 'eps': 0.005, 'k': None, 'seeds': 10, 'budgets': [2, 4], 'effect_thr': 0.01, 'min_headroom': 0.0, 'min_denom_frac': 0.0}

| kernel | configs | unique bins | O3 cycles | opt cycles | opt speedup | O3 pct-rank | critical flags | CARBS P agrees |
|---|---|---|---|---|---|---|---|---|
| k_stencil | 4 | 4 | 21706 | 21706 | 1.0000x | 0.00 | 0 | 0/0 |

Median score (95% bootstrap CI over seeds) per budget:

**k_stencil**
- carbs: B=2: 1.00 [1.00,1.00], B=4: nan [nan,nan]; headroom@4=nan; reached 0.9: {'fraction_reached_within_4': 1.0, 'median_among_reached': 1.0}
- random: B=2: 1.00 [-0.88,1.00], B=4: nan [nan,nan]; headroom@4=nan; reached 0.9: {'fraction_reached_within_4': 0.8, 'median_among_reached': 2.0}
- hillclimb: B=2: 0.06 [-1.23,1.00], B=4: nan [nan,nan]; headroom@4=-inf; reached 0.9: {'fraction_reached_within_4': 0.8, 'median_among_reached': 2.0}

Aggregate / Wilcoxon:

```
{
 "kernels_used": [
  "k_stencil"
 ],
 "kernels_skipped_no_headroom": [],
 "tuners": {
  "carbs": {
   "mean_over_kernels_of_median_score": {
    "2": 1.0,
    "4": NaN
   },
   "ci95_over_seeds": {
    "2": [
     1.0,
     1.0
    ],
    "4": [
     NaN,
     NaN
    ]
   }
  },
  "random": {
   "mean_over_kernels_of_median_score": {
    "2": 1.0,
    "4": NaN
   },
   "ci95_over_seeds": {
    "2": [
     -0.8825464678995968,
     1.0
    ],
    "4": [
     NaN,
     NaN
    ]
   }
  },
  "hillclimb": {
   "mean_over_kernels_of_median_score": {
    "2": 0.058726766050201606,
    "4": NaN
   },
   "ci95_over_seeds": {
    "2": [
     -1.23490706420393,
     1.0
    ],
    "4": [
     NaN,
     NaN
    ]
   }
  }
 },
 "wilcoxon_carbs_vs_random_auc": {
  "n_kernels": 1,
  "auc_carbs": [
   1.0
  ],
  "auc_random": [
   1.0
  ],
  "mean_auc_diff_carbs_minus_random": 0.0,
  "bootstrap_ci95_of_mean_diff_over_kernels": null,
  "note": "With only 1 kernels the Wilcoxon test has very low power (smallest attainable two-sided p = 1.0000); effect sizes and CIs are the primary result."
 }
}
```