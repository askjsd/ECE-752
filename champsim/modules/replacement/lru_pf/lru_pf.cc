#include "lru_pf.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>

lru_pf::lru_pf(CACHE* cache) : lru_pf(cache, cache->NUM_SET, cache->NUM_WAY) {}

lru_pf::lru_pf(CACHE* cache, long sets, long ways) : replacement(cache), NUM_WAY(ways), last_used_cycles(static_cast<std::size_t>(sets * ways), 0)
{
  if (const char* v = std::getenv("KNOBBED_INSERT")) {
    const std::string s = v;
    if (s == "lru")
      prefetch_at_lru = true;
    else if (s != "mru") {
      std::fprintf(stderr, "[lru_pf] KNOBBED_INSERT must be mru|lru, got %s\n", v);
      std::exit(1);
    }
  }
}

long lru_pf::find_victim(uint32_t triggering_cpu, uint64_t instr_id, long set, const champsim::cache_block* current_set, champsim::address ip,
                         champsim::address full_addr, access_type type)
{
  auto begin = std::next(std::begin(last_used_cycles), set * NUM_WAY);
  auto end = std::next(begin, NUM_WAY);

  // The way whose last use is most distant (a prefetch placed at LRU has stamp 0)
  auto victim = std::min_element(begin, end);
  assert(begin <= victim);
  assert(victim < end);
  return std::distance(begin, victim);
}

void lru_pf::replacement_cache_fill(uint32_t triggering_cpu, long set, long way, champsim::address full_addr, champsim::address ip,
                                    champsim::address victim_addr, access_type type)
{
  const auto idx = static_cast<std::size_t>(set * NUM_WAY + way);
  if (prefetch_at_lru && type == access_type::PREFETCH)
    last_used_cycles.at(idx) = 0; // insert at the LRU position
  else
    last_used_cycles.at(idx) = cycle++;
}

void lru_pf::update_replacement_state(uint32_t triggering_cpu, long set, long way, champsim::address full_addr, champsim::address ip,
                                      champsim::address victim_addr, access_type type, uint8_t hit)
{
  // A demand hit makes the block most-recently-used (skip writeback hits, as plain LRU does)
  if (hit && type != access_type::WRITE)
    last_used_cycles.at(static_cast<std::size_t>(set * NUM_WAY + way)) = cycle++;
}
