# Entangling instruction prefetcher: local review port

This is a native module for the existing Windows ChampSim tree. It ports the
authors' ISCA 2021 4K-entry implementation. It does not replace the original
next-line configuration, add a second simulator checkout, or implement the later
wrong-path-aware design. Changes are local and uncommitted.

## Paper and reference source

- Alberto Ros and Alexandra Jimborean, **A Cost-Effective Entangling Prefetcher
  for Instructions**, ISCA 2021.
  [Paper](https://webs.um.es/aros/papers/pdfs/aros-isca21.pdf),
  [DOI](https://doi.org/10.1109/ISCA52012.2021.00017).
- [Authors' repository](https://github.com/alberto-ros/EntanglingInstructionPrefetcher).
  The `IPC-1`, `ISCA-21`, and `TC-24` folders are different versions.
- Ported file:
  [`ISCA-21/ISCA_Entangling_4Ke.cc`](https://github.com/alberto-ros/EntanglingInstructionPrefetcher/blob/75c77536ac4b1fa1a801587df444ee1501f1b3aa/ISCA-21/ISCA_Entangling_4Ke.cc).
- Reference commit: `75c77536ac4b1fa1a801587df444ee1501f1b3aa`.
- Original Git blob: `e0f20ef6dc9464c386d2e1d59805bcf94defe056`.
- Original SHA-256: `c3ebf2925446c5a30d367de33929d6bbeeb979a99d91626710a0e2f89516c3fc`.
- An exact reference copy and its CC0-1.0 license are preserved under
  `reference/entangling/`. The `.txt` suffix keeps it out of module compilation.

The port also accounts for the shadow-structure synchronization pitfalls
discussed in Kalbande et al.,
[Untangling the Low Accuracy of the Entangling Instruction Prefetcher](https://www.cse.iitb.ac.in/~biswa/CAL26.pdf).
That follow-up discusses a newer wrong-path-aware implementation; it is useful
integration guidance, rather than the algorithm selected for this port.

## What the algorithm does

Suppose instruction line B is demanded at cycle 100 and fills at cycle 160.
Its observed fetch latency is 60 cycles. The predictor looks back through its
history for a source line A accessed by cycle 40, and learns A -> B. A later
access to A can request B early enough to hide that latency.

It also learns runs of consecutive instruction cache lines. If B begins a run
of four lines, triggering B can fetch B and the next three lines. These runs
are discovered dynamically from cache-line accesses; they are not the compiler's
static basic blocks.

The preserved reference mechanisms are:

| Mechanism | Reference setting/behavior |
| --- | --- |
| History | 16 entries; 20-bit time differences; six recent entries considered for merging |
| Entangled table | 256 sets x 16 ways = 4,096 entries; reference XOR hash and partial tags |
| Destination encoding | Six formats: 58, 28, 18, 13, 10, or 8 destination bits |
| Destination count | Up to six destinations, subject to the shared encoding format |
| Confidence | Two-bit counters; new/refreshed association at 3; zero invalidates |
| Feedback | Late/unused decreases confidence; timely use is credited on eviction |
| Regions | Six-bit number of lines after the head; original merging and size encoding |
| Latency | Original 12-bit modular timestamps |
| Source selection | Try two timely sources with space, then replace an association if necessary |
| Replacement | Reference enhanced FIFO/reallocation behavior |

The C++ objects consume host memory larger than the paper's compressed hardware
storage estimate. Keeping the original bit masks and entry limits models the
prediction behavior; this is not RTL or a hardware-area implementation.

## Changes needed for this ChampSim version

`prefetcher/entangling/entangling.h` declares the module. Most reference logic is
retained in the private state in `entangling.cc`, using its original `l1i_*`
helper names so it can be compared with the reference snapshot.

| Old interface | Port |
| --- | --- |
| Global arrays and global CPU selector | Independent heap-owned state per cache/module instance |
| `O3_CPU::l1i_prefetcher_*` callbacks | Current module callbacks on the L1 instruction cache |
| `current_core_cycle[]` | Cache time divided by the cache clock period |
| `prefetch_code_line` | Current `prefetch_line(address, true, metadata)` interface |
| Fixed compile-time L1I geometry | Shadow storage sized using the configured sets and ways |
| Approximate shadow victim selection | Actual victim way supplied by the fill callback |
| Assumed queue-to-fill completion | Resident/pending filters and removal of consumed requests without fills |
| Large per-address diagnostic array | Aggregate diagnostic counters; prediction tables unchanged |

The only added cache interface is the read-only
`CACHE::has_pending_request(address)` query. It searches the internal prefetch
queue, tag-check pipeline, translation stash, miss queue, and pending fills.
It does not change cache scheduling or request admission.

The port also guards a missing history entry and a full shadow timing table.
These cases must not cause an out-of-bounds write or assume unlimited storage.

## Required configuration and scope

Use the provided `champsim_entangling.json`:

- L1I `prefetcher`: `entangling`.
- L1I `virtual_prefetch`: `true`.
- L1I `prefetch_activate`: `LOAD`, so all demand tag checks, including hits and
  first uses of prefetched lines, are observed. Own prefetches do not train it.
- 64-byte cache lines, the current 4 KiB page configuration, at most 64 L1I sets.
  The virtual and physical set indices then use page-offset bits. The current
  64-set, 8-way L1I is supported. Larger set counts require a different shadow
  lookup/index mapping and are rejected.

The rest of the processor and cache hierarchy is inherited from the existing
baseline configuration. The new executable is `bin/champsim-entangling.exe`.
The ordinary `bin/champsim.exe` remains available.

Important interpretation limits:

1. Observations occur at current L1I demand tag checks, rather than the older
   simulator's CPU-side fetch callback. Queuing, translation, and access ordering
   can therefore differ. This is a reviewed API port, not a bit-for-bit timing
   reproduction of the old simulator.
2. This is the ISCA 2021 correct-path trace implementation. It does not add
   wrong-path instruction generation, squash recovery, or TC-24 behavior.
3. Virtual prefetches use the current simulator's translation machinery. Matching
   comparison configurations use the same virtual-address interface.
4. The original confidence, compression, and timestamp limitations remain.
   New confidence thresholds, destination caps, region caps, runtime hints, and
   CARBS control have deliberately not been added to the reference baseline.
5. Small FlagBench kernels can fit in L1I. A successful port does not imply a
   speedup on such a kernel; instruction-heavy workloads are needed for research.

## Build and inspect

From the ECE-752 root:

```powershell
python .\champsim\run_windows.py --build-only --build-config .\champsim\champsim_entangling.json
python .\champsim\tests\run_entangling_tests.py
```

Run the bounded three-policy validation:

```powershell
python .\champsim\check_entangling.py
```

This builds separate no-prefetch, next-line, and Entangling executables, keeping
all other configured parameters and the L1I virtual-address interface matched.
The script does not compile or trace a new user benchmark.

It runs:

- A generated 128 KiB instruction-footprint trace with repeated nonsequential
  region transitions, deliberately larger than the 32 KiB L1I. This is a
  mechanism stress test, not an application benchmark or paper replication.
- The previously saved Windows `k_stencil` trace window, when present. This
  window is not guaranteed to isolate the kernel's computation from startup.
- A repeated Entangling simulation for each input, checking identical metrics.

Both traces contain enough records for warmup plus measurement; the script fails
if it detects a trace replay. Results include cycles, IPC, L1I demand load misses,
and issued/useful/useless prefetches. The cache's useful/useless counts are not
identical to all of the paper's accuracy definitions; late and end-of-window
outstanding requests must be accounted for before publishing an accuracy value.

Focused tests cover timely source selection, timestamp wrap, missing history,
destination compression/capacity, confidence feedback, region length, actual-way
shadow fills/evictions, stale request removal, duplicate filtering, timing-table
pressure, and independent instances.

Logs and machine-readable results are under
`results/entangling-review-20261120/`, including `comparison.json`,
`comparison.csv`, and `tests.txt`. `--skip-build` reruns the comparison using
existing validation executables.

### Observed validation results, 2026-11-20

| Input | L1I policy | Cycles | L1I LOAD misses | Issued prefetches |
| --- | --- | ---: | ---: | ---: |
| Synthetic 128 KiB footprint, 400,000 measured instructions | None | 415,194 | 49,994 | 0 |
| Same synthetic trace | Next-line | 414,895 | 49,994 | 49,994 |
| Same synthetic trace | Entangling | 327,869 | 94 | 24,953 |
| Saved Windows window, 200,001 measured instructions | None | 157,417 | 1,791 | 0 |
| Same Windows trace | Next-line | 156,077 | 1,816 | 11,628 |
| Same Windows trace | Entangling | 157,507 | 1,361 | 444 |

Entangling's synthetic speedup over next-line is about 1.265x. On the saved
Windows window it takes about 0.92% more cycles despite fewer L1I misses. This is
evidence of working integration, not evidence of an application-level speedup.
The two Entangling reruns reproduced every collected metric exactly. No trace
replayed, and neither Entangling run reported shadow-cache resynchronizations.

The module's own `demand_accesses` and `misses` diagnostics count callback
invocations, including retries while the current cache waits for resources.
Use the standard cache counters in the table above for comparisons; the two
counter families do not have interchangeable denominators.

For a quick manual run after building:

```powershell
.\champsim\.upstream-windows\bin\champsim-entangling.exe `
  --warmup-instructions 10000 --simulation-instructions 200000 `
  .\champsim\results\windows-setup-20261120\trace-format-check\window.champsim
```

Review the reference algorithm first, then the adaptation table and validation
results. A next research step is a representative instruction-cache workload and
a matched measurement window before adding tunable issuance policies.
