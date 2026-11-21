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
KNOBBED_HINT="distance=4,degree=4,throttle=0" \
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

Set the default hint with `KNOBBED_HINT="key=value,key=value"`.

| Key | Values | Doc section |
|---|---|---|
| `enable` | `0` / `1` | 1.1 |
| `trigger` | `miss`, `tagged` (miss + first use of a prefetched block), `access` | 1.2 |
| `source` | `nextline`, `stride` (learned per PC) | 3.1 |
| `confirm` | 1-4 matching strides before issuing (`stride` only) | 3.1 |
| `distance` | 1-31 blocks (stride steps) ahead of the trigger | 1.3 |
| `degree` | 1-8 blocks issued per trigger | 1.4 |
| `fill` | confidence at or above this fills this cache level; below it fills the next level down | 2.1 |
| `drop` | confidence below this issues nothing | 2.1 |
| `insert` | `mru`, `mid`, `lru` (recorded in prefetch metadata only, see limits) | 2.2 |
| `throttle` | `0` / `1`: let the runtime learner adjust distance and degree | 1.6 |
| `word` | a packed 32-bit control word, e.g. `word=0x30c21b` (layout in `knobbed.h`) | 4 |

Region hints: set `KNOBBED_REGIONS` to a file with one region per line,
`<start> <end> <key=value,...>` (byte addresses; `#` starts a comment). Each
region inherits the default hint and overrides what it lists. The last matching
region wins. A region is a starting point for its own feedback controller.

## Telemetry and throttling (section 0)

Per region the module counts issued prefetches, timely and late useful ones,
useful ones that were sent to a lower level, unused-evicted blocks, and
early-and-hurtful misses. It prints accuracy, coverage and timeliness at the end
of the simulation. Coverage is approximated as useful / (misses + timely useful).

With `throttle=1`, the controller runs each epoch of 4096 demand accesses:

| Epoch result | Action |
|---|---|
| accuracy below 10% | suspend for 4 epochs, then re-probe |
| accuracy below 40% | lower degree |
| unused-evicted + early-hurtful above 30% of issued | lower distance |
| late above 10% of useful | raise distance |
| accuracy above 75% | raise degree |

Thresholds are in `knobbed_pf::throttle_params`.

## Limits and what is not done

- **Fill level:** ChampSim only lets a prefetcher choose "this level" or "the
  next level down", so the L1/L2/LLC thresholds of section 2.1 reduce to one
  `fill` threshold plus `drop`.
- **Insertion position (2.2):** the cache decides where a block is inserted. The
  chosen position travels in the prefetch metadata, but no replacement policy
  reads it yet.
- **Trigger `DEAD_BLOCK`** is reserved and behaves like `miss`.
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
