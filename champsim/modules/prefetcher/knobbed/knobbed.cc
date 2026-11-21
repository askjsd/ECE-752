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
// Map a threshold in [0,1] to the 3-bit field used in the packed hint word.
uint32_t quantize(double v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0, 1.0) * 7.0)); }

// Remove leading and trailing whitespace.
std::string trim(std::string s)
{
  const auto b = s.find_first_not_of(" \t\r\n");
  const auto e = s.find_last_not_of(" \t\r\n");
  return b == std::string::npos ? std::string{} : s.substr(b, e - b + 1);
}
} // namespace

// Pack the hint into the 32-bit control word (layout documented in knobbed.h).
// Distance, degree, confirm and the thresholds are clamped/quantized to the
// bit widths the word has room for.
uint32_t hint::to_word() const
{
  uint32_t w = 0;
  w |= (enable ? 1u : 0u);
  w |= (static_cast<uint32_t>(trigger) & 0x3u) << 1;
  w |= (static_cast<uint32_t>(source) & 0x1u) << 3;
  w |= (std::min<uint32_t>(distance, 31) & 0x1Fu) << 4;
  w |= ((std::clamp<uint32_t>(degree, 1, 8) - 1) & 0x7u) << 9; // stored as degree-1
  w |= (quantize(fill_thresh) & 0x7u) << 12;
  w |= (quantize(drop_thresh) & 0x7u) << 15;
  w |= (static_cast<uint32_t>(insertion) & 0x3u) << 18;
  w |= (throttle ? 1u : 0u) << 20;
  w |= ((std::clamp<uint32_t>(confirm, 1, 4) - 1) & 0x3u) << 21; // stored as confirm-1
  return w;
}

// Inverse of to_word(). Thresholds come back quantized to multiples of 1/7.
hint hint::from_word(uint32_t w)
{
  hint h;
  h.enable = w & 1u;
  h.trigger = static_cast<trigger_mode>((w >> 1) & 0x3u);
  h.source = static_cast<stride_source>((w >> 3) & 0x1u);
  h.distance = (w >> 4) & 0x1Fu;
  h.degree = static_cast<uint8_t>(((w >> 9) & 0x7u) + 1);
  h.fill_thresh = static_cast<double>((w >> 12) & 0x7u) / 7.0;
  h.drop_thresh = static_cast<double>((w >> 15) & 0x7u) / 7.0;
  h.insertion = static_cast<insertion_pos>((w >> 18) & 0x3u);
  h.throttle = (w >> 20) & 1u;
  h.confirm = static_cast<uint8_t>(((w >> 21) & 0x3u) + 1);
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
    // std::stoi/stod throw on malformed numbers; that is caught below.
    try {
      if (k == "enable")
        enable = (v == "1" || v == "on" || v == "true");
      else if (k == "trigger") {
        if (v == "miss")
          trigger = trigger_mode::MISS;
        else if (v == "tagged")
          trigger = trigger_mode::MISS_TAGGED;
        else if (v == "access")
          trigger = trigger_mode::ACCESS;
        else
          return fail("trigger must be miss|tagged|access");
      } else if (k == "source") {
        if (v == "nextline")
          source = stride_source::NEXT_LINE;
        else if (v == "stride")
          source = stride_source::PER_PC;
        else
          return fail("source must be nextline|stride");
      } else if (k == "confirm")
        confirm = static_cast<uint8_t>(std::clamp(std::stoi(v), 1, 4));
      else if (k == "distance")
        distance = static_cast<uint8_t>(std::clamp(std::stoi(v), 1, 31));
      else if (k == "degree")
        degree = static_cast<uint8_t>(std::clamp(std::stoi(v), 1, 8));
      else if (k == "fill")
        fill_thresh = std::clamp(std::stod(v), 0.0, 1.0);
      else if (k == "drop")
        drop_thresh = std::clamp(std::stod(v), 0.0, 1.0);
      else if (k == "insert") {
        if (v == "mru")
          insertion = insertion_pos::MRU;
        else if (v == "mid")
          insertion = insertion_pos::MID;
        else if (v == "lru")
          insertion = insertion_pos::LRU;
        else
          return fail("insert must be mru|mid|lru");
      } else if (k == "throttle")
        throttle = (v == "1" || v == "on" || v == "true");
      else if (k == "word") // a whole packed control word, e.g. word=0x30c21b
        *this = from_word(static_cast<uint32_t>(std::stoul(v, nullptr, 0)));
      else
        return fail("unknown knob: " + k);
    } catch (const std::exception&) {
      return fail("bad value for " + k + ": " + v);
    }
  }
  return true;
}
} // namespace knobbed_pf

using knobbed_pf::hint;
using knobbed_pf::region;
using knobbed_pf::stride_source;
using knobbed_pf::trigger_mode;

// ---------------------------------------------------------------------------
// Configuration
//
// ChampSim's module interface has no place for per-run parameters, so settings
// come from the environment:
//   KNOBBED_HINT="distance=4,degree=2,trigger=tagged,source=stride,throttle=0"
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
  std::printf("[knobbed %s] default hint: word=%#x enable=%d trigger=%d source=%d confirm=%d distance=%d degree=%d fill=%.2f drop=%.2f throttle=%d regions=%zu\n",
              intern_->NAME.c_str(), h.to_word(), h.enable, static_cast<int>(h.trigger), static_cast<int>(h.source), h.confirm, h.distance, h.degree,
              h.fill_thresh, h.drop_thresh, h.throttle, regions.size() - 1);
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
// next-line has no stride to confirm, so that term is 1.
double knobbed::confidence(const region& r, int stride_conf) const
{
  const double stride_c = r.active.source == stride_source::NEXT_LINE ? 1.0 : std::min(1.0, static_cast<double>(stride_conf + 1) / 4.0);
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
    if (auto it = resident.find(blk); it != resident.end()) {
      auto& owner = regions[it->second.region_idx];
      ++owner.epoch.useful_timely;
      ++owner.total.useful_timely;
      resident.erase(it); // counted once: the prefetch bit is cleared on first use
    }
  }
  if (!cache_hit) {
    ++reg.epoch.demand_misses;
    ++reg.total.demand_misses;
    if (auto lo = lower_tracked.find(blk); lo != lower_tracked.end()) {
      // We prefetched this into a lower level only, and the core did want it.
      // It will hit in the lower level instead of going to memory.
      auto& owner = regions[lo->second.region_idx];
      ++owner.epoch.useful_lower;
      ++owner.total.useful_lower;
      lower_tracked.erase(lo);
    } else if (auto it = inflight.find(blk); it != inflight.end()) {
      // Demand missed while the prefetch for it was still in flight: the
      // prefetch was useful but late (an MSHR hit on a prefetch).
      auto& owner = regions[it->second.region_idx];
      ++owner.epoch.useful_late;
      ++owner.total.useful_late;
      inflight.erase(it);
    } else if (auto ev = evicted_unused.find(blk); ev != evicted_unused.end()) {
      // We evicted this block unused and now it is needed again: the prefetch
      // was early and hurtful (it pushed out something and then was itself lost).
      auto& owner = regions[ev->second.region_idx];
      ++owner.epoch.early_hurtful;
      ++owner.total.early_hurtful;
      evicted_unused.erase(ev);
    }
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
  if (reg.suspended_epochs == 0 && h.enable) {
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
    case trigger_mode::DEAD_BLOCK: // reserved, not implemented: acts like MISS
      fire = !cache_hit;
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

    if (fire && trained && use_stride != 0) {
      // Confidence decides what to do with the prefetch: below `drop` issue
      // nothing, below `fill` send it only to the next level down, else fill
      // this level (the slide rule C*savings - (1-C)*cost, reduced to thresholds).
      const double c = confidence(reg, conf);
      if (c >= h.drop_thresh) {
        const bool fill_here = c >= h.fill_thresh;
        const auto meta = h.to_word(); // lets a downstream consumer see the hint
        for (int i = 0; i < h.degree; ++i) {
          // Candidate i is (distance + i) strides ahead of this access.
          const auto steps = static_cast<champsim::block_number::difference_type>(h.distance + i);
          champsim::address pf{champsim::block_number{cl_addr} + use_stride * steps};
          if (!intern_->virtual_prefetch && champsim::page_number{pf} != champsim::page_number{addr})
            break; // physical addresses: do not run off the page
          const uint64_t pblk = champsim::block_number{pf}.to<uint64_t>();
          if (inflight.count(pblk) || resident.count(pblk) || lower_tracked.count(pblk))
            continue; // already on the way or already here
          if (pending.size() >= PENDING_MAX)
            pending.pop_front(); // the oldest queued candidate is the stalest
          pending.push_back({pf, fill_here, ridx, meta});
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
//   accuracy < acc_floor           -> suspend the prefetcher for a while
//   accuracy < acc_low             -> degree - 1
//   unused/hurtful > early_high    -> distance - 1
//   late fraction > late_high      -> distance + 1
//   accuracy > acc_high            -> degree + 1
void knobbed::end_epoch(region& r, std::size_t idx)
{
  (void)idx;
  const auto& e = r.epoch;
  if (r.suspended_epochs > 0) {
    // Currently suspended: count down, then probe again from the starting hint.
    if (--r.suspended_epochs == 0)
      r.active = r.configured;
  } else if (e.total_issued() < tp.min_issued) {
    // Too little evidence. This can happen because confidence fell below the
    // `drop` threshold, so nothing is issued and accuracy could never be
    // re-measured (it would stay low forever). Drift back toward optimism so
    // the prefetcher tries again.
    if (e.demand_misses > 0)
      r.acc_ewma = 0.5 * r.acc_ewma + 0.5;
  } else if (e.total_issued() >= tp.min_issued) {
    r.acc_ewma = 0.5 * r.acc_ewma + 0.5 * e.accuracy(); // smoothed accuracy
    if (r.active.throttle) {
      auto& a = r.active;
      const double acc = e.accuracy();
      const double late = e.useful() ? static_cast<double>(e.useful_late) / static_cast<double>(e.useful()) : 0.0;
      const double early = static_cast<double>(e.unused_evicted + e.early_hurtful) / static_cast<double>(e.total_issued());
      const auto before = std::pair{a.distance, a.degree};
      if (acc < tp.acc_floor && e.total_issued() >= 2 * tp.min_issued) {
        r.suspended_epochs = tp.suspend_epochs; // "if unsure, don't prefetch" (P7:10)
      } else if (acc < tp.acc_low) {
        a.degree = static_cast<uint8_t>(std::max(1, a.degree - 1));
      } else if (early > tp.early_high) {
        a.distance = static_cast<uint8_t>(std::max(1, a.distance - 1));
      } else if (late > tp.late_high) {
        a.distance = std::min(tp.max_distance, static_cast<uint8_t>(a.distance + 1));
      } else if (acc > tp.acc_high) {
        a.degree = std::min(tp.max_degree, static_cast<uint8_t>(a.degree + 1));
      }
      if (before != std::pair{a.distance, a.degree} || r.suspended_epochs)
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
    if (auto it = resident.find(eblk); it != resident.end()) {
      auto& owner = regions[it->second.region_idx];
      ++owner.epoch.unused_evicted;
      ++owner.total.unused_evicted;
      note_evicted_unused(eblk, it->second.region_idx);
      resident.erase(it);
    }
  }

  // A prefetch we issued has landed: it moves from "in flight" to "resident,
  // unused". It leaves `resident` on its first demand hit or when evicted.
  if (prefetch) {
    if (auto it = inflight.find(blk); it != inflight.end()) {
      if (resident.size() >= TRACK_MAX)
        resident.erase(resident.begin()); // bound memory; losing old entries only drops stats
      resident[blk] = it->second;
      inflight.erase(it);
    }
  }
  return metadata_in;
}

// Called by ChampSim every cycle. Issues at most one queued candidate: a
// rejected request (prefetch queue full) stays at the front and is retried the
// next cycle.
void knobbed::prefetcher_cycle_operate()
{
  if (pending.empty())
    return;
  const auto c = pending.front();
  if (!intern_->prefetch_line(c.addr, c.fill_this_level, c.metadata))
    return; // PQ full: retry next cycle

  pending.pop_front();
  auto& r = regions[c.region_idx];
  const uint64_t pblk = champsim::block_number{c.addr}.to<uint64_t>();
  if (c.fill_this_level) {
    // The fill will be seen by prefetcher_cache_fill(), so track it there.
    if (inflight.size() >= TRACK_MAX)
      inflight.erase(inflight.begin());
    inflight[pblk] = tracked_block{c.region_idx};
    ++r.epoch.issued;
    ++r.total.issued;
  } else {
    // Fills a lower level only: this cache never sees the fill, so the only
    // observable outcome is a later demand miss on the block (useful_lower).
    if (lower_tracked.size() >= TRACK_MAX)
      lower_tracked.erase(lower_tracked.begin());
    lower_tracked[pblk] = tracked_block{c.region_idx};
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
      std::printf("[knobbed %s] region %zu [%#llx,%#llx) final distance=%d degree=%d changes=%llu | issued=%llu lower=%llu useful=%llu "
                  "(timely=%llu late=%llu lower=%llu) unused_evicted=%llu early_hurtful=%llu | accuracy=%.3f coverage=%.3f timeliness=%.3f\n",
                  intern_->NAME.c_str(), i, static_cast<unsigned long long>(r.start), static_cast<unsigned long long>(r.end), r.active.distance,
                  r.active.degree, static_cast<unsigned long long>(r.throttle_changes), static_cast<unsigned long long>(t.issued),
                  static_cast<unsigned long long>(t.issued_lower), static_cast<unsigned long long>(t.useful()),
                  static_cast<unsigned long long>(t.useful_timely), static_cast<unsigned long long>(t.useful_late),
                  static_cast<unsigned long long>(t.useful_lower), static_cast<unsigned long long>(t.unused_evicted),
                  static_cast<unsigned long long>(t.early_hurtful), t.accuracy(), t.coverage(), t.timeliness());
  }
}
