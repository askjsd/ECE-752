# CARBS: Compiler Autotuning via Randomized Biased Search
Run with time limitation:
```bash
python3 proba.py --run_dir "src_dir" --time_limitation 6000 --algorithm "top_diff_history" --random_seed {seed} --lr 0.9
```
Run with step limitation:
```bash
python3 proba.py --run_dir "src_dir" --steps 105 --algorithm "top_diff_history" --random_seed {seed} --lr 0.9
```
For datasets and experimental results, please refer to https://github.com/lv2020/RunBench. We provide the repeat results corresponding recommended optimizations in carbs_results.json. To compile and run the source code, please check new_config.f under each benchmark.

For code size reduction, please refer to carbs.py under code_size folder. You need to build the environment according to https://dl.acm.org/doi/pdf/10.1145/3696443.3708961 first. 

```bash
python3 carbs.py --random True --dep True --dataset "$repo" --seed "$seed"
```

---
## Local setup notes (added for this repo)
Cloned from https://github.com/lv2020/CARBS (not a GitHub fork). Local changes:
- `libfunc.c` / `build_libfunc.sh`: timing shim providing `_entering`/`_exiting` (missing from upstream). Run `./build_libfunc.sh RunBench/<bench>` for each benchmark before tuning.
- `utils.py`: `get_all_optimization()` now filters out flags the local gcc rejects.
- `RunBench/`: pruned to 30 benchmarks with stable, measurable runtimes (>=1 ms, low noise) plus `utilities`; get others from https://github.com/lv2020/RunBench. Only source files in `RunBench/` are tracked by git (see `.gitignore`); run `./fetch_runbench_data.sh` on a fresh clone to restore input data.

```bash
python3 -m venv .venv && .venv/bin/pip install numpy scipy torch   # CPU torch is enough
./build_libfunc.sh RunBench/PolyBench_adi
.venv/bin/python proba.py --run_dir "$PWD/RunBench/PolyBench_adi" --steps 10 --algorithm top_diff_history --random_seed 1 --lr 0.9
```
Results are written as a `.pkl` in the benchmark directory.
