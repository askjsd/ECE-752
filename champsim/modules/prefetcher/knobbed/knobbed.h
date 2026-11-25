// Knobbed prefetcher: a baseline ChampSim prefetcher whose behaviour is
// controlled by the "hint control word" knobs in flagbench/prefetcher_knobs.md.
//
//   Section 0 telemetry   -> struct telemetry (accuracy / coverage / timeliness
//                            plus the early-and-hurtful signal)
//   Section 1 "when"      -> trigger mode, aggressiveness ladder (distance+degree), throttling
//   Section 2 "where"     -> fill policy (which level a prefetch fills); insertion position
//                            is a separate replacement module (lru_pf) read from KNOBBED_INSERT
//   Section 3 "what"      -> engine: off | next-line | per-PC stride | Markov, confirmation,
//                            next-line fallback
//
// Each region of the address space can carry its own hint (the "region hints"
// a compiler or static tuner would emit). Addresses outside every region use
// the default hint. A hint is only a *starting point*: when throttling is on,
// a per-region feedback controller can step the aggressiveness level down (and back
// up, never above the hint) each epoch from the measured telemetry.
#ifndef KNOBBED_PREFETCHER_H
#define KNOBBED_PREFETCHER_H

#include <cstdint>
#include <deque>
#include <iterator>
#include <list>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "address.h"
#include "champsim.h"
#include "modules.h"
#include "msl/lru_table.h"

namespace knobbed_pf
{
// DEAD_BLOCK was removed: it behaved exactly like MISS and so gave MISS two codes.
enum class trigger_mode : uint8_t { MISS = 0, MISS_TAGGED = 1, ACCESS = 2 };
// The engine that predicts addresses. OFF is "prefetching disabled" (it replaces a
// separate enable bit so the 2-bit field has exactly 4 values and no aliases).
// MARKOV is a miss-to-miss correlation table: it follows a repeated irregular
// sequence such as a linked list that is walked more than once.
enum class stride_source : uint8_t { OFF = 0, NEXT_LINE = 1, PER_PC = 2, MARKOV = 3 };
// Which cache level a prefetch fills. ChampSim only offers "this level" or "the next
// level down", so the policy is: always this level, this level only when the
// confidence is high enough (two thresholds), or always the next level down.
enum class fill_policy : uint8_t { ALWAYS_L1D = 0, GATED_LOW = 1, GATED_HIGH = 2, ALWAYS_L2 = 3 };
constexpr double GATED_LOW_THRESHOLD = 0.34;  // confidence needed to fill this level under GATED_LOW
constexpr double GATED_HIGH_THRESHOLD = 0.67; // ... under GATED_HIGH

// One rung of the aggressiveness ladder: how far ahead (distance, in strides or
// chain links) and how many blocks per trigger (degree). Level 0 is the gentlest.
struct rung {
  uint8_t distance;
  uint8_t degree;
};
// The ladder differs per engine. Stride/next-line distances stop at 8 because the
// page limit (64 lines) cuts off anything further. Markov distance means "chain
// links skipped", and chains longer than a few links rarely exist, so its ladder
// stays short.
const rung& ladder_rung(stride_source source, int level);
constexpr int LADDER_LEVELS = 8;

// Section 4 of prefetcher_knobs.md. Every field here is runtime-changeable.
struct hint {
  stride_source source = stride_source::PER_PC;
  trigger_mode trigger = trigger_mode::MISS_TAGGED;
  uint8_t confirm = 2; // 1 or 2: matching strides / repeats needed before issuing (PER_PC, MARKOV)
  uint8_t level = 1;   // aggressiveness ladder level 0-7 (distance and degree come from the ladder)
  fill_policy fill = fill_policy::GATED_LOW;
  bool throttle = true;   // let the runtime controller step the level down (never above `level`)
  bool fallback = false;  // when the engine has nothing to issue, send a small next-line prefetch to the next level

  uint8_t distance() const { return ladder_rung(source, level).distance; }
  uint8_t degree() const { return ladder_rung(source, level).degree; }

  // Packed 32-bit control word (used as prefetch metadata and by `word=`).
  //  [1:0] source   [3:2] trigger (0-2)   [4] confirm-1   [7:5] level
  //  [9:8] fill     [10] throttle         [11] fallback
  uint32_t to_word() const;
  // False for a word with a code that means nothing (trigger 3).
  static bool word_valid(uint32_t word);
  static hint from_word(uint32_t word);
  // "key=value,key=value": source trigger confirm level fill throttle fallback word.
  // Returns false (and a message) on error.
  bool apply(const std::string& spec, std::string* error);
};

// Section 0 counters. Kept per region and per epoch.
struct telemetry {
  uint64_t issued = 0;         // prefetches accepted that fill this level
  uint64_t issued_lower = 0;   // prefetches that fill only a lower level (not measurable here)
  uint64_t useful_timely = 0;  // demand hit a prefetched block
  uint64_t useful_late = 0;    // demand missed while its prefetch was in flight
  uint64_t useful_lower = 0;   // demand missed here on a block we prefetched into a lower level
  uint64_t unused_evicted = 0; // prefetched block evicted before any use
  uint64_t early_hurtful = 0;  // demand missed on a block we evicted unused
  uint64_t demand_misses = 0;
  uint64_t demand_accesses = 0;

  uint64_t useful() const { return useful_timely + useful_late + useful_lower; }
  uint64_t total_issued() const { return issued + issued_lower; }
  // accuracy = useful / total prefetches (both fill levels)
  double accuracy() const { return total_issued() ? static_cast<double>(useful()) / static_cast<double>(total_issued()) : 0.0; }
  // coverage ~ useful / (misses + useful): baseline misses are not observable
  double coverage() const
  {
    const auto d = demand_misses + useful_timely;
    return d ? static_cast<double>(useful()) / static_cast<double>(d) : 0.0;
  }
  // timeliness = timely / useful
  // (a lower-level hit is counted as timely: the block was there in time)
  double timeliness() const { return useful() ? static_cast<double>(useful_timely + useful_lower) / static_cast<double>(useful()) : 1.0; }
};

// A map from block number to V that remembers insertion order, so the *oldest*
// entry can be dropped when it is full or too old (std::unordered_map::begin()
// is not the oldest, and std::map::begin() is the lowest address). put() makes an
// entry the newest, whether it is new or replaces an old one, so the same class
// gives FIFO (never re-put) or LRU (re-put on use).
template <typename V>
class fifo_map
{
public:
  explicit fifo_map(std::size_t capacity) : cap_(capacity) {}
  V* find(uint64_t k)
  {
    auto it = idx_.find(k);
    return it == idx_.end() ? nullptr : &it->second->second;
  }
  bool contains(uint64_t k) const { return idx_.count(k) != 0; }
  std::size_t size() const { return idx_.size(); }
  // `v` is taken by value so it may be a copy of an entry this call replaces.
  void put(uint64_t k, V v)
  {
    erase(k);
    items_.emplace_back(k, std::move(v));
    idx_[k] = std::prev(items_.end());
    if (idx_.size() > cap_) { // over capacity: drop the oldest
      idx_.erase(items_.front().first);
      items_.pop_front();
    }
  }
  bool erase(uint64_t k)
  {
    auto it = idx_.find(k);
    if (it == idx_.end())
      return false;
    items_.erase(it->second);
    idx_.erase(it);
    return true;
  }
  // Drop oldest entries while `stale(value)` is true (entries are in age order).
  template <typename F>
  void expire_oldest(F stale)
  {
    while (!items_.empty() && stale(items_.front().second)) {
      idx_.erase(items_.front().first);
      items_.pop_front();
    }
  }

private:
  std::size_t cap_;
  std::list<std::pair<uint64_t, V>> items_;
  std::unordered_map<uint64_t, typename std::list<std::pair<uint64_t, V>>::iterator> idx_;
};

struct region {
  uint64_t start = 0; // byte addresses, [start, end)
  uint64_t end = UINT64_MAX;
  hint configured;    // the starting hint
  hint active;        // configured + whatever the throttle has changed
  telemetry epoch, total;
  uint32_t suspended_epochs = 0;
  double acc_ewma = 1.0; // smoothed accuracy, used as part of the confidence
  uint64_t throttle_changes = 0;
};

struct throttle_params {
  uint64_t epoch_accesses = 4096; // demand accesses to a region per epoch
  uint64_t min_issued = 16;       // do not adjust on too little evidence
  double acc_floor = 0.10;        // below: suspend the prefetcher for a while
  double acc_low = 0.40;          // below: step the ladder level down
  double acc_high = 0.75;         // above (and not early/hurtful): step the level back up, never above the configured level
  double early_high = 0.30;       // unused-evicted + early-hurtful / issued above: step the level down
  uint32_t suspend_epochs = 4;
};
} // namespace knobbed_pf

// The prefetcher module itself. ChampSim calls the prefetcher_* methods below;
// everything else is this module's own state.
struct knobbed : public champsim::modules::prefetcher {
  // One row of the per-PC stride table (same shape as ChampSim's ip_stride,
  // plus a confidence counter). Strides are in cache lines.
  struct tracker_entry {
    champsim::address ip{};
    champsim::block_number last_cl_addr{};
    champsim::block_number::difference_type last_stride{};
    int conf = 0; // consecutive times last_stride repeated (0-3)

    auto index() const
    {
      using namespace champsim::data::data_literals;
      return ip.slice_upper<2_b>();
    }
    auto tag() const
    {
      using namespace champsim::data::data_literals;
      return ip.slice_upper<2_b>();
    }
  };

  // A prefetch waiting to be issued by prefetcher_cycle_operate().
  struct candidate {
    champsim::address addr;
    bool fill_this_level;   // false: fill only the next level down
    std::size_t region_idx; // region whose hint produced it (for telemetry credit)
    uint32_t metadata;      // the region's packed hint word, passed down with the prefetch
  };

  // Which region issued a prefetch we are tracking.
  struct tracked_block {
    std::size_t region_idx;
    uint64_t born = 0; // value of `now` when the prefetch was issued (used to expire stale in-flight entries)
  };

  // Miss-correlation table row: after a miss on block K the next miss was
  // `next`. `conf` counts how often that successor repeated (0-3).
  struct markov_entry {
    uint64_t next = 0;
    int conf = 0;
  };

  // Structural parameters: fixed per experiment, not part of the hint word.
  constexpr static std::size_t TRACKER_SETS = 256; // stride table: sets x ways = 1024 PCs
  constexpr static std::size_t TRACKER_WAYS = 4;
  constexpr static std::size_t PENDING_MAX = 64;   // candidate queue depth
  constexpr static std::size_t TRACK_MAX = 4096;   // cap on each tracking map below
  constexpr static std::size_t EVICTED_MAX = 256;  // how long evicted-unused blocks are remembered
  constexpr static int FALLBACK_DEGREE = 2;         // next-line blocks sent by the fallback
  constexpr static std::size_t MARKOV_MAX = 1 << 16; // correlation table size (entries)
  constexpr static uint64_t INFLIGHT_EXPIRE = 2000;  // cycles after which an unfilled prefetch stops counting as in flight
  constexpr static uint64_t LOWER_EXPIRE = 100000;   // cycles after which a lower-level prefetch is assumed evicted there

  champsim::msl::lru_table<tracker_entry> table{TRACKER_SETS, TRACKER_WAYS}; // per-PC stride table
  std::vector<knobbed_pf::region> regions; // regions[0] is the default hint
  knobbed_pf::throttle_params tp;          // throttle thresholds (same for every region)
  std::deque<candidate> pending;           // queued prefetches, issued one per cycle
  knobbed_pf::fifo_map<markov_entry> markov{MARKOV_MAX}; // miss block -> following miss block (LRU: refreshed on every training update)
  std::unordered_set<uint64_t> pending_blocks;      // blocks currently in `pending`, to avoid queueing duplicates
  uint64_t last_miss = 0;                  // previous miss (or first use of a prefetched block)
  bool have_last_miss = false;
  uint64_t now = 0;                        // cycles seen by prefetcher_cycle_operate()
  bool in_warmup = true;                   // lets us reset the totals when ChampSim ends warmup (it resets its own stats then)

  // Block-number -> issuing region. Together these classify every prefetch's
  // outcome (the keys are cache-line numbers, not byte addresses):
  // inflight and lower_tracked also age out by cycle count (see prefetcher_cycle_operate).
  knobbed_pf::fifo_map<tracked_block> inflight{TRACK_MAX};      // issued to this level, fill not seen yet
  knobbed_pf::fifo_map<tracked_block> resident{TRACK_MAX};      // filled by a prefetch, not yet used
  knobbed_pf::fifo_map<tracked_block> lower_tracked{TRACK_MAX}; // sent to a lower level only
  std::unordered_map<uint64_t, tracked_block> evicted_unused; // evicted unused, remembered briefly
  std::deque<uint64_t> evicted_order;                         // FIFO order for evicted_unused

  using champsim::modules::prefetcher::prefetcher;

  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                    uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);
  void prefetcher_cycle_operate();
  void prefetcher_final_stats();

  // Runtime interface: a driver (or a later hint decoder) can retarget a region
  // or add one while simulating.
  void set_region_hint(std::size_t idx, const knobbed_pf::hint& h);
  std::size_t add_region(uint64_t start, uint64_t end, const knobbed_pf::hint& h);

private:
  // Helpers (see knobbed.cc).
  std::size_t find_region(uint64_t byte_addr) const;
  void end_epoch(knobbed_pf::region& r, std::size_t idx);
  void note_evicted_unused(uint64_t blk, std::size_t idx);
  void load_environment();
  double confidence(const knobbed_pf::region& r, int stride_conf) const;
};

#endif
