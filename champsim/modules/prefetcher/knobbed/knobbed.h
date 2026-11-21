// Knobbed prefetcher: a baseline ChampSim prefetcher whose behaviour is
// controlled by the "hint control word" knobs in flagbench/prefetcher_knobs.md.
//
//   Section 0 telemetry   -> struct telemetry (accuracy / coverage / timeliness
//                            plus the early-and-hurtful signal)
//   Section 1 "when"      -> enable, trigger mode, distance, degree, throttling
//   Section 2 "where"     -> confidence-based fill level, insertion hint
//   Section 3.1 "what"    -> next-line vs per-PC learned stride, confirmation
//
// Each region of the address space can carry its own hint (the "region hints"
// a compiler or static tuner would emit). Addresses outside every region use
// the default hint. A hint is only a *starting point*: when throttling is on,
// a per-region feedback controller adjusts distance and degree each epoch from
// the measured telemetry.
#ifndef KNOBBED_PREFETCHER_H
#define KNOBBED_PREFETCHER_H

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "address.h"
#include "champsim.h"
#include "modules.h"
#include "msl/lru_table.h"

namespace knobbed_pf
{
enum class trigger_mode : uint8_t { MISS = 0, MISS_TAGGED = 1, ACCESS = 2, DEAD_BLOCK = 3 /* reserved: not implemented */ };
enum class stride_source : uint8_t { NEXT_LINE = 0, PER_PC = 1 };
enum class insertion_pos : uint8_t { MRU = 0, MID = 1, LRU = 2 }; // carried in prefetch metadata only

// Section 4 of prefetcher_knobs.md. Every field here is runtime-changeable.
struct hint {
  bool enable = true;
  trigger_mode trigger = trigger_mode::MISS_TAGGED;
  stride_source source = stride_source::PER_PC;
  uint8_t confirm = 2;  // matching strides needed before issuing (PER_PC)
  uint8_t distance = 1; // blocks ahead of the trigger (stride steps)
  uint8_t degree = 2;   // blocks issued per trigger
  double fill_thresh = 0.5; // confidence >= this: fill this cache level
  double drop_thresh = 0.1; // confidence <  this: do not prefetch at all
  insertion_pos insertion = insertion_pos::MRU;
  bool throttle = true; // let the runtime learner adjust distance/degree

  // Packed 32-bit control word (bit widths follow section 4 of the knob doc).
  //  [0]     enable            [1:2]  trigger        [3]    source
  //  [4:8]   distance (0-31)   [9:11] degree-1 (0-7) [12:14] fill thr (/7)
  //  [15:17] drop thr (/7)     [18:19] insertion     [20]   throttle
  //  [21:22] confirm-1 (0-3)
  uint32_t to_word() const;
  static hint from_word(uint32_t word);
  // "key=value,key=value": enable trigger source confirm distance degree
  // fill drop insert throttle word. Returns false (and a message) on error.
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
  double acc_low = 0.40;          // below: reduce degree
  double acc_high = 0.75;         // above (and not late/early): raise degree
  double late_high = 0.10;        // late / useful above: raise distance
  double early_high = 0.30;       // unused-evicted / issued above: lower distance
  uint8_t max_distance = 31;
  uint8_t max_degree = 8;
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
  };

  // Structural parameters: fixed per experiment, not part of the hint word.
  constexpr static std::size_t TRACKER_SETS = 256; // stride table: sets x ways = 1024 PCs
  constexpr static std::size_t TRACKER_WAYS = 4;
  constexpr static std::size_t PENDING_MAX = 64;   // candidate queue depth
  constexpr static std::size_t TRACK_MAX = 4096;   // cap on each tracking map below
  constexpr static std::size_t EVICTED_MAX = 256;  // how long evicted-unused blocks are remembered

  champsim::msl::lru_table<tracker_entry> table{TRACKER_SETS, TRACKER_WAYS}; // per-PC stride table
  std::vector<knobbed_pf::region> regions; // regions[0] is the default hint
  knobbed_pf::throttle_params tp;          // throttle thresholds (same for every region)
  std::deque<candidate> pending;           // queued prefetches, issued one per cycle

  // Block-number -> issuing region. Together these classify every prefetch's
  // outcome (the keys are cache-line numbers, not byte addresses):
  std::unordered_map<uint64_t, tracked_block> inflight;       // issued to this level, fill not seen yet
  std::unordered_map<uint64_t, tracked_block> resident;       // filled by a prefetch, not yet used
  std::unordered_map<uint64_t, tracked_block> lower_tracked;  // sent to a lower level only
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
