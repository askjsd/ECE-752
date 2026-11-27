# pfbench: prefetcher stress-test microbenchmarks

`pfbench` is a set of small C kernels that stress hardware prefetchers, including
adaptive ones such as Feedback Directed Prefetching (FDP). They test whether a
prefetcher converges on the right degree and distance, puts each fill in the right
cache level, and how it compares with software prefetching. The same binary runs
natively (with Linux perf counters), under gem5 (m5 ROI markers), and as a
DynamoRIO → ChampSim trace that starts at the ROI.

```bash
cd pfbench && make                    # pfbench + pfbench_noswpf
./pfbench                             # list kernels
./pfbench gather --help               # options of one kernel
./pfbench gather --dist=64            # one run -> one RESULT line
./run_all.py --cpu 2                  # whole suite -> results/results.csv
```

## Target geometry

Default working-set sizes come from the target hierarchy. It matches
[champsim_config.json](../champsim/champsim_config.json): L1D 64 sets × 8 ways,
L2 512 × 8, LLC 1024 × 16, with 64 B lines.

| Level | Lines | Bytes | Flag |
|---|---|---|---|
| L1D | 2^9 = 512 | 32 KiB | `--l1-lines` |
| L2 | 2^12 = 4,096 | 256 KiB | `--l2-lines` |
| L3 | 2^14 = 16,384 | 1 MiB | `--l3-lines` |

The line size is set with `--line`.

**Every kernel except `filllevel` is an L1-only study.** A "large" region is
16 × L1 by default (`--big-lines`, 8,192 lines, 512 KiB), which only has to be far
bigger than L1. Simulate these kernels with
[champsim_config_knobbed_l1only.json](../champsim/champsim_config_knobbed_l1only.json).
It is the knobbed config with L2 and LLC reduced to one 1-cycle line each, so an
L1D miss costs a DRAM access:

```bash
KNOBBED_CONFIG=$PWD/champsim/champsim_config_knobbed_l1only.json \
KNOBBED_BUILD=$HOME/champsim-knobbed-l1only champsim/build_knobbed.sh
# -> ~/champsim-knobbed-l1only/bin/champsim_knobbed_l1only
```

`filllevel` needs the full hierarchy (`champsim_config_knobbed.json`).

**Addresses are virtual only.** ChampSim's identity-mapped mode is hard-wired
(`identity_mapped()` in `champsim/source-*/src/ooo_cpu.cc`): no TLBs, no page walks,
and the knobbed prefetcher crosses pages freely. Pages play no part in any test. Keep
translation off.

**Native runs** still have L2 and L3 behind L1. Use `run_all.py --geometry host`
so the sizes match this machine's caches. Native misses beyond L1 are then served
mostly by L2, not DRAM.

## Review of the original spec: what changed and why

| Spec item | Problem | What pfbench does |
|---|---|---|
| Strides such as "+4, +8" | Prefetchers train on **line** addresses. A +4 or +8 *element* stride (16 or 32 B) stays inside one 64 B line, so a line-granular prefetcher sees a plain sequential stream. | Strides are given in lines (`--s1-lines`) or bytes (`--stride-bytes`), never elements. |
| Alternating direction "+4, −4, +4, −4" | With equal steps the walk only touches **two** addresses, so after the first two accesses everything hits and there is nothing to prefetch. | `stride_zigzag` uses +fwd/−back with net progress, default +3/−1 lines (every line visited once, out of order). Equal steps are allowed but produce a warning. |
| Stride > 2^n − 1 | A **signed** n-bit field already overflows at 2^(n−1). A truncated stride also does not just "miss": stride mod 2^n *aliases* to another stride, and the prefetcher issues wrong prefetches. | `stride_overflow` uses stride = 2^n + alias. The default is n = 11 bytes, alias 64 B, so 2112 B truncates to +64 B, a useless next-line prefetch. It prints the truncated value. Note: ChampSim's `ip_stride` and the knobbed prefetcher store full-width strides, so this tests a narrow-field table, not those. |
| Stride > page | Dropped: addresses are virtual with no translation, so page boundaries do not exist for the prefetcher. | `stride` is a plain constant-stride control (default 4 lines). Avoid large power-of-two strides: under identity mapping an 8192 B stride sent every access to one DRAM bank (IPC 0.35 with or without prefetching, against 0.64 → 0.72 at 8256 B). |
| Stream with holes | Skipping lines *in space* tests accuracy, not distance: no degree or distance setting avoids a skipped line. To test **distance**, the holes are *in time*: between consecutive stream lines come `gap` unique random accesses, each of which evicts something from L1. | `stream_holes` reads a stream line, then `--gap` unique random lines. A prefetch issued d steps early survives only if d·(gap+1) < L1 lines, so the useful distance is about L1/(gap+1): 3 at the default gap of 128, and 1 at gap 500. In the L1-only build, useful/issued prefetches are 441/475 at distance 1, 153/671 at distance 4 and 0/973 at distance 8. Spatial holes remain available with `--hole-lines`. |
| Phase test: random → stream → random | Purely random single accesses never train a stream or stride prefetcher, so it issues nothing and FDP has nothing to throttle. FDP also samples over intervals (the original uses 8192 evictions; knobbed uses epochs of demand accesses), so phases must be much longer than that. | The random phase uses short runs (`--rand-run-lines=2`) that *bait* the prefetcher into inaccurate, polluting prefetches. Phases are 2^18 accesses, with per-phase times as `PHASE` lines. `--rand-run-lines=1` gives the pure-random control. |
| Hot set + "unpredicted cold array" for pollution | Single-use random addresses are not prefetched, so they cause **demand** pollution, not prefetcher pollution. | The defaults are sized so demand traffic alone still fits in L1 (hot reuse distance 384 of 512 lines). Cold runs of 2+ lines bait prefetches past their end, and only those push the hot set out. Compare `--cold-every=0` and `--cold-run-lines=1`. |
| Late prefetching | Reading every word gives the prefetcher 8 accesses of slack per line. | `late` reads one word per line with no compute. `--work=N` adds dependent ALU ops to sweep the slack. |
| Fill-level test: "verify via L1/L2/L3 miss rates" | (1) Aggregate miss rates cannot say **which** region missed. (2) Reuse distance depends on **all** interleaved traffic, not just the region's own size. (3) A one-shot *sequential* region is the ideal prefetch target: the right policy is to prefetch it with low-priority or non-temporal insertion, not to skip it. Only a *random* one-shot region should not be prefetched. | `filllevel` computes each region's reuse distance with every region counted, prints it against capacity, and warns on a mismatch. It prints `REGION name start end` ranges for binning simulator misses by address, and `--p2/--p3/--ps=0` turns regions off for ablation. `--one-shot=seq|random` selects the one-shot pattern. Each region has its own load PC. |
| L1-target region "should be prefetched into L1" | In steady state a region that fits in L1 always hits, so there is nothing to prefetch. Prefetch placement matters for the L2/L3 regions and for whether their prefetches evict R1. | Measure R1's misses: ideally zero, and any R1 miss means R1 was evicted (pollution). |
| Pointer chasing, step size k | k is the number of next nodes prefetched along the chain (the degree); the distance d is how many hops ahead the first one is. A random list has no address pattern, so in hardware only a prefetcher that remembers the chain (Markov) can do this, after seeing the chain once. In software the nodes must carry pointers to upcoming nodes; these do not shorten the serial chain, they only let prefetches run ahead of it. | `chase`: plain traversal, two traversals in the ROI (the first trains the prefetcher; cover it with ChampSim warm-up); sweep the prefetcher's degree there. In the L1-only build with knobbed Markov (distance, degree): off 0.070 IPC, (1,1) 0.120, (1,2) 0.160, (1,8) 0.302. `jump --dist=d --k=k`: every k hops, software-prefetch the k nodes d..d+k−1 hops ahead (each node exactly once). |
| SW prefetch for A[B[C[i]]] | A prefetch of `A[B[C[i+d]]]` needs `B[C[i+d]]`, which can itself miss. | Staggered prefetching (Ainsworth & Jones, CGO'17): prefetch `&B[C[i+2d]]` and `&A[B[C[i+d]]]`. |
| "Compiled with and without `__builtin_prefetch`" | | Two binaries: `pfbench_noswpf` has no prefetch instructions at all, while `pfbench` has a separate loop per locality hint (`--locality` 3/2/1/0 → `prefetcht0/t1/t2/nta`). |
| Per-level miss rates natively | Linux generic perf events cover L1D and LLC but **not L2**. On this machine `perf_event_paranoid=4` blocks perf for normal users. | L1D/LLC read access and miss are counted when permitted. Add L2 with `--pmu-raw`, for example `--pmu-raw=l2_rd_access=0xe124,l2_rd_miss=0x2124` (Golden Cove `L2_RQSTS.ALL_DEMAND_DATA_RD`/`DEMAND_DATA_RD_MISS`; check `perf list`). Enable perf with `sudo sysctl kernel.perf_event_paranoid=1`. |

## Kernels

| Kernel | Pattern | Key options |
|---|---|---|
| `chase` | random linked list (one node per line), HW prefetching only | `--nodes --node-bytes --steps --warmup` |
| `jump` | same list; every k hops, SW-prefetch the next k nodes starting d hops ahead | `--k --dist --swpf --locality` (k > 6 needs `--node-bytes=128`) |
| `gather` | `sum += A[B[i]]` | `--dist --locality --pf-index --a-lines --n` |
| `scatter` | `A[B[i]] = i` (software prefetch uses write intent) | same as gather |
| `gather2` | `sum += A[B[C[i]]]` | same + `--b-lines` |
| `stride` | constant stride (control), default 4 lines | `--stride-bytes --region-lines --accesses` |
| `stride_overflow` | stride = 2^n + alias | `--bits --unit --signed --alias` |
| `stride_alt` | +s1, +s2, ... lines | `--s1-lines --s2-lines` |
| `stride_zigzag` | +fwd, −back, ... lines | `--fwd-lines --back-lines` |
| `bursts` | short strided bursts, then random single-use jumps, from one load PC | `--burst-lines --burst-stride-lines --jumps` |
| `stream` / `stream_rev` | forward / backward sequential stream | `--step-bytes --region-lines` |
| `stream_holes` | stream line, then `gap` unique random lines (too-early prefetch test) | `--gap --stream-accesses --gap-lines`, spatial: `--hole-lines --run-lines --random-holes` |
| `phase` | random (bait runs) ↔ stream phases | `--phase-accesses --phases --rand-run-lines` |
| `pollute` | hot set in L1 plus single-use cold runs | `--hot-lines --cold-every --cold-run-lines` |
| `late` | one load per line, minimal compute | `--work --step-bytes` |
| `filllevel` | R1/R2/R3 regions plus a one-shot region on a tick schedule | `--r1/r2/r3-lines --p2 --p3 --ps --order --one-shot` |

Common options: `--line --l1-lines --l2-lines --l3-lines --big-lines --seed
--quiet --perf --pmu-raw`. Integers accept `0x`, `K`, `M` and `G`. A misspelled
option is an error.

### Fill-level schedule

Each tick reads R1. Every `p2` ticks it also reads R2, every `p3` ticks R3, and
every `ps` ticks a never-reused line. With r1, r2, r3 = L1/2, L2/2, L3/2 and
p2, p3, ps = 2, 16, 64:

| Region | Lines | Reuse distance (all traffic) | Capacity | Utilisation |
|---|---|---|---|---|
| R1 | 256 | 256 + 128 + 16 + 4 = 404 | L1 512 | 79% |
| R2 | 2,048 | 256 + 2048 + 256 + 64 = 2,624 | L2 4,096 | 64% (> L1) |
| R3 | 8,192 | 256 + 2048 + 8192 + 2048 = 12,544 | L3 16,384 | 77% (> L2) |

Ideal steady state: R1 hits L1. R2 misses L1 and hits L2, or hits L1 if a
timely prefetch puts it there without evicting R1. R3 hits L3. One-shot lines
never cause an R1/R2/R3 miss.

## Implementation notes

- **Scalar, ordered accesses.** Pattern loads go through `volatile`, and the code
  is built with `-fno-tree-vectorize -fno-tree-loop-distribute-patterns
  -fno-unroll-loops`. Results go to a `volatile` sink.
- **One load instruction per pattern.** The stride variants share one loop that
  selects the delta with `cmov`, and `bursts` has one load for bursts and jumps.
  This matters for PC-indexed prefetchers: unrolling would give each PC its own
  constant stride and hide the pattern. Check with `make disasm`.
- **No address arrays where they would add a stream.** Random single-use orders
  come from a bijective hash (`perm_n` in `pfb.h`), so there is no index array
  whose own sequential stream a prefetcher could train on. The exceptions are the
  indirect kernels, where the index arrays are part of the pattern.
- **Memory.** Every region is 2 MiB-aligned and pre-touched before the ROI. A
  `REGION name start end` line is printed per region.

## ROI and measurement

`pfb_roi_begin()` and `pfb_roi_end()` are real non-inlined functions around the
measured loop. Setup and warm-up stay outside.

- **Native:** `CLOCK_MONOTONIC` time and perf counters, printed as one line:
  `RESULT kernel=... accesses=... time_ns=... ns_per_access=... [l1d_rd_miss_rate=...] checksum=...`.
  Pin to a P-core on this hybrid i9-12900 (`--cpu 2`, CPUs 0–15 are P-core threads).
- **gem5:** `make m5 M5_LIB=.../libm5.a` adds `m5_work_begin/end` (needs an x86
  `util/m5` build).
- **ChampSim:** trace from the ROI symbol (non-PIE build):

  ```bash
  export DR=~/pin-dl/dr/DynamoRIO-Linux-11.3.0-1 PY=~/pin-dl/venv/bin/python \
         LD_LIBRARY_PATH=~/pin-dl/libs/usr/lib/x86_64-linux-gnu
  ../champsim/dr_trace/trace_dr.sh -o gather.champsim.xz -S pfb_roi_begin -t 2000000 -W 50000000 \
      -- ./pfbench gather --dist=16
  KNOBBED_HINT="source=stride,level=4,throttle=0" \
      ~/champsim-knobbed-l1only/bin/champsim_knobbed_l1only \
      --warmup-instructions 500000 --simulation-instructions 1500000 gather.champsim.xz
  # or every benchmark:
  ./run_all.py --wrap "../champsim/dr_trace/trace_dr.sh -o traces/{name}.champsim.xz -S pfb_roi_begin -t 2000000 --"
  ```

  `-W` must cover the setup before the ROI as well as the traced instructions.
  Keep `-t` within the ROI (each RESULT line reports `accesses`, at about 3–10
  instructions per access).

### Caveat: software prefetches in ChampSim traces

[to_champsim.py](../champsim/dr_trace/to_champsim.py) records
`prefetch*` instructions as ordinary loads. In ChampSim a software prefetch
therefore (a) holds an LQ/ROB entry and **blocks retirement until its data
arrives**, unlike a real `PREFETCHh`, which retires immediately, (b) loses its
locality hint and always fills L1, and (c) is counted as a demand `LOAD`. The
software-prefetch speedups ChampSim reports are therefore conservative, and the
locality sweep is meaningful natively and in gem5 only. Comparing degrees is also
unreliable: in ChampSim `jump --dist=8` gives IPC 0.202 at k=1 but 0.171 at k=4,
while natively k=4 is fastest (5.0 against 6.3 ns per hop), because a batch of k
blocking prefetches stalls retirement. A planned fix is to tag prefetch records in
`to_champsim.py` and have `ooo_cpu` issue them as non-blocking L1D prefetches
(ChampSim's input format has no prefetch flag).

## Runner

`run_all.py` runs 56 configurations (the kernel defaults plus sweeps of
software-prefetch distance, step size k and locality, burst length, `stream_holes --gap`, `late --work`,
pollution-run length and fill-level ablations). It writes
`results/results.csv` with one column per RESULT key, keeps every run's log in
`results/logs/`, and supports `--only`, `--repeat`, `--cpu`,
`--geometry host`, `--wrap` and `--dry-run`. Options after `--` are passed to
every run.

## Status

**Verified so far**

- Native: all 56 `run_all.py` configurations run and pass (target geometry, pinned
  to a P-core with `--cpu 2` on the i9-12900 host). Native results are timing only,
  because `perf_event_paranoid=4` on that host blocks the cache-miss counters.
- `stream_holes --gap=128`, L1-only ChampSim, knobbed stride prefetching, useful
  out of issued prefetches: 441/475 at distance 1, 153/671 at distance 4, 0/973 at
  distance 8. Prefetching too early gets the line evicted, as intended.
- `gather` software prefetching, full ChampSim config: IPC 0.456 → 0.570.
- `chase` with the knobbed Markov source, L1-only ChampSim, IPC: off 0.070,
  (1,1) 0.120, (1,2) 0.160, (1,8) 0.302, (2,4) 0.249, (4,4) 0.286. The ROI holds
  two traversals so the first can train the prefetcher.
- `jump`, step size k = number of next nodes prefetched: natively 21.5 ns per hop
  with no prefetching, 6.3 at k=1, 5.0 at k=4.
- Page crossing in ChampSim, using a separate sequential-list trace (not part of
  this suite), knobbed next-line at level 7 (distance 8): IPC 0.254 with the old
  page limit, 0.370 with virtual-only mode hard-wired. IPC no longer falls as the
  distance grows.

**Not yet done**

- Simulating the whole suite in ChampSim. The stride pathologies, `bursts`,
  `stream`/`stream_rev`, `phase`, `pollute`, `late`, `scatter`, `gather2` and
  `filllevel` have been run natively only.
- Non-blocking software prefetches in ChampSim (see the caveat above).

**Caveats**

- Software prefetches are modelled as blocking loads in ChampSim (above).
  Compare distance and k natively, or treat ChampSim's numbers as a lower bound.
- `chase` needs ChampSim warm-up covering the first traversal (about `--nodes` ×
  5 instructions) for a Markov prefetcher to have learned the chain.
- `jump --k` above 6 needs `--node-bytes=128`.
- Avoid large power-of-two strides: they map every access to one DRAM bank.
- `filllevel` needs the full hierarchy. Every other kernel is an L1-only study.
- Native runs still have L2 and L3 behind L1, so a native L1 miss is usually an L2
  hit, not a DRAM access.
- `~/champsim-knobbed-windows` was built before virtual-only mode was hard-wired.
  Rebuild it before use.
