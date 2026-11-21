#include "tage.h"

#include <algorithm>
#include <cstdio>
#include <limits>

namespace
{
constexpr uint8_t ACTION_EXPLORE_TRIALS = 2;
constexpr uint8_t COUNTER_MAX = 31;
} // namespace

uint64_t tage::mix64(uint64_t value)
{
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

void tage::prefetcher_initialize()
{
  for (std::size_t level = 0; level < LEVELS; ++level)
    tables[level].resize(TABLE_SIZES[level]);
}

tage::context tage::make_context(champsim::address addr, champsim::address ip) const
{
  const uint64_t block = champsim::block_number{addr}.to<uint64_t>();
  const uint64_t pc = ip.to<uint64_t>();
  const uint64_t base = mix64(pc ^ mix64(block));

  context result;
  for (std::size_t level = 0; level < LEVELS; ++level) {
    auto folded = base;
    if (level > 0) {
      const auto history_length = HISTORY_LENGTHS[level];
      for (std::size_t i = 0; i < history_length && i < access_history.size(); ++i)
        folded = mix64(folded ^ mix64(access_history[i] + 0x9e3779b97f4a7c15ULL * (i + 1)));
    }

    const uint64_t indexed = mix64(folded ^ (0x243f6a8885a308d3ULL * (level + 1)));
    const uint64_t tagged = mix64(folded ^ pc ^ (0x13198a2e03707344ULL * (level + 1)));
    result.index[level] = static_cast<uint32_t>(indexed & (TABLE_SIZES[level] - 1));
    result.tag[level] = static_cast<uint16_t>(tagged >> 48);
    result.tie_break ^= mix64(indexed + tagged);
  }
  return result;
}

int tage::lookup_provider(const context& ctx) const
{
  for (std::size_t level = LEVELS; level-- > 0;) {
    const auto& row = tables[level][ctx.index[level]];
    if (row.valid && row.tag == ctx.tag[level])
      return static_cast<int>(level);
  }
  return -1;
}

void tage::allocate(unsigned level, const context& ctx, const entry* seed)
{
  auto& row = tables[level][ctx.index[level]];
  row = {};
  row.valid = true;
  row.tag = ctx.tag[level];
  if (seed != nullptr) {
    row.trials = seed->trials;
    row.hits = seed->hits;
    row.usefulness = seed->usefulness;
  }
}

uint8_t tage::choose_action(const context& ctx)
{
  int provider = lookup_provider(ctx);
  if (provider < 0) {
    allocate(0, ctx);
    provider = 0;
  }

  const auto& row = tables[provider][ctx.index[provider]];
  const auto start = static_cast<std::size_t>(ctx.tie_break % ACTIONS);
  uint8_t least_tried = std::numeric_limits<uint8_t>::max();
  uint8_t explore = static_cast<uint8_t>(start);
  for (std::size_t n = 0; n < ACTIONS; ++n) {
    const auto action_id = (start + n) % ACTIONS;
    if (row.trials[action_id] < least_tried) {
      least_tried = row.trials[action_id];
      explore = static_cast<uint8_t>(action_id);
    }
  }
  if (least_tried < ACTION_EXPLORE_TRIALS)
    return explore;

  uint8_t best = static_cast<uint8_t>(start);
  double best_rate = -1.0;
  for (std::size_t n = 0; n < ACTIONS; ++n) {
    const auto action_id = (start + n) % ACTIONS;
    const double rate = (row.hits[action_id] + 1.0) / (row.trials[action_id] + 2.0);
    if (rate > best_rate) {
      best = static_cast<uint8_t>(action_id);
      best_rate = rate;
    }
  }
  return best;
}

void tage::train(const token& training, bool hit)
{
  const auto provider = training.ctx.provider;
  if (provider < 0 || provider >= static_cast<int>(LEVELS))
    return;

  auto& row = tables[provider][training.ctx.index[provider]];
  if (!row.valid || row.tag != training.ctx.tag[provider])
    return; // the context was replaced before its feedback arrived

  const auto action_id = training.action_id;
  if (row.trials[action_id] >= COUNTER_MAX) {
    for (std::size_t i = 0; i < ACTIONS; ++i) {
      row.trials[i] = static_cast<uint8_t>((row.trials[i] + 1) / 2);
      row.hits[i] = static_cast<uint8_t>((row.hits[i] + 1) / 2);
    }
  }
  ++row.trials[action_id];
  if (hit) {
    ++row.hits[action_id];
    row.usefulness = std::min<uint8_t>(3, row.usefulness + 1);
    ++useful;
    ++action_hits[action_id];
  } else {
    if (row.usefulness > 0)
      --row.usefulness;
    ++unused;
  }
  ++action_trials[action_id];

  // A no-hit outcome is a TAGE-style allocation opportunity: keep the current
  // predictor as a fallback and copy its experience into a longer-history row.
  if (!hit && provider + 1 < static_cast<int>(LEVELS)) {
    const auto next = static_cast<unsigned>(provider + 1);
    const auto& longer = tables[next][training.ctx.index[next]];
    if (!longer.valid || longer.tag != training.ctx.tag[next])
      allocate(next, training.ctx, &row);
  }
}

void tage::update_stride(champsim::address addr, champsim::address ip,
                         champsim::block_number::difference_type* stride,
                         uint8_t* confidence)
{
  const uint64_t pc = ip.to<uint64_t>();
  const auto index = static_cast<std::size_t>(mix64(pc) & (STRIDE_SETS - 1));
  auto& row = strides[index];
  const champsim::block_number block{addr};
  *stride = 0;
  *confidence = 0;

  if (!row.valid || row.ip != pc) {
    row = {true, pc, block, 0, 0};
    return;
  }

  const auto delta = champsim::offset(row.last_addr, block);
  if (delta != 0) {
    if (delta == row.stride)
      row.confidence = std::min<uint8_t>(3, row.confidence + 1);
    else {
      row.stride = delta;
      row.confidence = 0;
    }
    row.last_addr = block;
  }
  *stride = row.stride;
  *confidence = row.confidence;
}

void tage::bound_map(std::unordered_map<uint64_t, token>& map)
{
  if (map.size() >= TRACK_MAX)
    map.erase(map.begin());
}

uint32_t tage::prefetcher_cache_operate(champsim::address addr, champsim::address ip,
                                        uint8_t cache_hit, bool useful_prefetch,
                                        access_type type, uint32_t metadata_in)
{
  if (type != access_type::LOAD && type != access_type::RFO)
    return metadata_in;

  ++accesses;
  const auto block = champsim::block_number{addr};
  const auto block_id = block.to<uint64_t>();

  if (useful_prefetch) {
    if (auto it = resident.find(block_id); it != resident.end()) {
      train(it->second, true);
      resident.erase(it);
    }
  }

  if (!cache_hit) {
    if (auto it = inflight.find(block_id); it != inflight.end()) {
      train(it->second, false); // the prefetch arrived too late to be a hit
      inflight.erase(it);
    }
  }

  champsim::block_number::difference_type learned_stride = 0;
  uint8_t stride_confidence = 0;
  update_stride(addr, ip, &learned_stride, &stride_confidence);

  // Tagged history is consulted on demand misses and on first use of a
  // prefetched line, so streams can continue after prefetching removes misses.
  if (!cache_hit || useful_prefetch) {
    auto ctx = make_context(addr, ip);
    const auto action_id = choose_action(ctx);
    ctx.provider = lookup_provider(ctx);
    token training{ctx, action_id};
    const auto selected = ACTION_SET[action_id];

    auto stride = champsim::block_number::difference_type{1};
    if (selected.use_stride && stride_confidence > 0 && learned_stride != 0)
      stride = learned_stride;

    for (int i = 0; i < selected.degree; ++i) {
      const auto steps = static_cast<champsim::block_number::difference_type>(selected.distance + i);
      const champsim::address pf{block + stride * steps};
      if (!intern_->virtual_prefetch && champsim::page_number{pf} != champsim::page_number{addr})
        break;

      const auto candidate_id = champsim::block_number{pf}.to<uint64_t>();
      if (inflight.count(candidate_id) || resident.count(candidate_id))
        continue;
      if (pending.size() >= PENDING_MAX)
        pending.pop_front();
      pending.push_back({pf, training});
    }
  }

  access_history.push_front(block_id);
  while (access_history.size() > HISTORY_MAX)
    access_history.pop_back();
  return metadata_in;
}

uint32_t tage::prefetcher_cache_fill(champsim::address addr, long, long, uint8_t prefetch,
                                     champsim::address evicted_addr, uint32_t metadata_in)
{
  if (evicted_addr != champsim::address{}) {
    const auto evicted = champsim::block_number{evicted_addr}.to<uint64_t>();
    if (auto it = resident.find(evicted); it != resident.end()) {
      train(it->second, false);
      resident.erase(it);
    }
  }

  if (prefetch) {
    const auto block_id = champsim::block_number{addr}.to<uint64_t>();
    if (auto it = inflight.find(block_id); it != inflight.end()) {
      bound_map(resident);
      resident[block_id] = it->second;
      inflight.erase(it);
    }
  }
  return metadata_in;
}

void tage::prefetcher_cycle_operate()
{
  if (pending.empty())
    return;

  const auto next = pending.front();
  if (!intern_->prefetch_line(next.addr, true, 0))
    return;

  pending.pop_front();
  const auto block_id = champsim::block_number{next.addr}.to<uint64_t>();
  bound_map(inflight);
  inflight[block_id] = next.training;
  ++issued;
}

void tage::prefetcher_final_stats()
{
  const auto outcomes = useful + unused;
  const double hit_rate = outcomes ? static_cast<double>(useful) / outcomes : 0.0;
  std::printf("TAGE prefetcher: accesses=%llu issued=%llu useful_hits=%llu unused_or_late=%llu hit_rate=%.3f\n",
              static_cast<unsigned long long>(accesses), static_cast<unsigned long long>(issued),
              static_cast<unsigned long long>(useful), static_cast<unsigned long long>(unused), hit_rate);
  for (std::size_t i = 0; i < ACTIONS; ++i) {
    const double rate = action_trials[i] ? static_cast<double>(action_hits[i]) / action_trials[i] : 0.0;
    std::printf("TAGE action %zu: trials=%llu hits=%llu hit_rate=%.3f\n", i,
                static_cast<unsigned long long>(action_trials[i]),
                static_cast<unsigned long long>(action_hits[i]), rate);
  }
}
