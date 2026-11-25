# TAGE-inspired prefetch setting learner

`tage` is a separate, hint-free alternative to the region-hint `knobbed`
prefetcher. It hashes the current load PC, cache-line address, and recent
memory-line history into tagged tables with history lengths 0, 1, 2, 4, and 8.
The longest matching table supplies a prefetch action. A miss allocates a more
specific history entry, as in TAGE-style tagged predictors.

Each entry tracks trials and useful hits for eight fixed actions. It tries the
least-tested actions first, then chooses the action with the best smoothed hit
rate. A demand hit on a prefetched line is positive feedback; a late prefetch or
an unused prefetched line evicted before use is negative feedback. The feedback
updates the exact tagged entry and action saved when that prefetch was issued.
No `KNOBBED_HINT` or region-hint file is read.

| Action | Address source | Distance | Degree |
|---:|---|---:|---:|
| 0 | next line | 1 | 1 |
| 1 | next line | 1 | 2 |
| 2 | learned per-PC stride, with next-line fallback | 1 | 1 |
| 3 | learned per-PC stride, with next-line fallback | 2 | 1 |
| 4 | learned per-PC stride, with next-line fallback | 4 | 1 |
| 5 | learned per-PC stride, with next-line fallback | 2 | 2 |
| 6 | learned per-PC stride, with next-line fallback | 4 | 2 |
| 7 | learned per-PC stride, with next-line fallback | 8 | 4 |

## Build and run

From the repository root in WSL:

```bash
./champsim/setup_linux.sh
./champsim/build_tage.sh

"$HOME/champsim-tage-linux/bin/champsim_tage" \
  --warmup-instructions 300000 \
  --simulation-instructions 2500000 \
  /path/to/trace.champsimtrace
```

The build copies `source-linux` and its vcpkg dependencies into a space-free
directory under `$HOME`; it does not modify the source tree. The simulation
prints overall useful-hit rate and per-action hit rates at completion. Use a
long trace so all actions receive feedback before comparing them.
