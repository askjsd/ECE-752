/* Mixed workloads: phase changes (FDP), hot set + cold pollution, fill-level placement. */
#define _GNU_SOURCE
#include "pfb.h"
#include <string.h>
#include <time.h>

/* Random single-use "runs": slot -> run of `run` consecutive lines at a random place,
 * each slot used once per cycle through the region (reuse distance = region). */
typedef struct { uint8_t *base; uint64_t slots; int bits; uint64_t key, ctr; long run; } runs_t;
static runs_t runs_make(long lines, long run, const char *name) {
    runs_t r;
    if (run < 1) die("run length must be >= 1");
    r.base = pfb_alloc((size_t)lines * G.line, name);
    r.run = run; r.slots = (uint64_t)(lines / run);
    if (r.slots < 2) die("%s: region too small for runs of %ld lines", name, run);
    r.bits = log2_ceil(r.slots); r.key = G.seed * 0x9E3779B97F4A7C15ULL; r.ctr = 0;
    return r;
}
static inline const uint8_t *runs_next(runs_t *r) {
    uint64_t s = perm_n(r->ctr++ % r->slots, r->slots, r->bits, r->key);
    return r->base + s * (uint64_t)r->run * G.line;
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static double now_ns(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1e9 / (double)freq.QuadPart;
}
#else
static double now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
#endif

/* Phase-changing workload: random, stream, random, stream, ...
 * A purely random phase gives a stream/stride prefetcher nothing to train on, so it
 * issues nothing and there is nothing to throttle. The random phase therefore uses
 * short runs (--rand-run-lines, default 2) at random single-use places: they trigger
 * training and produce inaccurate prefetches, which FDP should detect (low accuracy,
 * pollution) and throttle; the stream phase should ramp it back up. Phases must be
 * long compared with the controller's sampling interval (FDP: 8192 L2 evictions),
 * so the default is 2^18 accesses per phase. */
int k_phase(void) {
    long acc   = cli_long("phase-accesses", 1L << 18, "accesses per phase");
    long np    = cli_long("phases", 5, "number of phases (random first, then alternating)");
    long rrun  = cli_long("rand-run-lines", 2, "lines per random burst (1 = isolated random lines)");
    long rl    = cli_long("region-lines", big_lines(), "size of each phase's region in lines");
    long first = cli_long("start-with-stream", 0, "1 = begin with a stream phase");
    cli_done();
    runs_t rnd = runs_make(rl, rrun, "random");
    uint8_t *sb = pfb_alloc((size_t)rl * G.line, "stream");
    result_kv("phase_accesses", "%ld", acc); result_kv("phases", "%ld", np);
    result_kv("rand_run_lines", "%ld", rrun); result_kv("region_lines", "%ld", rl);
    double t[64]; if (np > 63) die("--phases <= 63");
    uint64_t sum = 0, n = 0; long soff = 0;
    pfb_roi_begin();
    t[0] = now_ns();
    for (long ph = 0; ph < np; ph++) {
        if (((ph & 1) == 0) != (first != 0)) {          /* random phase */
            for (long i = 0; i < acc; i += rrun) {
                const uint8_t *q = runs_next(&rnd);
                for (long r = 0; r < rrun; r++) sum += LD(q + r * G.line);
            }
        } else {                                       /* stream phase */
            for (long i = 0; i < acc; i++) {
                sum += LD(sb + soff);
                soff += G.line; if (soff >= rl * G.line) soff = 0;
            }
        }
        n += (uint64_t)acc;
        t[ph + 1] = now_ns();
    }
    pfb_roi_end(n, sum);
    for (long ph = 0; ph < np; ph++)
        printf("PHASE %ld kind=%s accesses=%ld ns_per_access=%.3f\n", ph,
               (((ph & 1) == 0) != (first != 0)) ? "random" : "stream", acc, (t[ph + 1] - t[ph]) / acc);
    return 0;
}

/* Hot set + cold pollution. A hot set (default half of L1) is read cyclically; every
 * --cold-every hot accesses one cold run is read at a random single-use place.
 * Defaults are chosen so that demand traffic ALONE still fits: the hot lines' reuse
 * distance is hot + hot/cold_every*cold_run = 256 + 64*2 = 384 lines (75% of 512). Only
 * prefetches for the cold traffic can push it past L1 and evict hot lines.
 * --cold-run-lines=1 gives pure random cold lines (nothing to train on: pollution
 * only from a prefetcher that issues on every miss); >1 gives short runs that bait a
 * stream prefetcher into prefetching past their end. Compare with --cold-every=0. */
int k_pollute(void) {
    long hot   = cli_long("hot-lines", G.l1 / 2, "hot working set in lines (default half of L1)");
    long ce    = cli_long("cold-every", 4, "hot accesses per cold run (0 = no cold traffic)");
    long crun  = cli_long("cold-run-lines", 2, "lines per cold run (1 = isolated random lines)");
    long cl    = cli_long("cold-lines", big_lines(), "cold region in lines");
    long acc   = cli_long("hot-accesses", 1L << 19, "hot accesses in the ROI");
    long warm  = cli_long("warmup", 2, "passes over the hot set before the ROI");
    cli_done();
    if (hot < 1) die("--hot-lines must be >= 1");
    uint8_t *hb = pfb_alloc((size_t)hot * G.line, "hot");
    runs_t cold = runs_make(cl, crun, "cold");
    double rd = hot + (ce ? (double)hot / ce * crun : 0);
    const char *lvl = rd <= G.l1 ? "L1" : rd <= G.l2 ? "L2" : rd <= G.l3 ? "L3" : "memory";
    info("hot reuse distance with demand traffic only: %.0f lines (L1=%ld, L2=%ld) -> fits %s", rd, G.l1, G.l2, lvl);
    result_kv("hot_lines", "%ld", hot); result_kv("cold_every", "%ld", ce); result_kv("cold_run_lines", "%ld", crun);
    result_kv("demand_rd_lines", "%.0f", rd); result_kv("hot_fits", "%s", lvl);

    uint64_t sum = 0, n = 0;
    for (long w = 0; w < warm; w++) for (long h = 0; h < hot; h++) sum += LD(hb + h * G.line);
    long h = 0, c = ce;
    pfb_roi_begin();
    for (long i = 0; i < acc; i++) {
        sum += LD(hb + h * G.line);
        if (++h == hot) h = 0;
        if (ce && --c == 0) {
            c = ce;
            const uint8_t *q = runs_next(&cold);
            for (long r = 0; r < crun; r++) sum += LD(q + r * G.line);
            n += (uint64_t)crun;
        }
    }
    n += (uint64_t)acc;
    pfb_roi_end(n, sum);
    return 0;
}

/* Fill-level placement. Four 2 MiB-aligned regions, each read by its own load
 * instruction, interleaved on a "tick" schedule:
 *   every tick       R1 (L1 target)  next line
 *   every p2 ticks   R2 (L2 target)  next line
 *   every p3 ticks   R3 (L3 target)  next line
 *   every ps ticks   RS (one-shot)   a never-reused line
 * The reuse distance of a region (distinct lines touched between two touches of the
 * same line, counting every region since demand misses fill all levels) is
 *   RD1 = r1 + r1/p2 + r1/p3 + r1/ps
 *   RD2 = r1 + r2 + r2*p2/p3 + r2*p2/ps
 *   RD3 = r1 + r2 + r3 + r3*p3/ps
 * and the defaults give RD1 = 404 (79% of L1), RD2 = 2624 (64% of L2), RD3 = 12544
 * (77% of L3). The program prints these and warns when a region misses its level.
 *
 * Aggregate miss rates cannot say WHICH region missed: bin simulator misses by
 * address with the printed REGION ranges, or compare runs with regions disabled
 * (--p2/--p3/--ps = 0). With --one-shot=seq the one-shot region is a perfect
 * stream: the right policy is to prefetch it but insert at low priority (or
 * non-temporally), not to skip it. With --one-shot=random it is unpredictable and
 * should not trigger prefetches at all. */
int k_filllevel(void) {
    long r1 = cli_long("r1-lines", G.l1 / 2, "L1-target region in lines (default L1/2 = 256)");
    long r2 = cli_long("r2-lines", G.l2 / 2, "L2-target region in lines (default L2/2 = 2048)");
    long r3 = cli_long("r3-lines", G.l3 / 2, "L3-target region in lines (default L3/2 = 8192)");
    long p2 = cli_long("p2", 2, "ticks per R2 access (0 = disable R2)");
    long p3 = cli_long("p3", 16, "ticks per R3 access (0 = disable R3)");
    long ps = cli_long("ps", 64, "ticks per one-shot access (0 = disable)");
    const char *order = cli_str("order", "seq", "seq | random: order within R1/R2/R3");
    const char *oneshot = cli_str("one-shot", "seq", "seq | random: one-shot region pattern");
    long ticks = cli_long("ticks", 0, "ticks in the ROI (0 = 4 full R3 cycles)");
    long warm = cli_long("warmup", 0, "ticks before the ROI (0 = one full R3 cycle)");
    cli_done();
    int rnd = !strcmp(order, "random"), srnd = !strcmp(oneshot, "random");
    if (!rnd && strcmp(order, "seq")) die("--order must be seq or random");
    if (!srnd && strcmp(oneshot, "seq")) die("--one-shot must be seq or random");
    if (r1 < 1 || r2 < 1 || r3 < 1 || p2 < 0 || p3 < 0 || ps < 0) die("bad region sizes/periods");
    long cyc3 = r3 * (p3 ? p3 : 1);
    if (ticks == 0) ticks = 4 * cyc3;
    if (warm == 0) warm = cyc3;

    /* expected reuse distances */
    double f2 = p2 ? 1.0 / p2 : 0, f3 = p3 ? 1.0 / p3 : 0, fs = ps ? 1.0 / ps : 0;
    double rd1 = r1 * (1 + f2 + f3 + fs);
    double rd2 = p2 ? r1 + r2 + r2 * p2 * (f3 + fs) : 0;
    double rd3 = p3 ? r1 + (p2 ? r2 : 0) + r3 + r3 * p3 * fs : 0;
    struct { const char *n; double rd; long cap, lower; int on; } rg[3] = {
        {"R1", rd1, G.l1, 0, 1}, {"R2", rd2, G.l2, G.l1, p2 != 0}, {"R3", rd3, G.l3, G.l2, p3 != 0}};
    for (int i = 0; i < 3; i++) {
        if (!rg[i].on) continue;
        info("%s reuse distance %.0f lines: %.0f%% of its level (%ld lines)%s", rg[i].n, rg[i].rd,
             100 * rg[i].rd / rg[i].cap, rg[i].cap, "");
        if (rg[i].rd > 0.9 * rg[i].cap) warn("%s reuse distance exceeds 90%% of its target level: expect conflict misses", rg[i].n);
        if (rg[i].rd <= rg[i].lower) warn("%s fits in the level above its target (%.0f <= %ld lines)", rg[i].n, rg[i].rd, rg[i].lower);
    }
    result_kv("rd1", "%.0f", rd1); result_kv("rd2", "%.0f", rd2); result_kv("rd3", "%.0f", rd3);
    result_kv("order", "%s", order); result_kv("one_shot", "%s", oneshot);
    result_kv("p2", "%ld", p2); result_kv("p3", "%ld", p3); result_kv("ps", "%ld", ps);

    uint8_t *b1 = pfb_alloc((size_t)r1 * G.line, "R1_L1");
    uint8_t *b2 = pfb_alloc((size_t)r2 * G.line, "R2_L2");
    uint8_t *b3 = pfb_alloc((size_t)r3 * G.line, "R3_L3");
    long sl = ps ? (ticks + warm) / ps + 2 : 1;
    uint8_t *bs = pfb_alloc((size_t)sl * G.line, "RS_oneshot");
    const int k1 = log2_ceil(r1), k2 = log2_ceil(r2), k3 = log2_ceil(r3), ks = log2_ceil(sl);
    const uint64_t key = G.seed * 0xD1B54A32D192ED03ULL;

    long c1 = 0, c2 = 0, c3 = 0, cs = 0;           /* positions within each region */
    long t2 = p2, t3 = p3, ts = ps;                /* countdowns */
    uint64_t sum = 0, n = 0;
#define FL_TICK() do {                                                                          \
        sum += LD(b1 + (rnd ? perm_n(c1, r1, k1, key) : (uint64_t)c1) * G.line);                          \
        if (++c1 == r1) c1 = 0;                                                                 \
        if (p2 && --t2 == 0) { t2 = p2; n++;                                                    \
            sum += LD(b2 + (rnd ? perm_n(c2, r2, k2, key) : (uint64_t)c2) * G.line); if (++c2 == r2) c2 = 0; } \
        if (p3 && --t3 == 0) { t3 = p3; n++;                                                    \
            sum += LD(b3 + (rnd ? perm_n(c3, r3, k3, key) : (uint64_t)c3) * G.line); if (++c3 == r3) c3 = 0; } \
        if (ps && --ts == 0) { ts = ps; n++;                                                    \
            sum += LD(bs + (srnd ? perm_n(cs, sl, ks, key) : (uint64_t)cs) * G.line); cs++; }             \
    } while (0)
    for (long t = 0; t < warm; t++) FL_TICK();
    n = 0;
    pfb_roi_begin();
    for (long t = 0; t < ticks; t++) FL_TICK();
    pfb_roi_end(n + (uint64_t)ticks, sum);
#undef FL_TICK
    return 0;
}
