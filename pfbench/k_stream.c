/* Streams: forward (control), reverse, stream with holes, and the late-prefetch loop. */
#include "pfb.h"
#include <string.h>

static long stream_region(void) { return cli_long("region-lines", big_lines(), "stream length in lines (default --big-lines)"); }

int k_stream(void) {
    long rl = stream_region();
    long step = cli_long("step-bytes", 8, "bytes between accesses (8 = every word, 64 = one per line)");
    long passes = cli_long("passes", 1, "passes over the region in the ROI");
    cli_done();
    if (step <= 0) die("--step-bytes must be > 0");
    const long R = rl * G.line;
    uint8_t *base = pfb_alloc((size_t)R, "stream");
    result_kv("region_lines", "%ld", rl); result_kv("step_bytes", "%ld", step);
    uint64_t sum = 0, n = 0;
    pfb_roi_begin();
    for (long p = 0; p < passes; p++)
        for (long off = 0; off < R; off += step) { sum += LD(base + off); n++; }
    pfb_roi_end(n, sum);
    return 0;
}

int k_stream_rev(void) {
    long rl = stream_region();
    long step = cli_long("step-bytes", 8, "bytes between accesses (8 = every word, 64 = one per line)");
    long passes = cli_long("passes", 1, "passes over the region in the ROI");
    cli_done();
    if (step <= 0) die("--step-bytes must be > 0");
    const long R = rl * G.line;
    uint8_t *base = pfb_alloc((size_t)R, "stream");
    result_kv("region_lines", "%ld", rl); result_kv("step_bytes", "%ld", step);
    uint64_t sum = 0, n = 0;
    pfb_roi_begin();
    for (long p = 0; p < passes; p++)
        for (long off = R - step; off >= 0; off -= step) { sum += LD(base + off); n++; }
    pfb_roi_end(n, sum);
    return 0;
}

/* Stream with holes: a timeliness / prefetch-distance test (too EARLY).
 *
 * The stream reads one line, then --gap unique random lines (the "hole"), then the
 * next stream line, and so on. The gap lines are single-use and random, so they
 * miss and each one inserts a line into L1. A stream line prefetched d steps before
 * it is used must survive d*(gap+1) insertions; with LRU it survives only while
 * d*(gap+1) < L1 lines. So the useful distance is bounded ABOVE:
 *     max distance ~= L1 / (gap + 1)      (default gap = L1/4 = 128 -> d <= 3)
 * A prefetcher that runs further ahead fetches lines that are evicted before use
 * (counted as useless prefetches, and the stream line misses anyway). The gap
 * accesses are independent loads that overlap each other, so each stream step is
 * long and distance 1 is already timely: lateness is not what is tested here (see
 * `late` for that).
 *
 * The stream and the gap use different load instructions, so a PC-indexed
 * prefetcher sees a clean +1-line stream; a PC-blind stream prefetcher has to find
 * it among the random lines. --hole-lines additionally skips lines in the stream
 * itself (spatial holes: periodic, or random with --random-holes=1). */
int k_stream_holes(void) {
    long rl   = stream_region();
    long gap  = cli_long("gap", G.l1 / 4, "unique random accesses between consecutive stream lines (default L1/4)");
    long gl   = cli_long("gap-lines", big_lines(), "region the random gap accesses come from");
    long sacc = cli_long("stream-accesses", 4096, "stream lines read in the ROI");
    long run  = cli_long("run-lines", 1, "spatial holes: stream lines read between skipped lines");
    long hole = cli_long("hole-lines", 0, "spatial holes: stream lines skipped per hole (0 = none)");
    long rnd  = cli_long("random-holes", 0, "1 = random spatial-hole positions with the same density");
    long warm = cli_long("warmup", 64, "stream steps (with gaps) before the ROI, to train the prefetcher");
    cli_done();
    if (gap < 0 || run < 1 || hole < 0 || sacc < 1) die("need --gap >= 0, --run-lines >= 1, --hole-lines >= 0");
    if (gl < 2 * G.l1) warn("--gap-lines (%ld) should be much larger than L1 so gap lines always miss", gl);
    const long period = run + hole;
    const long dmax = G.l1 / (gap + 1);
    if (dmax == 0) warn("gap >= L1: even a distance-1 prefetch is evicted before use");
    info("a prefetched stream line survives in L1 only if issued <= ~%ld stream steps ahead", dmax);
    uint8_t *sb = pfb_alloc((size_t)rl * G.line, "stream");
    uint8_t *gb = pfb_alloc((size_t)gl * G.line, "gap");
    const int gbits = log2_ceil((uint64_t)gl);
    const uint64_t key = G.seed * 0x9E3779B97F4A7C15ULL;
    result_kv("gap", "%ld", gap); result_kv("max_useful_distance", "%ld", dmax);
    result_kv("run_lines", "%ld", run); result_kv("hole_lines", "%ld", hole); result_kv("random_holes", "%ld", rnd);

    uint64_t sum = 0, gctr = 0;
    long l = 0, pos = 0;
#define HOLE_STEP() do {                                                                       \
        for (;;) {          /* next stream line that is not a spatial hole */                  \
            int keep = rnd ? perm_bits((uint64_t)l, 40, G.seed) % (uint64_t)period < (uint64_t)run \
                           : pos < run;                                                        \
            if (++pos == period) pos = 0;                                                       \
            long cur = l; if (++l == rl) l = 0;                                                 \
            if (keep) { sum += LD(sb + cur * G.line); break; }                                  \
        }                                                                                       \
        for (long g = 0; g < gap; g++)                                                          \
            sum += LD(gb + perm_n(gctr++ % (uint64_t)gl, (uint64_t)gl, gbits, key) * G.line);   \
    } while (0)
    for (long s = 0; s < warm; s++) HOLE_STEP();
    pfb_roi_begin();
    for (long s = 0; s < sacc; s++) HOLE_STEP();
#undef HOLE_STEP
    pfb_roi_end((uint64_t)sacc * (uint64_t)(gap + 1), sum);
    return 0;
}

/* Late prefetching. One access per line (--step-bytes=64) and no compute, so a new
 * line is demanded every couple of cycles; reading every word instead would give the
 * prefetcher 8x more time per line. Demand catches up with in-flight prefetches
 * unless distance >= memory latency / time per line. --work adds a chain of
 * dependent ALU ops per access to sweep how much slack the prefetcher gets. */
int k_late(void) {
    long rl = stream_region();
    long step = cli_long("step-bytes", G.line, "bytes between accesses (default one per line)");
    long work = cli_long("work", 0, "dependent multiply-add ops per access");
    long passes = cli_long("passes", 1, "passes over the region in the ROI");
    cli_done();
    if (step <= 0) die("--step-bytes must be > 0");
    const long R = rl * G.line;
    uint8_t *base = pfb_alloc((size_t)R, "stream");
    result_kv("region_lines", "%ld", rl); result_kv("step_bytes", "%ld", step); result_kv("work", "%ld", work);
    uint64_t sum = 0, n = 0;
    pfb_roi_begin();
    for (long p = 0; p < passes; p++) {
        if (work == 0) {
            for (long off = 0; off < R; off += step) sum += LD(base + off);
        } else {
            for (long off = 0; off < R; off += step) {
                uint64_t v = LD(base + off);
                for (long w = 0; w < work; w++) { v = v * 6364136223846793005ULL + 1; KEEP(v); }
                sum += v;
            }
        }
        n += (uint64_t)((R + step - 1) / step);
    }
    pfb_roi_end(n, sum);
    return 0;
}
