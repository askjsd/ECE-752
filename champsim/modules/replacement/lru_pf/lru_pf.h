// lru_pf: ChampSim's LRU replacement, plus one knob for where prefetched blocks
// are inserted. This makes the "insertion position" knob of the knobbed prefetcher
// real (the prefetcher cannot choose it itself: the cache decides where a fill goes).
//
//   KNOBBED_INSERT=mru   prefetch fills become most-recently-used (identical to plain LRU; default)
//   KNOBBED_INSERT=lru   prefetch fills become least-recently-used: the next miss in
//                        the set evicts them first, so a useless prefetch costs one way
//                        only until the next miss, but a useful one must be hit before then.
// A demand hit moves a block to MRU either way. Demand fills always insert at MRU.
#ifndef REPLACEMENT_LRU_PF_H
#define REPLACEMENT_LRU_PF_H

#include <vector>

#include "cache.h"
#include "modules.h"

class lru_pf : public champsim::modules::replacement
{
  long NUM_WAY;
  std::vector<uint64_t> last_used_cycles;
  uint64_t cycle = 1; // 0 is reserved for "LRU position"
  bool prefetch_at_lru = false;

public:
  explicit lru_pf(CACHE* cache);
  lru_pf(CACHE* cache, long sets, long ways);

  long find_victim(uint32_t triggering_cpu, uint64_t instr_id, long set, const champsim::cache_block* current_set, champsim::address ip,
                   champsim::address full_addr, access_type type);
  void replacement_cache_fill(uint32_t triggering_cpu, long set, long way, champsim::address full_addr, champsim::address ip, champsim::address victim_addr,
                              access_type type);
  void update_replacement_state(uint32_t triggering_cpu, long set, long way, champsim::address full_addr, champsim::address ip, champsim::address victim_addr,
                                access_type type, uint8_t hit);
};

#endif
