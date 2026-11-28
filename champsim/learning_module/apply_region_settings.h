#ifndef WARM_START_APPLY_REGION_SETTINGS_H
#define WARM_START_APPLY_REGION_SETTINGS_H

#include "region_lookup_table.h"

namespace warm_start
{
template <typename InstructionPrefetcher, typename DataPrefetcher>
inline void
apply_region_settings(RegionLookupTable &table, RegionId region,
                      const RegionEntry &initial,
                      InstructionPrefetcher &instruction_prefetcher,
                      DataPrefetcher &data_prefetcher)
{
    const auto &settings = table.enter_region(region, initial);

    instruction_prefetcher.set_knobs({
        settings.instruction.enabled,
        settings.instruction.confidence_threshold,
        settings.instruction.destination_cap
    });

    data_prefetcher.set_knobs({
        settings.data.enabled,
        settings.data.max_requests_per_target,
        settings.data.l1_fill_threshold_percent
    });
}
} // namespace warm_start

#endif