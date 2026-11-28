#ifndef WARM_START_REGION_LOOKUP_TABLE_H
#define WARM_START_REGION_LOOKUP_TABLE_H

#include <cstdint>
#include <unordered_map>

namespace warm_start
{
using RegionId = std::uint64_t;

struct L1iSettings
{
    int degree = 1;
    int distance = 1;
};

struct InstructionPrefetcherSettings
{
    bool enabled = true;
    int confidence_threshold = 1;
    int destination_cap = 6;
};

struct DataPrefetcherSettings
{
    bool enabled = true;
    int max_requests_per_target = 16;
    int l1_fill_threshold_percent = 50;
};

struct RegionEntry
{
    L1iSettings l1i;
    InstructionPrefetcherSettings instruction = {};
    DataPrefetcherSettings data = {};
};

class RegionLookupTable
{
  public:
    // Use the hint on the first visit. Later visits retrieve the saved
    // settings.
    RegionEntry &
    enter(RegionId region, L1iSettings hint = {})
    {
        return enter_region(region, RegionEntry{hint});
    }

    RegionEntry &
    enter_region(RegionId region, RegionEntry initial = {})
    {
        return entries_.emplace(region, initial).first->second;
    }

  private:
    std::unordered_map<RegionId, RegionEntry> entries_;
};
} // namespace warm_start

#endif
