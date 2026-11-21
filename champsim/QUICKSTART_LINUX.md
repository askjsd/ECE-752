# ChampSim + CARBS: Linux Quickstart

How to run the compiler-flag tuning experiments on **Linux**. This guide covers
shared machines where you have no root and `ptrace` is disabled. For the Windows
workflow and full detail, see [README.md](README.md).

## What this does

We want to find the best GCC flag combination (e.g. `unroll-loops`,
`tree-loop-vectorize`) for a small C kernel. "Best" means fewest simulated
CPU cycles. Each flag is on or off, so N flags give 2^N possible builds. That is
far too many to try all of them, so a **tuner** (CARBS) picks which ones to
test.

```
tuner picks flags -> compile kernel -> trace (DynamoRIO) -> simulate in ChampSim -> cycle count
        ^                                                                              |
        +--------------------------- cycle count goes back to the tuner ---------------+
```

The tuner's choice depends on earlier results, so this is called **adaptive**
simulation. Results are cached, and the binary and trace are deleted after each
measurement to save disk space.

Only `dynamorio/run_carbs_adaptive.py` needs to be run directly. It calls `dynamorio/run_carbs.py`
internally. Don't use `run_windows.py` on Linux.

## One-time setup

Tested on Ubuntu 24.04 (GCC 13) with no root and `ptrace_scope=3`. Run these from
the repo root.

### 1. Build ChampSim

```bash
./champsim/setup_linux.sh           # -> ~/champsim-linux/bin/champsim (about a minute, no root)
```

This builds the repo's `champsim/source-linux` in a copy at `~/champsim-linux`
(override with `CHAMPSIM_BUILD=/some/path`) and installs its dependencies there.
The build can't happen inside the repo because ChampSim's Makefile breaks on
paths containing spaces, such as `ece 752`. The source tree isn't modified.
Rerun the script after editing `champsim/champsim_config.json`.

`champsim/source-windows` is the tree the Windows runs use. Don't use it on
Linux. `champsim/setup.sh` and `champsim/.upstream` (a fresh clone of upstream
ChampSim) are kept as a fallback, but this guide doesn't use them.

### 2. Get DynamoRIO (the tracer)

DynamoRIO traces programs without ptrace and without root, and produces ChampSim
trace files. (Intel PIN can't run when `ptrace_scope` is 3. Check with
`cat /proc/sys/kernel/yama/ptrace_scope`.)

```bash
curl -L -o dr.tgz https://github.com/DynamoRIO/dynamorio/releases/download/release_11.3.0-1/DynamoRIO-Linux-11.3.0.tar.gz
tar xzf dr.tgz                      # -> DynamoRIO-Linux-11.3.0-1/
```

### 3. Python packages

```bash
python3 -m venv --system-site-packages venv
venv/bin/pip install capstone numpy matplotlib
```

### 4. One missing library (no sudo needed)

DynamoRIO's trace launcher needs `libsnappy`. Unpack it locally:

```bash
mkdir libs && cd libs && apt-get download libsnappy1v5 && dpkg -x libsnappy1v5*.deb . && cd ..
export LD_LIBRARY_PATH=$PWD/libs/usr/lib/x86_64-linux-gnu   # set this in every new shell
```

### 5. A `clang` on PATH

The runner's `--compiler clang` looks for `clang`. For genuine GCC flag
behaviour, put a symlink to `gcc` under that name in a folder on `PATH`:

```bash
mkdir -p ~/shim && ln -sf /usr/bin/gcc ~/shim/clang
export PATH=~/shim:$PATH
```

### Every new shell

```bash
export LD_LIBRARY_PATH=$PWD/libs/usr/lib/x86_64-linux-gnu
export PATH=~/shim:$PATH
export PYTHONPATH=$PWD              # repo root, so flagbench imports work
```

## Run an experiment

Replace `/path/to/DynamoRIO-Linux-11.3.0-1` with your extracted folder. The
commands below assume you are in the repo root.

### 1. Quick sanity check (about 10 seconds)

A small run on a kernel that responds strongly to flags:

```bash
venv/bin/python champsim/dynamorio/run_carbs_adaptive.py \
  --repo "$PWD" \
  --champsim-root "$HOME/champsim-linux" \
  --trace-backend dynamorio \
  --dynamorio /path/to/DynamoRIO-Linux-11.3.0-1 \
  --compiler clang \
  --out "$PWD/champsim/results/smoke" \
  --kernel k_optstress --nflags 4 --budget 4 \
  --trace-instructions 500000 --trace-skip-instructions 2000000 \
  --warmup-instructions 10000
```

It worked if the output ends with `Adaptive run complete`, and `--out` contains
`k_optstress.csv` and the two PNG graphs with `valid` rows.

### 2. Compare CARBS, random search, and hill climbing

Add `--compare` and `--seeds`. All three tuners share one cache, so no config
is simulated twice.

```bash
venv/bin/python champsim/dynamorio/run_carbs_adaptive.py \
  --repo "$PWD" \
  --champsim-root "$HOME/champsim-linux" \
  --trace-backend dynamorio \
  --dynamorio /path/to/DynamoRIO-Linux-11.3.0-1 \
  --compiler clang \
  --out "$PWD/champsim/results/compare-k-mix2" \
  --kernel k_mix2 --nflags 24 --compare --seeds 3 --budget 20
```

A 24-flag run with 3 seeds and a budget of 20 makes at most 180 requests. Lower
`--budget` or `--seeds` to finish faster.

### 3. Run just one tuner

Drop `--compare` and pick one algorithm:

```bash
... --algorithm carbs        # default; or: random, hillclimb
```

## Options you will change most

| Flag | Meaning |
|---|---|
| `--kernel` | Which program to tune (table below). |
| `--nflags` | How many curated flags to tune (up to 24). |
| `--flags a b c` | Use your own flags instead (up to 64). Check that your compiler supports each one. |
| `--budget` | Evaluations per tuner. Default is 20 with `--compare`, otherwise 100. |
| `--compare` | Run CARBS, random, and hillclimb together. |
| `--seeds` | Repeats for the confidence bands (default 3). |
| `--trace-instructions` | Length of the simulated trace. Shorter is faster but noisier (default 2M). |
| `--trace-skip-instructions` | Skip startup code so the trace covers the hot loop. |
| `--simulation-timeout` | Seconds before a config is marked invalid (default 900). |
| `--determinism 1` | Run each trace twice and require identical cycle counts. |
| `--trace-backend` | `dynamorio` (no ptrace needed) or `pin`. |
| `--out` | Output folder. Reuse the same folder to **resume** a run. |

## Kernels

| Kernel | Notes |
|---|---|
| `k_optstress` | Best first check. Very sensitive to vectorization flags. |
| `k_mix2` | Best for comparing tuners. Different phases prefer opposite flags. |
| `k_stencil`, `k_reduce`, `k_helper` | Moderate flag sensitivity. |
| `k_crc32`, `k_branchy` | Low sensitivity. |
| `k_ptrchase` | Control case. It is memory-bound, so flags barely matter. |

## What you get

Look in the `--out` folder:

| File | Contents |
|---|---|
| `score_<kernel>.png` | Best cycles found so far, by evaluation (lower is better). |
| `speedup_<kernel>.png` | Speedup over plain `-O3` (higher is better; 1.0x means the same as `-O3`). |
| `score_compare_<kernel>.png`, `speedup_compare_<kernel>.png` | The same plots for `--compare`, with median and 95% bands across seeds. |
| `comparison.json` | Per-seed traces and summary. |
| `<kernel>.csv` | Every measured configuration. |
| `measurements.json` | Cache, used for resume. |
| `carbs.json` | Run history. |

Plain `-O3` is measured separately as a reference. It does not count against
your budget. A finished example is in
[../docs/adaptive-results/](../docs/adaptive-results/).

## Kernels

| Kernel | Notes |
|---|---|
| `k_optstress` | Best first check. Very sensitive to vectorization flags. |
| `k_mix2` | Best for comparing tuners. Different phases prefer opposite flags. |
| `k_stencil`, `k_reduce`, `k_helper` | Moderate flag sensitivity. |
| `k_crc32`, `k_branchy` | Low sensitivity. |
| `k_ptrchase` | Control case. It is memory-bound, so flags barely matter. |

## What you get

Look in the `--out` folder:

| File | Contents |
|---|---|
| `score_<kernel>.png` | Best cycles found so far, by evaluation (lower is better). |
| `speedup_<kernel>.png` | Speedup over plain `-O3` (higher is better; 1.0x means the same as `-O3`). |
| `score_compare_<kernel>.png`, `speedup_compare_<kernel>.png` | The same plots for `--compare`, with median and 95% bands across seeds. |
| `comparison.json` | Per-seed traces and summary. |
| `<kernel>.csv` | Every measured configuration. |
| `measurements.json` | Cache, used for resume. |
| `carbs.json` | Run history. |

Plain `-O3` is measured separately as a reference. It does not count against
your budget. A finished example is in
[../docs/adaptive-results/](../docs/adaptive-results/).

## Troubleshooting

- **Run was interrupted.** Run the same command with the same `--out`. Cached
  measurements are reused.
- **A config shows as invalid.** It timed out, or its checksum did not match the
  `-O0` reference. The search continues. Failure logs are kept in `--out`.
- **Results look noisy.** Raise `--trace-instructions` or `--seeds`. A 10k-instruction
  trace is too short to be reliable.
- **Disk is filling up.** Adaptive mode removes traces and binaries as it goes.
  Don't use `run_windows.py` for full runs.
- **Don't commit** anything under `champsim/results/`.

## Using Intel PIN instead

Use PIN only if `ptrace_scope` is 0 or 1. Use PIN **3.31 or newer**, since 3.22
fails to build the tracer with GCC 13.

```bash
export PIN_ROOT=/path/to/pin-external-3.31-...-gcc-linux
make -C ~/champsim-linux/tracer/pin       # builds champsim_tracer.so (untested on source-linux)
# then replace the --trace-backend/--dynamorio lines above with:
#   --pin "$PIN_ROOT" \
#   --tracer "$HOME/champsim-linux/tracer/pin/obj-intel64/champsim_tracer.so" \
```

Don't compare cycle counts between PIN and DynamoRIO runs. The two backends
produce slightly different traces (DynamoRIO's register operands come from a
disassembler).

## How the DynamoRIO tracer works

[dr_trace/trace_dr.sh](dynamorio/dr_trace/trace_dr.sh) runs the binary under DynamoRIO's
`drmemtrace`, and [dr_trace/to_champsim.py](dynamorio/dr_trace/to_champsim.py) converts the
result to ChampSim's 64-byte records. Check a trace by hand with
`venv/bin/python champsim/dynamorio/dr_trace/check_trace.py trace_file`. To call it directly:

```bash
DR=/path/to/DynamoRIO-Linux-11.3.0-1 PY=venv/bin/python \
  bash champsim/dynamorio/dr_trace/trace_dr.sh -o out.trace -s 100000 -t 200000 -- ./prog
```

Binaries must be static non-PIE, which the runner's `gcc -static` build gives.

## Trying the knobbed prefetcher

[modules/prefetcher/knobbed/](modules/prefetcher/knobbed/README.md) is a tunable
prefetcher built as an external ChampSim module. Build it after step 1:

```bash
champsim/build_knobbed.sh           # -> ~/champsim-knobbed-linux/bin/champsim_knobbed
```

It is a separate simulator binary, so the commands above are unaffected. See its
README for the knobs.
