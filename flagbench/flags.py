"""Flag space, config encoding and tool detection shared by all flagbench scripts.

Config encoding: config_id is an N-bit int; bit j (value 1<<j) = 1 means flags[j] is ON (-fX),
0 means OFF (-fno-X). Every build passes every flag explicitly on top of BASE_FLAGS.
"""
import os
import re
import subprocess

ROOT = os.path.dirname(os.path.abspath(__file__))
GEM5_ROOT = os.path.dirname(ROOT)
GEM5_BIN = os.path.join(GEM5_ROOT, "build", "X86", "gem5.opt")
M5_INC = os.path.join(GEM5_ROOT, "include")
M5_LIB = os.path.join(GEM5_ROOT, "util", "m5", "build", "x86", "out")
SIM_CONFIG = os.path.join(ROOT, "sim_config.py")
KERNEL_DIR = os.path.join(ROOT, "kernels")
CC = os.environ.get("FLAGBENCH_CC", "gcc")
BASE_FLAGS = ["-O3", "-march=x86-64"]  # same for every build
MAX_N = 16

KERNELS = ["k_stencil", "k_ptrchase", "k_branchy", "k_reduce", "k_crc32", "k_helper"]

# Spec candidate list. Default selection (first N of DEFAULT_ORDER) is a hand-picked diverse subset;
# override with --flags. All must be semantics-preserving (no fast-math/trapv/aliasing flags).
CANDIDATES = [
    "unroll-loops", "unroll-all-loops", "peel-loops", "tree-loop-vectorize", "tree-slp-vectorize",
    "prefetch-loop-arrays", "inline-functions", "ipa-cp-clone", "unswitch-loops", "split-loops",
    "predictive-commoning", "tracer", "schedule-insns", "sched-pressure", "ivopts",
    "gcse-after-reload", "align-loops", "align-functions", "loop-interchange",
    "version-loops-for-strides",
]
DEFAULT_ORDER = [
    "unroll-loops", "peel-loops", "tree-loop-vectorize", "tree-slp-vectorize",
    "inline-functions", "ipa-cp-clone", "unswitch-loops",
    "predictive-commoning", "tracer", "schedule-insns", "ivopts",
    "split-loops", "loop-interchange", "gcse-after-reload", "sched-pressure",
    "align-loops",
]
# NOTE: prefetch-loop-arrays is excluded: with -march=x86-64 gcc 13.3 reports no state for it
# (unsupported on this target), so it would be a silent no-op.


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=isinstance(cmd, str), capture_output=True, text=True, **kw)


def gcc_version():
    return sh([CC, "--version"]).stdout.splitlines()[0]


# `gcc -Q --help=optimizers -O3` reports -funroll-loops as [enabled], but plain `gcc -O3` generates the same code
# as an explicit -fno-unroll-loops and NOT the code of an explicit -funroll-loops (verified by .text hash
# comparison on 15 kernels with check_o3_defaults.py; all other candidate flags agree with -Q).
# build_lut.py additionally asserts that the recorded -O3 config is byte-identical (.text) to plain -O3.
O3_DEFAULT_OVERRIDES = {"unroll-loops": False}


def o3_defaults(flag_names):
    """Parse `gcc -Q --help=optimizers -O3` -> {flag: bool enabled}. Raises if a flag is unknown."""
    out = sh([CC, *BASE_FLAGS, "-Q", "--help=optimizers"]).stdout
    table = {}
    for line in out.splitlines():
        m = re.match(r"\s+-f(\S+?)\s+\[?(enabled|disabled)\]?", line)
        if m:
            table[m.group(1)] = m.group(2) == "enabled"
    missing = [f for f in flag_names if f not in table]
    if missing:
        raise SystemExit(f"flags not supported by {gcc_version()}: {missing}")
    d = {f: table[f] for f in flag_names}
    for f, v in O3_DEFAULT_OVERRIDES.items():
        if f in d: d[f] = v
    return d


def check_both_forms(flag_names):
    """Each flag must be accepted by the compiler in both -fX and -fno-X form."""
    bad = []
    for f in flag_names:
        for form in (f"-f{f}", f"-fno-{f}"):
            r = sh([CC, *BASE_FLAGS, form, "-x", "c", "-c", "/dev/null", "-o", "/dev/null"])
            if r.returncode or r.stderr.strip():  # warnings (e.g. "ignored") also disqualify
                bad.append(form)
    if bad:
        raise SystemExit(f"compiler rejects: {bad}")


def select_flags(n, names=None):
    names = names or DEFAULT_ORDER[:n]
    if not (1 <= len(names) <= MAX_N):
        raise SystemExit(f"N must be in 1..{MAX_N}")
    for f in names:
        if f not in CANDIDATES:
            raise SystemExit(f"{f} is not an allowed candidate flag")
    check_both_forms(names)
    defaults = o3_defaults(names)
    return names, defaults


def config_flags(config_id, names):
    return [f"-f{f}" if (config_id >> j) & 1 else f"-fno-{f}" for j, f in enumerate(names)]


def o3_config_id(names, defaults):
    return sum(1 << j for j, f in enumerate(names) if defaults[f])


def work_dir(base, kernel):
    return os.path.join(base, kernel)
