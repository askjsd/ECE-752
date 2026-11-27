# knobbed: a tunable baseline prefetcher for ChampSim

A stride / next-line prefetcher whose behaviour is controlled by the knobs in
[flagbench/prefetcher_knobs.md](../../../../flagbench/prefetcher_knobs.md). It
lives here, outside the ChampSim source trees, and is loaded as an external
ChampSim module. It compiles against both trees in this repo:
`champsim/source-linux` and `champsim/source-windows`.

## Build and run

```bash
champsim/build_knobbed.sh            # Linux tree -> ~/champsim-knobbed-linux/bin/champsim_knobbed
champsim/build_knobbed.sh windows    # Windows tree, built on Linux (compile check only)
KNOBBED_HINT="source=stride,level=5,throttle=0" \
  ~/champsim-knobbed-linux/bin/champsim_knobbed \
  --warmup-instructions 300000 --simulation-instructions 2500000 trace.trace
```

The script copies the chosen source tree to a path without spaces (ChampSim's
Makefile breaks on spaces), adds this module, and builds there. It never modifies
the source trees. Neither tree contains the compiled vcpkg libraries, so it
copies them from `champsim/.upstream` (see
[QUICKSTART_LINUX.md](../../../QUICKSTART_LINUX.md) step 1). For
`source-windows` it also changes the Windows-only `-llibz` link flag to `-lz` in
the copy's Makefile.

The build uses [champsim_config_knobbed.json](../../../champsim_config_knobbed.json),
the normal config with `L1D.prefetcher = "knobbed"`. To attach the module
elsewhere (for example L1I or L2C), change `prefetcher` in that file.

**On Windows** (not tested): from inside `champsim\source-windows`, run
`python config.sh --module-dir <repo>\champsim\modules ..\champsim_config_knobbed.json`
and then your usual `make`. The module sticks to standard C++ and 64-bit
integer types, so the Windows 32-bit `long` should not matter. The Windows
runners (`run_windows.py`) do not build this configuration yet.

Both trees produce identical cycle counts for the same trace and settings
(checked on the streaming trace below).

## Knobs

Set the default hint with `KNOBBED_HINT="key=value,key=value"` (the keys are the
knob names below). Every knob uses all of its codes, so no setting duplicates another.

| Key | Values | What it does |
|---|---|---|
| `source` | `off`, `nextline`, `stride` (learned per PC), `markov` (miss-to-miss correlation: follows a repeated irregular sequence such as a re-walked linked list) | The engine. `off` disables prefetching. |
| `trigger` | `miss`, `tagged` (miss + first use of a prefetched block), `access` | When to issue. The tuner searches `tagged` and `access` only: `miss` was never better in the sampled data. |
| `level` | 0-7 | Aggressiveness ladder: sets distance and degree together (table below). |
| `fill` | `l1d`, `low`, `high`, `l2` | Which level a prefetch fills: always this level, this level only when confidence is at least 0.34 (`low`) or 0.67 (`high`), or always the next level down. |
| `confirm` | 1, 2 | Matching strides (stride) or repeats of the first link (markov) needed before issuing. |
| `throttle` | `0` / `1` | Let the runtime controller step the level down when accuracy is low or prefetches pollute, and back up when healthy. It never goes above `level`. |
| `fallback` | `0` / `1` | When the engine has nothing to issue (stride not trained yet, no Markov link), send 2 next-line blocks to the next level down. |
| `word` | a packed 32-bit control word, e.g. `word=0x4a5` (layout in `knobbed.h`) | Sets every field at once. Words with an undefined field (trigger 3) are rejected. |

Ladder (distance, degree):

| level | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| nextline / stride | 1,1 | 1,2 | 1,4 | 1,8 | 2,4 | 4,4 | 4,8 | 8,8 |
| markov | 1,1 | 1,2 | 1,4 | 1,8 | 2,2 | 2,4 | 2,8 | 4,4 |

Distances stop at 8 for stride and next-line. This cap dates from when prefetches
could not cross a 4 KB page (a larger distance would only have meant "prefetch less
often"). This project runs ChampSim in virtual-only mode, hard-wired in `ooo_cpu.cc`
(identity mapping, no TLBs or page walks), so prefetches cross pages freely. Keep it
that way: do not re-enable translation. For Markov, distance is how many chain
links are skipped, and long chains are rare.

**Insertion position** is a separate module, `modules/replacement/lru_pf`, used by
the L1D in `champsim_config_knobbed.json`. `KNOBBED_INSERT=mru` (default, identical to
plain LRU) or `lru` puts prefetched blocks at the least-recently-used position, so a
useless prefetch is evicted first. It applies to the whole cache, not per region.

Removed in this version: `enable` (now `source=off`), `drop` and the two fill
thresholds (now the four-value `fill` policy), `distance`/`degree` (now `level`),
`maxinflight` (it only capped prefetches that fill this level and overlaps with
degree and the prefetch queue), `pagecross` (prefetches always cross pages: this project
runs with translation off), the `DEAD_BLOCK` trigger (it behaved like `miss`) and `insert` in
the hint (now `KNOBBED_INSERT`).

Region hints: set `KNOBBED_REGIONS` to a file with one region per line,
`<start> <end> <key=value,...>` (byte addresses; `#` starts a comment). Each
region inherits the default hint and overrides what it lists. The last matching
region wins. A region is a starting point for its own feedback controller.

## Fixed constants (never tuned; printed at the start of every run)

| Constant | Value |
|---|---|
| Throttle epoch | 4096 demand accesses per region |
| Minimum prefetches per epoch to act on | 16 |
| Suspend below accuracy | 0.10 (for 4 epochs, then re-probe) |
| Step down below accuracy / above pollution | 0.40 / 0.30 of issued |
| Step back up above accuracy | 0.75 |
| Initial smoothed accuracy | 1.0 (updated every epoch even with `throttle=0`) |
| Fill gates | 0.34 (`low`), 0.67 (`high`) |
| Stride table | 256 sets x 4 ways |
| Markov table | 65536 links, LRU by training use |
| Pending queue / tracking maps | 64 / 4096 entries |
| In-flight / lower-level expiry | 2000 / 100000 cycles |
| Fallback | 2 next-line blocks to the next level |

`acc_ewma` and the confidence-gated fill adapt even when `throttle=0`: only the
ladder stepping and suspension are switched by the throttle knob.

## Telemetry and throttling

Per region the module counts issued prefetches, timely and late useful ones,
useful ones that were sent to a lower level, unused-evicted blocks, and
early-and-hurtful misses. It prints accuracy, coverage and timeliness at the end
of the simulation (totals restart when ChampSim ends warmup; learned state does not).
Coverage is approximated as useful / (misses + timely useful). "Issued" matches
ChampSim's own L1D `prefetch issued` count; "late" counts any demand miss on a block
with a prefetch outstanding, which is broader than ChampSim's `useful prefetch`.

With `throttle=1`, the controller runs each epoch of 4096 demand accesses:

| Epoch result | Action |
|---|---|
| accuracy below 10% | suspend for 4 epochs, then re-probe from the configured level |
| accuracy below 40%, or unused + early-hurtful above 30% of issued | one rung down |
| accuracy above 75% | one rung up, never above the configured level |

## Limits and what is not done

- **Fill level:** ChampSim only lets a prefetcher choose "this level" or "the
  next level down", so the L1/L2/LLC thresholds of section 2.1 reduce to the
  four-value `fill` policy.
- **Insertion position (2.2):** global per cache (`KNOBBED_INSERT`), mru or lru only;
  "mid" was dropped as unlikely to add anything.
- **Markov table thrashes when the list is larger than the table.** Under LRU a cyclic
  walk over more nodes than the table holds evicts every link just before it is
  needed; with 8192 entries on a 16384-node list it issued nothing. The default of
  65536 links is fine for the 16384-node kernel but not for large SPEC working sets.
- **Not implemented:** prefetch-aware replacement (2.3), Markov, GHB, SMS,
  content-directed and indirect prefetchers (3.2-3.6), and an instruction
  (Entangling) prefetcher. The telemetry and hint plumbing are written so those
  engines can be added behind the same hint word.
- **Validation so far:** tested on one synthetic streaming trace (unit-stride,
  256 B stride and random-gather loops, 2.5M instructions). IPC went from 0.205
  with `enable=0` to about 0.53 with the default hint and throttling, and
  distance, degree, trigger, source and confirm all changed the result. The
  unused-evicted and early-hurtful paths never fired on that trace, so they are
  untested.
