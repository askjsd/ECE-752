// Port of Alberto Ros and Alexandra Jimborean's ISCA 2021 Entangling
// prefetcher. Original algorithm: CC0-1.0; see
// champsim/reference/entangling/LICENSE.
#include <fmt/core.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <vector>

#include "../entangling_controls.h"
#include "cache.h"
#include "entangling.h"
#include "util/bits.h"

struct entangling::state
{
    static constexpr unsigned NUM_CPUS =
        1; // State is owned separately by each cache.
    CACHE *cache;
    entangling::knobs settings;
    unsigned cpu = 0;
    uint32_t fill_way = 0;
    uint64_t
    now() const
    {
        return static_cast<uint64_t>(cache->current_time.time_since_epoch() /
                                     cache->clock_period);
    }
    std::size_t
    timing_mshr_capacity() const
    {
        return cache->PQ_SIZE + cache->MSHR_SIZE + 64;
    }
    // To access cpu in my functions
    uint32_t l1i_cpu_id;

    uint64_t l1i_last_basic_block;
    uint32_t l1i_consecutive_count;
    uint32_t l1i_basic_block_merge_diff;

    bool debug = 0;
    bool all_warmed_up = false;

#define EIP_HIST_TABLE_ENTRIES 16

    // LINE AND MERGE BASIC BLOCK SIZE

#define EIP_MERGE_BBSIZE_BITS 6
#define EIP_MERGE_BBSIZE_MAX_VALUE ((1 << EIP_MERGE_BBSIZE_BITS) - 1)

    // TIME AND OVERFLOWS

#define EIP_TIME_DIFF_BITS 20
#define EIP_TIME_DIFF_OVERFLOW ((uint64_t)1 << EIP_TIME_DIFF_BITS)
#define EIP_TIME_DIFF_MASK (EIP_TIME_DIFF_OVERFLOW - 1)

#define EIP_TIME_BITS 12
#define EIP_TIME_OVERFLOW ((uint64_t)1 << EIP_TIME_BITS)
#define EIP_TIME_MASK (EIP_TIME_OVERFLOW - 1)

    uint64_t
    l1i_get_latency(uint64_t cycle, uint64_t cycle_prev)
    {
        uint64_t cycle_masked = cycle & EIP_TIME_MASK;
        uint64_t cycle_prev_masked = cycle_prev & EIP_TIME_MASK;
        if (cycle_prev_masked > cycle_masked) {
            return (cycle_masked + EIP_TIME_OVERFLOW) - cycle_prev_masked;
        }
        return cycle_masked - cycle_prev_masked;
    }

    // ENTANGLED COMPRESSION FORMAT

#define EIP_ENTANGLED_MAX_FORMATS 7

    // Diagnostic counters; unlike prediction tables, these are not modeled
    // hardware.
    struct l1i_stats_entry
    {
        uint64_t accesses = 0, misses = 0, hits = 0, late = 0, wrong = 0;
    };
    l1i_stats_entry l1i_stats_totals{};
    uint64_t l1i_stats_discarded_prefetches = 0,
             l1i_stats_evict_entangled_j_table = 0;
    uint64_t l1i_stats_evict_entangled_k_table = 0, l1i_stats_max_bb_size = 0;
    uint64_t l1i_stats_formats[EIP_ENTANGLED_MAX_FORMATS]{};
    uint64_t l1i_stats_hist_lookups[EIP_HIST_TABLE_ENTRIES + 2]{};
    uint64_t l1i_stats_basic_blocks[EIP_MERGE_BBSIZE_MAX_VALUE + 1]{};
    uint64_t l1i_stats_entangled[EIP_ENTANGLED_MAX_FORMATS + 1]{};
    uint64_t l1i_stats_basic_blocks_ent[EIP_MERGE_BBSIZE_MAX_VALUE + 1]{};
    uint64_t associations = 0, accepted = 0, rejected = 0, stale_requests = 0,
             shadow_resyncs = 0;
    void
    l1i_init_stats_table()
    {
        l1i_stats_totals = {};
        l1i_stats_discarded_prefetches = l1i_stats_evict_entangled_j_table =
            l1i_stats_evict_entangled_k_table = l1i_stats_max_bb_size = 0;
        for (auto &x : l1i_stats_formats) {
            x = 0;
        }
        for (auto &x : l1i_stats_hist_lookups) {
            x = 0;
        }
        for (auto &x : l1i_stats_basic_blocks) {
            x = 0;
        }
        for (auto &x : l1i_stats_entangled) {
            x = 0;
        }
        for (auto &x : l1i_stats_basic_blocks_ent) {
            x = 0;
        }
        associations = accepted = rejected = stale_requests = shadow_resyncs =
            0;
    }
    void
    l1i_print_stats_table()
    {
        fmt::print("[entangling] demand_accesses={} misses={} "
                   "timely_first_uses={} late_first_uses={} "
                   "unused_evictions={} association_inserts={} accepted={} "
                   "rejected={} stale_requests={} shadow_resyncs={}\n",
                   l1i_stats_totals.accesses, l1i_stats_totals.misses,
                   l1i_stats_totals.hits, l1i_stats_totals.late,
                   l1i_stats_totals.wrong, associations, accepted, rejected,
                   stale_requests, shadow_resyncs);
    }
    // HISTORY TABLE (BUFFER)

#define EIP_HIST_TABLE_MASK (EIP_HIST_TABLE_ENTRIES - 1)
#define EIP_BB_MERGE_ENTRIES 6
#define EIP_HIST_TAG_BITS 58
#define EIP_HIST_TAG_MASK (((uint64_t)1 << EIP_HIST_TAG_BITS) - 1)

    typedef struct __l1i_hist_entry
    {
        uint64_t tag;       // EIP_HIST_TAG_BITS bits
        uint64_t time_diff; // EIP_TIME_DIFF_BITS bits
        uint32_t bb_size;   // EIP_MERGE_BBSIZE_BITS bits
    } l1i_hist_entry;

    l1i_hist_entry l1i_hist_table[NUM_CPUS][EIP_HIST_TABLE_ENTRIES];
    uint64_t l1i_hist_table_head[NUM_CPUS]; // log_2 (EIP_HIST_TABLE_ENTRIES)
    uint64_t l1i_hist_table_head_time[NUM_CPUS]; // 64 bits

    void
    l1i_init_hist_table()
    {
        l1i_hist_table_head[l1i_cpu_id] = 0;
        l1i_hist_table_head_time[l1i_cpu_id] = now();
        for (uint32_t i = 0; i < EIP_HIST_TABLE_ENTRIES; i++) {
            l1i_hist_table[l1i_cpu_id][i].tag = 0;
            l1i_hist_table[l1i_cpu_id][i].time_diff = 0;
            l1i_hist_table[l1i_cpu_id][i].bb_size = 0;
        }
    }

    uint64_t
    l1i_find_hist_entry(uint64_t line_addr)
    {
        uint64_t tag = line_addr & EIP_HIST_TAG_MASK;
        for (uint32_t count = 0, i = (l1i_hist_table_head[l1i_cpu_id] +
                                      EIP_HIST_TABLE_MASK) %
                                     EIP_HIST_TABLE_ENTRIES;
             count < EIP_HIST_TABLE_ENTRIES;
             count++, i = (i + EIP_HIST_TABLE_MASK) % EIP_HIST_TABLE_ENTRIES) {
            if (l1i_hist_table[l1i_cpu_id][i].tag == tag) {
                return i;
            }
        }
        return EIP_HIST_TABLE_ENTRIES;
    }

    // It can have duplicated entries if the line was evicted in between
    uint32_t
    l1i_add_hist_table(uint64_t line_addr)
    {
        // Insert empty addresses in hist not to have timediff overflows
        while (now() - l1i_hist_table_head_time[l1i_cpu_id] >=
               EIP_TIME_DIFF_OVERFLOW) {
            l1i_hist_table[l1i_cpu_id][l1i_hist_table_head[l1i_cpu_id]].tag =
                0;
            l1i_hist_table[l1i_cpu_id][l1i_hist_table_head[l1i_cpu_id]]
                .time_diff = EIP_TIME_DIFF_MASK;
            l1i_hist_table[l1i_cpu_id][l1i_hist_table_head[l1i_cpu_id]]
                .bb_size = 0;
            l1i_hist_table_head[l1i_cpu_id] =
                (l1i_hist_table_head[l1i_cpu_id] + 1) % EIP_HIST_TABLE_ENTRIES;
            l1i_hist_table_head_time[l1i_cpu_id] += EIP_TIME_DIFF_MASK;
        }

        // Allocate a new entry (evict old one if necessary)
        l1i_hist_table[l1i_cpu_id][l1i_hist_table_head[l1i_cpu_id]].tag =
            line_addr & EIP_HIST_TAG_MASK;
        l1i_hist_table[l1i_cpu_id][l1i_hist_table_head[l1i_cpu_id]].time_diff =
            (now() - l1i_hist_table_head_time[l1i_cpu_id]) &
            EIP_TIME_DIFF_MASK;
        l1i_hist_table[l1i_cpu_id][l1i_hist_table_head[l1i_cpu_id]].bb_size =
            0;
        uint32_t pos = l1i_hist_table_head[l1i_cpu_id];
        l1i_hist_table_head[l1i_cpu_id] =
            (l1i_hist_table_head[l1i_cpu_id] + 1) % EIP_HIST_TABLE_ENTRIES;
        l1i_hist_table_head_time[l1i_cpu_id] = now();
        return pos;
    }

    void
    l1i_add_bb_size_hist_table(uint64_t line_addr, uint32_t bb_size)
    {
        uint64_t index = l1i_find_hist_entry(line_addr);
        if (index < EIP_HIST_TABLE_ENTRIES) {
            l1i_hist_table[l1i_cpu_id][index].bb_size =
                bb_size & EIP_MERGE_BBSIZE_MAX_VALUE;
        }
    }

    uint32_t
    l1i_find_bb_merge_hist_table(uint64_t line_addr)
    {
        uint64_t tag = line_addr & EIP_HIST_TAG_MASK;
        for (uint32_t count = 0, i = (l1i_hist_table_head[l1i_cpu_id] +
                                      EIP_HIST_TABLE_MASK) %
                                     EIP_HIST_TABLE_ENTRIES;
             count < EIP_HIST_TABLE_ENTRIES;
             count++, i = (i + EIP_HIST_TABLE_MASK) % EIP_HIST_TABLE_ENTRIES) {
            if (count >= EIP_BB_MERGE_ENTRIES) {
                return 0;
            }
            if (tag > l1i_hist_table[l1i_cpu_id][i].tag &&
                (tag - l1i_hist_table[l1i_cpu_id][i].tag) <=
                    l1i_hist_table[l1i_cpu_id][i].bb_size) {
                //&& (tag - l1i_hist_table[l1i_cpu_id][i].tag) ==
                // l1i_hist_table[l1i_cpu_id][i].bb_size) {
                return tag - l1i_hist_table[l1i_cpu_id][i].tag;
            }
        }
        assert(false);
    }

    // return src-entangled pair
    uint64_t
    l1i_get_src_entangled_hist_table(uint64_t line_addr, uint32_t pos_hist,
                                     uint64_t latency, uint32_t skip = 0)
    {
        assert(pos_hist < EIP_HIST_TABLE_ENTRIES);
        uint64_t tag = line_addr & EIP_HIST_TAG_MASK;
        assert(tag);
        if (l1i_hist_table[l1i_cpu_id][pos_hist].tag != tag) {
            l1i_stats_hist_lookups[EIP_HIST_TABLE_ENTRIES]++;
            return 0; // removed
        }
        uint32_t next_pos =
            (pos_hist + EIP_HIST_TABLE_MASK) % EIP_HIST_TABLE_ENTRIES;
        uint32_t first =
            (l1i_hist_table_head[l1i_cpu_id] + EIP_HIST_TABLE_MASK) %
            EIP_HIST_TABLE_ENTRIES;
        uint64_t time_i = l1i_hist_table[l1i_cpu_id][pos_hist].time_diff;
        uint32_t num_skipped = 0;
        for (uint32_t count = 0, i = next_pos; i != first;
             count++, i = (i + EIP_HIST_TABLE_MASK) % EIP_HIST_TABLE_ENTRIES) {
            // Against the time overflow
            if (l1i_hist_table[l1i_cpu_id][i].tag == tag) {
                return 0; // Second time it appeared (it was evicted in
                          // between) or many for the same set. No entangle
            }
            if (l1i_hist_table[l1i_cpu_id][i].tag && time_i >= latency) {
                if (skip == num_skipped) {
                    l1i_stats_hist_lookups[count]++;
                    return l1i_hist_table[l1i_cpu_id][i].tag;
                } else {
                    num_skipped++;
                }
            }
            time_i += l1i_hist_table[l1i_cpu_id][i].time_diff;
        }
        l1i_stats_hist_lookups[EIP_HIST_TABLE_ENTRIES + 1]++;
        return 0;
    }

    // TIMING TABLES

#define EIP_SET_BITS (champsim::lg2(cache->NUM_SET))

#define EIP_TIMING_MSHR_TAG_BITS 42
#define EIP_TIMING_MSHR_TAG_MASK (((uint64_t)1 << EIP_HIST_TAG_BITS) - 1)
#define EIP_TIMING_CACHE_TAG_BITS (EIP_TIMING_MSHR_TAG_BITS - EIP_SET_BITS)
#define EIP_TIMING_CACHE_TAG_MASK (((uint64_t)1 << EIP_HIST_TAG_BITS) - 1)

#define EIP_ENTANGLED_TABLE_WAYS 16

    // We do not have access to the MSHR, so we aproximate it using this
    // structure
    typedef struct __l1i_timing_mshr_entry
    {
        bool valid;          // 1 bit
        uint64_t tag;        // EIP_TIMING_MSHR_TAG_BITS bits
        uint32_t source_set; // 8 bits
        uint32_t source_way; // 6 bits
        uint64_t timestamp;  // EIP_TIME_BITS bits // time when issued
        bool accessed;       // 1 bit
        uint32_t pos_hist;   // 1 bit
    } l1i_timing_mshr_entry;

    // We do not have access to the cache, so we aproximate it using this
    // structure
    typedef struct __l1i_timing_cache_entry
    {
        bool valid;          // 1 bit
        uint64_t tag;        // EIP_TIMING_CACHE_TAG_BITS bits
        uint32_t source_set; // 8 bits
        uint32_t source_way; // 6 bits
        bool accessed;       // 1 bit
    } l1i_timing_cache_entry;

    std::vector<l1i_timing_mshr_entry> l1i_timing_mshr_table[NUM_CPUS];
    std::vector<std::vector<l1i_timing_cache_entry>>
        l1i_timing_cache_table[NUM_CPUS];

    void
    l1i_init_timing_tables()
    {
        for (uint32_t i = 0; i < timing_mshr_capacity(); i++) {
            l1i_timing_mshr_table[l1i_cpu_id][i].valid = 0;
        }
        for (uint32_t i = 0; i < cache->NUM_SET; i++) {
            for (uint32_t j = 0; j < cache->NUM_WAY; j++) {
                l1i_timing_cache_table[l1i_cpu_id][i][j].valid = 0;
            }
        }
    }

    uint64_t
    l1i_find_timing_mshr_entry(uint64_t line_addr)
    {
        for (uint32_t i = 0; i < timing_mshr_capacity(); i++) {
            if (l1i_timing_mshr_table[l1i_cpu_id][i].tag ==
                    (line_addr & EIP_TIMING_MSHR_TAG_MASK) &&
                l1i_timing_mshr_table[l1i_cpu_id][i].valid) {
                return i;
            }
        }
        return timing_mshr_capacity();
    }

    uint64_t
    l1i_find_timing_cache_entry(uint64_t line_addr)
    {
        uint64_t i = line_addr % cache->NUM_SET;
        for (uint32_t j = 0; j < cache->NUM_WAY; j++) {
            if (l1i_timing_cache_table[l1i_cpu_id][i][j].tag ==
                    ((line_addr >> EIP_SET_BITS) &
                     EIP_TIMING_CACHE_TAG_MASK) &&
                l1i_timing_cache_table[l1i_cpu_id][i][j].valid) {
                return j;
            }
        }
        return cache->NUM_WAY;
    }

    uint32_t
    l1i_get_invalid_timing_mshr_entry()
    {
        for (uint32_t i = 0; i < timing_mshr_capacity(); i++) {
            if (!l1i_timing_mshr_table[l1i_cpu_id][i].valid) {
                return i;
            }
        }
        return static_cast<uint32_t>(timing_mshr_capacity());
    }

    uint32_t
    l1i_get_invalid_timing_cache_entry(uint64_t line_addr)
    {
        uint32_t i = line_addr % cache->NUM_SET;
        for (uint32_t j = 0; j < cache->NUM_WAY; j++) {
            if (!l1i_timing_cache_table[l1i_cpu_id][i][j].valid) {
                return j;
            }
        }
        assert(false); // It must return a free entry
        return cache->NUM_WAY;
    }

    void
    l1i_add_timing_entry(uint64_t line_addr, uint32_t source_set,
                         uint32_t source_way)
    {
        // First find for coalescing
        if (l1i_find_timing_mshr_entry(line_addr) < timing_mshr_capacity()) {
            return;
        }
        if (l1i_find_timing_cache_entry(line_addr) < cache->NUM_WAY) {
            return;
        }

        uint32_t i = l1i_get_invalid_timing_mshr_entry();
        if (i == timing_mshr_capacity()) {
            ++l1i_stats_discarded_prefetches;
            return;
        }
        l1i_timing_mshr_table[l1i_cpu_id][i].valid = true;
        l1i_timing_mshr_table[l1i_cpu_id][i].tag =
            line_addr & EIP_TIMING_MSHR_TAG_MASK;
        l1i_timing_mshr_table[l1i_cpu_id][i].source_set = source_set;
        l1i_timing_mshr_table[l1i_cpu_id][i].source_way = source_way;
        l1i_timing_mshr_table[l1i_cpu_id][i].timestamp = now() & EIP_TIME_MASK;
        l1i_timing_mshr_table[l1i_cpu_id][i].accessed = false;
    }

    void
    l1i_invalid_timing_mshr_entry(uint64_t line_addr)
    {
        uint32_t index = l1i_find_timing_mshr_entry(line_addr);
        assert(index < timing_mshr_capacity());
        l1i_timing_mshr_table[l1i_cpu_id][index].valid = false;
    }

    void
    l1i_move_timing_entry(uint64_t line_addr)
    {
        uint32_t index_mshr = l1i_find_timing_mshr_entry(line_addr);
        if (index_mshr == timing_mshr_capacity()) {
            uint32_t set = line_addr % cache->NUM_SET;
            uint32_t index_cache = fill_way;
            l1i_timing_cache_table[l1i_cpu_id][set][index_cache].valid = true;
            l1i_timing_cache_table[l1i_cpu_id][set][index_cache].tag =
                (line_addr >> EIP_SET_BITS) & EIP_TIMING_CACHE_TAG_MASK;
            l1i_timing_cache_table[l1i_cpu_id][set][index_cache].source_way =
                EIP_ENTANGLED_TABLE_WAYS;
            l1i_timing_cache_table[l1i_cpu_id][set][index_cache].accessed =
                true;
            return;
        }
        uint64_t set = line_addr % cache->NUM_SET;
        uint64_t index_cache = fill_way;
        l1i_timing_cache_table[l1i_cpu_id][set][index_cache].valid = true;
        l1i_timing_cache_table[l1i_cpu_id][set][index_cache].tag =
            (line_addr >> EIP_SET_BITS) & EIP_TIMING_CACHE_TAG_MASK;
        l1i_timing_cache_table[l1i_cpu_id][set][index_cache].source_set =
            l1i_timing_mshr_table[l1i_cpu_id][index_mshr].source_set;
        l1i_timing_cache_table[l1i_cpu_id][set][index_cache].source_way =
            l1i_timing_mshr_table[l1i_cpu_id][index_mshr].source_way;
        l1i_timing_cache_table[l1i_cpu_id][set][index_cache].accessed =
            l1i_timing_mshr_table[l1i_cpu_id][index_mshr].accessed;
        l1i_invalid_timing_mshr_entry(line_addr);
    }

    // returns if accessed
    bool
    l1i_invalid_timing_cache_entry(uint64_t line_addr, uint32_t &source_set,
                                   uint32_t &source_way)
    {
        uint32_t set = line_addr % cache->NUM_SET;
        uint32_t way = l1i_find_timing_cache_entry(line_addr);
        if (way == cache->NUM_WAY) {
            ++shadow_resyncs;
            source_way = EIP_ENTANGLED_TABLE_WAYS;
            return true;
        }
        l1i_timing_cache_table[l1i_cpu_id][set][way].valid = false;
        source_set = l1i_timing_cache_table[l1i_cpu_id][set][way].source_set;
        source_way = l1i_timing_cache_table[l1i_cpu_id][set][way].source_way;
        return l1i_timing_cache_table[l1i_cpu_id][set][way].accessed;
    }

    void
    l1i_access_timing_entry(uint64_t line_addr, uint32_t pos_hist,
                            uint32_t &source_set, uint32_t &source_way)
    {
        uint32_t index = l1i_find_timing_mshr_entry(line_addr);
        if (index < timing_mshr_capacity()) {
            if (!l1i_timing_mshr_table[l1i_cpu_id][index]
                     .accessed) { // Prefetch accessed while in MSHR: late
                l1i_timing_mshr_table[l1i_cpu_id][index].accessed = true;
                l1i_timing_mshr_table[l1i_cpu_id][index].pos_hist = pos_hist;
                if (l1i_timing_mshr_table[l1i_cpu_id][index].source_way <
                    EIP_ENTANGLED_TABLE_WAYS) {
                    source_set =
                        l1i_timing_mshr_table[l1i_cpu_id][index].source_set;
                    source_way =
                        l1i_timing_mshr_table[l1i_cpu_id][index].source_way;
                    l1i_timing_mshr_table[l1i_cpu_id][index].source_set = 0;
                    l1i_timing_mshr_table[l1i_cpu_id][index].source_way =
                        EIP_ENTANGLED_TABLE_WAYS;
                }
            }
            return;
        }
        uint32_t set = line_addr % cache->NUM_SET;
        uint32_t way = l1i_find_timing_cache_entry(line_addr);
        if (way < cache->NUM_WAY) {
            l1i_timing_cache_table[l1i_cpu_id][set][way].accessed = true;
        }
    }

    bool
    l1i_is_accessed_timing_entry(uint64_t line_addr)
    {
        uint32_t index = l1i_find_timing_mshr_entry(line_addr);
        if (index < timing_mshr_capacity()) {
            return l1i_timing_mshr_table[l1i_cpu_id][index].accessed;
        }
        uint32_t set = line_addr % cache->NUM_SET;
        uint32_t way = l1i_find_timing_cache_entry(line_addr);
        if (way < cache->NUM_WAY) {
            return l1i_timing_cache_table[l1i_cpu_id][set][way].accessed;
        }
        return false;
    }

    bool
    l1i_completed_request(uint64_t line_addr)
    {
        return l1i_find_timing_cache_entry(line_addr) < cache->NUM_WAY;
    }

    bool
    l1i_ongoing_request(uint64_t line_addr)
    {
        return l1i_find_timing_mshr_entry(line_addr) < timing_mshr_capacity();
    }

    bool
    l1i_ongoing_accessed_request(uint64_t line_addr)
    {
        uint32_t index = l1i_find_timing_mshr_entry(line_addr);
        if (index == timing_mshr_capacity()) {
            return false;
        }
        return l1i_timing_mshr_table[l1i_cpu_id][index].accessed;
    }

    uint64_t
    l1i_get_latency_timing_mshr(uint64_t line_addr, uint32_t &pos_hist)
    {
        uint32_t index = l1i_find_timing_mshr_entry(line_addr);
        if (index == timing_mshr_capacity()) {
            return 0;
        }
        if (!l1i_timing_mshr_table[l1i_cpu_id][index].accessed) {
            return 0;
        }
        pos_hist = l1i_timing_mshr_table[l1i_cpu_id][index].pos_hist;
        return l1i_get_latency(
            now(), l1i_timing_mshr_table[l1i_cpu_id][index].timestamp);
    }

    // ENTANGLED TABLE

    uint32_t EIP_ENTANGLED_FORMATS[EIP_ENTANGLED_MAX_FORMATS] = {
        58, 28, 18, 13, 10, 8, 6};
#define EIP_ENTANGLED_NUM_FORMATS 6

    uint32_t
    l1i_get_format_entangled(uint64_t line_addr, uint64_t entangled_addr)
    {
        for (uint32_t i = EIP_ENTANGLED_NUM_FORMATS; i != 0; i--) {
            if ((line_addr >> EIP_ENTANGLED_FORMATS[i - 1]) ==
                (entangled_addr >> EIP_ENTANGLED_FORMATS[i - 1])) {
                return i;
            }
        }
        assert(false);
    }

    uint64_t
    l1i_extend_format_entangled(uint64_t line_addr, uint64_t entangled_addr,
                                uint32_t format)
    {
        return ((line_addr >> EIP_ENTANGLED_FORMATS[format - 1])
                << EIP_ENTANGLED_FORMATS[format - 1]) |
               (entangled_addr &
                (((uint64_t)1 << EIP_ENTANGLED_FORMATS[format - 1]) - 1));
    }

    uint64_t
    l1i_compress_format_entangled(uint64_t entangled_addr, uint32_t format)
    {
        return entangled_addr &
               (((uint64_t)1 << EIP_ENTANGLED_FORMATS[format - 1]) - 1);
    }

#define EIP_ENTANGLED_TABLE_INDEX_BITS 8
#define EIP_ENTANGLED_TABLE_SETS (1 << EIP_ENTANGLED_TABLE_INDEX_BITS)
#define EIP_MAX_ENTANGLED_PER_LINE EIP_ENTANGLED_NUM_FORMATS
#define EIP_TAG_BITS (18 - EIP_ENTANGLED_TABLE_INDEX_BITS)
#define EIP_TAG_MASK (((uint64_t)1 << EIP_TAG_BITS) - 1)
#define EIP_CONFIDENCE_COUNTER_BITS 2
#define EIP_CONFIDENCE_COUNTER_MAX_VALUE                                      \
    ((1 << EIP_CONFIDENCE_COUNTER_BITS) - 1)
#define EIP_CONFIDENCE_COUNTER_THRESHOLD 1
#define EIP_TRIES_AVAIL_ENTANGLED 2

    typedef struct __l1i_entangled_entry
    {
        uint64_t tag;    // EIP_TAG_BITS bits
        uint32_t format; // log2(EIP_ENTANGLED_NUM_FORMATS) bits
        uint64_t entangled_addr[EIP_MAX_ENTANGLED_PER_LINE]; // JUST DIFF
        uint32_t entangled_conf
            [EIP_MAX_ENTANGLED_PER_LINE]; // EIP_CONFIDENCE_COUNTER_BITS
                                          // bits
        uint32_t bb_size;                 // EIP_MERGE_BBSIZE_BITS bits
    } l1i_entangled_entry;

    l1i_entangled_entry l1i_entangled_table[NUM_CPUS][EIP_ENTANGLED_TABLE_SETS]
                                           [EIP_ENTANGLED_TABLE_WAYS];
    // Each set needs log2(EIP_ENTANGLED_TABLE_WAYS) bits for its FIFO index.
    uint32_t l1i_entangled_fifo[NUM_CPUS][EIP_ENTANGLED_TABLE_SETS];

    uint64_t
    l1i_hash(uint64_t line_addr)
    {
        return line_addr ^ (line_addr >> 2) ^ (line_addr >> 5);
    }

    void
    l1i_init_entangled_table()
    {
        for (uint32_t i = 0; i < EIP_ENTANGLED_TABLE_SETS; i++) {
            for (uint32_t j = 0; j < EIP_ENTANGLED_TABLE_WAYS; j++) {
                l1i_entangled_table[l1i_cpu_id][i][j].tag = 0;
                l1i_entangled_table[l1i_cpu_id][i][j].format = 1;
                for (uint32_t k = 0; k < EIP_MAX_ENTANGLED_PER_LINE; k++) {
                    l1i_entangled_table[l1i_cpu_id][i][j].entangled_addr[k] =
                        0;
                    l1i_entangled_table[l1i_cpu_id][i][j].entangled_conf[k] =
                        0;
                }
                l1i_entangled_table[l1i_cpu_id][i][j].bb_size = 0;
            }
            l1i_entangled_fifo[l1i_cpu_id][i] = 0;
        }
    }

    uint32_t
    l1i_get_way_entangled_table(uint64_t line_addr)
    {
        uint64_t tag =
            (l1i_hash(line_addr) >> EIP_ENTANGLED_TABLE_INDEX_BITS) &
            EIP_TAG_MASK;
        uint32_t set = l1i_hash(line_addr) % EIP_ENTANGLED_TABLE_SETS;
        for (uint32_t i = 0; i < EIP_ENTANGLED_TABLE_WAYS; i++) {
            if (l1i_entangled_table[l1i_cpu_id][set][i].tag == tag) { // Found
                return i;
            }
        }
        return EIP_ENTANGLED_TABLE_WAYS;
    }

    void
    l1i_try_realocate_evicted_in_available_entangled_table(uint32_t set)
    {
        uint64_t way = l1i_entangled_fifo[l1i_cpu_id][set];
        bool dest_free_way = true;
        for (uint32_t k = 0; k < EIP_MAX_ENTANGLED_PER_LINE; k++) {
            if (l1i_entangled_table[l1i_cpu_id][set][way].entangled_conf[k] >=
                EIP_CONFIDENCE_COUNTER_THRESHOLD) {
                dest_free_way = false;
                break;
            }
        }
        if (dest_free_way &&
            l1i_entangled_table[l1i_cpu_id][set][way].bb_size == 0) {
            return;
        }
        uint32_t free_way = way;
        bool free_with_size = false;
        for (uint32_t i = (way + 1) % EIP_ENTANGLED_TABLE_WAYS; i != way;
             i = (i + 1) % EIP_ENTANGLED_TABLE_WAYS) {
            bool dest_free = true;
            for (uint32_t k = 0; k < EIP_MAX_ENTANGLED_PER_LINE; k++) {
                if (l1i_entangled_table[l1i_cpu_id][set][i]
                        .entangled_conf[k] >=
                    EIP_CONFIDENCE_COUNTER_THRESHOLD) {
                    dest_free = false;
                    break;
                }
            }
            if (dest_free) {
                if (free_way == way) {
                    free_way = i;
                    free_with_size =
                        (l1i_entangled_table[l1i_cpu_id][set][i].bb_size != 0);
                } else if (free_with_size &&
                           l1i_entangled_table[l1i_cpu_id][set][i].bb_size ==
                               0) {
                    free_way = i;
                    free_with_size = false;
                    break;
                }
            }
        }
        if (free_way != way &&
            ((!free_with_size) ||
             (free_with_size &&
              !dest_free_way))) { // Only evict if it has more information
            l1i_entangled_table[l1i_cpu_id][set][free_way].tag =
                l1i_entangled_table[l1i_cpu_id][set][way].tag;
            l1i_entangled_table[l1i_cpu_id][set][free_way].format =
                l1i_entangled_table[l1i_cpu_id][set][way].format;
            for (uint32_t k = 0; k < EIP_MAX_ENTANGLED_PER_LINE; k++) {
                l1i_entangled_table[l1i_cpu_id][set][free_way]
                    .entangled_addr[k] =
                    l1i_entangled_table[l1i_cpu_id][set][way]
                        .entangled_addr[k];
                l1i_entangled_table[l1i_cpu_id][set][free_way]
                    .entangled_conf[k] =
                    l1i_entangled_table[l1i_cpu_id][set][way]
                        .entangled_conf[k];
            }
            l1i_entangled_table[l1i_cpu_id][set][free_way].bb_size =
                l1i_entangled_table[l1i_cpu_id][set][way].bb_size;
        }
    }

    void
    l1i_add_entangled_table(uint64_t line_addr, uint64_t entangled_addr)
    {
        ++associations;
        uint64_t tag =
            (l1i_hash(line_addr) >> EIP_ENTANGLED_TABLE_INDEX_BITS) &
            EIP_TAG_MASK;
        uint32_t set = l1i_hash(line_addr) % EIP_ENTANGLED_TABLE_SETS;
        uint32_t way = l1i_get_way_entangled_table(line_addr);
        if (way == EIP_ENTANGLED_TABLE_WAYS) {
            l1i_try_realocate_evicted_in_available_entangled_table(set);
            way = l1i_entangled_fifo[l1i_cpu_id][set];
            l1i_entangled_table[l1i_cpu_id][set][way].tag = tag;
            l1i_entangled_table[l1i_cpu_id][set][way].format = 1;
            for (uint32_t k = 0; k < EIP_MAX_ENTANGLED_PER_LINE; k++) {
                l1i_entangled_table[l1i_cpu_id][set][way].entangled_addr[k] =
                    0;
                l1i_entangled_table[l1i_cpu_id][set][way].entangled_conf[k] =
                    0;
            }
            l1i_entangled_table[l1i_cpu_id][set][way].bb_size = 0;
            l1i_entangled_fifo[l1i_cpu_id][set] =
                (l1i_entangled_fifo[l1i_cpu_id][set] + 1) %
                EIP_ENTANGLED_TABLE_WAYS;
        }
        for (uint32_t k = 0; k < EIP_MAX_ENTANGLED_PER_LINE; k++) {
            if (l1i_entangled_table[l1i_cpu_id][set][way].entangled_conf[k] >=
                    EIP_CONFIDENCE_COUNTER_THRESHOLD &&
                l1i_extend_format_entangled(
                    line_addr,
                    l1i_entangled_table[l1i_cpu_id][set][way]
                        .entangled_addr[k],
                    l1i_entangled_table[l1i_cpu_id][set][way].format) ==
                    entangled_addr) {
                l1i_entangled_table[l1i_cpu_id][set][way].entangled_conf[k] =
                    EIP_CONFIDENCE_COUNTER_MAX_VALUE;
                return;
            }
        }

        // Adding a new entangled
        uint32_t format_new =
            l1i_get_format_entangled(line_addr, entangled_addr);
        l1i_stats_formats[format_new - 1]++;

        // Check for evictions
        while (true) {
            uint32_t min_format = format_new;
            uint32_t num_valid = 1;
            uint32_t min_value = EIP_CONFIDENCE_COUNTER_MAX_VALUE + 1;
            uint32_t min_pos = 0;
            for (uint32_t k = 0; k < EIP_MAX_ENTANGLED_PER_LINE; k++) {
                if (l1i_entangled_table[l1i_cpu_id][set][way]
                        .entangled_conf[k] >=
                    EIP_CONFIDENCE_COUNTER_THRESHOLD) {
                    num_valid++;
                    uint32_t format_k = l1i_get_format_entangled(
                        line_addr,
                        l1i_extend_format_entangled(
                            line_addr,
                            l1i_entangled_table[l1i_cpu_id][set][way]
                                .entangled_addr[k],
                            l1i_entangled_table[l1i_cpu_id][set][way].format));
                    if (format_k < min_format) {
                        min_format = format_k;
                    }
                    if (l1i_entangled_table[l1i_cpu_id][set][way]
                            .entangled_conf[k] < min_value) {
                        min_value = l1i_entangled_table[l1i_cpu_id][set][way]
                                        .entangled_conf[k];
                        min_pos = k;
                    }
                }
            }
            if (num_valid > min_format) { // Eviction is necessary. We chose
                                          // the lower confidence one
                l1i_stats_evict_entangled_k_table++;
                l1i_entangled_table[l1i_cpu_id][set][way]
                    .entangled_conf[min_pos] = 0;
            } else {
                // Reformat
                for (uint32_t k = 0; k < EIP_MAX_ENTANGLED_PER_LINE; k++) {
                    if (l1i_entangled_table[l1i_cpu_id][set][way]
                            .entangled_conf[k] >=
                        EIP_CONFIDENCE_COUNTER_THRESHOLD) {
                        l1i_entangled_table[l1i_cpu_id][set][way]
                            .entangled_addr[k] = l1i_compress_format_entangled(
                            l1i_extend_format_entangled(
                                line_addr,
                                l1i_entangled_table[l1i_cpu_id][set][way]
                                    .entangled_addr[k],
                                l1i_entangled_table[l1i_cpu_id][set][way]
                                    .format),
                            min_format);
                    }
                }
                l1i_entangled_table[l1i_cpu_id][set][way].format = min_format;
                break;
            }
        }
        for (uint32_t k = 0; k < EIP_MAX_ENTANGLED_PER_LINE; k++) {
            if (l1i_entangled_table[l1i_cpu_id][set][way].entangled_conf[k] <
                EIP_CONFIDENCE_COUNTER_THRESHOLD) {
                l1i_entangled_table[l1i_cpu_id][set][way].entangled_addr[k] =
                    l1i_compress_format_entangled(
                        entangled_addr,
                        l1i_entangled_table[l1i_cpu_id][set][way].format);
                l1i_entangled_table[l1i_cpu_id][set][way].entangled_conf[k] =
                    EIP_CONFIDENCE_COUNTER_MAX_VALUE;
                return;
            }
        }
    }

    bool
    l1i_avail_entangled_table(uint64_t line_addr, uint64_t entangled_addr,
                              bool insert_not_present)
    {
        uint32_t set = l1i_hash(line_addr) % EIP_ENTANGLED_TABLE_SETS;
        uint32_t way = l1i_get_way_entangled_table(line_addr);
        if (way == EIP_ENTANGLED_TABLE_WAYS) {
            return insert_not_present;
        }
        for (uint32_t k = 0; k < EIP_MAX_ENTANGLED_PER_LINE; k++) {
            if (l1i_entangled_table[l1i_cpu_id][set][way].entangled_conf[k] >=
                    EIP_CONFIDENCE_COUNTER_THRESHOLD &&
                l1i_extend_format_entangled(
                    line_addr,
                    l1i_entangled_table[l1i_cpu_id][set][way]
                        .entangled_addr[k],
                    l1i_entangled_table[l1i_cpu_id][set][way].format) ==
                    entangled_addr) {
                return true;
            }
        }
        // Check for availability
        uint32_t min_format =
            l1i_get_format_entangled(line_addr, entangled_addr);
        uint32_t num_valid = 1;
        for (uint32_t k = 0; k < EIP_MAX_ENTANGLED_PER_LINE; k++) {
            if (l1i_entangled_table[l1i_cpu_id][set][way].entangled_conf[k] >=
                EIP_CONFIDENCE_COUNTER_THRESHOLD) {
                num_valid++;
                uint32_t format_k = l1i_get_format_entangled(
                    line_addr,
                    l1i_extend_format_entangled(
                        line_addr,
                        l1i_entangled_table[l1i_cpu_id][set][way]
                            .entangled_addr[k],
                        l1i_entangled_table[l1i_cpu_id][set][way].format));
                if (format_k < min_format) {
                    min_format = format_k;
                }
            }
        }
        if (num_valid > min_format) { // Eviction is necessary
            return false;
        } else {
            return true;
        }
    }

    void
    l1i_add_bbsize_table(uint64_t line_addr, uint32_t bb_size)
    {
        uint64_t tag =
            (l1i_hash(line_addr) >> EIP_ENTANGLED_TABLE_INDEX_BITS) &
            EIP_TAG_MASK;
        uint32_t set = l1i_hash(line_addr) % EIP_ENTANGLED_TABLE_SETS;
        uint32_t way = l1i_get_way_entangled_table(line_addr);
        if (way == EIP_ENTANGLED_TABLE_WAYS) {
            l1i_try_realocate_evicted_in_available_entangled_table(set);
            way = l1i_entangled_fifo[l1i_cpu_id][set];
            l1i_entangled_table[l1i_cpu_id][set][way].tag = tag;
            l1i_entangled_table[l1i_cpu_id][set][way].format = 1;
            for (uint32_t k = 0; k < EIP_MAX_ENTANGLED_PER_LINE; k++) {
                l1i_entangled_table[l1i_cpu_id][set][way].entangled_addr[k] =
                    0;
                l1i_entangled_table[l1i_cpu_id][set][way].entangled_conf[k] =
                    0;
            }
            l1i_entangled_table[l1i_cpu_id][set][way].bb_size = 0;
            l1i_entangled_fifo[l1i_cpu_id][set] =
                (l1i_entangled_fifo[l1i_cpu_id][set] + 1) %
                EIP_ENTANGLED_TABLE_WAYS;
        }
        if (bb_size > l1i_entangled_table[l1i_cpu_id][set][way].bb_size) {
            l1i_entangled_table[l1i_cpu_id][set][way].bb_size =
                bb_size & EIP_MERGE_BBSIZE_MAX_VALUE;
        }
        if (bb_size > l1i_stats_max_bb_size) {
            l1i_stats_max_bb_size = bb_size;
        }
    }

    uint64_t
    l1i_get_entangled_addr_entangled_table(uint64_t line_addr,
                                           uint32_t index_k, uint32_t &set,
                                           uint32_t &way)
    {
        set = l1i_hash(line_addr) % EIP_ENTANGLED_TABLE_SETS;
        way = l1i_get_way_entangled_table(line_addr);
        if (way < EIP_ENTANGLED_TABLE_WAYS) {
            if (l1i_entangled_table[l1i_cpu_id][set][way]
                    .entangled_conf[index_k] >=
                static_cast<uint32_t>(settings.confidence_threshold)) {
                return l1i_extend_format_entangled(
                    line_addr,
                    l1i_entangled_table[l1i_cpu_id][set][way]
                        .entangled_addr[index_k],
                    l1i_entangled_table[l1i_cpu_id][set][way].format);
            }
        }
        return 0;
    }

    uint32_t
    l1i_get_bbsize_entangled_table(uint64_t line_addr)
    {
        uint32_t set = l1i_hash(line_addr) % EIP_ENTANGLED_TABLE_SETS;
        uint32_t way = l1i_get_way_entangled_table(line_addr);
        if (way < EIP_ENTANGLED_TABLE_WAYS) {
            return l1i_entangled_table[l1i_cpu_id][set][way].bb_size;
        }
        return 0;
    }

    void
    l1i_update_confidence_entangled_table(uint32_t set, uint32_t way,
                                          uint64_t entangled_addr,
                                          bool accessed)
    {
        if (way < EIP_ENTANGLED_TABLE_WAYS) {
            for (uint32_t k = 0; k < EIP_MAX_ENTANGLED_PER_LINE; k++) {
                if (l1i_entangled_table[l1i_cpu_id][set][way]
                            .entangled_conf[k] >=
                        EIP_CONFIDENCE_COUNTER_THRESHOLD &&
                    l1i_compress_format_entangled(
                        l1i_entangled_table[l1i_cpu_id][set][way]
                            .entangled_addr[k],
                        l1i_entangled_table[l1i_cpu_id][set][way].format) ==
                        l1i_compress_format_entangled(
                            entangled_addr,
                            l1i_entangled_table[l1i_cpu_id][set][way]
                                .format)) {
                    if (accessed && l1i_entangled_table[l1i_cpu_id][set][way]
                                            .entangled_conf[k] <
                                        EIP_CONFIDENCE_COUNTER_MAX_VALUE) {
                        l1i_entangled_table[l1i_cpu_id][set][way]
                            .entangled_conf[k]++;
                    }
                    if (!accessed && l1i_entangled_table[l1i_cpu_id][set][way]
                                             .entangled_conf[k] > 0) {
                        l1i_entangled_table[l1i_cpu_id][set][way]
                            .entangled_conf[k]--;
                    }
                }
            }
        }
    }

    // INTERFACE

    void
    initialize()
    {
        std::cout << "CPU " << cpu << " Entangling prefetcher" << std::endl;

        l1i_cpu_id = cpu;
        l1i_init_stats_table();
        l1i_last_basic_block = 0;
        l1i_consecutive_count = 0;
        l1i_basic_block_merge_diff = 0;

        l1i_init_hist_table();
        l1i_init_timing_tables();
        l1i_init_entangled_table();
    }

    void
    observe(uint64_t v_addr, uint8_t cache_hit, uint8_t prefetch_hit)
    {
        l1i_cpu_id = cpu;
        uint64_t line_addr = v_addr >> LOG2_BLOCK_SIZE;

        if (!cache_hit) {
            assert(!prefetch_hit);
        }
        // The fill callback maintains shadow tags using the actual victim way.
        // Observe all LOAD accesses, including hits to prefetched lines.

        l1i_stats_totals.accesses++;
        if (!cache_hit) {
            l1i_stats_totals.misses++;
            if (l1i_ongoing_request(line_addr) &&
                !l1i_is_accessed_timing_entry(line_addr)) {
                l1i_stats_totals.late++;
            }
        }
        if (prefetch_hit) {
            l1i_stats_totals.hits++;
        }

        bool consecutive = false;

        if (l1i_last_basic_block + l1i_consecutive_count ==
            line_addr) { // Same
            return;
        } else if (l1i_last_basic_block + l1i_consecutive_count + 1 ==
                   line_addr) { // Consecutive
            l1i_consecutive_count++;
            consecutive = true;
        }

        // Queue basic block prefetches
        uint32_t bb_size = l1i_get_bbsize_entangled_table(line_addr);
        if (bb_size) {
            l1i_stats_basic_blocks[bb_size]++;
        }
        for (uint32_t i = 1; settings.enabled && i <= bb_size; i++) {
            uint64_t pf_addr = v_addr + i * (1 << LOG2_BLOCK_SIZE);
            if (!l1i_ongoing_request(pf_addr >> LOG2_BLOCK_SIZE)) {
                if (prefetch_code_line(pf_addr)) {
                    l1i_add_timing_entry(pf_addr >> LOG2_BLOCK_SIZE, 0,
                                         EIP_ENTANGLED_TABLE_WAYS);
                }
            }
        }

        // Queue entangled and basic block of entangled prefetches
        uint32_t num_entangled = 0;
        std::array<uint32_t, EIP_MAX_ENTANGLED_PER_LINE> order{};
        std::iota(order.begin(), order.end(), 0);
        if (settings.enabled &&
            settings.destination_cap < EIP_MAX_ENTANGLED_PER_LINE) {
            const auto way = l1i_get_way_entangled_table(line_addr);
            if (way < EIP_ENTANGLED_TABLE_WAYS) {
                const auto set =
                    l1i_hash(line_addr) % EIP_ENTANGLED_TABLE_SETS;
                const auto &confidence =
                    l1i_entangled_table[l1i_cpu_id][set][way].entangled_conf;
                std::stable_sort(order.begin(), order.end(),
                                 [&confidence](auto a, auto b) {
                                     return confidence[a] > confidence[b];
                                 });
            }
        }
        for (uint32_t k : order) {
            if (!settings.enabled ||
                num_entangled >=
                    static_cast<uint32_t>(settings.destination_cap)) {
                break;
            }
            uint32_t source_set = 0;
            uint32_t source_way = EIP_ENTANGLED_TABLE_WAYS;
            uint64_t entangled_line_addr =
                l1i_get_entangled_addr_entangled_table(line_addr, k,
                                                       source_set, source_way);
            if (entangled_line_addr && (entangled_line_addr != line_addr)) {
                num_entangled++;
                uint32_t bb_size =
                    l1i_get_bbsize_entangled_table(entangled_line_addr);
                if (bb_size) {
                    l1i_stats_basic_blocks_ent[bb_size]++;
                }
                for (uint32_t i = 0; i <= bb_size; i++) {
                    uint64_t pf_line_addr = entangled_line_addr + i;
                    if (!l1i_ongoing_request(pf_line_addr)) {
                        if (prefetch_code_line(pf_line_addr
                                               << LOG2_BLOCK_SIZE)) {
                            l1i_add_timing_entry(
                                pf_line_addr, source_set,
                                (i == 0) ? source_way
                                         : EIP_ENTANGLED_TABLE_WAYS);
                        }
                    }
                }
            }
        }
        if (num_entangled) {
            l1i_stats_entangled[num_entangled]++;
        }

        if (!consecutive) { // New basic block found
            uint32_t max_bb_size =
                l1i_get_bbsize_entangled_table(l1i_last_basic_block);

            // Check for merging bb opportunities
            if (l1i_consecutive_count) { // single blocks no need to merge and
                                         // are not inserted in the entangled
                                         // table
                if (l1i_basic_block_merge_diff > 0) {
                    l1i_add_bbsize_table(
                        l1i_last_basic_block - l1i_basic_block_merge_diff,
                        l1i_consecutive_count + l1i_basic_block_merge_diff);
                    l1i_add_bb_size_hist_table(
                        l1i_last_basic_block - l1i_basic_block_merge_diff,
                        l1i_consecutive_count + l1i_basic_block_merge_diff);
                } else {
                    l1i_add_bbsize_table(
                        l1i_last_basic_block,
                        std::max(max_bb_size, l1i_consecutive_count));
                    l1i_add_bb_size_hist_table(
                        l1i_last_basic_block,
                        std::max(max_bb_size, l1i_consecutive_count));
                }
            }
        }

        if (!consecutive) { // New basic block found
            l1i_consecutive_count = 0;
            l1i_last_basic_block = line_addr;
        }

        if (!consecutive) {
            l1i_basic_block_merge_diff =
                l1i_find_bb_merge_hist_table(l1i_last_basic_block);
        }

        // Add the request in the history buffer
        uint32_t pos_hist = EIP_HIST_TABLE_ENTRIES;
        if (!consecutive && l1i_basic_block_merge_diff == 0) {
            if ((l1i_find_hist_entry(line_addr) == EIP_HIST_TABLE_ENTRIES)) {
                pos_hist = l1i_add_hist_table(line_addr);
            } else {
                if (!cache_hit && !l1i_ongoing_accessed_request(line_addr)) {
                    pos_hist = l1i_add_hist_table(line_addr);
                }
            }
        }

        // Add miss in the latency table
        if (!cache_hit && !l1i_ongoing_request(line_addr)) {
            l1i_add_timing_entry(line_addr, 0, EIP_ENTANGLED_TABLE_WAYS);
        }

        uint32_t source_set = 0;
        uint32_t source_way = EIP_ENTANGLED_TABLE_WAYS;
        l1i_access_timing_entry(line_addr, pos_hist, source_set, source_way);

        // Update confidence if late
        if (source_way < EIP_ENTANGLED_TABLE_WAYS) {
            l1i_update_confidence_entangled_table(source_set, source_way,
                                                  line_addr, false);
        }
    }

    void
    tick()
    {
        synchronize_requests();
        if (!all_warmed_up && !cache->warmup) {
            l1i_init_stats_table();
            all_warmed_up = true;
        }
    }

    void
    fill(uint64_t v_addr, uint32_t set, uint32_t way, uint8_t prefetch,
         uint64_t evicted_v_addr)
    {
        l1i_cpu_id = cpu;
        uint64_t line_addr = (v_addr >> LOG2_BLOCK_SIZE);
        uint64_t evicted_line_addr = (evicted_v_addr >> LOG2_BLOCK_SIZE);

        // Line is in cache
        if (evicted_v_addr) {
            uint32_t source_set = 0;
            uint32_t source_way = EIP_ENTANGLED_TABLE_WAYS;
            bool accessed = l1i_invalid_timing_cache_entry(
                evicted_line_addr, source_set, source_way);
            if (!accessed) {
                l1i_stats_totals.wrong++;
            }
            if (source_way < EIP_ENTANGLED_TABLE_WAYS) {
                // If accessed hit, but if not wrong
                l1i_update_confidence_entangled_table(
                    source_set, source_way, evicted_line_addr, accessed);
            }
        }

        uint32_t pos_hist = EIP_HIST_TABLE_ENTRIES;
        uint64_t latency = l1i_get_latency_timing_mshr(line_addr, pos_hist);

        l1i_move_timing_entry(line_addr);

        // Get and update entangled
        if (latency && pos_hist < EIP_HIST_TABLE_ENTRIES) {
            bool inserted = false;
            for (uint32_t i = 0; i < EIP_TRIES_AVAIL_ENTANGLED; i++) {
                uint64_t src_entangled = l1i_get_src_entangled_hist_table(
                    line_addr, pos_hist, latency, i);
                if (src_entangled && line_addr != src_entangled) {
                    if (l1i_avail_entangled_table(src_entangled, line_addr,
                                                  false)) {
                        l1i_add_entangled_table(src_entangled, line_addr);
                        inserted = true;
                        break;
                    }
                }
            }
            if (!inserted) {
                uint64_t src_entangled = l1i_get_src_entangled_hist_table(
                    line_addr, pos_hist, latency);
                if (src_entangled && line_addr != src_entangled) {
                    l1i_add_entangled_table(src_entangled, line_addr);
                }
            }
        }
    }

    void
    final_stats()
    {
        std::cout << "CPU " << cpu << " L1I Entangling prefetcher final stats"
                  << std::endl;
        l1i_print_stats_table();
    }

    explicit state(CACHE *owner) : cache(owner)
    {
        l1i_timing_mshr_table[0].resize(timing_mshr_capacity());
        l1i_timing_cache_table[0].resize(
            cache->NUM_SET,
            std::vector<l1i_timing_cache_entry>(cache->NUM_WAY));
    }

    bool
    prefetch_code_line(uint64_t addr)
    {
        const auto line = addr >> LOG2_BLOCK_SIZE;
        for (const auto &b : cache->block) {
            if (b.valid &&
                (b.v_address.to<uint64_t>() >> LOG2_BLOCK_SIZE) == line) {
                return false;
            }
        }
        if (cache->has_pending_request(champsim::address{addr})) {
            return false;
        }
        bool ok = cache->prefetch_line(champsim::address{addr}, true, 0);
        ok ? ++accepted : ++rejected;
        return ok;
    }

    void
    synchronize_requests()
    {
        // Admission into the internal PQ does not guarantee a fill: hits and
        // merges can consume a queued prefetch. Do not keep phantom
        // outstanding requests.
        for (auto &e : l1i_timing_mshr_table[0]) {
            if (e.valid && !cache->has_pending_request(
                               champsim::address{e.tag << LOG2_BLOCK_SIZE})) {
                e.valid = false;
                ++stale_requests;
            }
        }
    }
};
#undef EIP_HIST_TABLE_ENTRIES
#undef EIP_MERGE_BBSIZE_BITS
#undef EIP_MERGE_BBSIZE_MAX_VALUE
#undef EIP_TIME_DIFF_BITS
#undef EIP_TIME_DIFF_OVERFLOW
#undef EIP_TIME_DIFF_MASK
#undef EIP_TIME_BITS
#undef EIP_TIME_OVERFLOW
#undef EIP_TIME_MASK
#undef EIP_ENTANGLED_MAX_FORMATS
#undef EIP_HIST_TABLE_MASK
#undef EIP_BB_MERGE_ENTRIES
#undef EIP_HIST_TAG_BITS
#undef EIP_HIST_TAG_MASK
#undef EIP_SET_BITS
#undef EIP_TIMING_MSHR_TAG_BITS
#undef EIP_TIMING_MSHR_TAG_MASK
#undef EIP_TIMING_CACHE_TAG_BITS
#undef EIP_TIMING_CACHE_TAG_MASK
#undef EIP_ENTANGLED_TABLE_WAYS
#undef EIP_ENTANGLED_NUM_FORMATS
#undef EIP_ENTANGLED_TABLE_INDEX_BITS
#undef EIP_ENTANGLED_TABLE_SETS
#undef EIP_MAX_ENTANGLED_PER_LINE
#undef EIP_TAG_BITS
#undef EIP_TAG_MASK
#undef EIP_CONFIDENCE_COUNTER_BITS
#undef EIP_CONFIDENCE_COUNTER_MAX_VALUE
#undef EIP_CONFIDENCE_COUNTER_THRESHOLD
#undef EIP_TRIES_AVAIL_ENTANGLED

entangling::entangling(CACHE *owner) : prefetcher(owner)
{}
entangling::~entangling()
{
    entangling_controls::unregister_instance(this);
}
entangling::entangling(entangling &&) noexcept = default;
entangling &entangling::operator=(entangling &&) noexcept = default;
entangling *
entangling::find(std::string_view cache_name)
{
    return entangling_controls::find<entangling>(cache_name);
}

entangling::knobs
entangling::get_knobs() const
{
    return settings;
}

void
entangling::set_knobs(knobs values)
{
    settings = {values.enabled, std::clamp(values.confidence_threshold, 1, 3),
                std::clamp(values.destination_cap, 1, 6)};
    if (impl) {
        impl->settings = settings;
    }
}

void
entangling::prefetcher_initialize()
{
    if (!intern_->virtual_prefetch || BLOCK_SIZE != 64 ||
        intern_->NUM_SET > 64) {
        throw std::invalid_argument(
            "Entangling requires virtual_prefetch=true, 64-byte lines, and at "
            "most 64 L1I sets");
    }
    impl = std::make_unique<state>(intern_);
    impl->initialize();
    using entangling_controls::env_int;
    set_knobs(
        {env_int("EIP_ENABLED", settings.enabled) != 0,
         env_int("EIP_CONFIDENCE_THRESHOLD", settings.confidence_threshold),
         env_int("EIP_DESTINATION_CAP", settings.destination_cap)});
    entangling_controls::instances<entangling>()[intern_->NAME] = this;
}
uint32_t
entangling::prefetcher_cache_operate(champsim::address addr, champsim::address,
                                     bool hit, bool useful, access_type type,
                                     uint32_t metadata)
{
    if (type == access_type::LOAD) {
        impl->observe(addr.to<uint64_t>(), hit, useful);
    }
    return metadata;
}
uint32_t
entangling::prefetcher_cache_fill(champsim::address addr, long set, long way,
                                  bool pf, champsim::address evicted,
                                  uint32_t metadata)
{
    if (way >= 0 && static_cast<uint32_t>(way) < intern_->NUM_WAY) {
        impl->fill_way = static_cast<uint32_t>(way);
        impl->fill(addr.to<uint64_t>(), static_cast<uint32_t>(set),
                   static_cast<uint32_t>(way), pf, evicted.to<uint64_t>());
    }
    return metadata;
}
void
entangling::prefetcher_cycle_operate()
{
    impl->tick();
}
void
entangling::prefetcher_final_stats()
{
    impl->final_stats();
}
