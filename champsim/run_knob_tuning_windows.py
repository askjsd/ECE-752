#!/usr/bin/env python3
r"""Run the knobbed-prefetcher tuning workflow on native Windows.

Stages, per run:
  1. preflight   python + numpy, clang, make, vcpkg DLLs (no xz.exe needed: ChampSim reads .xz itself)
  2. build       copy source-windows + champsim/modules to a path WITHOUT spaces
                 (ChampSim's Makefile breaks on spaces; "ECE-752" has none, "ece 752" does)
                 and build champsim_knobbed.exe (or champsim_knobbed) from champsim_config_knobbed.json
  3. tune        champsim/knob_carbs.py on every trace: CARBS, random search and hill
                 climbing over the prefetcher knobs (optionally an exhaustive sweep first)
  4. tests       flagbench/test_tuners.py (the tuners themselves are unchanged)
  5. report      results table, plus a speedup bar chart if matplotlib is installed

Unlike run_windows.py this does not compile benchmarks or trace them with PIN: it
takes ChampSim traces you already have (SPEC .xz traces, or traces made earlier).

Examples (PowerShell, from the repo root):
  python champsim\run_knob_tuning_windows.py --rebuild --trace 605.mcf_s-1554B
  python champsim\run_knob_tuning_windows.py --all --trace-dir D:\traces
  python champsim\run_knob_tuning_windows.py --trace C:\traces\605.mcf_s-1554B.champsimtrace.xz
  python champsim\run_knob_tuning_windows.py --trace a.trace b.trace --budget 30 --seeds 3
  python champsim\run_knob_tuning_windows.py --trace k_stencil.trace --exhaustive --jobs 8
  python champsim\run_knob_tuning_windows.py --all --auto-hierarchy --trace-jobs 4 --jobs 6

Parallelism: --trace-jobs traces are tuned at once, and each runs up to --jobs
simulations at once (its tuner runs side by side, or its exhaustive sweep). By
default the two multiply to about the number of CPUs: without --exhaustive each
trace gets 3 x --seeds slots (one per tuner run) and the rest go to more traces;
with --exhaustive one trace at a time gets every CPU. Each trace's output goes to
<out>/<kernel>.log.

If --warmup-instructions + --sim-instructions is longer than a trace, knob_carbs.py
scales both down to fit, so ChampSim never replays a short trace.
  python champsim\run_knob_tuning_windows.py --report-only champsim\results\knob-carbs-spec-v2
"""

import argparse
import json
import math
import os
import re
import shutil
import stat
import statistics
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHAMPSIM = Path(__file__).resolve().parent
SOURCE_WINDOWS = CHAMPSIM / "source-windows"
MODULES = CHAMPSIM / "modules"
CONFIG = CHAMPSIM / "champsim_config_knobbed.json"
TUNER = CHAMPSIM / "knob_carbs.py"
LLVM_BIN = Path(r"C:\Program Files\LLVM\bin")
TRACE_SUFFIXES = (
    ".champsimtrace.xz",
    ".champsimtrace",
    ".trace.xz",
    ".trace",
    ".xz",
)


def fail(message):
    raise SystemExit(message)


def command_path(name, hint):
    path = shutil.which(name)
    if path:
        return Path(path)
    if name in ("clang", "clang++"):
        candidate = LLVM_BIN / f"{name}.exe"
        if candidate.is_file():
            return candidate
    if name == "make":
        winget_pkgs = (
            Path.home()
            / "AppData"
            / "Local"
            / "Microsoft"
            / "WinGet"
            / "Packages"
        )
        for candidate in winget_pkgs.glob("ezwinports.make_*/bin/make.exe"):
            if candidate.is_file():
                return candidate
    fail(f"{name} was not found on PATH. {hint}")


def run_stage(label, args, cwd=None, env=None):
    print(f"\n== {label}", flush=True)
    subprocess.run([str(a) for a in args], cwd=cwd, env=env, check=True)


def copy_overwrite_readonly(src, dst):
    """shutil.copy2 wrapper that clears Windows read-only bits before overwriting."""
    if os.path.exists(dst):
        try:
            os.chmod(dst, stat.S_IWRITE | stat.S_IREAD)
        except OSError:
            pass
    return shutil.copy2(src, dst)


def rmtree_force(path):
    """Remove a directory tree even if files inside have the Windows read-only attribute."""
    if not os.path.exists(path):
        return

    def _on_error(func, p, _exc):
        try:
            os.chmod(p, stat.S_IWRITE | stat.S_IREAD)
            func(p)
        except OSError:
            pass

    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=_on_error)
    else:
        shutil.rmtree(path, onerror=_on_error)


def kernel_name(trace):
    """File name without the trace suffixes: 605.mcf_s-1554B.champsimtrace.xz -> 605.mcf_s-1554B."""
    return re.sub(
        r"\.(champsimtrace|trace|champsim)?(\.xz)?$", "", Path(trace).name
    )


def find_trace(name, trace_dir):
    """A path, or a bare name (605.mcf_s-1554B) resolved inside trace_dir."""
    direct = Path(name).expanduser()
    if direct.is_file():
        return direct
    for suffix in ("", *TRACE_SUFFIXES):
        cand = trace_dir / (name + suffix)
        if cand.is_file():
            return cand
    fail(
        f"Trace not found: {name} (also looked in {trace_dir}). Download with champsim\\download_spec_traces.ps1."
    )


def install_win_shims(build_dir):
    """Create cmd.exe wrappers (python3.cmd, mkdir.cmd) for GNU Make's Windows shell mode."""
    shim_dir = build_dir / ".win_shims"
    shim_dir.mkdir(parents=True, exist_ok=True)
    py_win = str(Path(sys.executable).resolve())

    cmd_shim = shim_dir / "python3.cmd"
    cmd_shim.write_bytes(f'@echo off\r\n"{py_win}" %*\r\n'.encode())

    # Handle Unix-style `mkdir -p dir1 dir2` inside cmd.exe Makefile recipes
    mkdir_shim = shim_dir / "mkdir.cmd"
    mkdir_shim.write_bytes(
        b"@echo off\r\n"
        b":loop\r\n"
        b'if "%~1"=="" goto :eof\r\n'
        b'if /i "%~1"=="-p" (shift & goto :loop)\r\n'
        b'set "d=%~1"\r\n'
        b'set "d=%d:/=\\%"\r\n'
        b'if not exist "%d%" md "%d%" 2>nul\r\n'
        b"shift\r\n"
        b"goto :loop\r\n"
    )
    return shim_dir


def patch_native_windows_makefile(build_dir, exe_name="champsim_knobbed"):
    """Ensure native Windows clang++/lld-link compatibility in the copied Makefile."""
    (build_dir / "bin").mkdir(parents=True, exist_ok=True)
    (build_dir / "test" / "bin").mkdir(parents=True, exist_ok=True)

    mk_files = [build_dir / "Makefile", build_dir / "_configuration.mk"]
    csconfig = build_dir / ".csconfig"
    if csconfig.is_dir():
        mk_files.extend(sorted(csconfig.glob("*.mk")))

    found_main_mk = False
    for mk_path in mk_files:
        if not mk_path.is_file():
            continue
        if mk_path.name == "Makefile" and mk_path.parent == build_dir:
            found_main_mk = True
        text = mk_path.read_text(encoding="utf-8", errors="replace")

        # 0. Avoid trailing-slash directory target/prerequisite `bin\` in GNU Make on Windows
        text = text.replace("$(BIN_ROOT)/", "$(BIN_ROOT)")
        text = re.sub(r"(?<=\s)bin\\(?=\s|$)", " ", text)
        text = text.replace(
            f"$(BIN_ROOT)\\{exe_name}", f"$(BIN_ROOT)/{exe_name}"
        )
        text = text.replace(f"bin\\{exe_name}", f"bin/{exe_name}")

        # 1. Disable broken .d dependency generation (-MM) with @touch $@
        text = re.sub(
            r"\$\(CXX\)\s+\$\(attach_options\)\s+\$\(DEPFLAGS\)\s+\$\(CPPFLAGS\)\s+-MF\s+\$@\s+\$<",
            "@touch $@",
            text,
        )

        # 2. Filter out duplicate stock prefetcher tage.o when knobbed_modules prefetcher tage is built
        for var in ("base_module_objs", "nonbase_module_objs"):
            filtered = (
                f"$(filter-out .csconfig/modules/prefetcher\\tage/tage.o "
                f".csconfig/modules/prefetcher/tage/tage.o,$({var}))"
            )
            if filtered not in text:
                text = text.replace(f"$({var})", filtered)

        # 3. Remove bare -l flags that clash with explicit vcpkg .lib paths on lld-link
        text = re.sub(
            r"\s*-lCLI11\s+-llzma\s+-llibz\s+-lbz2\s+-lfmt", "", text
        )

        mk_path.write_text(text, encoding="utf-8")

    if not found_main_mk:
        fail(f"Makefile not found in {build_dir}")


def ensure_module_decl_inc(build_dir):
    """Ensure .csconfig/module_decl.inc exists for cache.h and ooo_cpu.h."""
    target = build_dir / ".csconfig" / "module_decl.inc"
    if target.is_file():
        return
    candidates = [
        SOURCE_WINDOWS / ".csconfig" / "module_decl.inc",
        CHAMPSIM / ".upstream-windows" / ".csconfig" / "module_decl.inc",
        CHAMPSIM / "champsim-source" / ".csconfig" / "module_decl.inc",
    ]
    for cand in candidates:
        if cand.is_file():
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(cand, target)
            return
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(
        "#ifndef  CHAMPSIM_LEGACY_MODULE_DECL\n"
        "#define  CHAMPSIM_LEGACY_MODULE_DECL\n"
        "#endif\n",
        encoding="utf-8",
    )


def config_exe_name(config_path):
    """Read executable_name from a ChampSim config JSON, defaulting to champsim_knobbed."""
    if config_path.is_file():
        data = json.loads(config_path.read_text(encoding="utf-8"))
        name = data.get("executable_name")
        if name:
            return name
    return "champsim_knobbed"


def find_built_binary(build_dir, exe_name="champsim_knobbed"):
    """Locate the compiled knobbed simulator binary (.exe or extensionless on Windows)."""
    for name in (f"{exe_name}.exe", exe_name):
        cand = build_dir / "bin" / name
        if cand.is_file():
            return cand
    return None


# ---- 1. preflight ---------------------------------------------------------------------
def preflight(traces=None):
    if not SOURCE_WINDOWS.is_dir():
        fail(f"ChampSim Windows source was not found: {SOURCE_WINDOWS}")
    command_path("python", "Install Python 3.")
    clang_path = command_path(
        "clang", "Install LLVM/Clang and add it to PATH."
    )
    clangpp_path = command_path(
        "clang++", "Install LLVM/Clang++ and add it to PATH."
    )
    make_path = command_path(
        "make", "Install GNU Make for Windows and add it to PATH."
    )
    try:
        import numpy  # noqa: F401  (knob_carbs.py needs it)
    except ImportError:
        fail("numpy is missing. Run: python -m pip install numpy")
    # .xz traces need no external xz: ChampSim decompresses them with its built-in lzma support.
    runtime = SOURCE_WINDOWS / "vcpkg_installed" / "x64-windows" / "bin"
    if not runtime.is_dir():
        fail(
            f"vcpkg runtime DLLs not found: {runtime}. Build ChampSim's vcpkg dependencies first "
            "(see champsim/README.md, Windows prerequisites)."
        )
    extra_dirs = [str(runtime)]
    for tool_path in (clang_path, clangpp_path, make_path):
        parent = str(tool_path.parent)
        if parent not in extra_dirs:
            extra_dirs.append(parent)
    os.environ["PATH"] = os.pathsep.join(
        [*extra_dirs, os.environ.get("PATH", "")]
    )


# ---- 2. build (native Windows clang++ + GNU Make) -----------------------------------
def build(build_dir, rebuild, config_path=CONFIG):
    config_path = Path(config_path)
    exe_name = config_exe_name(config_path)
    exe = find_built_binary(build_dir, exe_name)
    if exe is not None and not rebuild:
        print(f"Reusing {exe} (pass --rebuild to rebuild)")
        return exe

    print(
        f"Building {exe_name} natively on Windows in {build_dir}", flush=True
    )
    if rebuild and build_dir.exists():
        rmtree_force(build_dir)
    build_dir.mkdir(parents=True, exist_ok=True)

    # Copy source-windows tree into build_dir (skipping .csconfig and bin for a clean configure)
    ignore_names = shutil.ignore_patterns(
        ".csconfig", "bin", ".git", "__pycache__"
    )
    shutil.copytree(
        SOURCE_WINDOWS,
        build_dir,
        dirs_exist_ok=True,
        copy_function=copy_overwrite_readonly,
        ignore=ignore_names,
    )

    # Copy knobbed config and custom modules into build_dir
    cfg_dst = build_dir / config_path.name
    if config_path.is_file():
        copy_overwrite_readonly(config_path, cfg_dst)
    knobbed_modules_dst = build_dir / "knobbed_modules"
    if MODULES.is_dir():
        if knobbed_modules_dst.exists():
            rmtree_force(knobbed_modules_dst)
        shutil.copytree(
            MODULES,
            knobbed_modules_dst,
            dirs_exist_ok=True,
            copy_function=copy_overwrite_readonly,
        )

    shim_dir = install_win_shims(build_dir)
    build_env = dict(os.environ)
    build_env["PATH"] = os.pathsep.join(
        [str(shim_dir), build_env.get("PATH", "")]
    )

    # Run config.sh if present
    config_script = build_dir / "config.sh"
    if config_script.is_file():
        config_args = [sys.executable, str(config_script)]
        if knobbed_modules_dst.is_dir():
            config_args.extend(["--module-dir", str(knobbed_modules_dst)])
        if cfg_dst.is_file():
            config_args.append(str(cfg_dst))
        run_stage(
            "configure knobbed ChampSim",
            config_args,
            cwd=build_dir,
            env=build_env,
        )

    abs_opts = build_dir / "absolute.options"
    if abs_opts.is_file():
        abs_opts.unlink()

    ensure_module_decl_inc(build_dir)
    patch_native_windows_makefile(build_dir, exe_name)

    jobs = os.environ.get("NUMBER_OF_PROCESSORS") or str(os.cpu_count() or 4)
    run_stage(
        f"compile knobbed ChampSim (clang++, -j{jobs})",
        [
            "make",
            "-j",
            jobs,
            "CXX=clang++",
            "CC=clang",
            "CXXFLAGS=-O3 -w -Wno-c++11-narrowing -fms-runtime-lib=dll",
            "LDLIBS=-lshell32",
        ],
        cwd=build_dir,
        env=build_env,
    )

    exe = find_built_binary(build_dir, exe_name)
    if exe is None or not exe.is_file():
        fail(
            f"Build finished but no knobbed binary was found in {build_dir / 'bin'}; "
            f"check executable_name in {config_path}."
        )
    return exe


# ---- 5. report ------------------------------------------------------------------------
def load_results(directory):
    rows = []
    for path in sorted(Path(directory).glob("*.json")):
        data = json.loads(path.read_text(encoding="utf-8"))
        ref = data.get("reference", {})
        off, default = ref.get("prefetch_off"), ref.get("default_hint")
        best = data.get("best", {}).get("cycles")
        if not off or not best or "traces" not in data:
            continue
        speed = {}
        for name, runs in data["traces"].items():
            finals = [
                t[-1] for t in runs if t and math.isfinite(t[-1]) and t[-1] > 0
            ]
            speed[name] = (
                off / statistics.median(finals) if finals else float("nan")
            )
        rows.append(
            {
                "kernel": data["kernel"],
                "default": off / default if default else float("nan"),
                "best": off / best,
                **speed,
            }
        )
    return rows


def plot(rows, out_png):
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print(
            "matplotlib not installed; skipping the plot (python -m pip install matplotlib)"
        )
        return
    series = [
        ("default", "default hint"),
        ("carbs", "CARBS"),
        ("random", "random"),
        ("hillclimb", "hill-climb"),
        ("best", "best found"),
    ]
    width = 0.16
    fig, ax = plt.subplots(figsize=(max(6, 1.3 * len(rows)), 4.5))
    for i, (key, label) in enumerate(series):
        ax.bar(
            [x + (i - 2) * width for x in range(len(rows))],
            [r.get(key, float("nan")) for r in rows],
            width,
            label=label,
        )
    ax.axhline(1.0, color="gray", linewidth=0.8)
    ax.set_xticks(range(len(rows)))
    ax.set_xticklabels(
        [r["kernel"] for r in rows], rotation=35, ha="right", fontsize=8
    )
    ax.set_ylabel("speedup over prefetch off")
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(out_png, dpi=140)
    print(f"Plot: {out_png}")


def report(directory, no_plots):
    subprocess.run(
        [sys.executable, str(TUNER), "--summarize", str(directory)],
        check=True,
        env=dict(os.environ, PYTHONPATH=str(ROOT)),
    )
    rows = load_results(directory)
    if rows and not no_plots:
        plot(rows, Path(directory) / "speedup.png")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--trace",
        nargs="+",
        help="ChampSim traces: paths, or bare names like 605.mcf_s-1554B looked up in --trace-dir",
    )
    parser.add_argument(
        "--trace-dir",
        default=str(Path.home() / "spec-traces"),
        help="where download_spec_traces.ps1 puts traces (default ~/spec-traces)",
    )
    parser.add_argument(
        "--all", action="store_true", help="tune on every trace in --trace-dir"
    )
    parser.add_argument(
        "--out",
        help="results directory (default champsim/results/knob-carbs-windows)",
    )
    parser.add_argument(
        "--budget", type=int, default=20, help="evaluations per tuner run"
    )
    parser.add_argument("--seeds", type=int, default=2)
    parser.add_argument("--warmup-instructions", type=int, default=1_000_000)
    parser.add_argument(
        "--sim-instructions",
        type=int,
        default=3_000_000,
        help="scaled down with the warmup if the trace is shorter",
    )
    parser.add_argument(
        "--exhaustive",
        action="store_true",
        help="simulate every distinct config first (2,305 per trace; ground truth)",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=None,
        help="simulations at once per trace (default: 3 x --seeds, or all CPUs with --exhaustive)",
    )
    parser.add_argument(
        "--trace-jobs",
        type=int,
        default=None,
        help="traces tuned at once (default: CPUs / --jobs, at least 1)",
    )
    parser.add_argument(
        "--build-dir",
        default=r"C:\champsim-knobbed-win",
        help="must not contain spaces",
    )
    parser.add_argument(
        "--l1only-build-dir",
        default=r"C:\champsim-knobbed-l1only-win",
        help="L1-only build dir for --auto-hierarchy (must not contain spaces)",
    )
    parser.add_argument(
        "--auto-hierarchy",
        action="store_true",
        help="use full hierarchy for fill_* and *_l2gap* traces and L1-only for all other pfbench traces",
    )
    parser.add_argument(
        "--config", default=str(CONFIG), help="ChampSim config JSON"
    )
    parser.add_argument(
        "--build-only",
        action="store_true",
        help="build the simulator binary and exit",
    )
    parser.add_argument(
        "--rebuild",
        action="store_true",
        help="rebuild even if the binary exists",
    )
    parser.add_argument(
        "--skip-tests",
        action="store_true",
        help="skip flagbench/test_tuners.py",
    )
    parser.add_argument("--no-plots", action="store_true")
    parser.add_argument(
        "--report-only",
        metavar="DIR",
        help="only print the table and plot for an existing results directory (works on any OS)",
    )
    args = parser.parse_args()

    if args.report_only:
        report(args.report_only, args.no_plots)
        return
    build_dir = Path(args.build_dir)
    if " " in str(build_dir):
        fail(f"--build-dir must not contain spaces: {build_dir}")
    config_path = Path(args.config).resolve()
    if not config_path.is_file():
        fail(f"Config not found: {config_path}")

    if args.build_only:
        preflight()
        exe = build(build_dir, args.rebuild, config_path)
        print(f"\nBuild complete: {exe}")
        return

    trace_dir = Path(args.trace_dir).expanduser()
    if args.all:
        traces = sorted(
            p
            for p in trace_dir.iterdir()
            if p.is_file() and any(p.name.endswith(s) for s in TRACE_SUFFIXES)
        )
        if not traces:
            fail(
                f"No trace files ({', '.join(TRACE_SUFFIXES)}) found in {trace_dir}"
            )
    elif args.trace:
        traces = [find_trace(t, trace_dir) for t in args.trace]
    else:
        parser.error(
            "--trace, --all, or --build-only is required (or use --report-only DIR)"
        )
    traces = [Path(t).resolve() for t in traces]

    preflight(traces)
    exe = build(build_dir, args.rebuild, config_path)
    exe_l1only = exe
    if args.auto_hierarchy:
        l1_dir = Path(args.l1only_build_dir)
        if " " in str(l1_dir):
            fail(f"--l1only-build-dir must not contain spaces: {l1_dir}")
        exe_l1only = build(
            l1_dir,
            args.rebuild,
            CHAMPSIM / "champsim_config_knobbed_l1only.json",
        )

    output = (
        Path(args.out).resolve()
        if args.out
        else CHAMPSIM / "results" / "knob-carbs-windows"
    )
    output.mkdir(parents=True, exist_ok=True)
    env = dict(
        os.environ, PYTHONPATH=str(ROOT)
    )  # knob_carbs.py imports flagbench.tuners from the repo root
    ncpu = os.cpu_count() or 4
    jobs = args.jobs or (ncpu if args.exhaustive else min(ncpu, 3 * args.seeds))
    trace_jobs = args.trace_jobs or max(1, ncpu // jobs)
    trace_jobs = min(trace_jobs, len(traces))
    print(
        f"\n== tuning knobs on {len(traces)} trace(s): {trace_jobs} at a time, "
        f"up to {jobs} simulations each",
        flush=True,
    )

    def tune(trace):
        kernel = kernel_name(trace)
        sim_exe = (
            exe
            if (
                not args.auto_hierarchy
                or kernel.startswith("fill_")
                or kernel == "filllevel"
                or "_l2gap" in kernel  # stream_holes --gap-level=l2
            )
            else exe_l1only
        )
        cmd = [
            sys.executable,
            TUNER,
            "--kernel",
            kernel,
            "--trace",
            trace,
            "--out",
            output / f"{kernel}.json",
            "--binary",
            sim_exe,
            "--budget",
            args.budget,
            "--seeds",
            args.seeds,
            "--warmup",
            args.warmup_instructions,
            "--sim",
            args.sim_instructions,
            "--jobs",
            jobs,
            *(("--exhaustive",) if args.exhaustive else ()),
        ]
        log = output / f"{kernel}.log"
        with open(log, "w", encoding="utf-8") as f:
            rc = subprocess.run(
                [str(a) for a in cmd], env=env, stdout=f, stderr=subprocess.STDOUT
            ).returncode
        if rc:
            raise RuntimeError(f"exit {rc}, see {log}")
        return f"{kernel} ({sim_exe.name})"

    failed = []
    with ThreadPoolExecutor(max_workers=trace_jobs) as ex:
        futs = {ex.submit(tune, t): kernel_name(t) for t in traces}
        for i, fut in enumerate(as_completed(futs), 1):
            try:
                print(f"[{i}/{len(futs)}] tuned {fut.result()}", flush=True)
            except Exception as e:
                failed.append(futs[fut])
                print(f"[{i}/{len(futs)}] FAILED {futs[fut]}: {e}", flush=True)
    if failed:
        fail(f"{len(failed)} trace(s) failed: {', '.join(failed)}")

    if not args.skip_tests:
        run_stage(
            "tuner unit tests",
            [sys.executable, ROOT / "flagbench" / "test_tuners.py"],
            cwd=ROOT / "flagbench",
        )

    print("\n== report")
    report(output, args.no_plots)
    print("\nWorkflow complete.")
    print(f"Results:    {output}")
    print(f"Simulator:  {exe}")


if __name__ == "__main__":
    main()
