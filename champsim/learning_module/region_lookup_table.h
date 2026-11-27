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

struct RegionEntry
{
    L1iSettings l1i;
};

class RegionLookupTable
{
  public:
    // Use the hint on the first visit. Later visits retrieve the saved
    // settings.
    RegionEntry &
    enter(RegionId region, L1iSettings hint = {})
    {
        return entries_.try_emplace(region, RegionEntry{hint}).first->second;
    }

  private:
    std::unordered_map<RegionId, RegionEntry> entries_;
};
} // namespace warm_start

#endif