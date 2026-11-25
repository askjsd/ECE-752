#ifndef PREFETCHER_NEXT_N_LINE_H
#define PREFETCHER_NEXT_N_LINE_H

#include <cstdint>
#include <string_view>

#include "address.h"
#include "modules.h"

// Next-N-line prefetcher with two tunable knobs.
//
// On every access to cache line L it requests lines
//     L + distance, L + distance + 1, ..., L + distance + degree - 1
//
//   degree   : how many lines to prefetch per access (0 turns the prefetcher off), 0..16, default 1
//   distance : how far ahead the first prefetched line is (1 = the very next line), 1..64, default 1
//
// degree = 1, distance = 1 behaves exactly like the stock next_line prefetcher.
//
// The knobs can be changed while the program runs, by hints or the learning module:
//
//   next_n_line* pf = next_n_line::find("cpu0_L1I"); // nullptr if that cache has no next_n_line
//   int d = pf->get_degree();                         // read
//   pf->set_knobs(2, 1);                              // write (degree, distance), clamped to the valid range
//
// find() works once the simulator has initialized the caches, i.e. from the start of warmup on.
// A new value takes effect on the cache's next access.
// Starting values can be set without recompiling through the environment
// variables NNL_DEGREE and NNL_DISTANCE.
struct next_n_line : public champsim::modules::prefetcher {
  constexpr static int MAX_DEGREE = 16;
  constexpr static int MAX_DISTANCE = 64;

  using prefetcher::prefetcher;

  // The next_n_line attached to the cache with this name (e.g. "cpu0_L1I"), or nullptr.
  static next_n_line* find(std::string_view cache_name);

  int get_degree() const { return degree; }
  int get_distance() const { return distance; }

  // Change the knobs; values are clamped to the supported range.
  void set_knobs(int new_degree, int new_distance);

  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                    uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);
  void prefetcher_final_stats();

private:
  // Private so every change goes through set_knobs() and stays in range.
  int degree = 1;
  int distance = 1;
};

#endif
