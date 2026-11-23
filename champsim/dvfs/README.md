# DVFS hint knobs

Six knobs decide where a compiler pass boosts the core clock and how aggressively; CARBS tunes them to
minimize ED² (energy × delay²) against an unhinted baseline. This is a simulation experiment: the hints are
no-ops on real hardware (see [Scope](#scope)).

Three pieces fit together:

- **Pass** (`fp_boost_pass.py`): reads `gcc -S` output, finds FP-heavy loops, and wraps them in clock-boost
  hints (a harmless `nopl` on real hardware).
- **ChampSim** (branch `clock`, `champsim/source-linux`): turns each hint into a mid-run core frequency
  change, with a transition stall and an optional dwell governor.
- **Tuner** (`dvfs_carbs.py`): searches the 13-bit knob space (8,192 configs) with CARBS, random search and
  hill climbing.

## Tuned knobs

CARBS searches these six knobs, 13 bits in all. Ordered fields are Gray coded, so neighbouring values differ
by one bit.

| Knob | Bits | Values | What it does | Needs new trace? |
| --- | --- | --- | --- | --- |
| `boost` | 2 | off, 1.25x, 1.5x, 2x core frequency | How far to raise the clock inside a boosted region; "off" is the baseline | No (set at simulation time) |
| `fp_min` | 3 | 0.05, 0.10, 0.15, 0.20, 0.30, 0.40, 0.50, 0.70 | Minimum fraction of a loop's instructions that are FP arithmetic for it to be boosted | Yes |
| `mem_max` | 2 | 0.30, 0.45, 0.60, 0.90 | Maximum fraction of loop instructions with a memory operand; memory-heavy loops are skipped because DRAM time does not shrink with a faster core | Yes |
| `hoist` | 2 | 0.02, 0.05, 0.15, never | Climb from a qualifying loop to enclosing loops with at least this FP density; prevents clock thrashing | Yes |
| `min_size` | 2 | 4, 8, 16, 32 instructions | Minimum static loop size, a rough proxy for region length | Yes |
| `dwell` | 2 | 0, 5, 20, 50 µs | Governor: holds a clock request that arrives too soon after the last change, so short regions get skipped | No (set at simulation time) |

The pass always emits hint level 1. The boost frequency is applied at simulation time through
`CHAMPSIM_DVFS_PERIODS_PS`, and traces are cached by the hash of the hinted assembly, so pass-knob
combinations that give identical code share one trace.

## Fixed settings

These are not tuned; change them from the command line or environment for sensitivity runs.

| Setting | Default | Where to change it |
| --- | --- | --- |
| Transition penalty (fetch stall per clock change) | 2 µs | `--penalty-ns` or `CHAMPSIM_DVFS_PENALTY_NS` |
| Static power fraction at base clock | 0.3 | `--static-frac` |
| Objective metric | `ed2p` | `--metric ed2p`, `edp` or `time` |
| Compiler optimization level | `-O2` | `--opt` |
| Base clock period | 500 ps (2 GHz) | `--base-period-ps` in `dvfs_eval.py`; fixed in the CARBS script |
| Trace window and simulation length | 6M traced, 5M simulated | `--trace-count`, `--sim` |
| Level-to-period table | 500, 400, 300, 250 ps | `CHAMPSIM_DVFS_PERIODS_PS` (the tuner sets it per run) |
| Dwell governor (ChampSim) | 0 (off) | `CHAMPSIM_DVFS_MIN_DWELL_NS` |

## Not tunable yet

- **Lowering the clock in memory-bound regions.** Every level is at or above base. A static pass cannot tell
  memory-bound from compute-bound code reliably, because DRAM stalls hide the difference.
- **Voltage-frequency exponent.** `dvfs_energy.py` accepts it as `vf_exp` (default 1.0, so dynamic power
  scales as f³), but the CARBS script does not expose it.

## Energy model

`dvfs_energy.py`: voltage scales linearly with frequency, so dynamic power ∝ f³ and static power ∝ f, with
static fraction `s` at the base clock:

    P_rel(f) = s * f_rel + (1 - s) * f_rel^3

Energy is the sum of P_rel × time spent at each clock period; transition stalls are charged at the running
period. Only ratios between runs mean anything. **These are assumptions, not measurements.**

## Results so far

Ratios are hinted over unhinted baseline (below 1.0 is better), 2 µs transition penalty.

| Kernel and setting | Time | Energy | EDP | ED² | Transitions |
| --- | --- | --- | --- | --- | --- |
| `k_stencil`, inner loop only (original pass) | 26.07 | 158.5 | 4132 | 107,705 | 11,983 |
| `k_stencil`, hoisting on, 2x | 0.558 | 2.88 | 1.61 | 0.90 | 1 |
| `k_stencil`, dwell 20 µs, no hoisting, 2x | 0.796 | 2.57 | 2.05 | 1.63 | 32 |
| `k_mix`, 1.25x | 0.949 | 1.10 | 1.045 | 0.991 | 2 |
| `k_mix`, 1.5x | 0.897 | 1.32 | 1.185 | 1.063 | 2 |
| `k_mix`, 2x | 0.871 | 1.54 | 1.342 | 1.170 | 2 |

(Column "ED²" is the ED²P ratio.)

- **Hoisting is the main fix.** Boosting only a small inner loop made `k_stencil` 26x slower from constant
  clock switching; hoisting to the whole loop nest made it 44% faster.
- **On `k_mix` the pass is selective.** It boosts only 2 FP regions out of 37 loops, so the time gain is
  small (5-13%).

### Full tuning run

Budget 24 evaluations per tuner, 2 seeds, 2 µs transition penalty, ED²P objective.

| Kernel | Best ED²P | Time | Energy | Transitions | Notes |
| --- | --- | --- | --- | --- | --- |
| `k_multistage_gemm` | 0.795 | 0.508 | 3.08 | 1 | 2x boost; tuners reached 0.795-0.81 |
| `k_mix` | 0.923 | 0.905 | 1.13 | 3 | 1.25x boost; all three tuners reached 0.923 |
| `k_epistasis_pipe` | 1.000 | 1.000 | 1.00 | 0 | Best config is no hints at all |

- **`k_epistasis_pipe` gets faster but never wins.** Many configurations cut time by up to 29%, but its FP
  regions are small and scattered (12-230 transitions), so energy rises by more than the time saved.
- **Two simulations aborted from clock thrashing.** One trace had a hint every 71 instructions (84,135 in
  6M) and another every 12; ChampSim's livelock detector stopped them. The tuner now scores such configs as
  a heavy penalty (100x) instead of discarding them. Those two kernels were run before this change, so their
  searches skipped the aborted samples.
- **Hoisting and dwell are what avoid thrashing.** Both aborted configs had hoisting off or at its lowest
  threshold and no dwell protection. The best configs used hoisting (0.02-0.05) and mostly a 20-50 µs dwell,
  though one near-best gemm config used no dwell.
- **Caveats:** one trace window per kernel, 2 seeds, and the energy model above is assumed. Too little data
  to rank the tuners.

## How to run

All scripts need DynamoRIO, a Python with Capstone, and libsnappy on the library path; build ChampSim first
with `champsim/build_knobbed.sh linux`.

```bash
export DR=~/pin-dl/dr/DynamoRIO-Linux-11.3.0-1
export PY=~/pin-dl/venv/bin/python
export LD_LIBRARY_PATH=~/pin-dl/libs/usr/lib/x86_64-linux-gnu

# 1. Insert hints into one kernel's assembly (prints which loops it boosted)
gcc -O2 -DNO_M5OPS -I include -S flagbench/kernels/k_mix.c -o k_mix.s
python3 champsim/dvfs/fp_boost_pass.py k_mix.s -o k_mix_h.s --report --fp-min 0.25 --hoist-fp-min 0.05

# 2. One hand-chosen configuration: time, energy, EDP, ED2P vs baseline
python3 champsim/dvfs/dvfs_eval.py flagbench/kernels/k_mix.c --level 1 --dwell-ns 0

# 3. Tune all knobs with CARBS, random search and hill climbing
python3 champsim/dvfs/dvfs_carbs.py flagbench/kernels/k_mix.c --out results/dvfs/k_mix.json --metric ed2p --budget 30 --seeds 2
```

Each new hinted binary needs a DynamoRIO trace, which takes minutes, so expect a tuning run to take hours.

## Scope

The pipeline needs no human to set the clock: the pass places hints, ChampSim applies them, CARBS picks the
policy. But it is **simulation only**. On a real CPU the `nopl` hint is ignored, and real frequency control
is privileged (OS `cpufreq`, MSR writes or a hardware governor), so the result is an architectural claim
("a machine that honors compiler hints could save time"), not something today's hardware does.

## Files

- `fp_boost_pass.py`: assembly pass (flags: `--fp-min`, `--min-instrs`, `--mem-max`, `--hoist-fp-min`, `--level`).
- `dvfs_eval.py`: build, trace and simulate one configuration; prints time, energy, EDP, ED²P.
- `dvfs_energy.py`: the energy model.
- `dvfs_carbs.py`: tuner wrapper around `flagbench/tuners.py`.
- `../dynamorio/dr_trace/to_champsim.py`: recognizes the hint `nopl` and writes the marker record.
- `../../flagbench/kernels/dvfs_hint.h`: `DVFS_HINT(level)` macro for hand-placed hints.
- `../dynamorio/dr_trace/examples/fp_dvfs_demo.c`: hand-hinted demo kernel.
