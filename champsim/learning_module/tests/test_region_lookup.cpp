#include <cassert>

#include "../region_lookup_table.h"

int
main()
{
    warm_start::RegionLookupTable table;

    // First visit to region A uses its compiler hint.
    auto &region_a = table.enter(0x1000, {2, 4});
    assert(region_a.l1i.degree == 2);
    assert(region_a.l1i.distance == 4);

    // Pretend the learner found a better setting for region A.
    region_a.l1i = {3, 8};

    // Region B has no hint, so it starts with the defaults.
    const auto &region_b = table.enter(0x2000);
    assert(region_b.l1i.degree == 1);
    assert(region_b.l1i.distance == 1);

    // Returning to A retrieves the learned settings.
    const auto &revisited = table.enter(0x1000, {2, 4});
    assert(revisited.l1i.degree == 3);
    assert(revisited.l1i.distance == 8);

    warm_start::RegionEntry hint;
    hint.instruction = {false, 3, 2};
    hint.data = {true, 4, 25};
    auto &region_c = table.enter_region(0x3000, hint);
    assert(!region_c.instruction.enabled);
    assert(region_c.instruction.confidence_threshold == 3);
    assert(region_c.instruction.destination_cap == 2);
    assert(region_c.data.enabled);
    assert(region_c.data.max_requests_per_target == 4);
    assert(region_c.data.l1_fill_threshold_percent == 25);

    region_c.instruction = {true, 2, 5};
    region_c.data = {false, 8, 0};

    const auto &defaults = table.enter_region(0x4000);
    assert(defaults.instruction.enabled);
    assert(defaults.instruction.confidence_threshold == 1);
    assert(defaults.instruction.destination_cap == 6);
    assert(defaults.data.enabled);
    assert(defaults.data.max_requests_per_target == 16);
    assert(defaults.data.l1_fill_threshold_percent == 50);

    const auto &saved = table.enter_region(0x3000, hint);
    assert(saved.instruction.enabled);
    assert(saved.instruction.confidence_threshold == 2);
    assert(saved.instruction.destination_cap == 5);
    assert(!saved.data.enabled);
    assert(saved.data.max_requests_per_target == 8);
    assert(saved.data.l1_fill_threshold_percent == 0);
}
