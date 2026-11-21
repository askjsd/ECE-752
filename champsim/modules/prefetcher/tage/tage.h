#ifndef TAGE_PREFETCHER_H
#define TAGE_PREFETCHER_H

#include <array>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include "address.h"
#include "champsim.h"
#include "modules.h"

struct tage : public champsim::modules::prefetcher
{
  static constexpr std::size_t LEVELS = 5;
  static constexpr std::size_t ACTIONS = 8;
  static constexpr std::size_t STRIDE_SETS = 1024;
  static constexpr std::size_t TRACK_MAX = 8192;
  static constexpr std::size_t PENDING_MAX = 64;
  static constexpr std::size_t HISTORY_MAX = 16;

  struct action
  {
    bool use_stride;
    uint8_t distance;
    uint8_t degree;
  };

  struct entry
  {
    bool valid = false;
    uint16_t tag = 0;
    uint8_t usefulness = 0;
    std::array<uint8_t, ACTIONS> trials{};
    std::array<uint8_t, ACTIONS> hits{};
  };

  struct context
  {
    std::array<uint32_t, LEVELS> index{};
    std::array<uint16_t, LEVELS> tag{};
    int provider = -1;
    uint64_t tie_break = 0;
  };

  struct token
  {
    context ctx;
    uint8_t action_id = 0;
  };

  struct stride_entry
  {
    bool valid = false;
    uint64_t ip = 0;
    champsim::block_number last_addr{};
    champsim::block_number::difference_type stride = 0;
    uint8_t confidence = 0;
  };

  struct candidate
  {
    champsim::address addr;
    token training;
  };

  using champsim::modules::prefetcher::prefetcher;

  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip,
                                   uint8_t cache_hit, bool useful_prefetch,
                                   access_type type, uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way,
                                 uint8_t prefetch, champsim::address evicted_addr,
                                 uint32_t metadata_in);
  void prefetcher_cycle_operate();
  void prefetcher_final_stats();

private:
  static constexpr std::array<std::size_t, LEVELS> TABLE_SIZES = {1024, 512, 512, 256, 256};
  static constexpr std::array<uint8_t, LEVELS> HISTORY_LENGTHS = {0, 1, 2, 4, 8};
  static constexpr std::array<action, ACTIONS> ACTION_SET = {{{false, 1, 1},
                                                               {false, 1, 2},
                                                               {true, 1, 1},
                                                               {true, 2, 1},
                                                               {true, 4, 1},
                                                               {true, 2, 2},
                                                               {true, 4, 2},
                                                               {true, 8, 4}}};

  std::array<std::vector<entry>, LEVELS> tables;
  std::array<stride_entry, STRIDE_SETS> strides{};
  std::deque<uint64_t> access_history;
  std::deque<candidate> pending;
  std::unordered_map<uint64_t, token> inflight;
  std::unordered_map<uint64_t, token> resident;
  std::array<uint64_t, ACTIONS> action_trials{};
  std::array<uint64_t, ACTIONS> action_hits{};
  uint64_t accesses = 0;
  uint64_t issued = 0;
  uint64_t useful = 0;
  uint64_t unused = 0;

  static uint64_t mix64(uint64_t value);
  context make_context(champsim::address addr, champsim::address ip) const;
  int lookup_provider(const context& ctx) const;
  void allocate(unsigned level, const context& ctx, const entry* seed = nullptr);
  uint8_t choose_action(const context& ctx);
  void train(const token& training, bool hit);
  void update_stride(champsim::address addr, champsim::address ip,
                     champsim::block_number::difference_type* stride,
                     uint8_t* confidence);
  void bound_map(std::unordered_map<uint64_t, token>& map);
};

#endif
