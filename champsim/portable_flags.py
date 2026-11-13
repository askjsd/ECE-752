"""Portable FlagBench flag-space definition.

The logical names intentionally match the gem5/GCC experiment.  The native
Windows backend uses WSL MinGW GCC so these switches retain GCC semantics while
the resulting PE executable remains traceable by Windows Intel PIN.
"""

DEFAULT_FLAGS = (
    "unroll-loops", "peel-loops", "tree-loop-vectorize",
    "tree-slp-vectorize", "inline-functions", "ipa-cp-clone",
    "unswitch-loops", "predictive-commoning", "tracer",
    "schedule-insns", "ivopts", "split-loops",
)

MAX_FLAGS = 16


def validate(names):
    names = tuple(names)
    if not 1 <= len(names) <= MAX_FLAGS:
        raise ValueError(f"flag space must contain 1..{MAX_FLAGS} flags")
    if len(set(names)) != len(names):
        raise ValueError("flag names must be unique")
    return names


def config_flags(config_id, names):
    return [
        f"-f{name}" if config_id & (1 << bit) else f"-fno-{name}"
        for bit, name in enumerate(names)
    ]
