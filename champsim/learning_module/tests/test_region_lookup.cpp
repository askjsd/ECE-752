#include <cassert>

#include "region_lookup_table.h"

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
}