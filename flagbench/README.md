# flagbench — fully enumerable compiler-flag benchmark on gem5 (for evaluating CARBS)

For each kernel: compile **every** combination of N GCC flags (2^N configs), simulate each *unique* binary
in gem5, store the results in a lookup table (LUT), then evaluate tuners by **replaying against the LUT**.
Every number in a LUT comes from a real gem5 run; nothing is imputed (failed/incorrect configs have empty cycles).

## Fixed choices (confirmed with the user)
| item | value |
|---|---|
| ISA / mode | X86, SE mode, static binaries |
| compiler | `gcc` 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1), recorded in every LUT JSON |
| base flags | `-O3 -march=x86-64` (baseline SSE2; gem5's x86 model has little/no AVX) |
| flags | N=12 (configurable, max 16), see `flags.py` (`DEFAULT_ORDER`); each is `-fX` (bit=1) / `-fno-X` (bit=0) |
| CPU | 1× DerivO3CPU (stdlib `CPUTypes.O3` defaults: 8-wide, 192 ROB, 32 LQ/SQ), 2 GHz |
| caches | L1I 32 KiB 8-way, L1D 32 KiB 8-way, L2 256 KiB 16-way (latencies 1/1/10 cycles; stdlib defaults, dumped in gem5 `config.json`) |
| memory | SingleChannelDDR3_1600, 1 GiB |
| ROI | `m5_work_begin/m5_work_end`; stdlib resets stats on begin and dumps on end → the **first** stats block of `stats.txt` is the ROI |

Default 12 flags: unroll-loops, peel-loops, tree-loop-vectorize, tree-slp-vectorize, inline-functions, ipa-cp-clone,
unswitch-loops, predictive-commoning, tracer, schedule-insns, ivopts, split-loops. `-O3` defaults are read from
`gcc -Q --help=optimizers -O3`, **except `-funroll-loops`**: `-Q` reports it enabled, but plain `gcc -O3` generates the
code of an explicit `-fno-unroll-loops` (verified by `.text` hash comparison, `check_o3_defaults.py`; override in
`flags.O3_DEFAULT_OVERRIDES`). `build_lut.py` asserts that the recorded -O3 config is byte-identical to plain `gcc -O3`. **`-fprefetch-loop-arrays` was dropped**: with `-march=x86-64` gcc reports no state
for it (unsupported on the target → silent no-op). Flags that produce any compiler warning are rejected.
Plain `-O3` = `o3_config_id` in each LUT JSON (3326 for the default 12 flags).

## Kernels (`kernels/`)
`k_stencil` (5-pt Jacobi), `k_ptrchase` (list > L2; control), `k_branchy` (bytecode interpreter), `k_reduce`
(reduction + prefix sum, array > L2), `k_crc32` (bitwise), `k_helper` (hot loop calling small helpers).
Fixed-seed in-program inputs, no I/O, only the kernel is inside the ROI, checksum printed after. ROI sizes at `-O3`:
1.4–3.1 M instructions (`k_ptrchase`: 1.5 M instructions but ~46 M cycles).
Correctness = simulated checksum equals the checksum of a native `-O0` reference build (`-DNO_M5OPS`).

## Commands
```bash
# one-time: m5ops library for X86 (needed to link m5_work_begin/end)
(cd ../util/m5 && scons build/x86/out/m5)

./run_sanity.sh k_stencil          # end-to-end check: N=4, one kernel, tiny budgets (~1 min)
python3 build_and_hash.py -n 12    # 1. compile all 2^N configs x 6 kernels, hash .text, keep 1 binary/hash
python3 simulate.py                # 2. gem5 once per unique hash + -O0 reference + determinism re-runs (5/kernel)
python3 build_lut.py               # 3. -> luts/<kernel>.csv + luts/<kernel>.json
python3 test_tuners.py             #    unit tests (analytic random baseline vs brute force, CARBS invariants)
python3 evaluate.py                # 4. replay tuners -> results/{summary.md,report.json,score_*.png}
```
All stages are resumable (they skip configs/hashes already in `work/<kernel>/*.jsonl`) and parallel (`-j`).
`evaluate.py` options: `--seeds 100 --budgets 10 20 50 100 200 --eps 0.005 --eta 0.9 --h 5 --gamma 0.9 --k <int>
--effect-thr 0.01 --min-headroom 0.001 --min-denom-frac 0.01 --no-hillclimb`.

## Pipeline notes
* **Dedup hash** = SHA-256 of the raw `.text` section (`objcopy -O binary -j .text`) of the static binary.
* LUT CSV columns: `config_id, f_<flag>... (0/1), binary_hash, compiled_ok, correct, cycles, instructions, ipc`.
* Determinism: 5 random binaries per kernel re-simulated; `simulate.py` exits non-zero if any cycle count differs
  (all 30 re-runs were identical).
* Measured time (this machine, 24 cores, ~12 sims in parallel): one simulation takes 4.5–7 s for crc32/helper/
  stencil/reduce, ~18 s for branchy, ~33 s for ptrchase. Unique binaries: stencil 40, ptrchase 20, branchy 26,
  reduce 12, crc32 8, helper 30 (136 total) ⇒ ≈ 30 CPU-minutes, ~3 min wall at 22 workers. Compiling 24 576
  binaries took ~2 min. Estimate for another configuration: `Σ_kernels unique_binaries × per_run_seconds / workers`.

## Tuners (`tuners.py`)
* **CARBS** (Algorithm 1 as specified): P=0.5; O3 evaluated first (counts toward budget); top-k forced on
  (`k=max(1,round(0.1N))`; the paper's k=10 assumes ~260 flags); Case 1/2 updates against the last h configs with
  rate `eta*gamma^(i-1)`; significance = relative difference > `eps` (default 0.5%) instead of a t-test; P clipped to [0,1].
  Defaults eta=0.9, h=5, gamma=0.9 (gamma is not stated in the paper). **Assumption:** the Eq. 4/5 formulas were not
  available, so signs follow the reference code in `../carbs/optimizers.py` (not-better config pushes agreeing-enabled
  flags down, agreeing-disabled flags up). That released code also differs from the described algorithm (extra
  "better than best" branch, `lr^(i-1)` rate, P clipped to [0.1,0.9]); the spec was followed, not the code.
  Failed/incorrect configs consume budget and are not used for updates. Repeats are not memoised.
* **Random** (uniform without replacement; the -O3 config is *not* given for free) and **exact expected random
  best-of-n** (analytic over sorted LUT values, in cycles and in speedup; unit-tested vs brute force).
* **Hill climber** (first-improvement one-bit flips, random restart at a local optimum).
* Traces are prefix-consistent, so one 200-eval run per seed gives every smaller budget.

## Evaluation definitions
Speedup S = T_O3 / T. Score(B) = (S_tuner − E[S_random(B)]) / (S_opt − E[S_random(B)]); headroom(B) = (S_tuner − 1)/(S_opt − 1).
Score is **undefined (NaN)** where E[random] already captures > 99% of the headroom (`--min-denom-frac`), and kernels with
optimum speedup < 1.001 are reported as controls and excluded from scoring/aggregates (`--min-headroom`).
Bootstrap CIs resample seeds; the aggregate is the mean over kernels of per-kernel medians. Wilcoxon is a paired
test on per-kernel AUC (CARBS vs random); with 3–4 kernels it has almost no power (smallest attainable two-sided p is 0.25 for 3 kernels,
0.125 for 4), so effect sizes/CIs are the primary result. Flag effects: OLS on ±1-coded
full-factorial data for log(cycles) (main effects = change when a flag goes off→on; pairwise interactions = difference
of differences), classified good/bad/neutral at ±1%.

## Findings

### Original six kernels (default 12 flags, `luts/`, `results/`): too easy
(Numbers corrected after the -O3 / `-funroll-loops` fix; an earlier version of this README overstated `k_helper`'s
headroom (1.48x) because the reference "-O3" accidentally included `-funroll-loops`.)
* Only 8–40 of the 4096 configs per kernel yield distinct binaries and only 1–5 flags are non-neutral (±1%).
  6–12.5% of configs sit exactly at the optimum. Optimum speedups over -O3: reduce 1.57x, stencil 1.08x, ptrchase 1.011x,
  branchy 1.010x, helper 1.0001x, crc32 1.0000x (the last two are controls).
* Random search captures >=98% of the headroom by B=50 on every kernel with headroom, so CARBS and random are
  indistinguishable at B>=20. These kernels cannot discriminate tuners.

### `k_mix`: a kernel built so that random search is NOT optimal (`luts_mix/`, `results_mix/`)
The paper (Sec. 3.8) says a uniform random trial succeeds with probability 0.5^(|G|+|B|); to defeat random search the
kernel needs many critical flags, few don't-cares, conflicts and interactions. `kernels/k_mix.c` is one region of
interest made of 8 phases, each sensitive to specific flags (stencil, reduction, hot helper, bitwise CRC, loop-invariant
branch, constant-argument callee, column-order loops, runtime-stride loop). Flags: unroll-loops, unroll-all-loops,
tree-loop-vectorize, tree-slp-vectorize, ipa-cp-clone, unswitch-loops, predictive-commoning, tracer, schedule-insns,
ivopts, loop-interchange, version-loops-for-strides (chosen by `screen.py` one-at-a-time screening; peel-loops,
split-loops, gcse-after-reload and sched-pressure had no code/cycle effect on any of 15 kernels and were dropped).
Measured on the full factorial (4096 configs, 1120 unique binaries, ROI 2.6 M cycles / 4.9 M instructions at -O3):
* optimum 1.243x over plain -O3 (worst config 0.42x); **8 configs (0.2%) are optimal, 24 (0.6%) within 1%**.
* The optimum needs: unroll-loops ON (-O3 default off), tracer ON (off), schedule-insns ON (off), slp-vectorize OFF
  (default on), predictive-commoning OFF (on), loop-interchange OFF (on), and vectorize/ipa-cp-clone/unswitch/ivopts/
  versioning-for-strides ON. Only 3 flags are true don't-cares (unroll-all-loops, slp-vectorize, predictive-commoning
  in the sense that all 8 optimal configs vary only in those bits).
* 8/12 flags are critical by main effect (>1%); 23 pairwise interactions exceed 1% (e.g. vectorize x
  versioning-for-strides -28%, unroll-loops x ivopts -17%, unroll-loops x vectorize +15%).
* Random search needs ~200 draws for only a 32% chance to hit the optimum; E[random best speedup] = 1.057 / 1.117 /
  1.174 / 1.206 / 1.228 at B = 10/20/50/100/200.
* Replay results (100 seeds, median score, 95% CIs in `results_mix/summary.md`): CARBS 0.23 / 0.47 / 1.00 / 1.00 / 1.00
  at B = 10/20/50/100/200; random -0.04 / -0.07 / -0.16 / 0.71 / 0.80; one-bit hill climber -0.08 / 0.97 / 1.00 / 1.00 / 1.00.
  CARBS reaches the 0.9 score in a median of 27 evaluations vs 51 for random (and 18.5 for hill climbing); CARBS's
  final P agrees with the ground-truth good/bad classification on 8/8 critical flags.
* Caveats: this is ONE hand-designed kernel (n=1, so no meaningful across-kernel test); it was engineered to be hard
  for random search, so it shows the tuners behave as the paper predicts on a space with many critical flags, not that
  the effect holds on real code. The hill climber beats CARBS at B=20 on this kernel.

Commands for `k_mix` (separate work/LUT dirs; flags differ from the default 12):
```bash
FL="unroll-loops unroll-all-loops tree-loop-vectorize tree-slp-vectorize ipa-cp-clone unswitch-loops predictive-commoning tracer schedule-insns ivopts loop-interchange version-loops-for-strides"
python3 build_and_hash.py --work work_mix --kernels k_mix --flags $FL
python3 simulate.py --work work_mix --kernels k_mix
python3 build_lut.py --work work_mix --out luts_mix --kernels k_mix
python3 evaluate.py --luts luts_mix --out results_mix
# one-at-a-time flag screening for designing kernels:  python3 screen.py kernels/k_mix.c --flags $FL
```
Simulation took ~1 h wall for 1120 binaries (22 workers). Run it detached (`setsid nohup ...`) so it survives the shell;
it is resumable.
