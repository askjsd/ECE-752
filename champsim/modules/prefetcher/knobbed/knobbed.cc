// Implementation of the knobbed prefetcher. See knobbed.h for the big picture
// and README.md for how to configure it.
//
// Flow of one demand access through prefetcher_cache_operate():
//   1. find the region (hint) the address belongs to
//   2. update telemetry (was it a timely / late / lower-level / hurtful event?)
//   3. train the per-PC stride table
//   4. if the region's trigger mode fires, queue prefetch candidates
//   5. if the region's epoch is over, run the throttling controller
// prefetcher_cycle_operate() then drains the candidate queue, one per cycle.
#include "knobbed.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "cache.h"

namespace knobbed_pf
{
namespace
{
// Remove leading and trailing whitespace.
std::string trim(std::string s)
{
  const auto b = s.find_first_not_of(" \t\r\n");
  const auto e = s.find_last_not_of(" \t\r\n");
  return b == std::string::npos ? std::string{} : s.substr(b, e - b + 1);
}

bool is_on(const std::string& v) { return v == "1" || v == "on" || v == "true"; }

// Aggressiveness ladders, gentlest first (distance, degree). Indexed by level.
constexpr rung STRIDE_LADDER[LADDER_LEVELS] = {{1, 1}, {1, 2}, {1, 4}, {1, 8}, {2, 4}, {4, 4}, {4, 8}, {8, 8}};
constexpr rung MARKOV_LADDER[LADDER_LEVELS] = {{1, 1}, {1, 2}, {1, 4}, {1, 8}, {2, 2}, {2, 4}, {2, 8}, {4, 4}};
} // namespace

const rung& ladder_rung(stride_source source, int level)
{
  const auto i = static_cast<std::size_t>(std::clamp(level, 0, LADDER_LEVELS - 1));
  return source == stride_source::MARKOV ? MARKOV_LADDER[i] : STRIDE_LADDER[i];
}

// Pack the hint into the 32-bit control word (layout documented in knobbed.h).
uint32_t hint::to_word() const
{
  uint32_t w = 0;
  w |= (static_cast<uint32_t>(source) & 0x3u);
  w |= (static_cast<uint32_t>(trigger) & 0x3u) << 2;
  w |= ((std::clamp<uint32_t>(confirm, 1, 2) - 1) & 0x1u) << 4; // stored as confirm-1
  w |= (std::min<uint32_t>(level, LADDER_LEVELS - 1) & 0x7u) << 5;
  w |= (static_cast<uint32_t>(fill) & 0x3u) << 8;
  w |= (throttle ? 1u : 0u) << 10;
  w |= (fallback ? 1u : 0u) << 11;
  return w;
}

// A word is valid if every field holds a defined value. Source, fill, level and
// confirm use all of their codes; only trigger has an unused code (3). Accepting
// it would make it an alias of another trigger and skew a search over raw bits.
bool hint::word_valid(uint32_t w) { return ((w >> 2) & 0x3u) <= static_cast<uint32_t>(trigger_mode::ACCESS); }

// Inverse of to_word().
hint hint::from_word(uint32_t w)
{
  hint h;
  h.source = static_cast<stride_source>(w & 0x3u);
  h.trigger = static_cast<trigger_mode>((w >> 2) & 0x3u);
  h.confirm = static_cast<uint8_t>(((w >> 4) & 0x1u) + 1);
  h.level = static_cast<uint8_t>((w >> 5) & 0x7u);
  h.fill = static_cast<fill_policy>((w >> 8) & 0x3u);
  h.throttle = (w >> 10) & 1u;
  h.fallback = (w >> 11) & 1u;
  return h;
}

// Parse "key=value,key=value" and apply each setting on top of the current
// values, so a region spec only needs to list what it changes. A bad key or
// value leaves a message in *error and returns false.
bool hint::apply(const std::string& spec, std::string* error)
{
  auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  std::stringstream ss(spec);
  std::string item;
  while (std::getline(ss, item, ',')) {
    item = trim(item);
    if (item.empty())
      continue;
    const auto eq = item.find('=');
    if (eq == std::string::npos)
      return fail("expected key=value: " + item);
    const auto k = trim(item.substr(0, eq));
    const auto v = trim(item.substr(eq + 1));
    // std::stoi throws on malformed numbers; that is caught below.
    try {
      if (k == "source") {
        if (v == "off")
          source = stride_source::OFF;
        else if (v == "nextline")
          source = stride_source::NEXT_LINE;
        else if (v == "stride")
          source = stride_source::PER_PC;
        else if (v == "markov")
          source = stride_source::MARKOV;
        else
          return fail("source must be off|nextline|stride|markov");
      } else if (k == "trigger") {
        if (v == "miss")
          trigger = trigger_mode::MISS;
        else if (v == "tagged")
          trigger = trigger_mode::MISS_TAGGED;
        else if (v == "access")
          trigger = trigger_mode::ACCESS;
        else
          return fail("trigger must be miss|tagged|access");
      } else if (k == "confirm") {
        const int n = std::stoi(v);
        if (n < 1 || n > 2)
          return fail("confirm must be 1 or 2");
        confirm = static_cast<uint8_t>(n);
      } else if (k == "level") {
        const int n = std::stoi(v);
        if (n < 0 || n >= LADDER_LEVELS)
          return fail("level must be 0-7");
        level = static_cast<uint8_t>(n);
      } else if (k == "fill") {
        if (v == "l1d")
          fill = fill_policy::ALWAYS_L1D;
        else if (v == "low")
          fill = fill_policy::GATED_LOW;
        else if (v == "high")
          fill = fill_policy::GATED_HIGH;
        else if (v == "l2")
          fill = fill_policy::ALWAYS_L2;
        else
          return fail("fill must be l1d|low|high|l2");
      } else if (k == "throttle")
        throttle = is_on(v);
      else if (k == "fallback")
        fallback = is_on(v);
      else if (k == "word") { // a whole packed control word, e.g. word=0x4a5
        const auto w = static_cast<uint32_t>(std::stoul(v, nullptr, 0));
        if (!word_valid(w))
          return fail("word has an undefined field (trigger 3): " + v);
        *this = from_word(w);
      } else
        return fail("unknown knob: " + k);
    } catch (const std::exception&) {
      return fail("bad value for " + k + ": " + v);
    }
  }
  return true;
}
} // namespace knobbed_pf

using knobbed_pf::fill_policy;
using knobbed_pf::hint;
using knobbed_pf::region;
using knobbed_pf::stride_source;
using knobbed_pf::trigger_mode;

// ---------------------------------------------------------------------------
// Configuration
//
// ChampSim's module interface has no place for per-run parameters, so settings
// come from the environment:
//   KNOBBED_HINT="source=stride,trigger=tagged,level=3,fill=low,throttle=0"
//       the default hint, used wherever no region matches
//   KNOBBED_REGIONS=<file>
//       one region per line:  <start> <end> <key=value,...>
//       (byte addresses, '#' starts a comment). A region starts from the
//       default hint and overrides only the keys it lists.
// A malformed setting is a fatal error so a typo never silently runs the
// wrong experiment.
// ---------------------------------------------------------------------------
void knobbed::load_environment()
{
  std::string err;
  if (const char* spec = std::getenv("KNOBBED_HINT")) {
    if (!regions[0].configured.apply(spec, &err)) {
      std::fprintf(stderr, "[knobbed] KNOBBED_HINT: %s\n", err.c_str());
      std::exit(1);
    }
    regions[0].active = regions[0].configured;
  }
  if (const char* path = std::getenv("KNOBBED_REGIONS")) {
    std::ifstream in(path);
    if (!in) {
      std::fprintf(stderr, "[knobbed] cannot open KNOBBED_REGIONS file %s\n", path);
      std::exit(1);
    }
    std::string line;
    while (std::getline(in, line)) {
      line = line.substr(0, line.find('#')); // strip comments
      std::stringstream ss(line);
      std::string s, e, rest;
      if (!(ss >> s >> e))
        continue; // blank line
      std::getline(ss, rest);
      hint h = regions[0].configured; // inherit the default, then override
      if (!h.apply(rest, &err)) {
        std::fprintf(stderr, "[knobbed] %s: %s\n", path, err.c_str());
        std::exit(1);
      }
      add_region(std::stoull(s, nullptr, 0), std::stoull(e, nullptr, 0), h);
    }
  }
}

// Called once by ChampSim before simulation. Region 0 is the catch-all default;
// regions from KNOBBED_REGIONS are appended after it.
void knobbed::prefetcher_initialize()
{
  regions.clear();
  regions.emplace_back(); // default region covers every address
  load_environment();
  const auto& h = regions[0].configured;
  std::printf("[knobbed %s] default hint: word=%#x source=%d trigger=%d confirm=%d level=%d (distance=%d degree=%d) fill=%d throttle=%d fallback=%d regions=%zu\n",
              intern_->NAME.c_str(), h.to_word(), static_cast<int>(h.source), static_cast<int>(h.trigger), h.confirm, h.level, h.distance(), h.degree(),
              static_cast<int>(h.fill), h.throttle, h.fallback, regions.size() - 1);
  // Fixed (never tuned) constants, printed so every result can be tied to them.
  std::printf("[knobbed %s] fixed constants: epoch=%llu min_issued=%llu acc_floor=%.2f acc_low=%.2f acc_high=%.2f early_high=%.2f suspend_epochs=%u "
              "acc_ewma_start=1.0 gate_low=%.2f gate_high=%.2f table_sets=%zu table_ways=%zu pending_max=%zu track_max=%zu markov_max=%zu inflight_expire=%llu lower_expire=%llu\n",
              intern_->NAME.c_str(), static_cast<unsigned long long>(tp.epoch_accesses), static_cast<unsigned long long>(tp.min_issued), tp.acc_floor, tp.acc_low,
              tp.acc_high, tp.early_high, tp.suspend_epochs, knobbed_pf::GATED_LOW_THRESHOLD, knobbed_pf::GATED_HIGH_THRESHOLD, TRACKER_SETS, TRACKER_WAYS,
              PENDING_MAX, TRACK_MAX, MARKOV_MAX, static_cast<unsigned long long>(INFLIGHT_EXPIRE), static_cast<unsigned long long>(LOWER_EXPIRE));
}

// Add a region covering byte addresses [start, end) and return its index.
std::size_t knobbed::add_region(uint64_t start, uint64_t end, const hint& h)
{
  region r;
  r.start = start;
  r.end = end;
  r.configured = h;
  r.active = h;
  regions.push_back(r);
  return regions.size() - 1;
}

// Replace a region's hint at run time (for a driver that decodes hint words).
// The throttle's accumulated changes are discarded and any suspension cleared.
void knobbed::set_region_hint(std::size_t idx, const hint& h)
{
  if (idx < regions.size()) {
    regions[idx].configured = h;
    regions[idx].active = h;
    regions[idx].suspended_epochs = 0;
  }
}

// Later regions win over earlier ones; index 0 is the catch-all default.
std::size_t knobbed::find_region(uint64_t a) const
{
  for (std::size_t i = regions.size(); i-- > 1;)
    if (a >= regions[i].start && a < regions[i].end)
      return i;
  return 0;
}

// Confidence C in [0,1]: how sure we are that a prefetch will be used. The
// lecture slides choose the fill level from C (P7:9-10). Here:
//   C = (how well-confirmed the stride is) x (the region's recent accuracy)
// The stride term runs from 0.25 (just seen) to 1.0 (confirmed three times);
// next-line and markov have no stride to confirm, so that term is 1.
double knobbed::confidence(const region& r, int stride_conf) const
{
  const double stride_c = r.active.source != stride_source::PER_PC ? 1.0 : std::min(1.0, static_cast<double>(stride_conf + 1) / 4.0);
  return stride_c * r.acc_ewma;
}

// Remember that block `blk` was evicted before it was ever used. If a demand
// miss for it follows soon, that is the "early and hurtful" case. The set is
// small and FIFO so old evictions age out.
void knobbed::note_evicted_unused(uint64_t blk, std::size_t idx)
{
  if (evicted_unused.emplace(blk, tracked_block{idx}).second)
    evicted_order.push_back(blk);
  while (evicted_order.size() > EVICTED_MAX) {
    evicted_unused.erase(evicted_order.front());
    evicted_order.pop_front();
  }
}

// Called by the cache for every access it handles (before hit/miss is applied).
uint32_t knobbed::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                           uint32_t metadata_in)
{
  // Train and measure on demand traffic only (not other prefetchers' requests,
  // writebacks or page-table walks).
  if (type != access_type::LOAD && type != access_type::RFO)
    return metadata_in;

  const champsim::block_number cl_addr{addr};
  const uint64_t blk = cl_addr.to<uint64_t>();
  const auto ridx = find_region(addr.to<uint64_t>());
  auto& reg = regions[ridx];
  ++reg.epoch.demand_accesses;
  ++reg.total.demand_accesses;

  // ---- 2. telemetry (section 0 of the knob doc) ---------------------------
  // Each prefetched block ends up in exactly one of these buckets. Credit goes
  // to the region that *issued* the prefetch, which may differ from the region
  // of the demand address that found it.
  if (useful_prefetch) {
    // Demand hit a block a prefetch brought in: a timely prefetch.
    if (const auto* t = resident.find(blk)) {
      auto& owner = regions[t->region_idx];
      ++owner.epoch.useful_timely;
      ++owner.total.useful_timely;
      resident.erase(blk); // counted once: the prefetch bit is cleared on first use
    }
  }
  if (!cache_hit) {
    ++reg.epoch.demand_misses;
    ++reg.total.demand_misses;
    if (const auto* lo = lower_tracked.find(blk)) {
      // We prefetched this into a lower level only, and the core did want it.
      // It will hit in the lower level instead of going to memory.
      auto& owner = regions[lo->region_idx];
      ++owner.epoch.useful_lower;
      ++owner.total.useful_lower;
      lower_tracked.erase(blk);
    } else if (const auto* it = inflight.find(blk)) {
      // Demand missed while the prefetch for it was still in flight: the
      // prefetch was useful but late (an MSHR hit on a prefetch).
      auto& owner = regions[it->region_idx];
      ++owner.epoch.useful_late;
      ++owner.total.useful_late;
      inflight.erase(blk);
    } else if (auto ev = evicted_unused.find(blk); ev != evicted_unused.end()) {
      // We evicted this block unused and now it is needed again: the prefetch
      // was early and hurtful (it pushed out something and then was itself lost).
      auto& owner = regions[ev->second.region_idx];
      ++owner.epoch.early_hurtful;
      ++owner.total.early_hurtful;
      evicted_unused.erase(ev);
    }
  }

  // ---- 3a. miss-correlation learning (for source=markov) -------------------
  // Learn "after a miss on A, the next miss was B". Trained on misses and on
  // first uses of prefetched blocks, so the chain stays complete once prefetching
  // has turned the misses into hits. Always trained, like the stride table.
  if (!cache_hit || useful_prefetch) {
    if (have_last_miss && last_miss != blk) {
      // put() makes the link the newest, so the table is LRU by training use and
      // the oldest link is the one dropped when it is full.
      if (const auto* e = markov.find(last_miss); e && e->next == blk)
        markov.put(last_miss, markov_entry{blk, std::min(e->conf + 1, 3)});
      else
        markov.put(last_miss, markov_entry{blk, 0});
    }
    last_miss = blk;
    have_last_miss = true;
  }

  // ---- 3. stride learning --------------------------------------------------
  // Always trained, even when the region's prefetcher is off or uses next-line,
  // so a hint change takes effect immediately with a warm table.
  //
  // Strides are in cache lines. A repeat access to the same line (delta 0)
  // leaves the learned stride and its confidence untouched. A table that stores
  // the zero instead (as ChampSim's ip_stride does) never locks on to loops
  // that touch several elements per line.
  champsim::block_number::difference_type stride = 0;
  int conf = 0; // number of consecutive times `stride` has repeated, capped at 3
  auto found = table.check_hit({ip, cl_addr, 0, 0});
  if (found.has_value()) {
    const auto delta = champsim::offset(found->last_cl_addr, cl_addr);
    if (delta == 0) {
      stride = found->last_stride;
      conf = found->conf;
    } else {
      stride = delta;
      conf = (delta == found->last_stride) ? std::min(found->conf + 1, 3) : 0;
    }
  }
  table.fill({ip, cl_addr, stride, conf});

  // ---- 4. trigger -----------------------------------------------------------
  const auto& h = reg.active;
  if (reg.suspended_epochs == 0 && h.source != stride_source::OFF) {
    bool fire = false;
    switch (h.trigger) {
    case trigger_mode::MISS:
      fire = !cache_hit;
      break;
    case trigger_mode::MISS_TAGGED:
      // A miss, or the first demand use of a prefetched block. The second case
      // keeps a stream going once prefetching has made the misses disappear
      // (plain next-line otherwise stalls at 50% coverage; P6:9-13).
      fire = !cache_hit || useful_prefetch;
      break;
    case trigger_mode::ACCESS:
      fire = true;
      break;
    }

    // Which stride to use? Next-line always steps one line. Per-PC uses the
    // learned stride, but only once it has repeated `confirm` times (confirm=1
    // trusts the first observed stride; larger values wait longer but are
    // more accurate).
    champsim::block_number::difference_type use_stride = 1;
    bool trained = true;
    if (h.source == stride_source::PER_PC) {
      use_stride = stride;
      trained = (stride != 0 && conf >= static_cast<int>(h.confirm) - 1);
    }

    if (fire) {
      const auto meta = h.to_word(); // lets a downstream consumer see the hint
      // Queue one candidate unless it is already on the way or already here.
      auto enqueue = [&](champsim::address pf, bool fill_here) {
        const uint64_t pblk = champsim::block_number{pf}.to<uint64_t>();
        if (inflight.contains(pblk) || resident.contains(pblk) || lower_tracked.contains(pblk) || pending_blocks.count(pblk))
          return false;
        if (pending.size() >= PENDING_MAX) { // the oldest queued candidate is the stalest
          pending_blocks.erase(champsim::block_number{pending.front().addr}.to<uint64_t>());
          pending.pop_front();
        }
        pending.push_back({pf, fill_here, ridx, meta});
        pending_blocks.insert(pblk);
        return true;
      };
      // Virtual-only mode is hard-wired (ooo_cpu.cc): every address is virtual, so
      // prefetches cross pages freely. Keep it that way: do not add translation back.

      // Fill policy: confidence C decides whether the block fills this level.
      const double conf_c = confidence(reg, conf);
      bool fill_here = true;
      switch (h.fill) {
      case fill_policy::ALWAYS_L1D: fill_here = true; break;
      case fill_policy::GATED_LOW: fill_here = conf_c >= knobbed_pf::GATED_LOW_THRESHOLD; break;
      case fill_policy::GATED_HIGH: fill_here = conf_c >= knobbed_pf::GATED_HIGH_THRESHOLD; break;
      case fill_policy::ALWAYS_L2: fill_here = false; break;
      }

      const auto rg = knobbed_pf::ladder_rung(h.source, h.level);
      bool engine_had_something = false; // did the engine have a prediction (even if it was already queued)?
      if (h.source == stride_source::MARKOV) {
        // Follow the chain of remembered successors. The first (distance-1)
        // links are skipped, the next `degree` links are prefetched. The first
        // link must have repeated `confirm` times; a missing link ends the walk.
        uint64_t p = blk;
        for (int i = 0; i < rg.distance + rg.degree - 1; ++i) {
          const auto* it = markov.find(p);
          if (!it || (i == 0 && it->conf < static_cast<int>(h.confirm) - 1))
            break;
          p = it->next;
          if (i + 1 >= rg.distance) {
            engine_had_something = true;
            enqueue(champsim::address{champsim::block_number{p}}, fill_here);
          }
        }
      } else if (trained && use_stride != 0) {
        engine_had_something = true;
        for (int i = 0; i < rg.degree; ++i) {
          // Candidate i is (distance + i) strides ahead of this access.
          const auto steps = static_cast<champsim::block_number::difference_type>(rg.distance + i);
          champsim::address pf{champsim::block_number{cl_addr} + use_stride * steps};
          enqueue(pf, fill_here);
        }
      }
      // Fallback: the engine had nothing to say (stride not trained yet, Markov
      // has no link). Send a small next-line prefetch to the next level down, where
      // a wrong guess costs little. Next-line itself has no such case.
      if (h.fallback && !engine_had_something && h.source != stride_source::NEXT_LINE) {
        for (int i = 1; i <= FALLBACK_DEGREE; ++i) {
          champsim::address pf{champsim::block_number{cl_addr} + i};
          enqueue(pf, false);
        }
      }
    }
  }

  // ---- 5. epoch / throttling -------------------------------------------------
  if (reg.epoch.demand_accesses >= tp.epoch_accesses)
    end_epoch(reg, ridx);

  return metadata_in;
}

// The feedback controller: run once per region per epoch (tp.epoch_accesses
// demand accesses). It first updates the region's smoothed accuracy, then (if
// the region's hint allows throttling) nudges distance or degree.
// Rules are checked in this order, and only the first that applies is used:
//   accuracy < acc_floor                  -> suspend the prefetcher for a while
//   accuracy < acc_low, or pollution      -> one rung down the ladder
//   unused/hurtful > early_high
//   accuracy > acc_high                   -> one rung up, never above the configured level
// So the controller can only reduce aggressiveness relative to the hint (and
// recover); "throttle on" never overrides the hint upward. Note that acc_ewma and
// the fill-policy confidence gating adapt even when throttle is off; only the
// ladder stepping and suspension are switched by the throttle knob.
void knobbed::end_epoch(region& r, std::size_t idx)
{
  (void)idx;
  const auto& e = r.epoch;
  if (r.suspended_epochs > 0) {
    // Currently suspended: count down, then probe again from the starting hint.
    if (--r.suspended_epochs == 0)
      r.active = r.configured;
  } else if (e.total_issued() < tp.min_issued) {
    // Too little evidence (few triggers, or the fill policy sent nearly everything
    // to the next level where it is not counted here). Accuracy could then never be
    // re-measured and would stay low forever, so drift back toward optimism and
    // let the prefetcher try again.
    if (e.demand_misses > 0)
      r.acc_ewma = 0.5 * r.acc_ewma + 0.5;
  } else if (e.total_issued() >= tp.min_issued) {
    r.acc_ewma = 0.5 * r.acc_ewma + 0.5 * e.accuracy(); // smoothed accuracy
    if (r.active.throttle) {
      auto& a = r.active;
      const double acc = e.accuracy();
      const double early = static_cast<double>(e.unused_evicted + e.early_hurtful) / static_cast<double>(e.total_issued());
      const auto before = a.level;
      if (acc < tp.acc_floor && e.total_issued() >= 2 * tp.min_issued) {
        r.suspended_epochs = tp.suspend_epochs; // "if unsure, don't prefetch" (P7:10)
      } else if (acc < tp.acc_low || early > tp.early_high) {
        a.level = static_cast<uint8_t>(std::max(0, a.level - 1)); // too inaccurate or polluting: one rung down
      } else if (acc > tp.acc_high) {
        // Healthy again: climb back, but never above what the hint asked for.
        a.level = std::min<uint8_t>(r.configured.level, static_cast<uint8_t>(a.level + 1));
      }
      if (before != a.level || r.suspended_epochs)
        ++r.throttle_changes;
    }
  }
  r.epoch = {}; // start the next epoch with fresh counters
}

// Called by the cache when a block is filled into it. `prefetch` is true when
// the fill was caused by a prefetch; `evicted_addr` is the block it displaced.
uint32_t knobbed::prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in)
{
  const uint64_t blk = champsim::block_number{addr}.to<uint64_t>();

  // If the displaced block was a prefetched block nobody used, that is the
  // pollution signal (and it may turn into an "early and hurtful" miss later).
  if (evicted_addr != champsim::address{}) {
    const uint64_t eblk = champsim::block_number{evicted_addr}.to<uint64_t>();
    if (const auto* t = resident.find(eblk)) {
      const auto idx = t->region_idx;
      auto& owner = regions[idx];
      ++owner.epoch.unused_evicted;
      ++owner.total.unused_evicted;
      note_evicted_unused(eblk, idx);
      resident.erase(eblk);
    }
  }

  // A prefetch we issued has landed: it moves from "in flight" to "resident,
  // unused". It leaves `resident` on its first demand hit or when evicted.
  if (prefetch) {
    if (const auto* t = inflight.find(blk)) {
      const auto tb = *t; // copy: erase() below invalidates t
      inflight.erase(blk);
      resident.put(blk, tb); // oldest entry is dropped if full; that only loses stats
    }
  }
  return metadata_in;
}

// Called by ChampSim every cycle. Issues at most one queued candidate: a
// rejected request (prefetch queue full) stays at the front and is retried the
// next cycle.
void knobbed::prefetcher_cycle_operate()
{
  ++now;
  // ChampSim zeroes its own statistics when warmup ends. Do the same for our
  // reported totals (the epoch counters and learned state carry on) so they cover
  // the same instructions as ChampSim's.
  if (in_warmup && !intern_->warmup) {
    in_warmup = false;
    for (auto& r : regions)
      r.total = {};
  }
  // Forget prefetches whose outcome we can no longer observe. An in-flight
  // prefetch that never produced a fill (block already present, or merged with
  // a demand miss) must not stay "in flight" forever: it would block prefetching
  // that block again and make a much later demand miss look like a late prefetch.
  // A lower-level prefetch is assumed evicted from that level after LOWER_EXPIRE
  // cycles; a demand miss after that is not credited to the prefetcher.
  // Both maps are in issue order, so only the front needs checking.
  inflight.expire_oldest([&](const tracked_block& t) { return now - t.born > INFLIGHT_EXPIRE; });
  lower_tracked.expire_oldest([&](const tracked_block& t) { return now - t.born > LOWER_EXPIRE; });

  // Drop queued candidates that became redundant while they waited (a demand or
  // an earlier prefetch already brought the block in).
  while (!pending.empty()) {
    const uint64_t b = champsim::block_number{pending.front().addr}.to<uint64_t>();
    if (!(inflight.contains(b) || resident.contains(b) || lower_tracked.contains(b)))
      break;
    pending_blocks.erase(b);
    pending.pop_front();
  }
  if (pending.empty())
    return;
  const auto c = pending.front();
  if (!intern_->prefetch_line(c.addr, c.fill_this_level, c.metadata))
    return; // PQ full: retry next cycle

  pending.pop_front();
  const uint64_t pblk = champsim::block_number{c.addr}.to<uint64_t>();
  pending_blocks.erase(pblk);
  auto& r = regions[c.region_idx];
  if (c.fill_this_level) {
    // The fill will be seen by prefetcher_cache_fill(), so track it there.
    inflight.put(pblk, tracked_block{c.region_idx, now});
    ++r.epoch.issued;
    ++r.total.issued;
  } else {
    // Fills a lower level only: this cache never sees the fill, so the only
    // observable outcome is a later demand miss on the block (useful_lower).
    lower_tracked.put(pblk, tracked_block{c.region_idx, now});
    ++r.epoch.issued_lower;
    ++r.total.issued_lower;
  }
}

// Printed by ChampSim at the end of the simulation: where each region ended up
// and its overall telemetry.
void knobbed::prefetcher_final_stats()
{
  for (std::size_t i = 0; i < regions.size(); ++i) {
    const auto& r = regions[i];
    const auto& t = r.total;
    if (i > 0 || t.demand_accesses)
      std::printf("[knobbed %s] region %zu [%#llx,%#llx) final level=%d (distance=%d degree=%d) changes=%llu | issued=%llu lower=%llu useful=%llu "
                  "(timely=%llu late=%llu lower=%llu) unused_evicted=%llu early_hurtful=%llu | accuracy=%.3f coverage=%.3f timeliness=%.3f\n",
                  intern_->NAME.c_str(), i, static_cast<unsigned long long>(r.start), static_cast<unsigned long long>(r.end), r.active.level, r.active.distance(),
                  r.active.degree(), static_cast<unsigned long long>(r.throttle_changes), static_cast<unsigned long long>(t.issued),
                  static_cast<unsigned long long>(t.issued_lower), static_cast<unsigned long long>(t.useful()),
                  static_cast<unsigned long long>(t.useful_timely), static_cast<unsigned long long>(t.useful_late),
                  static_cast<unsigned long long>(t.useful_lower), static_cast<unsigned long long>(t.unused_evicted),
                  static_cast<unsigned long long>(t.early_hurtful), t.accuracy(), t.coverage(), t.timeliness());
  }
  std::printf("[knobbed %s] markov table: %zu of %zu entries used\n", intern_->NAME.c_str(), markov.size(), MARKOV_MAX);
}
