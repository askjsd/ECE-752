// Exercise both registered modules in one uninterrupted single-core
// simulation.
#include <algorithm>
#include <iostream>
#include <stdexcept>

#include "../modules/prefetcher/edp/edp.h"
#include "../modules/prefetcher/entangling/entangling.h"

#include "core_inst.inc"

#include "event_listeners.h"
#include "phase_info.h"
#include "tracereader.h"

namespace champsim
{
phase_stats do_phase(const phase_info &, environment &,
                     std::vector<tracereader> &, chrono::clock &);
}

using configured_environment =
    champsim::configured::generated_environment<CHAMPSIM_BUILD>;
const std::size_t NUM_CPUS = configured_environment::num_cpus;
const unsigned BLOCK_SIZE = configured_environment::block_size;
const unsigned PAGE_SIZE = configured_environment::page_size;
const unsigned LOG2_BLOCK_SIZE = champsim::lg2(BLOCK_SIZE);
const unsigned LOG2_PAGE_SIZE = champsim::lg2(PAGE_SIZE);

namespace
{
void
require(bool passed, const char *message)
{
    if (!passed) {
        throw std::runtime_error(message);
    }
}

// One shared load walks an array. Between loads, the core visits 1,024
// two-line code regions in a fixed order that exceeds the instruction cache.
struct code_and_data_trace
{
    uint64_t position = 0;
    ooo_model_instr
    operator()()
    {
        constexpr uint64_t load_pc = 0x502104;
        const auto visit = position / 33;
        const auto slot = position++ % 33;
        input_instr raw{};
        if (slot == 0) {
            raw.ip = load_pc;
            raw.source_memory[0] = 0x802000 + visit * 64;
        } else {
            raw.ip = 0x100000 + (visit * 37 % 1024) * 128 + (slot - 1) * 4;
            if (slot == 32) {
                raw.is_branch = raw.branch_taken = 1;
                raw.destination_registers[0] =
                    champsim::REG_INSTRUCTION_POINTER;
            }
        }
        ooo_model_instr instr{0, raw};
        if (raw.branch_taken) {
            instr.branch_target = champsim::address{load_pc};
        }
        return instr;
    }
};

const cache_stats &
check_cache(const champsim::phase_stats &stats, const char *name, bool enabled)
{
    const auto &caches = stats.roi_cache_stats;
    auto cache =
        std::find_if(caches.begin(), caches.end(),
                     [name](const auto &item) { return item.name == name; });
    require(cache != caches.end(), "Missing cache statistics");
    std::cout << stats.name << ' ' << name << " requested "
              << cache->pf_requested << ", issued " << cache->pf_issued
              << '\n';
    require(enabled ? cache->pf_issued > 0 : cache->pf_requested == 0,
            "Prefetch activity did not match the requested setting");
    return *cache;
}

void
run_test()
{
    configured_environment env{};
    for (champsim::operable &part : env.operable_view()) {
        part.initialize();
    }

    auto *code = entangling::find("cpu0_L1I");
    auto *data = edp::find("cpu0_L1D");
    require(code && data, "Could not find both Entangling prefetchers");
    const auto code_defaults = code->get_knobs();
    const auto data_defaults = data->get_knobs();
    require(code_defaults.enabled && code_defaults.confidence_threshold == 1 &&
                code_defaults.destination_cap == 6,
            "Wrong instruction defaults");
    require(data_defaults.enabled &&
                data_defaults.max_requests_per_target == 16 &&
                data_defaults.l1_fill_threshold_percent == 50,
            "Wrong data defaults");

    std::vector<champsim::tracereader> traces;
    traces.emplace_back(code_and_data_trace{});
    champsim::chrono::clock clock;
    auto run_phase = [&](const char *name, bool warmup, long long length) {
        handle_event<Event::BEGIN_PHASE>(warmup);
        return champsim::do_phase({name, warmup, length, {0}, {"generated"}},
                                  env, traces, clock);
    };
    run_phase("Warmup", true, 32000);
    auto measure = [&](const char *name, bool code_on, bool data_on) {
        constexpr long long length = 65000;
        auto stats = run_phase(name, false, length);
        require(stats.roi_cpu_stats.at(0).instrs() >= length,
                "Phase ended early");
        check_cache(stats, "cpu0_L1I", code_on);
        return check_cache(stats, "cpu0_L1D", data_on).pf_fill;
    };
    measure("Defaults", true, true);

    code->set_knobs({false, 4, 0});
    const auto strict = code->get_knobs();
    require(!strict.enabled && strict.confidence_threshold == 3 &&
                strict.destination_cap == 1,
            "Instruction limits or readback failed");
    measure("Data only", false, true);

    data->set_knobs({false, 0, 101});
    const auto limited = data->get_knobs();
    require(!limited.enabled && limited.max_requests_per_target == 1 &&
                limited.l1_fill_threshold_percent == 100,
            "Data limits or readback failed");
    measure("Both off", false, false);

    code->set_knobs({true, 0, 7});
    const auto restored = code->get_knobs();
    require(restored.enabled && restored.confidence_threshold == 1 &&
                restored.destination_cap == 6,
            "Instruction restore failed");
    measure("Instruction only", true, false);

    data->set_knobs({true, 17, -1});
    const auto lower = data->get_knobs();
    require(lower.enabled && lower.max_requests_per_target == 16 &&
                lower.l1_fill_threshold_percent == 0,
            "Data limits or re-enable failed");
    require(
        measure("Both on, data to L2", true, true) == 0,
        "A data prefetch filled L1 while all requests were directed to L2");
    code->set_knobs({true, 3, 1});
    data->set_knobs({true, 1, 50});
    const auto limited_code = code->get_knobs();
    const auto limited_data = data->get_knobs();
    require(limited_code.confidence_threshold == 3 &&
                limited_code.destination_cap == 1 &&
                limited_data.max_requests_per_target == 1 &&
                limited_data.l1_fill_threshold_percent == 50,
            "In-range settings did not take effect on enabled prefetchers");
    measure("Limited predictions", true, true);
    code->set_knobs(code_defaults);
    data->set_knobs(data_defaults);
    measure("Restored defaults", true, true);
}
} // namespace

int
main()
{
    try {
        run_test();
        require(!entangling::find("cpu0_L1I") && !edp::find("cpu0_L1D"),
                "Destroyed prefetcher still registered");
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
