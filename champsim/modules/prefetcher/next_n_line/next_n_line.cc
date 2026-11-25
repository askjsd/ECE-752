#include "next_n_line.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <fmt/core.h>

#include "cache.h"

namespace
{
// Every next_n_line in the simulation, keyed by the name of the cache it is attached to.
// Filled in prefetcher_initialize(); used by next_n_line::find().
std::map<std::string, next_n_line*, std::less<>>& instances()
{
  static std::map<std::string, next_n_line*, std::less<>> by_cache_name;
  return by_cache_name;
}

// Read an integer knob from the environment. Returns `fallback` if the
// variable is unset or is not a whole number.
int knob_from_env(const char* name, int fallback)
{
  const char* text = std::getenv(name);
  if (text == nullptr || *text == '\0') {
    return fallback;
  }

  char* end = nullptr;
  long value = std::strtol(text, &end, 10);
  if (*end != '\0') {
    fmt::print("[next_n_line] ignoring {}={} (not a whole number)\n", name, text);
    return fallback;
  }
  return static_cast<int>(value);
}
} // namespace

next_n_line* next_n_line::find(std::string_view cache_name)
{
  auto& by_name = instances();
  auto found = by_name.find(cache_name);
  return found == by_name.end() ? nullptr : found->second;
}

void next_n_line::set_knobs(int new_degree, int new_distance)
{
  degree = std::clamp(new_degree, 0, MAX_DEGREE);
  distance = std::clamp(new_distance, 1, MAX_DISTANCE);
}

void next_n_line::prefetcher_initialize()
{
  // Registered here rather than in the constructor: by now the cache has been
  // built in its final place, so `this` and intern_ stay valid for the whole run.
  instances()[intern_->NAME] = this;

  set_knobs(knob_from_env("NNL_DEGREE", degree), knob_from_env("NNL_DISTANCE", distance));
  fmt::print("{} next_n_line: degree {} distance {}\n", intern_->NAME, degree, distance);
}

uint32_t next_n_line::prefetcher_cache_operate(champsim::address addr, champsim::address /*ip*/, uint8_t /*cache_hit*/, bool /*useful_prefetch*/,
                                               access_type /*type*/, uint32_t metadata_in)
{
  champsim::block_number line{addr}; // byte address -> cache-line number

  for (int i = 0; i < degree; ++i) {
    // prefetch_line returns false when the prefetch queue is full;
    // the remaining requests for this access would be dropped too, so stop.
    if (!prefetch_line(champsim::address{line + (distance + i)}, true, metadata_in)) {
      break;
    }
  }

  return metadata_in;
}

uint32_t next_n_line::prefetcher_cache_fill(champsim::address /*addr*/, long /*set*/, long /*way*/, uint8_t /*prefetch*/,
                                            champsim::address /*evicted_addr*/, uint32_t metadata_in)
{
  return metadata_in;
}

void next_n_line::prefetcher_final_stats() { fmt::print("{} next_n_line final knobs: degree {} distance {}\n", intern_->NAME, degree, distance); }
