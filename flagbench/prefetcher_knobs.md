# Warm Start — Tunable Prefetcher Knobs

Candidate settings that the static (CARBS-style) tuner can encode as region hints and the runtime learner can adjust. Grounded in ECE/CS 752 *Cache and Memory Architecture* Parts 4–7 (cited as `P<part>:<slide>`). Anything not from the slides is marked **[beyond slides]**.

Each knob is tagged:

- **Runtime-changeable?** Can it flip mid-execution without rebuilding structures? (Only these belong in the hint control word.)
- **Hint value:** How much a compiler can know about the right setting statically.
- **Feedback signal:** What the runtime learner watches to adjust it.

---

## 0. Telemetry the runtime learner needs (P5:39–43)

All knob adjustments below key off these three counters, so they should exist per region / per epoch:

| Metric | Formula | Needs |
|---|---|---|
| Accuracy | useful prefetches / total prefetches | a "prefetched, not yet used" bit per block; count on first demand hit and on eviction |
| Coverage | useful prefetches / misses *without* prefetching | baseline miss count — not directly observable at runtime; approximate with (misses + useful prefetches) |
| Timeliness | timely prefetches / useful prefetches | detect demand hits on in-flight prefetches (MSHR hit on a prefetch) as "late" |

Distance outcomes split into three regimes (P6:98), which map directly to learner actions:

| Regime | Symptom | Action |
|---|---|---|
| Timely | demand hits on prefetched blocks | hold |
| Late but useful | demand hits MSHR entries allocated by prefetches | ↑ distance |
| Early and hurtful | prefetched block evicted unused, then demand-missed | ↓ distance (or ↓ degree) |

---

## 1. *When* knobs — trigger and aggressiveness

### 1.1 Enable / disable
- **Values:** on / off (per prefetcher, per cache level)
- **Runtime-changeable:** yes
- **Hint value:** high. Compiler can turn off a stride prefetcher in pointer-chasing regions or turn off content-directed prefetching in dense-array regions.
- **Feedback:** accuracy below a floor → off (P7:10, "if unsurer → don't prefetch")

### 1.2 Trigger event (P6:8–13, P6:87–88)
- **Values:**
  - on miss only
  - on miss **+ first use of a tagged prefetched block** (fixes the 50% coverage problem of plain next-line; P6:9–13 shows coverage going 50% → 100%)
  - on every access (earlier, but more traffic/overhead)
  - on dead-block prediction (prefetch the block that replaced the dead one last time; P5:26–27, P6:88)
- **Runtime-changeable:** yes, if the tag bit already exists
- **Hint value:** medium. Streaming loops benefit from tagged triggering; irregular code may not.
- **Feedback:** coverage and traffic

### 1.3 Prefetch distance (P6:89–102)
- **Values:** blocks (or iterations) ahead of the trigger; slide example: 1–5 late, 6–13 timely, 14+ hurtful for 300-cycle memory, 50-cycle loop body, 8-block cache
- **Runtime-changeable:** yes
- **Hint value:** medium. Compiler knows loop-body length and stride, but **not** memory latency or contention. Slide P6:102: prefetches add their own queueing, so a distance tuned in one environment goes late under load. This is the strongest argument for "hint as starting point, runtime adjusts."
- **Feedback:** late fraction ↑ → distance ↑; unused-evicted fraction ↑ → distance ↓
- **Note:** distance "buys time" (P6:101)

### 1.4 Prefetch degree (P6:100–101)
- **Values:** blocks issued per trigger (1, 2, 4, …); also the Markov "up to N successors" fan-out (P6:22)
- **Runtime-changeable:** yes
- **Hint value:** medium. Large regular sweeps → higher degree.
- **Feedback:** coverage ↑ wanted and accuracy still high → degree ↑; accuracy ↓ → degree ↓
- **Note:** degree "buys coverage" (P6:101)

### 1.5 Learned-distance override (Best-Offset; P6:103–108)
- **Values:** candidate offset list (slide example: {1, 2, 4, 6, 8}); whether the hint's distance seeds or overrides BO's learned offset
- **Runtime-changeable:** yes
- **Hint value:** high for *seeding*. BO is already a runtime distance learner, so this is a natural place for Warm Start: the hint picks BO's initial best offset (and possibly a narrowed candidate list) so it skips its learning rounds on region entry.
- **Feedback:** BO's own scoring (would P[i−X] have prefetched P[i] in time?)

### 1.6 Throttling **[partly beyond slides]**
- P7:11 introduces feedback-directed prefetching (Srinath et al., HPCA 2007) as "prefetcher → insertion policy." The original paper also throttles distance/degree from accuracy, lateness, and pollution; the slides only cover the insertion side.
- **Knob:** aggressiveness level (a bundled distance + degree setting), plus the accuracy/lateness thresholds that move between levels
- **Runtime-changeable:** yes; this is essentially what the runtime learner *is*

---

## 2. *Where* knobs — fill level and insertion

### 2.1 Fill level (P7:5–10)
- **Values:** L1 / L2 / LLC / drop
- **Rule from slides:** fill by confidence *C*, comparing `C × savings − (1 − C) × cost` per level (P7:9–10). Example: C = 0.9 → L1; C = 0.5 → L2; C = 0.2 → LLC.
- **Exposed knobs:** the two confidence thresholds separating L1/L2 and L2/LLC (and LLC/drop)
- **Runtime-changeable:** yes
- **Hint value:** medium. Compiler can say "this region's accesses are highly regular" (raise confidence) but cannot know the cost of polluting a given cache under the current working set.
- **Feedback:** per-level accuracy and pollution (prefetch-caused evictions of live blocks)

### 2.2 Insertion position of prefetched blocks (P7:12, P4:59–94)
- **Values:** MRU / middle / LRU position, or RRIP state 00–11, chosen by confidence (sure → MRU, unsure → middle, unsurer → LRU)
- **Runtime-changeable:** yes
- **Hint value:** medium-high for streaming regions: insert prefetched streaming data at LRU / "may evict" so it does not push out high-reuse blocks (P4:92–94, P7:44)
- **Feedback:** hits on prefetched blocks vs. evictions they cause

### 2.3 Prefetch-aware replacement (P7:13–22)
- **Idea:** Demand-MIN evicts blocks whose next access is a prefetch, since they will come back anyway
- **Knob:** on/off for "deprioritize blocks the prefetcher is confident it will re-fetch"
- **Runtime-changeable:** yes as a mode bit; the full Demand-MIN oracle is not realizable, so in practice this is an approximation **[beyond slides: approximation details]**
- **Hint value:** low–medium

### 2.4 Attach point / training stream (P7:7–8, P7:41–43) — **static, not a hint**
- Closer to core sees PCs, virtual addresses, every access; farther away has room for bigger tables.
- L1 filtering mostly does **not** hurt L2 prefetching (misses and their spatial pattern survive), unlike L2 replacement.
- This is a design-time choice for the composite prefetcher, not a per-region knob.

---

## 3. *What* knobs — per prefetcher family

Structural parameters (table sizes) are **not** runtime-changeable; list them so the group can fix them per experiment, but keep them out of the hint word.

### 3.1 Stride / next-line (P5:46–48, P6:6–13)
| Knob | Values | Runtime? | Notes |
|---|---|---|---|
| Stride source | fixed BLOCK_SIZE (next-line) vs. learned per PC | yes | next-line is the cheap fallback |
| Table entries / hash | e.g., 64–256 PC entries | no | structural |
| Stride confirmation count **[beyond slides]** | 1–3 matching strides before issuing | yes | trades warm-up for accuracy; a hint could set this to 1 in known-strided loops |

### 3.2 Pair-wise / Markov (P6:14–37)
| Knob | Values | Runtime? | Notes |
|---|---|---|---|
| Successors per entry (N) | 1 (pair-wise), 2–4 | no | structural; slide P6:36 notes low utilization |
| Successors issued per miss | ≤ N | yes | this is the degree knob for Markov |
| Successor confidence threshold | counter value to issue | yes | slide example reaches 75% accuracy with 2 successors |

### 3.3 Global History Buffer (P6:38–47)
| Knob | Values | Runtime? | Notes |
|---|---|---|---|
| GHB length | ≥ pattern period (6 for the slide's ABCABD) | no | structural |
| History depth (pattern length k) | 1–3+ addresses | yes (if table is rebuilt lazily) | slide uses k = 3; longer k disambiguates but needs more history |
| Index scheme | hash into history table vs. pointer-chain traversal | no | trades storage for computation (P6:46) |
| Traversal depth | how many links to follow | yes | more prefetches vs. more latency per trigger |

### 3.4 Spatial Memory Streaming (P6:48–58)
| Knob | Values | Runtime? | Notes |
|---|---|---|---|
| Region size | e.g., 2 KB | no | structural |
| Index | hash(PC, offset) | no | structural |
| Min fingerprint bits to stream **[beyond slides]** | e.g., ≥ 2 set bits | yes | suppress sparse patterns |
| Enable | on/off | yes | **high hint value**: compiler sees struct layout (e.g., the 32 B `purchase` struct → fingerprint `01100110`) and can even supply the fingerprint directly. Slide P6:58: struggles on unstructured data |

### 3.5 Content-directed (P6:59–62)
| Knob | Values | Runtime? | Notes |
|---|---|---|---|
| Compare bits | # upper address bits that must match the block address | yes | fewer bits → more candidates, lower accuracy |
| Recursion depth | how many levels of "prefetch, scan, prefetch" | yes | depth ~ distance for linked structures |
| Enable | on/off | yes | **high hint value**: compiler knows when a region is pointer-chasing (`p = p->next`), which software prefetching cannot handle (P5:45) |

### 3.6 Indirect Memory Prefetcher (P6:63–76)
| Knob | Values | Runtime? | Notes |
|---|---|---|---|
| Shift candidates | {2 (int), 3 (double), …} | yes | |
| Base-address confirmation count | matches before trusting `&A[0]` | yes | |
| Seeded base + shift | (base, shift) pair | yes | **highest hint value in this list**: for `A[B[i]]` the compiler knows the base of `A` and `sizeof(A[0])` exactly, so the hint can skip learning entirely |

### 3.7 Execution-based (P6:77–85) — likely out of scope
- Runahead enable, decoupled access/execute queue depth (how far the access stream may slip ahead), helper-thread slices. These are core-pipeline mechanisms rather than prefetcher settings; listed for completeness. Slide P6:83: decoupling fails when address computation depends on loaded values.

---

## 4. Suggested hint control word (first cut)

Only runtime-changeable, workload-dependent knobs. Bit widths are placeholders for the group to size **[beyond slides]**.

| Field | Bits | Source section |
|---|---|---|
| Per-prefetcher enable mask | 4–6 | 1.1 |
| Trigger mode | 2 | 1.2 |
| Distance (or BO seed offset) | 4–5 | 1.3, 1.5 |
| Degree | 2–3 | 1.4 |
| Fill-level thresholds | 2 × 3 | 2.1 |
| Prefetch insertion position | 2 | 2.2 |
| Family-specific payload (e.g., IMP shift, CDP depth, SMS fingerprint) | variable / sideband | 3.x |

Everything structural (table sizes, region size, GHB length, attach point) is fixed per experiment and swept offline, not encoded in hints.

---

## 5. Knobs where hints help most vs. least

**Hint is close to sufficient** (compiler knows the answer): IMP base/shift, SMS fingerprint/enable, CDP enable for pointer-chasing regions, prefetcher enable/disable by access pattern.

**Hint is only a starting point** (depends on latency, contention, cache state, which compilers can't see; P5:45, P6:102): distance, degree, fill-level thresholds, insertion position.

**Not suitable for hints:** structural sizes and attach point.

---

## Open items
- Entangling (the instruction prefetcher) is not covered in these lectures; its knobs need to come from the paper.
- Decide whether coverage is approximated at runtime or only measured offline for evaluation.
- Check which of the above the ChampSim prefetcher interface already exposes (e.g., per-prefetch fill-level choice) vs. what needs simulator changes.
