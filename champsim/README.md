# ChampSim FlagBench workflow

This is a separate x86 trace-driven version of FlagBench. It does not reuse
gem5 outputs or the RISC-V binaries.

## Datapath mapping

The ChampSim configuration in `champsim_config.json` approximates the current
FlagBench datapath:

- one out-of-order x86 core
- 4-wide fetch/decode/dispatch/execute/retire
- 128-entry ROB and 32-entry load/store queues
- 32 KiB, 8-way L1 instruction/data caches
- 256 KiB, 8-way L2 cache
- 1 MiB, 16-way LLC
- 2 GHz nominal frequency
- no prefetcher and LRU LLC replacement

ChampSim is trace-driven, so it models the pipeline and memory hierarchy after
the x86 binary has been traced. It does not execute the ELF directly like gem5.

## One-command native Windows workflow

Use the Python entry point [run_windows.py](C:/Users/yogev/Documents/ECE-752/champsim/run_windows.py).
It is the native Windows equivalent of the gem5 stages:

1. compile every selected compiler-flag configuration;
2. run each binary natively to verify its checksum;
3. generate a Windows Intel PIN ChampSim trace;
4. simulate the trace in native Windows ChampSim;
5. write the measured LUT and replay CARBS;
6. run tuner tests;
7. compare CARBS, randomized search, and hill climbing and generate graphs.

From the repository root, run:

```powershell
python .\champsim\run_windows.py
```

The defaults use all six gem5 kernels (`k_stencil`, `k_ptrchase`, `k_branchy`,
`k_reduce`, `k_crc32`, and `k_helper`) plus the new `k_mix2` flag-stress kernel,
2,000,000 traced instructions, 100,000 warmup instructions, 100 evaluation seeds,
and the same budgets as gem5: `10 20 50 100 200`. Each kernel gets an isolated
directory under `champsim\results\windows`; root-level LUT aliases are created
for the shared graph evaluator.

## Adaptive runs for larger flag spaces

`run_carbs_adaptive.py` is an optional online workflow. It preserves
`run_carbs.py` and `run_windows.py` for exhaustive experiments, but asks the
selected tuner for one configuration at a time and compiles, traces, and
simulates only that configuration. The sampled measurements are cached, so a
repeated request does not create another binary or trace. Compiled reference
baselines are reused, and configurations with the same `.text` hash reuse the
recorded simulation. After each result is saved, the one-use binary and trace
are deleted; only small logs, baselines, and the measurement cache remain. This
lets you explore larger spaces without first materializing all `2^N` variants
or retaining a trace for every evaluation.

For example, use 20 flags and let CARBS request up to 100 evaluations:

```powershell
python .\champsim\run_carbs_adaptive.py `
  --repo . `
  --champsim-root .\champsim\.upstream-windows `
  --pin C:\path\to\pin `
  --tracer .\champsim\.upstream-windows\tracer\pin\obj-intel64\champsim_tracer.dll `
  --out .\champsim\results\adaptive-k-mix2 `
  --kernel k_mix2 `
  --nflags 20 `
  --budget 100 `
  --algorithm carbs
```

Use `--algorithm random` or `--algorithm hillclimb` to drive the same online
measurement loop with those selectors. The output includes `measurements.json`
(resume cache), the sparse `<kernel>.csv`, `carbs.json` (run history), and
`score_<kernel>.png` (best measured cycles by evaluation). The graph describes
the adaptive run only; sparse sampling does not provide exhaustive ground truth
or support the full-factorial CARBS-versus-random comparison produced by
`run_windows.py`. Increase `--budget` to spend more compilation and simulation
time. The per-request backend currently starts the legacy runner, so a large
budget has some process-startup overhead. Set `--determinism 1` to rerun each
requested trace once and require identical cycle counts before accepting it.
Each adaptive configuration also has a 900-second simulator timeout by
default; timed-out configurations are recorded as invalid and the search
continues. Adjust this with `--simulation-timeout` if a valid long trace needs
more time. The legacy exhaustive runner leaves timeouts disabled unless that
option is passed directly. Windows skips the first 1,000 PIN instructions by
default, matching the one-command Windows workflow. `carbs.json` records
whether the selected flag space's `-O3` identity was verified. If it was not,
CARBS uses configuration 0 as its bootstrap measurement, matching the legacy
runner's documented fallback.

### Compare tuners without exhaustive enumeration

Add `--compare` to run CARBS, randomized search, and hill climbing online with
the same evaluation budget and seeds. All three share the measurement cache,
so a requested configuration is compiled and simulated only once. The runner
writes `score_compare_<kernel>.png` with the median best cycle count and 95%
bootstrap bands, plus `comparison.json` with per-seed traces and summary
statistics. Lower cycle counts are better. This compares search progress on
the sampled evaluations; it does not claim to know the global optimum of
unevaluated configurations.

The curated flag list supports `--nflags` through **24 flags** (16,777,216
possible configurations). Adaptive mode also accepts up to **64 custom flags**
through `--flags`; each must be supported by the selected compiler in both
enabled and disabled forms.
Runtime depends on the requested evaluations, not on that total. For example,
a 24-flag comparison with 3 seeds and a budget of 20 makes at most 180 tuner
requests per kernel, before duplicate configurations and identical binaries
are reused:

```powershell
python .\champsim\run_carbs_adaptive.py `
  --repo . `
  --champsim-root .\champsim\.upstream-windows `
  --pin C:\path\to\pin `
  --tracer .\champsim\.upstream-windows\tracer\pin\obj-intel64\champsim_tracer.dll `
  --out .\champsim\results\compare-k-mix2-24flag `
  --kernel k_mix2 `
  --nflags 24 `
  --compare `
  --seeds 3 `
  --budget 20
```

A 2M-instruction evaluation took about one minute on the validation machine.
At that rate, the 180-request upper bound is roughly three hours per kernel;
actual time varies with hardware, kernels, and reuse. Reduce `--budget` to
shorten the comparison or increase `--seeds` for more stable confidence bands.

### Kernels

| Kernel | Description | Expected flag sensitivity |
|---|---|---|
| `k_stencil` | 5-point Jacobi stencil | Moderate: vectorize + unroll |
| `k_ptrchase` | Pointer chase > L2 (control) | Very low: memory-latency bound |
| `k_branchy` | Bytecode interpreter | Low: data-dependent branches |
| `k_reduce` | Reduction + prefix sum | Moderate: unroll + vectorize |
| `k_crc32` | Bitwise CRC32 | Low-moderate: unroll + schedule |
| `k_helper` | Hot helper calls | Moderate: inline + IPA |
| `k_mix2` | 10-phase flag-stress kernel | **High**: stencil, reduce, CRC, helper, interchange, stride, FP matmul, IPA stress — opposing flag preferences produce the widest cycle range |

`k_mix2` is the recommended kernel for tuner discrimination: its FP matrix-multiply
phase benefits strongly from vectorization (4-8x with the right flags) while its CRC
phase prefers scalar code, and loop-interchange helps its column-major matrix phase
but can hurt the strided AXPY. No flag combination dominates every phase, so the
optimum is a real trade-off — exactly the kind of space CARBS is designed to search.
Correctness is guaranteed by a fixed-seed FNV-1a checksum verified against the
-O0 native reference for every configuration.

The default compiler backend is `wsl-mingw`. It uses Windows-targeting MinGW
GCC from WSL to produce PE executables, preserving the complete gem5/GCC flag
space while Windows Intel PIN can still trace the result:

```powershell
wsl.exe sudo apt-get update
wsl.exe sudo apt-get install -y mingw-w64
```

This does not use a WSL Linux executable or a WSL trace. The executable is
written into the Windows results directory and traced by native Windows PIN
and ChampSim. Group members can reproduce the compiler setup without a
machine-specific Windows compiler installation.

For a quick end-to-end smoke test:

```powershell
python .\champsim\run_windows.py `
  --trace-instructions 1000 `
  --warmup-instructions 0 `
  --seeds 20 `
  --budgets 1 2 3 4 `
  --out .\champsim\results\windows-smoke
```

The smoke command is intentionally small and is not a performance benchmark.
The earlier CARBS graph used only two flags, which produced four configurations
and made random search appear better simply because it could exhaust the space
within a few evaluations. That result cannot determine whether CARBS performs
well on the real experiment. The default run uses 12 flags, or 4,096
configurations per kernel, and should be used for tuner comparisons.

### Full-run time and flag-space choices

A full run is intentionally expensive. It evaluates up to 4,096 configurations
per kernel, traces up to 2,000,000 instructions for each unique binary, runs
ChampSim, and repeats determinism checks. With seven kernels (six gem5 +
`k_mix2`), that is up to 28,672 configurations before binary deduplication.
On a typical Windows machine, the full experiment can take many hours and may
appear quiet while Clang/MinGW, PIN, or ChampSim is working.

**What makes a full 12-flag run finish faster without sacrificing correctness:**

* **Binary deduplication (automatic):** `run_carbs.py` hashes each compiled
  binary's `.text` section and only traces/simulates *unique* binaries. On
  `k_mix2` roughly 25-35% of 4,096 configs are unique — the rest share a
  `.text` hash and are skipped for tracing/simulation.
* **Parallel compilation (`--jobs`):** defaults to 8 workers. Increase on
  machines with more cores: `--jobs 16` or `--jobs 32`.
* **Resume support (automatic):** already-compiled binaries, existing traces,
  and existing simulation logs are skipped. Interrupt and re-run at any time;
  nothing is re-done.
* **Fewer kernels for development:** `k_mix2` alone with `--nflags 12` is a
  complete tuner experiment in one kernel — run the other kernels only for the
  final aggregate comparison.
* **Shorter traces for iteration:** `--trace-instructions 100000` for smoke
  tests, 2,000,000 for the definitive run.

Use these tiers when developing or comparing changes:

| Command | Configurations per kernel | Purpose |
|---|---:|---|
| `--nflags 2` | 4 | Fast smoke test only; not a tuner benchmark |
| `--nflags 4` | 16 | End-to-end debugging and graph validation |
| `--nflags 6` | 64 | Short development comparison |
| `--nflags 8` | 256 | More representative intermediate experiment |
| `--nflags 12` | 4,096 | Definitive gem5-equivalent flag-space run |
| `--nflags 16` | 65,536 | Large space: needs `--jobs 16+` and deduplication |
| `--nflags 20` | ~1.05M | Very large space: only viable with heavy deduplication |

The exhaustive workflow is capped at 24 flags. Adaptive mode supports up to
64 custom flags because it never builds a full LUT or enumerates the space.
With binary deduplication only unique `.text` hashes
are traced and simulated, so large spaces are tractable when many configs
produce identical code — but compilation of all 2^N configs still happens,
so use `--jobs` to match your core count and expect long runtimes.

`--nflags` supports the curated 24-flag order. Flags 13-24 extend the default
12 with `unroll-all-loops`, `sched-pressure`, `gcse-after-reload`,
`align-loops`, `align-functions`, `loop-interchange`,
`version-loops-for-strides`, `loop-unroll-and-jam`,
`tree-loop-distribution`, `ipa-icf`, `split-paths`, and `schedule-insns2`.
These switches were checked with the WSL MinGW compiler. Use `--flags` when
you want a custom order or subset. For example:

```powershell
# 14-flag space: default 12 + loop-interchange + version-loops-for-strides
python .\champsim\run_windows.py `
  --kernels k_mix2 `
  --flags unroll-loops peel-loops tree-loop-vectorize tree-slp-vectorize `
          inline-functions ipa-cp-clone unswitch-loops predictive-commoning `
          tracer schedule-insns ivopts split-loops loop-interchange `
          version-loops-for-strides `
  --out .\champsim\results\k_mix2_14flag
```

Do not pass `--nflags` and `--flags` together.

The 12-flag run should be completed at least once for the final CARBS versus
random-search versus hill-climbing comparison. Use fewer flags, fewer kernels,
or shorter trace lengths for iteration, but do not describe those results as
the full experiment. For example:

```powershell
# Development run: one kernel, eight flags, shorter trace
python .\champsim\run_windows.py `
  --kernels k_stencil `
  --nflags 8 `
  --trace-instructions 100000 `
  --warmup-instructions 10000 `
  --out .\champsim\results\development-8-flag

# k_mix2-only definitive run: 12 flags, fastest path to tuner results
python .\champsim\run_windows.py `
  --kernels k_mix2 `
  --nflags 12 `
  --out .\champsim\results\full-12-flag-mix2

# Definitive run: all seven kernels and the complete 12-flag space
python .\champsim\run_windows.py `
  --nflags 12 `
  --out .\champsim\results\full-12-flag
```

Add `--build` when the native simulator or PIN tracer needs rebuilding:

```powershell
python .\champsim\run_windows.py --build
```

Use `--pin C:\path\to\pin` when PIN is not installed under `Downloads`.
Use `--skip-tests` to omit the shared tuner unit tests, or `--no-graphs` to
stop after native simulation and CARBS.

### Output

Each run contains:

```text
<output>\
  <kernel>\
    binaries\     compiled Windows x86 binaries
    traces\       Intel PIN ChampSim traces
    simulations\  ChampSim logs
    compile-logs\ compiler stderr/stdout (reference, plain -O3, per-config)
    trace-logs\   Intel PIN trace stderr/stdout (per config)
    <kernel>.csv  measured LUT
    <kernel>.json LUT metadata
    carbs.json    CARBS replay result
  <kernel>.csv    root-level evaluator alias
  <kernel>.json   root-level evaluator alias
  graphs\
    score_<kernel>.png
    score_aggregate.png
    summary.md
    report.json
```

The graph files use the same evaluator and visual style as
`flagbench\results`:

- `score_<kernel>.png`: per-kernel CARBS, randomized search, and hill-climber
  normalized-score comparison with 95% confidence bands
- `score_aggregate.png`: aggregate comparison across the selected kernels
- `summary.md`: numeric comparison, headroom, and evaluations-to-target
- `report.json`: complete machine-readable evaluation report

Graph budgets default to the same values as the gem5 workflow:
10, 20, 50, 100, and 200 evaluations. To use different replay budgets and
seeds:

```powershell
python .\champsim\run_windows.py `
  --seeds 100 `
  --budgets 10 20 50 100 200
```

The older `run_windows.ps1` wrapper remains available, but the Python command
above is the supported interface. Use `--kernels k_stencil k_reduce` to run a
subset, and `--flags unroll-loops peel-loops` to choose a smaller space.
The same selection can be written as `--nflags 12`; `--nflags 12` is the
default and means all 12 flags below. Do not pass `--nflags` and `--flags`
together.
The default space is the 12-flag gem5 space and every configuration passes
both `-f<name>` and `-fno-<name>` to GCC. Use `--compiler clang` only as an
explicit fallback; it is not equivalent for GCC-only optimization flags.
The 12-flag default contains 4,096 configurations per kernel, so use a
smaller explicit `--flags` list for smoke tests.
Use `--strict-o3` when the selected flag space must contain an exact hash match
for plain `-O3`; otherwise the workflow records that the O3 identity is
unverified and uses configuration zero as the comparison baseline.

The graph lines are the median normalized score over seeds; shaded regions are
95% bootstrap confidence intervals. The report also includes speedup,
headroom, evaluations-to-target, critical flag effects, and aggregate
comparisons.

- Python 3
- LLVM/Clang, including `clang.exe` and `llvm-objcopy.exe`
- GNU Make if using `-Build`
- Intel PIN for Windows, with `pin.exe`
- The vcpkg runtime DLLs under
  `champsim\.upstream-windows\vcpkg_installed\x64-windows\bin`

The visible ChampSim source is kept in
`champsim\.upstream-windows`; generated binaries, traces, dependency trees,
and results remain ignored by Git.

### Windows prerequisites

- Python 3
- LLVM/Clang, including `clang.exe` and `llvm-objcopy.exe`
- GNU Make, only when using `--build`
- Intel PIN for Windows, with `pin.exe`
- ChampSim's vcpkg runtime DLLs under
  `champsim\.upstream-windows\vcpkg_installed\x64-windows\bin`

The script automatically adds the standard LLVM installation directory and
finds the newest `pin-external-*-windows` directory under `Downloads`.

The Windows PIN tracer records the low 32 bits of each instruction pointer.
This keeps instruction identity stable for legacy ChampSim modules that use
Windows' 32-bit `long` ABI; data and memory addresses remain native 64-bit x86
addresses.

## Setup in WSL/Linux

Install Intel PIN 3.22 separately, then set `PIN_ROOT`. Build the ChampSim
tracer and simulator:

```bash
cd ~/ECE-752-linux
./champsim/setup.sh
cd champsim/.upstream/tracer/pin
make
```

Run the four-configuration experiment:

```bash
cd ~/ECE-752-linux
source .venv/bin/activate
python3 champsim/run_carbs.py \
  --repo "$PWD" \
  --champsim-root "$PWD/champsim/.upstream" \
  --pin "$PIN_ROOT" \
  --tracer "$PWD/champsim/.upstream/tracer/pin/obj-intel64/champsim_tracer.so" \
  --out "$PWD/champsim/results/k_stencil" \
  --kernel k_stencil
```

The workflow is:

```text
x86 flag configurations
        -> static x86 binaries
        -> Intel PIN ChampSim traces
        -> ChampSim pipeline/cache simulation
        -> measured LUT
        -> CARBS replay
```

Generated traces, binaries, logs, and results stay under the requested output
directory and should not be committed.
