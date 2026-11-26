# knobbed_bp: a tunable branch direction predictor

The branch-predictor counterpart of the [knobbed prefetcher](../../prefetcher/knobbed/README.md).
One ChampSim module whose behaviour is set by a small hint word. The knobs come from the
Instruction Flow lectures (Parts 3-5). It replaces ChampSim's direction predictor
(`branch_predictor` in the config); the BTB is a separate module and is not covered.

## Build and run

```bash
KNOBBED_BUILD=~/champsim-knobbed-bp KNOBBED_CONFIG=$PWD/champsim/champsim_config_knobbed_bp.json \
  champsim/build_knobbed.sh linux                  # -> ~/champsim-knobbed-bp/bin/champsim_knobbed_bp
KNOBBED_BP_HINT="engine=tage,level=5,loop=1,hybrid=1" \
  ~/champsim-knobbed-bp/bin/champsim_knobbed_bp --warmup-instructions 500000 --simulation-instructions 2000000 trace.xz
```

`champsim_config_knobbed_bp.json` is the no-prefetch config with `branch_predictor = knobbed_bp`.
ChampSim's own `Branch Prediction Accuracy` / `MPKI` lines cover the measured window.

## Knobs (`KNOBBED_BP_HINT="key=value,..."`)

| Key | Values | Lecture idea |
|---|---|---|
| `engine` | `bimodal`, `gshare`, `perceptron`, `tage` | BHT of saturating counters; correlated two-level (hash of PC and history); perceptron weight table; TAGE (4 partially tagged tables, geometric history lengths, usefulness bits, alt-prediction when the provider is weak, allocate a longer-history entry on a mispredict) |
| `history` | `global`, `path`, `local`, `imli` | BHR of all outcomes; path history (1 bit per branch from its PC and outcome); per-branch local history (max 64 bits); global history + innermost-loop iteration count (multidimensional history) |
| `level` | 0-7 | History length ladder (below) |
| `counter` | 1, 2 | 1-bit vs 2-bit saturating counters (bimodal, gshare, TAGE base; TAGE tagged counters get one extra bit; perceptron ignores it) |
| `fold` | `0` / `1` | XOR-fold history longer than the index vs. truncate it |
| `hybrid` | `0` / `1` | Tournament selector choosing between the engine and a plain bimodal |
| `loop` | `0` / `1` | Loop-count predictor that overrides the engine for branches with a confident trip count |
| `word` | e.g. `word=0x4a5` | Whole 11-bit word: `[1:0]` engine `[3:2]` history `[6:4]` level `[7]` counter-1 `[8]` loop `[9]` hybrid `[10]` fold |

History length ladder (bits): gshare 2,4,6,8,10,12,16,24; perceptron 4,8,12,16,24,32,48,64;
TAGE (longest table; the other three are geometric from 2) 8,12,16,24,32,48,64,128. Bimodal ignores `level`/`history`.
Every field uses all its codes, so there are no aliased words (except configs that cannot differ, e.g. `hybrid` with `bimodal`).

## Regions (PC ranges)

`KNOBBED_BP_REGIONS=<file>`: one region per line, `<start> <end> <key=value,...>` (branch PC byte addresses,
`#` comments). A region starts from the default hint and overrides only the keys it lists; later regions win.
History registers are shared across regions; prediction tables are private to each region.

## Output

At exit, one line per region: branches, mispredicts, rate, loop overrides/correct, hybrid selector choices/correct.
ChampSim has no end-of-run hook for branch predictors, so this is printed from the destructor and **covers the
whole run including warmup**.

## Implementation notes / caveats

- ChampSim calls `predict_branch` for every instruction, not just branches. A small "seen as a branch" filter skips
  the work for instructions that have never been a branch, so the first dynamic occurrence of each static branch is
  predicted not-taken (it is still trained).
- Only conditional branches train the tables; every branch type feeds the global and path histories.
- Sizes are fixed: 4096 counters, 512 perceptron rows, TAGE 4 x 1024, loop table 64, 1024 local histories.
- Not covered: BTB, return address stack, branch-resolution techniques (PUBS, decomposed branches, control-flow decoupling).
- Quick check (omnetpp_s-141B, 2M instructions): mispredict rate bimodal 8.6%, gshare 7.1%, perceptron 5.1%,
  TAGE+loop+hybrid 4.0%.
