# flagbench results

params: {'eta': 0.9, 'h': 5, 'gamma': 0.9, 'eps': 0.005, 'k': None, 'seeds': 100, 'budgets': [10, 20, 50, 100, 200], 'effect_thr': 0.01, 'min_headroom': 0.001, 'min_denom_frac': 0.01}

| kernel | configs | unique bins | O3 cycles | opt cycles | opt speedup | O3 pct-rank | critical flags | CARBS P agrees |
|---|---|---|---|---|---|---|---|---|
| k_branchy | 4096 | 26 | 4041332 | 3999843 | 1.0104x | 0.38 | 1 | 1/1 |
| k_crc32 | 4096 | 8 | 1188179 | 1188179 | 1.0000x | 0.00 | 2 | 1/2 |
| k_helper | 4096 | 30 | 1081450 | 1081359 | 1.0001x | 0.12 | 2 | 2/2 |
| k_ptrchase | 4096 | 20 | 46649626 | 46145285 | 1.0109x | 0.75 | 1 | 1/1 |
| k_reduce | 4096 | 12 | 997243 | 633813 | 1.5734x | 0.50 | 4 | 4/4 |
| k_stencil | 4096 | 40 | 886873 | 820101 | 1.0814x | 0.38 | 5 | 4/5 |

Median score (95% bootstrap CI over seeds) per budget:

**k_branchy**
- carbs: B=10: 1.00 [1.00,1.00], B=20: 1.00 [1.00,1.00], B=50: nan [nan,nan], B=100: nan [nan,nan], B=200: nan [nan,nan]; headroom@200=1.00; reached 0.9: {'fraction_reached_within_200': 0.99, 'median_among_reached': 7.0}
- random: B=10: 1.00 [1.00,1.00], B=20: 1.00 [1.00,1.00], B=50: nan [nan,nan], B=100: nan [nan,nan], B=200: nan [nan,nan]; headroom@200=1.00; reached 0.9: {'fraction_reached_within_200': 0.98, 'median_among_reached': 6.0}
- hillclimb: B=10: -2.01 [-2.01,1.00], B=20: 1.00 [1.00,1.00], B=50: nan [nan,nan], B=100: nan [nan,nan], B=200: nan [nan,nan]; headroom@200=1.00; reached 0.9: {'fraction_reached_within_200': 1.0, 'median_among_reached': 12.0}

**k_crc32**
- carbs: B=10: nan [nan,nan], B=20: nan [nan,nan], B=50: nan [nan,nan], B=100: nan [nan,nan], B=200: nan [nan,nan]; headroom@200=nan; reached 0.9: {'fraction_reached_within_200': 0.0, 'median_among_reached': None}
- random: B=10: nan [nan,nan], B=20: nan [nan,nan], B=50: nan [nan,nan], B=100: nan [nan,nan], B=200: nan [nan,nan]; headroom@200=nan; reached 0.9: {'fraction_reached_within_200': 0.0, 'median_among_reached': None}
- hillclimb: B=10: nan [nan,nan], B=20: nan [nan,nan], B=50: nan [nan,nan], B=100: nan [nan,nan], B=200: nan [nan,nan]; headroom@200=nan; reached 0.9: {'fraction_reached_within_200': 0.0, 'median_among_reached': None}

**k_helper**
- carbs: B=10: nan [nan,nan], B=20: nan [nan,nan], B=50: nan [nan,nan], B=100: nan [nan,nan], B=200: nan [nan,nan]; headroom@200=nan; reached 0.9: {'fraction_reached_within_200': 0.0, 'median_among_reached': None}
- random: B=10: nan [nan,nan], B=20: nan [nan,nan], B=50: nan [nan,nan], B=100: nan [nan,nan], B=200: nan [nan,nan]; headroom@200=nan; reached 0.9: {'fraction_reached_within_200': 0.0, 'median_among_reached': None}
- hillclimb: B=10: nan [nan,nan], B=20: nan [nan,nan], B=50: nan [nan,nan], B=100: nan [nan,nan], B=200: nan [nan,nan]; headroom@200=nan; reached 0.9: {'fraction_reached_within_200': 0.0, 'median_among_reached': None}

**k_ptrchase**
- carbs: B=10: nan [nan,nan], B=20: nan [nan,nan], B=50: nan [nan,nan], B=100: nan [nan,nan], B=200: nan [nan,nan]; headroom@200=1.00; reached 0.9: {'fraction_reached_within_200': 1.0, 'median_among_reached': 2.0}
- random: B=10: nan [nan,nan], B=20: nan [nan,nan], B=50: nan [nan,nan], B=100: nan [nan,nan], B=200: nan [nan,nan]; headroom@200=1.00; reached 0.9: {'fraction_reached_within_200': 0.85, 'median_among_reached': 2.0}
- hillclimb: B=10: nan [nan,nan], B=20: nan [nan,nan], B=50: nan [nan,nan], B=100: nan [nan,nan], B=200: nan [nan,nan]; headroom@200=1.00; reached 0.9: {'fraction_reached_within_200': 0.6, 'median_among_reached': 1.0}

**k_reduce**
- carbs: B=10: 1.00 [0.01,1.00], B=20: 1.00 [1.00,1.00], B=50: nan [nan,nan], B=100: nan [nan,nan], B=200: nan [nan,nan]; headroom@200=1.00; reached 0.9: {'fraction_reached_within_200': 1.0, 'median_among_reached': 9.0}
- random: B=10: 1.00 [0.01,1.00], B=20: 1.00 [1.00,1.00], B=50: nan [nan,nan], B=100: nan [nan,nan], B=200: nan [nan,nan]; headroom@200=1.00; reached 0.9: {'fraction_reached_within_200': 0.93, 'median_among_reached': 9.0}
- hillclimb: B=10: 0.01 [-1.76,1.00], B=20: 1.00 [1.00,1.00], B=50: nan [nan,nan], B=100: nan [nan,nan], B=200: nan [nan,nan]; headroom@200=1.00; reached 0.9: {'fraction_reached_within_200': 0.97, 'median_among_reached': 12.0}

**k_stencil**
- carbs: B=10: 0.80 [0.80,1.00], B=20: 1.00 [1.00,1.00], B=50: nan [nan,nan], B=100: nan [nan,nan], B=200: nan [nan,nan]; headroom@200=1.00; reached 0.9: {'fraction_reached_within_200': 0.96, 'median_among_reached': 6.0}
- random: B=10: 0.80 [0.80,0.80], B=20: 1.00 [1.00,1.00], B=50: nan [nan,nan], B=100: nan [nan,nan], B=200: nan [nan,nan]; headroom@200=1.00; reached 0.9: {'fraction_reached_within_200': 0.93, 'median_among_reached': 6.0}
- hillclimb: B=10: -2.20 [-2.40,0.80], B=20: 1.00 [1.00,1.00], B=50: nan [nan,nan], B=100: nan [nan,nan], B=200: nan [nan,nan]; headroom@200=1.00; reached 0.9: {'fraction_reached_within_200': 0.92, 'median_among_reached': 14.0}

Aggregate / Wilcoxon:

```
{
 "kernels_used": [
  "k_branchy",
  "k_reduce",
  "k_stencil"
 ],
 "kernels_skipped_no_headroom": [
  "k_crc32",
  "k_helper",
  "k_ptrchase"
 ],
 "tuners": {
  "carbs": {
   "mean_over_kernels_of_median_score": {
    "10": 0.9323250755032536,
    "20": 1.0,
    "50": NaN,
    "100": NaN,
    "200": NaN
   },
   "ci95_over_seeds": {
    "10": [
     0.6010608876745691,
     1.0
    ],
    "20": [
     1.0,
     1.0
    ],
    "50": [
     NaN,
     NaN
    ],
    "100": [
     NaN,
     NaN
    ],
    "200": [
     NaN,
     NaN
    ]
   }
  },
  "random": {
   "mean_over_kernels_of_median_score": {
    "10": 0.9323250755032536,
    "20": 1.0,
    "50": NaN,
    "100": NaN,
    "200": NaN
   },
   "ci95_over_seeds": {
    "10": [
     0.6010608876745691,
     0.9323250755032536
    ],
    "20": [
     1.0,
     1.0
    ],
    "50": [
     NaN,
     NaN
    ],
    "100": [
     NaN,
     NaN
    ],
    "200": [
     NaN,
     NaN
    ]
   }
  },
  "hillclimb": {
   "mean_over_kernels_of_median_score": {
    "10": -1.398903565800478,
    "20": 1.0,
    "50": NaN,
    "100": NaN,
    "200": NaN
   },
   "ci95_over_seeds": {
    "10": [
     -1.9880911552996878,
     -0.06759243493782441
    ],
    "20": [
     1.0,
     1.0
    ],
    "50": [
     NaN,
     NaN
    ],
    "100": [
     NaN,
     NaN
    ],
    "200": [
     NaN,
     NaN
    ]
   }
  }
 },
 "wilcoxon_carbs_vs_random_auc": {
  "n_kernels": 3,
  "auc_carbs": [
   1.0,
   1.0,
   0.8984876132548804
  ],
  "auc_random": [
   1.0,
   1.0,
   0.8984876132548804
  ],
  "mean_auc_diff_carbs_minus_random": 0.0,
  "bootstrap_ci95_of_mean_diff_over_kernels": [
   0.0,
   0.0
  ],
  "note": "With only 3 kernels the Wilcoxon test has very low power (smallest attainable two-sided p = 0.2500); effect sizes and CIs are the primary result."
 }
}
```