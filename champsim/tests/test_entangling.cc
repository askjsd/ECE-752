// Exercise the port's actual prediction state, including boundary cases that
// normal small kernels do not reliably cover. Reference algorithm is CC0-1.0.
#include <iostream>
#include <memory>
#include <stdexcept>
#include "../modules/prefetcher/entangling/entangling.cc"

const std::size_t NUM_CPUS = 1;
const unsigned BLOCK_SIZE = 64, PAGE_SIZE = 4096, LOG2_BLOCK_SIZE = 6,
               LOG2_PAGE_SIZE = 12;

static void
require(bool value, const char *message)
{
    if (!value) {
        throw std::runtime_error(message);
    }
}

struct entangling_test_access
{
    static void
    run()
    {
        champsim::channel lower;
        CACHE cache{champsim::cache_builder{}
                        .name("test-L1I")
                        .sets(64)
                        .ways(8)
                        .pq_size(8)
                        .mshr_size(4)
                        .hit_latency(2)
                        .fill_latency(1)
                        .tag_bandwidth(champsim::bandwidth::maximum_type{2})
                        .fill_bandwidth(champsim::bandwidth::maximum_type{2})
                        .lower_level(&lower)};
        cache.virtual_prefetch = true;
        auto s = std::make_unique<entangling::state>(&cache);
        s->initialize();

        require(s->l1i_get_latency(5, 4090) == 11, "12-bit latency wrap");
        cache.current_time =
            champsim::chrono::clock::time_point{cache.clock_period * 10};
        s->l1i_add_hist_table(0x100);
        cache.current_time =
            champsim::chrono::clock::time_point{cache.clock_period * 100};
        auto pos = s->l1i_add_hist_table(0x300);
        require(s->l1i_get_src_entangled_hist_table(0x300, pos, 50) == 0x100,
                "timely source selection");
        require(s->l1i_get_src_entangled_hist_table(0x300, pos, 100) == 0,
                "reject source without enough lead time");
        s->l1i_add_bb_size_hist_table(
            0x99999, 3); // Missing history must not write entry 16.
        std::cout
            << "ok history lead time, wrap, and missing-entry boundary\n";

        constexpr uint64_t source = 0x1234567800ULL;
        constexpr uint64_t target = source + 7;
        s->l1i_add_entangled_table(source, target);
        uint32_t set = 0, way = 0;
        require(s->l1i_get_entangled_addr_entangled_table(source, 0, set,
                                                          way) == target,
                "compressed destination round trip");
        s->l1i_update_confidence_entangled_table(set, way, target, false);
        s->l1i_update_confidence_entangled_table(set, way, target, true);
        require(s->l1i_entangled_table[0][set][way].entangled_conf[0] == 3,
                "timely confidence saturates");
        for (int i = 0; i < 3; ++i) {
            s->l1i_update_confidence_entangled_table(set, way, target, false);
        }
        require(s->l1i_get_entangled_addr_entangled_table(source, 0, set,
                                                          way) == 0,
                "late or unused confidence invalidates");
        for (uint64_t i = 1; i <= 7; ++i) {
            s->l1i_add_entangled_table(source, source + i);
        }
        unsigned destinations = 0;
        for (unsigned i = 0; i < 6; ++i) {
            destinations += s->l1i_get_entangled_addr_entangled_table(
                                source, i, set, way) != 0;
        }
        require(destinations == 6,
                "compressed entry retains six nearby destinations");
        require(s->l1i_stats_evict_entangled_k_table > 0,
                "seventh destination evicts a low-confidence association");
        std::cout << "ok destination compression, capacity, and confidence "
                     "feedback\n";

        s->l1i_add_bbsize_table(source, 5);
        require(s->l1i_get_bbsize_entangled_table(source) == 5,
                "region stores five lines after its head");
        s->fill_way = 3;
        s->l1i_move_timing_entry(0x521);
        require(s->l1i_find_timing_cache_entry(0x521) == 3,
                "shadow uses actual cache victim way");
        uint32_t feedback_set = 0, feedback_way = 16;
        require(s->l1i_invalid_timing_cache_entry(0x521, feedback_set,
                                                  feedback_way),
                "demand fill marked accessed");
        require(s->l1i_find_timing_cache_entry(0x521) == cache.NUM_WAY,
                "eviction removes shadow entry");
        std::cout
            << "ok region size, fill-way synchronization, and eviction\n";

        s->l1i_add_timing_entry(0x600, 0, 16);
        s->synchronize_requests();
        require(!s->l1i_ongoing_request(0x600),
                "consumed queue request cannot remain a phantom miss");
        require(s->prefetch_code_line(0x700 << 6), "accept a new prefetch");
        s->l1i_add_timing_entry(0x700, 0, 16);
        s->synchronize_requests();
        require(s->l1i_ongoing_request(0x700),
                "retain genuinely queued prefetch");
        require(!s->prefetch_code_line(0x700 << 6),
                "suppress a duplicate queued prefetch");
        for (uint64_t i = 0; i < s->timing_mshr_capacity() + 1; ++i) {
            s->l1i_add_timing_entry(0x800 + i, 0, 16);
        }
        require(s->l1i_stats_discarded_prefetches > 0,
                "bounded timing table handles pressure without an "
                "out-of-bounds write");
        std::cout << "ok request admission, duplicate filtering, and bounded "
                     "timing state\n";

        auto independent = std::make_unique<entangling::state>(&cache);
        independent->initialize();
        require(independent->l1i_get_entangled_addr_entangled_table(
                    source, 0, set, way) == 0,
                "cache instances do not share predictor state");
        std::cout << "ok independent predictor instances\n";
    }
};

int
main()
{
    try {
        entangling_test_access::run();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
