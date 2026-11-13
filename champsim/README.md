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

The defaults use `k_stencil`, 2,000,000 traced instructions, 100,000 warmup
instructions, 100 evaluation seeds, and the same budgets as gem5:
`10 20 50 100 200`. Results are written to
`champsim\results\k_stencil-windows`.

For a quick end-to-end smoke test:

```powershell
python .\champsim\run_windows.py `
  --trace-instructions 1000 `
  --warmup-instructions 0 `
  --seeds 20 `
  --budgets 1 2 3 4 `
  --out .\champsim\results\windows-smoke
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
  binaries\       compiled Windows x86 binaries
  traces\         Intel PIN ChampSim traces
  simulations\    ChampSim logs
  champsim.csv    measured LUT
  champsim.json   LUT metadata
  carbs.json      CARBS replay result
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
above is the supported interface.

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
