# Prefetcher knob tuning: results and how to reproduce

Everything here is for the **Linux** setup. If you haven't set up Linux yet, do
[QUICKSTART_LINUX.md](QUICKSTART_LINUX.md) first (DynamoRIO, venv, libsnappy).

## What this is

- `champsim/modules/prefetcher/knobbed/` is a tunable L1D prefetcher (an external ChampSim module).
- `champsim/knob_carbs.py` lets CARBS, random search and hill climbing tune its knobs.
- It runs on the small kernels in `flagbench/` and on 10 memory-bound SPEC CPU 2017 traces.

Knobs (v2 space, 12 bits, 2,305 distinct configs): `source` (off / nextline / stride / markov),
`trigger` (tagged / access), `level` (0-7 aggressiveness ladder = distance + degree), `fill` policy
(always L1D / gated low / gated high / always L2), `throttle`, `confirm` (1-2), `fallback`, and `insert`
(prefetch fills at MRU or LRU). The ladder and fill fields use Gray code so neighbours differ by one bit.
Details: [modules/prefetcher/knobbed/README.md](modules/prefetcher/knobbed/README.md).

## Results

**V2 (current): SPEC CPU 2017, 10 memory-bound traces, new knob space.**
Speedup over prefetch off (3M-instruction window after a 1M warmup, 20 evaluations x 2 seeds per tuner,
median of the seeds; "best config found" is the best of everything any run sampled, about 80 simulations per trace).
Run on the module with de-duplicated and aged prefetch tracking, so these are NOT directly comparable with the v1 tables below.

| Trace | CARBS | Random | Hill-climb | Default hint | Best config found | Best config |
|---|---|---|---|---|---|---|
| xalancbmk_s-165B | 2.13x | 2.32x | 2.32x | 1.24x | 2.41x | `nextline,tagged,L3,fill:l1d,thr0,c1,fb0,ins:mru` |
| mcf_s-1554B | 2.11x | 1.70x | 1.75x | 1.06x | 2.12x | `markov,access,L6,fill:high,thr0,c1,fb1,ins:lru` |
| fotonik3d_s-1176B | 1.93x | 1.92x | 1.95x | 1.84x | 1.95x | `nextline,access,L2,fill:low,thr0,c1,fb0,ins:mru` |
| mcf_s-1536B | 1.88x | 1.30x | 1.88x | 1.00x | 1.88x | `nextline,tagged,L1,fill:low,thr0,c1,fb0,ins:mru` |
| gcc_s-734B | 1.66x | 1.63x | 1.65x | 1.46x | 1.66x | `nextline,access,L3,fill:l2,thr0,c1,fb0,ins:mru` |
| roms_s-294B | 1.43x | 1.41x | 1.39x | 1.36x | 1.43x | `markov,tagged,L0,fill:high,thr0,c2,fb1,ins:mru` |
| lbm_s-2676B | 1.06x | 1.08x | 1.07x | 1.00x | 1.08x | `nextline,access,L6,fill:l2,thr1,c1,fb0,ins:lru` |
| lbm_s-4268B | 1.06x | 1.07x | 1.07x | 1.00x | 1.08x | `nextline,access,L6,fill:l2,thr1,c1,fb0,ins:lru` |
| omnetpp_s-874B | 1.01x | 1.01x | 1.01x | 1.01x | 1.01x | `markov,tagged,L1,fill:low,thr0,c1,fb1,ins:lru` |
| omnetpp_s-141B | 1.00x | 1.00x | 1.00x | 1.00x | 1.00x | `markov,tagged,L5,fill:low,thr1,c1,fb0,ins:lru` |

Takeaways (v2):
- Knobs matter: on 8 of 10 traces the best config beats the default hint by 5-99% (`mcf` 1.07x -> 2.11x, `xalancbmk` 1.24x -> 2.41x). Only `omnetpp` does not respond (under 1.5% either way).
- The winning engine differs per trace: next-line on most, **Markov on `mcf_s-1554B`** (level 6, fill high, fallback on, insert LRU), always-to-L2 fill on `gcc` and `lbm`. One fixed default cannot cover them.
- Tuners: CARBS finished best on 4 traces (`gcc`, `mcf_s-1554B`, `omnetpp_s-874B`, `roms`), hill-climb on 3 (`xalancbmk`, `fotonik3d`, and `mcf_s-1536B` by a hair), random on 2 (`lbm` x2), one tie. Random search was far behind on `mcf` (1.30x vs 1.88x and 1.70x vs 2.11x); CARBS was 8% behind on `xalancbmk`. Two seeds is too few to call a winner.
- Bad settings still hurt: the worst sampled config was up to 33% slower than prefetch off (`mcf_s-1536B`) and 14% on `omnetpp`.
- Summary table: `champsim/spec_char/knob_tuning_v2_summary.csv`; bar chart: `champsim/spec_char/knob_tuning_v2_speedup.png`. Raw results (gitignored): `champsim/results/knob-carbs-spec-v2/`.

### V1 (history): first knob space, before the telemetry fixes

> The tables below were measured with the first knob space and before the telemetry fixes. The v1 results
> are kept in `champsim/results/knob-carbs*/` and the v1 adapter in `knob_carbs_v1.py`.

**A. Small kernels (13 of them): knobs barely matter.** Their compute dominates.
On 12 of 13 the default hint is within 0.5% of the best config found.
Only `k_ptrchase` moved: 1.01x with the default hint, 1.81x with next-line, degree 8, trigger on miss.

**B. SPEC CPU 2017, 10 memory-bound traces: knobs matter a lot.**
Speedup over prefetch off (3M-instruction window, 20 evaluations x 2 seeds per tuner):

| Trace | CARBS | Random | Hill-climb | Default hint | Best config found |
|---|---|---|---|---|---|
| xalancbmk_s-165B | 1.97x | 2.00x | 2.11x | 1.30x | 2.15x |
| fotonik3d_s-1176B | 1.84x | 1.91x | 1.87x | 1.87x | 1.94x |
| gcc_s-734B | 1.60x | 1.61x | 1.62x | 1.46x | 1.66x |
| roms_s-294B | 1.33x | 1.38x | 1.37x | 1.25x | 1.41x |
| mcf_s-1554B | 1.32x | 1.25x | 1.26x | 1.10x | 1.36x |
| lbm_s-2676B | 1.07x | 1.07x | 1.08x | 1.00x | 1.08x |
| lbm_s-4268B | 1.07x | 1.07x | 1.07x | 1.00x | 1.08x |
| mcf_s-1536B | 1.04x | 1.06x | 1.00x | 1.00x | 1.13x |
| omnetpp_s-874B | 1.005x | 1.002x | 1.004x | 1.006x | 1.006x |
| omnetpp_s-141B | 1.002x | 1.002x | 1.002x | 1.003x | 1.003x |

Takeaways:
- The best config beats the default hint by 4-65% on 8 of 10 traces. Next-line or stride with high degree and fill-this-level wins most often.
- No tuner clearly wins (CARBS best on 2, random on 3, hill-climb on 4, one tie). Gaps are mostly under 5%.
- None converged in 20 evaluations: the best config any run sampled beats every tuner's median on 8 of 10 traces.
- Bad settings can be up to 24% slower than prefetch off.

**C. Trace characterization** (prefetching off, 10M warmup + 50M sim): 10 KEEP, 4 MAYBE, 4 DROP.
Full table: `champsim/spec_char/trace_characterization.csv`.

**Caveats:** "best" means best sampled (no exhaustive ground truth yet; the v2 space has 2,305 distinct configs, so a sweep is feasible);
one short trace window per workload; 2 seeds is too few to rank tuners; the "prefetch off"
reference here only disables the L1D prefetcher (L1I next-line stays on);
in v1 numeric knobs were plain binary bits (v2 uses Gray code); the LLC is 1 MB, which inflates LLC MPKI.

## How to run it (step by step)

All commands run from the repo root (`ece 752/`). Steps 1-3 are one-time setup.

1. Build ChampSim for Linux (takes a few minutes):
   `bash champsim/setup_linux.sh`

2. Build the knobbed prefetcher binary (output: `~/champsim-knobbed-linux/bin/champsim_knobbed`):
   `bash champsim/build_knobbed.sh linux`

3. Set up your shell. Use your own paths if you installed the venv or shim somewhere else
   (see QUICKSTART_LINUX.md):
   `export PYTHONPATH=$PWD LD_LIBRARY_PATH=$HOME/pin-dl/libs/usr/lib/x86_64-linux-gnu PATH=$HOME/shim:$PATH`
   `PY=$HOME/pin-dl/venv/bin/python`

4. Try one knob setting by hand (any trace works; `sample.trace` is a placeholder):
   `KNOBBED_HINT="source=nextline,trigger=tagged,level=3,fill=l1d,throttle=0" ~/champsim-knobbed-linux/bin/champsim_knobbed --warmup-instructions 100000 --simulation-instructions 2000000 sample.trace`
   Look for the line `CPU 0 cumulative IPC: ... cycles: N`. Lower cycles is better.
   The last `[knobbed ...]` line shows accuracy, coverage and timeliness.

5. Tune the knobs on one trace with CARBS, random search and hill climbing
   (writes a JSON file with every result):
   `$PY champsim/knob_carbs.py --kernel mykernel --trace sample.trace --out champsim/results/knob-carbs/mykernel.json --budget 30 --seeds 2`

   Add `--exhaustive --jobs 8` to first simulate all 2,305 distinct configs (ground truth); the tuners then
   run against that table for free. That is about 2,300 simulations per trace.

6. Print the results table for a whole folder:
   `$PY champsim/knob_carbs.py --summarize champsim/results/knob-carbs`
   (saved results: `champsim/results/knob-carbs/` and `knob-carbs-spec/` are v1; `knob-carbs-spec-v2/` is v2)

### Reproduce the SPEC results

7. Make a folder and list the available traces:
   `mkdir -p ~/spec-traces && cd ~/spec-traces && curl -s https://dpc3.compas.cs.stonybrook.edu/champsim-traces/speccpu/ -o listing.html`
   Use exact file names from `listing.html` (for example `605.mcf_s-1554B.champsimtrace.xz`).

8. Download a trace and check it (repeat per trace; the 10 KEEP traces total about 2.6 GB):
   `wget -c https://dpc3.compas.cs.stonybrook.edu/champsim-traces/speccpu/605.mcf_s-1554B.champsimtrace.xz && xz -t 605.mcf_s-1554B.champsimtrace.xz`
   No output from `xz -t` means the file is good.

9. Go back to the repo root and tune the knobs on a SPEC trace. One trace takes roughly
   one to two hours on a busy machine; run several in parallel (10 at a time worked):
   `cd - && $PY champsim/knob_carbs.py --kernel 605.mcf_s-1554B --trace ~/spec-traces/605.mcf_s-1554B.champsimtrace.xz --out champsim/results/knob-carbs-spec-v2/605.mcf_s-1554B.json --budget 20 --seeds 2 --warmup 1000000 --sim 3000000`
   To launch all 10 KEEP traces at once, use `champsim/spec_char/launch_tuning.sh <trace names>`.

10. Print the SPEC results table:
    `$PY champsim/knob_carbs.py --summarize champsim/results/knob-carbs-spec-v2`

### Reproduce the trace characterization (optional)

11. Build the no-prefetch ChampSim (config: `champsim/champsim_config_nopf.json`):
    `mkdir -p ~/champsim-nopf-linux && cp -r champsim/source-linux/. ~/champsim-nopf-linux/ && cp -r ~/champsim-linux/vcpkg_installed ~/champsim-nopf-linux/ && cp champsim/champsim_config_nopf.json ~/champsim-nopf-linux/ && cd ~/champsim-nopf-linux && python3 ./config.sh champsim_config_nopf.json && make -j12 && cd -`

12. Run one trace (10M warmup, 50M simulated; takes 2-17 minutes):
    `bash champsim/spec_char/run_one.sh 605.mcf_s-1554B`

13. Count its branches, then rebuild the CSV after all traces are done:
    `python3 champsim/spec_char/count_branches.py 605.mcf_s-1554B >> champsim/spec_char/branches.jsonl`
    `python3 champsim/spec_char/make_csv.py`
    Labels: KEEP if LLC MPKI is 3 or more, MAYBE for 1 to 3, DROP below 1.

## Windows (untested)

Nothing in this section has been run on Windows. `champsim/run_knob_tuning_windows.py` is one script
that does the whole thing (preflight, build, tune each trace, tuner tests, table and plot). It follows
the same structure and build commands as `run_windows.py` / `run_windows.ps1` (clang, GNU make, the vcpkg DLLs).
It does not compile or trace benchmarks: it takes ChampSim traces you already have.
`run_knob_tuning_windows.ps1` is an equivalent PowerShell version for one trace.

1. Install the prerequisites (same as `run_windows.py`, plus numpy and, for `.xz` traces, xz; matplotlib is optional, for the plot):
   `python -m pip install numpy matplotlib`
2. Run it from the repo root (the first run builds, which takes a few minutes):
   `python champsim\run_knob_tuning_windows.py --trace C:\traces\605.mcf_s-1554B.champsimtrace.xz`
3. Options: several traces (`--trace a.xz b.xz`), `--budget` and `--seeds` for the search, `--exhaustive --jobs 8` for the
   ground-truth sweep, `--rebuild`, `--skip-tests`, `--no-plots`, and `--report-only <results folder>` to print the table
   and draw `speedup.png` for existing results (this mode works on any OS and was tested on Linux).

What differs from Linux, and what could still go wrong:
- **Builds in `C:\champsim-knobbed-win`.** ChampSim's Makefile breaks on spaces and this repo's folder is `ece 752`. The script copies `source-windows` and `modules` there (like `build_knobbed.sh` does on Linux).
- **Needs `source-windows\vcpkg_installed`**, which is not in git. If the dependency build was never done on that machine, nothing links.
- **Build flags are copied from `run_windows.ps1`** (`CXX=clang++ CXXFLAGS=-Wno-c++11-narrowing -fms-runtime-lib=dll LDLIBS=-lshell32`). The new modules use only standard C++ and 64-bit integer types, so Windows' 32-bit `long` should not matter, but this has never been compiled with clang on Windows. The Linux build of the same sources works.
- **`.xz` traces need `xz.exe` on PATH.** Without it, decompress the trace with 7-Zip first.
- **PIN traces from Windows are different** (the tracer keeps only the low 32 bits of each instruction pointer and the register numbering differs; see the PIN note in `champsim/README.md`). The SPEC traces are not affected. Results from a Windows-made PIN trace of a flagbench kernel may not match the Linux DynamoRIO trace of the same kernel.
- **The tuner itself (`knob_carbs.py`) is plain Python** and passes the knobs to the simulator through environment variables (`KNOBBED_HINT`, `KNOBBED_INSERT`), which work the same on Windows. It has no Linux-only calls.
- **Easiest alternative: WSL2.** Inside WSL2 the Linux steps above should work unchanged (including PIN, because WSL2 allows ptrace); you only lose native Windows PIN traces.

## Troubleshooting

- **`cannot open ... champsim_knobbed`:** run step 2 first.
- **Every result is `None` or the script errors on all runs:** the trace path is wrong, or `PYTHONPATH` is not set (step 3).
- **Same cycles for every config:** the trace window is insensitive (initialization code). Use a later part of the trace.
- **The tuner is slow:** each simulation takes 25-45 s on SPEC traces. Lower `--budget` or `--sim`, or run fewer jobs at once.
- **The original `run_carbs.py` and `run_carbs_adaptive.py` in `champsim/` are for the compiler-flag workflow; this README does not change them.**
