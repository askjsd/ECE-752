# Entangling instruction prefetcher: integration and tuning plan

Status: proposed work, not implemented by this document. Repository inspected on
2026-11-06 at `7f630d979fb7df4ba5322a6b3ed9c96d8f0c3851`.

The goal is to integrate an Entangling instruction prefetcher, expose a small set
of settings, and test whether offline-selected starting settings help a runtime
tuner. Start by establishing that different settings actually change performance.
All ranges below are proposed experiments, not optimal ranges established by the
Entangling paper.

## 1. What the repository currently does

The existing [flagbench workflow](README.md) compiles combinations of GCC flags,
deduplicates binaries, runs gem5, and evaluates search algorithms by replaying
measured lookup tables (LUTs). This provides useful experiment infrastructure, but
does not yet implement a runtime hardware tuner or compiler hints for prefetching.

| Existing file | Current role | What the Entangling work needs |
|---|---|---|
| [flags.py](flags.py) | Binary GCC flag space, x86 build paths, active simulator configuration | A separate hardware-setting schema; do not turn prefetcher settings into GCC flags. |
| [sim_config.py](sim_config.py) | One x86 O3 core, classic L1I/L1D/L2 caches, SE workloads | Explicitly attach the instruction prefetcher and pass its initial settings. |
| [simulate.py](simulate.py) | Runs once per unique binary hash and extracts ROI cycles/instructions/IPC | Run once per binary **and hardware configuration**; collect prefetch outcomes too. |
| [build_lut.py](build_lut.py) | Joins compiler builds and simulations by binary hash | A hardware LUT keyed by the full experiment identity. |
| [tuners.py](tuners.py), [evaluate.py](evaluate.py) | Binary-flag search and whole-run LUT replay | An explicit adapter for categorical hardware settings; later, actual in-run configuration changes. |
| [prefetcher_knobs.md](prefetcher_knobs.md) | General prefetcher candidates from lectures | This document supplies the Entangling-specific plan. |

The intended target discussed for this project is a generic, single-core,
out-of-order RISC-V processor. The active flagbench path is currently **x86**:
`flags.py` selects `build/X86/gem5.opt`, x86 GCC flags, and `sim_config.py`, which
requires `ISA.X86`. [se_x86_ooo.py](se_x86_ooo.py) is a different configuration and
is not selected by that path. Confirm the experiment target before implementation;
this document does not migrate either configuration. A RISC-V run also needs
matching binaries, toolchain, m5ops, and workload setup. Native x86 checksum
reference runs do not directly execute RISC-V binaries.

The prefetcher policies below do not require the Entangling paper's ISA, pipeline
width, cache capacities, table sizes, or compressed destination encoding. Keep
those structural choices fixed and recorded for each experiment.

## 2. Structures and an example flow

Entangling adds metadata alongside instruction fetching. It does not replace the
core's rename, issue, execution, or retirement machinery.

| Structure | Information or work |
|---|---|
| L1 instruction cache (L1I) | Holds instruction cache lines used by fetching. |
| Region tracking registers | Track the first line and length of the current consecutive instruction-line sequence. |
| History buffer | Records earlier region heads and their access times. |
| Entangled table | Maps a source line to destination region heads and per-association confidence; also retains region lengths. A destination's length can be obtained through a separate lookup. |
| Prefetch queue (PQ) | Holds generated requests before issue. |
| MSHRs / outstanding-fill tracking | Track cache fills in flight. Additional metadata associates outcomes with the prediction that caused them. |

Illustrative learning flow: access source line A at cycle 20; demand access B at
cycle 100 misses; B fills at cycle 160. Its observed miss latency is 60 cycles.
Search history for a source accessed by cycle 40; A qualifies. Learn A -> B.
When later accesses traverse B, B+1, B+2, B+3 before leaving the sequence, record
a four-line region beginning at B. Here +1 means one **cache line**, not one
instruction. These are dynamic instruction-line regions, not compiler basic blocks.

Suppose other observations also establish A -> C:

| Source | Destination head | Destination region length, including its head | Confidence |
|---|---|---:|---:|
| A | B | 4 | 3 |
| A | C | 2 | 2 |

On a later access to A, look up the associations, select destinations, expand their
regions into line candidates, and admit allowed requests to the PQ. Requests that
need a new cache fill use outstanding-fill tracking; returned lines enter L1I.
Without added limits, the destination candidates are B, B+1, B+2, B+3, C, C+1.
The examples assume these lines are not already cached, queued, or in flight.

Entangling also fetches the continuation of the current source region. For the
first implementation, T, K, and L below affect **entangled destinations only**;
preserve the original source-continuation behavior. Enable, queue admission, and
optional trigger-frequency controls apply to all new Entangling prefetch requests.

## 3. Settings to expose

These names are proposed interfaces, not existing command-line options or SimObject
parameters. Store initial values in configuration and maintain a mutable C++ policy
object for subsequent runtime updates. Changing Python initialization parameters
alone does not implement in-run tuning.

| Setting | Proposed starting values | Where it acts | Priority |
|---|---|---|---|
| `enabled` | true / false | Gate new prefetch generation and admission. | First |
| `confidence_threshold` (T) | 1, 2, 3 if retaining two-bit counters | Filter source-destination associations after lookup. | First |
| `max_destinations` (K) | 1, 2, all retained destinations | Select how many destination regions to follow. | First |
| `max_region_lines` (L) | 1, 2, 4, full learned length | Limit expansion of each selected destination. | Next |
| `queue_occupancy_limit_pct` (Q) | 50, 75, disabled | Admit fewer new requests when the chosen PQ is occupied. | Later |
| `trigger_interval` | 1, 2, 4 eligible triggers | Issue only on every nth eligible source trigger. | Optional |
| `latency_margin` (alpha) | 1.0 first; exploratory 0.75 and 1.25 | Change the lead-time requirement when learning associations. | Optional; changes training |

### 3.1 Confidence threshold T

Allow A -> d only when its confidence is at least T. In the example, T=3 admits
B and suppresses C; T=2 admits both. If retaining the paper's confidence scheme,
new associations start at 3, timely outcomes increment the counter, late or unused
outcomes decrement it, and zero invalidates an association. A stricter issuance
threshold is our proposed extension. It does not filter a new association's first
request because new counters start at their maximum.

Keep suppressed associations available for reconsideration. Add a documented,
bounded exploration rule, or another recovery mechanism, so that low-confidence
associations can receive fresh evidence. Use identical exploration rules and costs
across tuned comparisons. Do not decrement confidence merely because our policy
suppressed a request. This counter reflects timeliness as well as usefulness;
it is not simply a probability that a destination will execute.

### 3.2 Destination cap K

After confidence filtering, follow at most K destinations, ranked by decreasing
confidence with a stable tie-break. K=1 chooses B and produces four line candidates;
K=2 chooses B and C and produces six. This caps **regions selected**, not requests.
Keep all learned destinations in the table; selection does not delete them.

Use an explicit `all` representation, rather than assuming the paper's maximum
number of compressed destinations. The original/default baseline must preserve
its original selection order; rank-limited variants should have documented order.

### 3.3 Region-length cap L

For each selected destination, consider min(learned line count, L) lines, beginning
with its head. With both destinations selected and L=2, candidates are B, B+1, C,
C+1. With K=1 and L=2, they are only B and B+1.

Keep the full learned size in metadata. This limits issuance, not region learning
or the maximum representable region length. Convert carefully if an implementation
stores the number of lines *after* the head rather than the total line count.

### 3.4 Queue-occupancy threshold Q

Choose and name the exact queue being observed. For a PQ with n entries and fixed
capacity N, admit a request only while `100 * n < Q * N`, checking again for each
actual insertion. With an eight-entry PQ initially holding two requests and Q=50,
admit B and B+1, reaching four occupied entries, then suppress the remaining
candidates. This example assumes no queue drain during the admission sequence.

`disabled` bypasses our additional admission gate and preserves the queue's normal
full-buffer policy. Lowering Q does not cancel existing requests. Start with
drop-on-suppression, not deferred replay; report those drops. If address translation
uses a second queue, specify separate accounting or explicitly exclude it. MSHR
occupancy is a different signal and must not silently replace PQ occupancy.

This policy does not resize the queue. It is also different from gem5's existing
accuracy-based `throttle_control_percentage`.

### 3.5 Enable / disable

Disabled means no new requests from this prefetcher. Continue observing demand
accesses and learning region/association metadata. On disabling, remove unsent
Entangling requests from its owned queues, release their bookkeeping, and let
already-issued memory requests finish with normal feedback. Define this transition
before runtime experiments so old queued work does not keep issuing while disabled.

Keep two separate controls: (a) disabled from startup with no training, and (b)
issuance disabled while training continues. The first is the no-prefetch baseline;
the second is a runtime policy with metadata and training costs.

### 3.6 Optional trigger frequency

For interval n, allow issuance on every nth eligible source trigger; n=1 allows
every trigger. Define the counter phase and reset it on a region-policy reload.
Continue training on skipped events. This is request frequency, not clock frequency.
It can miss the only timely opportunity for a destination, so evaluate it after
the simpler caps.

### 3.7 Optional timing margin, not a fixed address offset

Entangling learns a source that precedes destination demand by enough cycles to
cover the observed fetch latency. An extension can require a gap of at least
`alpha * measured_latency`. Alpha=1 retains that rule; larger values seek earlier
sources, while smaller values permit shorter gaps and risk late prefetches.

Changing alpha affects new/retrained associations, not the issue time of an already
stored association. Specify whether existing associations remain valid and account
for adaptation delay. Missing suitable history is a measurable failure case.
Do not describe this as a best-offset knob: fixed address offsets belong to a
different prediction mechanism. A training change should be evaluated separately
from immediate issuance-policy changes.

## 4. gem5 integration work identified in this repository

Relevant existing code is in [Prefetcher.py](../src/mem/cache/prefetch/Prefetcher.py),
[base.hh](../src/mem/cache/prefetch/base.hh), [base.cc](../src/mem/cache/prefetch/base.cc),
[queued.hh](../src/mem/cache/prefetch/queued.hh), and
[queued.cc](../src/mem/cache/prefetch/queued.cc).
No Entangling implementation is listed in the current prefetch directory or
declared in the inspected `Prefetcher.py`; integrating it is prerequisite work.

1. **Attach the right prefetcher.** Add an Entangling SimObject and implementation,
   register it in [SConscript](../src/mem/cache/prefetch/SConscript), and attach it
   explicitly to L1I in the chosen cache hierarchy. The current stdlib
   [L1ICache](../src/python/gem5/components/cachehierarchies/classic/caches/l1icache.py)
   instantiates `StridePrefetcher` by default; that class has `on_inst=False`.
   A prefetcher object being present does not establish instruction prefetching.
2. **Verify the observed instruction stream.** Entangling needs source hits as
   well as misses. In this version, `Base::observeAccess` checks `prefetchOnPfHit`
   before `prefetchOnAccess` on a hit. An all-access prototype needs instruction
   notifications enabled, `on_miss=False`, `prefetch_on_access=True`, and
   `prefetch_on_pf_hit=False`; merely enabling `prefetch_on_access` is insufficient.
   Verify ordinary hits, prefetched hits, misses, and repeated accesses within a
   line with an event trace. Deduplicate within-line accesses for region tracking.
3. **Implement timing and outcome attribution.** Base provides access, fill, and
   eviction hooks, but Entangling still needs association IDs/generations,
   timestamps, first-demand-use tracking, and a way to detect demand arriving
   while its prefetch is queued or in flight. Do not confuse timestamping queue
   entry with actual issue; define and measure both where needed. Ignore stale
   feedback when a source table slot has been reused.
4. **Place the policy checks.** `Queued::notify` calls `calculatePrefetch`, then
   admits generated addresses through `insert`. T/K/L naturally act during
   Entangling candidate selection/expansion. Q needs an admission hook that sees
   actual queue occupancy. Keep shared queue changes narrowly scoped; do not alter
   other prefetchers' behavior by default. Existing queued parameters such as
   `queueSize` and `throttleControlPct` are C++ constants, not runtime controls.
5. **Audit cross-page handling.** `Queued::notify` currently allows a cross-page
   candidate only when an MMU is registered. Entangling associations are not
   intrinsically restricted to one page. Choose a consistent virtual/physical
   address model and implement appropriate translation/permission handling;
   otherwise useful destinations can be silently filtered. Do not assume adjacent
   virtual pages are physically consecutive. Add a cross-page example to checks.
6. **Fix speculative-execution semantics.** State whether training and triggers
   observe speculative fetches or retired instructions. Cache access hooks alone
   do not provide retirement confirmation. Retire-based triggering requires
   additional core integration and recalibrated timing; do not add a retirement
   delay while retaining timing learned for fetch-triggered operation.

A first milestone can be a clearly labeled simplified Entangling-style prototype.
Call it a reproduction of the paper only after its training, timing, region,
confidence, and event semantics have been checked against the paper.

## 5. Implementation order and deliverables

- [ ] Confirm active ISA/configuration and capture generated gem5 `config.json`.
- [ ] Integrate an instruction-only Entangling baseline and verify A -> B learning,
      ordinary-hit triggering, region length, timely/late/unused outcomes, and
      cross-page behavior with small deterministic traces.
- [ ] Add mutable `enabled`, T, and K, with an explicit baseline-equivalent policy.
- [ ] Enumerate T in {1,2,3} and K in {1,2,all}: nine enabled configurations, plus
      no-prefetch and original/default Entangling baselines. Keep L full and Q off.
- [ ] Add L and verify the candidate counts in Section 3 before broader runs.
- [ ] Add Q only after queue admission and occupancy counters are observable.
- [ ] Measure sensitivity on several workloads/regions before choosing tuner knobs.
- [ ] Connect offline region settings and actual in-run policy updates; compare
      defaults, offline-only, runtime-only, and offline-initialized runtime tuning.
- [ ] Consider frequency or timing-margin extensions only if results motivate them.

Implement counters for candidates, each suppression reason, admitted requests,
issued requests, timely first demand hits, demand-to-in-flight matches, and unused
evictions. Distinguish selected regions from generated lines, admitted requests,
and issued transactions. Suppression is not evidence that a prefetch was wrong.
An unused eviction alone does not prove cache pollution or distinguish wrong-path
prediction from excessively early arrival. Audit gem5's statistic definitions:
its existing `pfLate` counts prefetch requests finding cached/in-flight/write-buffer
entries and is not automatically the paper's demand-arrived-before-prefetch metric.

## 6. Fit with flagbench and the Warm Start evaluation

**First measure fixed hardware settings.** Compile a chosen binary once and run it
under each policy. Hold compiler flags and data-prefetcher settings fixed initially;
varying them simultaneously makes instruction-prefetcher effects harder to interpret.
Use a separate result namespace so existing compiler-flag results remain comparable.

Use a run key containing the binary hash, hardware policy, gem5 binary/source
identity, full simulator configuration, workload/input, ROI definition, initial
state, and seed where relevant. Existing binary-hash-only deduplication would
incorrectly merge different hardware experiments. Record actual parameters and
outcomes; do not populate new LUTs using old cycle counts.

The existing `LUT` and CARBS implementation assume independent binary flags and
2^N configurations. T and K are categorical settings, so define a valid encoding
or categorical adapter before reuse. Avoid invalid one-hot combinations and
overweighting equivalent disabled configurations. Exhaustive enumeration is cheap
enough for the initial nine-setting space and gives a useful fixed-policy reference;
it does not establish that CARBS is needed or superior.

**Then measure changes during execution.** A whole-run LUT is suitable for offline
search over fixed policies. It cannot demonstrate runtime adaptation: caches,
queues, and learned associations depend on earlier settings. Run sequential epochs
inside gem5 with mutable controls. Apply hints at defined region boundaries and
count exploration, switching, hint delivery, and learning costs. Offline settings
must be selected on training inputs/regions, with evaluation on held-out executions.

Hints initialize policy settings, not the learned association table. Distinguish
tuner convergence time from Entangling's own training time. Resetting gem5 statistics
at ROI entry does not reset cache or predictor state; use the same documented
warm-up/state policy across comparisons. Attribute delayed feedback to the issuing
association/policy and document how requests spanning epochs are counted.

Report cycles and IPC, L1I misses/MPKI, issued instruction-prefetch traffic, timely
and late usefulness, unused evictions, queue occupancy and suppression, and time
to a predefined tuning target. Runtime coverage is only an estimate; use matched
no-prefetch runs for offline miss reduction. A best fixed policy is not an ideal
dynamic oracle. Evaluate realistic hint size/storage/delivery overhead separately
from any idealized zero-cost boundary callback.

Finally, screen workloads for instruction-side pressure. The existing small
compiler-flag kernels can have very little L1I tuning headroom; `k_branchy` being
branch-heavy does not establish a large instruction working set. Include code
footprints and execution patterns that actually cause instruction misses, retain
low-miss controls, and report null results rather than selecting only wins.

## References

- Alberto Ros and Alexandra Jimborean, *A Cost-Effective Entangling Prefetcher for
  Instructions*, ISCA 2021, Sections II-III and Figure 4.
  [DOI: 10.1109/ISCA52012.2021.00017](https://doi.org/10.1109/ISCA52012.2021.00017).
- Repository links above describe the inspected implementation; proposed setting
  ranges, selection caps, admission policies, and evaluation steps are our design
  choices, not claims that the paper implemented them.
