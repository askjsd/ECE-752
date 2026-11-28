#include <cassert>

#include "../apply_region_settings.h"

namespace
{
template <typename Settings>
struct FakePrefetcher
{
    Settings active{};

    void set_knobs(Settings settings)
    {
        active = settings;
    }
};
}

int main()
{
    warm_start::RegionLookupTable table;
    FakePrefetcher<warm_start::InstructionPrefetcherSettings> instruction;
    FakePrefetcher<warm_start::DataPrefetcherSettings> data;

    constexpr warm_start::RegionId region_a = 0x1000;
    constexpr warm_start::RegionId region_b = 0x2000;

    warm_start::RegionEntry hint_a;
    hint_a.instruction = {true, 2, 4};
    hint_a.data = {true, 4, 25};

    // First visit to A applies its hint to both prefetchers.
    warm_start::apply_region_settings(table, region_a, hint_a, instruction, data);
    assert(instruction.active.confidence_threshold == 2);
    assert(instruction.active.destination_cap == 4);
    assert(data.active.max_requests_per_target == 4);
    assert(data.active.l1_fill_threshold_percent == 25);

    // Pretend the learner found better settings for A.
    auto &saved_a = table.enter_region(region_a);
    saved_a.instruction = {false, 3, 1};
    saved_a.data = {false, 8, 0};

    // B starts with the defaults.
    warm_start::apply_region_settings(table, region_b, {}, instruction, data);
    assert(instruction.active.enabled);
    assert(instruction.active.confidence_threshold == 1);
    assert(instruction.active.destination_cap == 6);
    assert(data.active.enabled);
    assert(data.active.max_requests_per_target == 16);
    assert(data.active.l1_fill_threshold_percent == 50);

    // Returning to A applies the saved settings even with the old hint.
    warm_start::apply_region_settings(table, region_a, hint_a, instruction, data);
    assert(!instruction.active.enabled);
    assert(instruction.active.confidence_threshold == 3);
    assert(instruction.active.destination_cap == 1);
    assert(!data.active.enabled);
    assert(data.active.max_requests_per_target == 8);
    assert(data.active.l1_fill_threshold_percent == 0);
}