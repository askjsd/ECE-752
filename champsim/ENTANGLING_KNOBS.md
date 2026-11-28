# Entangling in the team’s ChampSim

The combined configuration is `champsim_config_entangling.json`. It uses the
instruction version in L1I and the DPC-4 data version in L1D. Both are enabled by
default. The configuration uses one simulated core and 64-byte cache lines.

## Settings for the learner

| Prefetcher | Field | Values and default | Startup environment variable |
|---|---|---|---|
| Instruction | `enabled` | 0 or 1, default 1 | `EIP_ENABLED` |
| Instruction | `confidence_threshold` | 1–3, default 1 | `EIP_CONFIDENCE_THRESHOLD` |
| Instruction | `destination_cap` | 1–6, default 6 (all eligible destinations) | `EIP_DESTINATION_CAP` |
| Data | `enabled` | 0 or 1, default 1 | `EDP_ENABLED` |
| Data | `max_requests_per_target` | 1–16, default 16 | `EDP_MAX_REQUESTS_PER_TARGET` |
| Data | `l1_fill_threshold_percent` | 0–100, default 50 | `EDP_L1_FILL_THRESHOLD_PERCENT` |

The instruction cap counts learned destinations. Each can request multiple lines.
Under a restricted cap, higher-confidence destinations come first. Prefetches
for the current run of consecutive code lines are controlled by `enabled` and
are outside the confidence and destination limits.

The data cap counts requests generated per target instruction. The fill cutoff
uses the percentage of occupied slots for outstanding misses. An eligible
request can fill L1D below the cutoff. A cutoff of 0 sends all requests to L2.
Requests selected for L2 by the original data policy still go to L2. EDP's
built-in confidence rules still decide which predictions qualify for L1 or
L2. The exposed fill cutoff controls how cache load affects that decision.
A new cap applies when generating requests. A new fill cutoff applies when
sending them.
Turning EDP off also clears requests it has queued internally. Already-sent
requests can still finish. Both prefetchers keep learning while disabled.

From C++ in the simulator, include `entangling.h` and `edp.h` from their
module directories. After cache initialization, find the relevant module by
cache name, read the knobs, then update them on the simulation thread.

```cpp
if (auto* instruction = entangling::find("cpu0_L1I")) {
  auto settings = instruction->get_knobs();
  settings.confidence_threshold = 3;
  instruction->set_knobs(settings);
}
if (auto* data = edp::find("cpu0_L1D")) {
  auto settings = data->get_knobs();
  settings.max_requests_per_target = 1;
  data->set_knobs(settings);
}
```

The setters clamp numeric values to the ranges in the table. Lookup returns
`nullptr` if the cache has a different module or has yet to initialize.

## Build and run

With the team's Linux ChampSim dependencies installed, run from the repository
root:

```bash
KNOBBED_CONFIG="$PWD/champsim/champsim_config_entangling.json" \
  bash champsim/build_knobbed.sh linux
EDP_ENABLED=1 EIP_ENABLED=0 EDP_MAX_REQUESTS_PER_TARGET=1 \
  ~/champsim-knobbed-linux/bin/champsim_entangling \
  --warmup-instructions 100000 --simulation-instructions 500000 trace.champsimtrace.xz
```

To check the controls on a completed Linux build, run this from the repository
root. The test generates its own instruction and data accesses.

```bash
bash champsim/tests/run_entangling_tests.sh ~/champsim-knobbed-linux
```

The test reads the defaults, switches each prefetcher off and back on during
one simulation, checks how many requests each one sends, and verifies that
the L2-only setting sends no data prefetch fills into L1. It also checks the
numeric limits and restores the original settings. It uses the same
functions a learner can call on the simulation thread.

Use the same executable and trace when comparing settings. The one-core runs
used a synthetic instruction trace, a saved stencil window, a synthetic data
trace, and a short window of the public DPC-4 `607.cactuBSSN_s-4248B` trace.
The [settings results](validation/entangling/comparisons.csv) and the
[comparison with Celeste's prefetcher](validation/entangling/instruction-comparison.csv)
are checked in. These are short integration checks.

- The defaults matched the original instruction port on both instruction tests.
  On the synthetic instruction trace, Entangling took 302,230 cycles and
  Celeste's default next-N-line took 389,352. On the short stencil trace,
  Entangling took 99,610 cycles and Celeste's took 97,840.
- On the 500,000-instruction `cactuBSSN` window, the data version cut cycles
  from 361,169 with prefetching off to 317,064. The one-request cap issued 450
  requests versus 460 with defaults. Sending all requests to L2 produced zero
  useful L1D prefetch fills and took 316,755 cycles in that same window.
- During a running simulation, the setters successfully read back clamped
  values. Turning each prefetcher off halfway through stopped new requests.
  All five focused instruction tests passed on the team simulator build.
  Native Windows spot checks matched the Linux cycle and request counts for
  the synthetic instruction trace and the SPEC data trace.

## Source and port scope

- Instruction baseline: [Ros and Jimborean’s ISCA 2021 source](https://github.com/alberto-ros/EntanglingInstructionPrefetcher/tree/75c77536ac4b1fa1a801587df444ee1501f1b3aa/ISCA-21).
  The [original Kewei snapshot](https://github.com/askjsd/ECE-752/tree/3fbf1698d2cfd73b165750d62c81ba70a770dc31)
  preserves the earlier Windows-only setup and reference files.
- Data baseline: [Navarro-Torres, Singh, Panda, and Ros’s DPC-4 submission](https://github.com/CMU-SAFARI/DPC4/tree/051684493c8439312b9c4c3af38fa25100834afa/submissions/EDP).
  The port uses the authors’ L1D module and the team’s existing cache interface.
  It keeps the original request-generation logic and uses the team’s L2 and
  last-level cache settings. The DPC-4 bandwidth signal and last-level-cache
  throttle messages are absent from this configuration. The user-controlled cap
  and the local queue and cache-fill checks govern requests here.
