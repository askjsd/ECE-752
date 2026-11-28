#include <iostream>
#include <stdexcept>

#include "../apply_region_settings.h"
#include "../../modules/prefetcher/edp/edp.h"
#include "../../modules/prefetcher/entangling/entangling.h"

#include "core_inst.inc"

using configured_environment =
    champsim::configured::generated_environment<CHAMPSIM_BUILD>;
const std::size_t NUM_CPUS = configured_environment::num_cpus;
const unsigned BLOCK_SIZE = configured_environment::block_size;
const unsigned PAGE_SIZE = configured_environment::page_size;
const unsigned LOG2_BLOCK_SIZE = champsim::lg2(BLOCK_SIZE);
const unsigned LOG2_PAGE_SIZE = champsim::lg2(PAGE_SIZE);

int main()
{
    configured_environment env{};
    for (champsim::operable &part : env.operable_view()) {
        part.initialize();
    }

    auto *instruction = entangling::find("cpu0_L1I");
    auto *data = edp::find("cpu0_L1D");
    if (!instruction || !data) {
        std::cerr << "Expected both prefetchers in the ChampSim configuration\n";
        return 1;
    }

    warm_start::RegionLookupTable table;
    constexpr warm_start::RegionId region_a = 0x1000;
    constexpr warm_start::RegionId region_b = 0x2000;

    auto check_region = [&](warm_start::RegionId region,
                            const warm_start::RegionEntry &initial,
                            const warm_start::RegionEntry &expected) {
        warm_start::apply_region_settings(table, region, initial, *instruction,
                                          *data);
        const auto i = instruction->get_knobs();
        const auto d = data->get_knobs();
        if (i.enabled != expected.instruction.enabled ||
            i.confidence_threshold != expected.instruction.confidence_threshold ||
            i.destination_cap != expected.instruction.destination_cap ||
            d.enabled != expected.data.enabled ||
            d.max_requests_per_target != expected.data.max_requests_per_target ||
            d.l1_fill_threshold_percent != expected.data.l1_fill_threshold_percent) {
            throw std::runtime_error("Prefetcher settings did not match");
        }
    };

    try {
        warm_start::RegionEntry hint;
        hint.instruction = {true, 2, 4};
        hint.data = {true, 4, 25};
        check_region(region_a, hint, hint);

        auto &saved = table.enter_region(region_a);
        saved.instruction = {false, 3, 1};
        saved.data = {false, 8, 0};

        check_region(region_b, {}, {});
        check_region(region_a, hint, saved);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    std::cout << "Live prefetcher region settings passed\n";
}
