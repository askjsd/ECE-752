"""Portable FlagBench flag-space definition.

The logical names intentionally match the gem5/GCC experiment.  The native
Windows backend uses WSL MinGW GCC so these switches retain GCC semantics while
the resulting PE executable remains traceable by Windows Intel PIN.
"""

DEFAULT_FLAGS = (
    "unroll-loops",
    "peel-loops",
    "tree-loop-vectorize",
    "tree-slp-vectorize",
    "inline-functions",
    "ipa-cp-clone",
    "unswitch-loops",
    "predictive-commoning",
    "tracer",
    "schedule-insns",
    "ivopts",
    "split-loops",
)

EXTENDED_FLAGS = (
    "unroll-all-loops",
    "sched-pressure",
    "gcse-after-reload",
    "align-loops",
    "align-functions",
    "loop-interchange",
    "version-loops-for-strides",
    "loop-unroll-and-jam",
    "tree-loop-distribution",
    "ipa-icf",
    "split-paths",
    "schedule-insns2",
)

SUPPORTED_FLAGS = DEFAULT_FLAGS + EXTENDED_FLAGS
MAX_NFLAGS = len(SUPPORTED_FLAGS)
MAX_FLAGS = 64


def validate(names):
    names = tuple(names)
    if not 1 <= len(names) <= MAX_FLAGS:
        raise ValueError(f"flag space must contain 1..{MAX_FLAGS} flags")
    if len(set(names)) != len(names):
        raise ValueError("flag names must be unique")
    return names


def select(nflags=None, names=None):
    if names is not None and nflags is not None:
        raise ValueError("use --nflags or --flags, not both")
    if nflags is not None and not 1 <= nflags <= MAX_NFLAGS:
        raise ValueError(f"nflags must be in 1..{MAX_NFLAGS}")
    if names is None:
        selected = (
            DEFAULT_FLAGS if nflags is None else SUPPORTED_FLAGS[:nflags]
        )
    else:
        selected = names
    return validate(selected)


def config_flags(config_id, names):
    return [
        f"-f{name}" if config_id & (1 << bit) else f"-fno-{name}"
        for bit, name in enumerate(names)
    ]
