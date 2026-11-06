# flagbench results

params: {'eta': 0.9, 'h': 5, 'gamma': 0.9, 'eps': 0.005, 'k': None, 'seeds': 100, 'budgets': [10, 20, 50, 100, 200], 'effect_thr': 0.01, 'min_headroom': 0.001, 'min_denom_frac': 0.01}

| kernel | configs | unique bins | O3 cycles | opt cycles | opt speedup | O3 pct-rank | critical flags | CARBS P agrees |
|---|---|---|---|---|---|---|---|---|
| k_mix | 4096 | 1120 | 2597309 | 2090135 | 1.2427x | 0.12 | 8 | 8/8 |

Median score (95% bootstrap CI over seeds) per budget:

**k_mix**
- carbs: B=10: 0.23 [0.19,0.41], B=20: 0.47 [0.27,0.88], B=50: 1.00 [0.96,1.00], B=100: 1.00 [1.00,1.00], B=200: 1.00 [1.00,1.00]; headroom@200=1.00; reached 0.9: {'fraction_reached_within_200': 1.0, 'median_among_reached': 27.0}
- random: B=10: -0.04 [-0.12,0.08], B=20: -0.07 [-0.16,0.13], B=50: -0.16 [-0.33,0.04], B=100: 0.71 [-0.02,0.85], B=200: 0.80 [0.80,0.80]; headroom@200=0.99; reached 0.9: {'fraction_reached_within_200': 0.61, 'median_among_reached': 51.0}
- hillclimb: B=10: -0.08 [-0.32,0.19], B=20: 0.97 [0.70,0.98], B=50: 1.00 [1.00,1.00], B=100: 1.00 [1.00,1.00], B=200: 1.00 [1.00,1.00]; headroom@200=1.00; reached 0.9: {'fraction_reached_within_200': 1.0, 'median_among_reached': 18.5}

Aggregate / Wilcoxon:

```
{
 "kernels_used": [
  "k_mix"
 ],
 "kernels_skipped_no_headroom": [],
 "tuners": {
  "carbs": {
   "mean_over_kernels_of_median_score": {
    "10": 0.22932186842033797,
    "20": 0.4737487194788959,
    "50": 1.0,
    "100": 1.0,
    "200": 1.0
   },
   "ci95_over_seeds": {
    "10": [
     0.1912358815272684,
     0.39539713293175055
    ],
    "20": [
     0.2679846036602258,
     0.8767074274559048
    ],
    "50": [
     0.9561183832146511,
     1.0
    ],
    "100": [
     1.0,
     1.0
    ],
    "200": [
     1.0,
     1.0
    ]
   }
  },
  "random": {
   "mean_over_kernels_of_median_score": {
    "10": -0.041593836344098606,
    "20": -0.06534473707368119,
    "50": -0.16062522013253158,
    "100": 0.7131361305804358,
    "200": 0.7963014109712673
   },
   "ci95_over_seeds": {
    "10": [
     -0.12104670990643732,
     0.10118097417541846
    ],
    "20": [
     -0.16255382480859043,
     0.1349864997622099
    ],
    "50": [
     -0.33286953799228186,
     0.17267473127707952
    ],
    "100": [
     -0.021413833520118258,
     0.8468064449292988
    ],
    "200": [
     0.7963014109712673,
     0.7963014109712673
    ]
   }
  },
  "hillclimb": {
   "mean_over_kernels_of_median_score": {
    "10": -0.08176787103957354,
    "20": 0.965548590216429,
    "50": 1.0,
    "100": 1.0,
    "200": 1.0
   },
   "ci95_over_seeds": {
    "10": [
     -0.3656863488131138,
     0.1657108189610328
    ],
    "20": [
     0.8767074274559048,
     0.9760105982496674
    ],
    "50": [
     1.0,
     1.0
    ],
    "100": [
     1.0,
     1.0
    ],
    "200": [
     1.0,
     1.0
    ]
   }
  }
 },
 "wilcoxon_carbs_vs_random_auc": {
  "n_kernels": 1,
  "auc_carbs": [
   0.7406141175798467
  ],
  "auc_random": [
   0.24837474960027833
  ],
  "mean_auc_diff_carbs_minus_random": 0.4922393679795684,
  "bootstrap_ci95_of_mean_diff_over_kernels": null,
  "note": "With only 1 kernels the Wilcoxon test has very low power (smallest attainable two-sided p = 1.0000); effect sizes and CIs are the primary result."
 }
}
```