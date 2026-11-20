#ifndef PREFETCHER_ENTANGLING_H
#define PREFETCHER_ENTANGLING_H
#include <cstdint>
#include <memory>

#include "address.h"
#include "modules.h"

// ISCA 2021, 4K-entry version. Each L1I owns independent prediction state.
struct entangling : champsim::modules::prefetcher {
  explicit entangling(CACHE*);
  ~entangling();
  entangling(entangling&&) noexcept;
  entangling& operator=(entangling&&) noexcept;
  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address, champsim::address, bool, bool, access_type, uint32_t);
  uint32_t prefetcher_cache_fill(champsim::address, long, long, bool, champsim::address, uint32_t);
  void prefetcher_cycle_operate();
  void prefetcher_final_stats();

private:
  struct state;
  std::unique_ptr<state> impl;
  friend struct entangling_test_access;
};
#endif
