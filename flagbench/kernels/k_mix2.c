/* k_mix2.c - Amplified flag-sensitivity kernel for massive cycle spread across compile flags.
 *
 * Design goals:
 *   * Wide cycle range between best and worst flag configurations.
 *   * Phases with opposing flag preferences (vectorize helps phases 1/2/9,
 *     but hurts phase 5's scalar bitwise loop; loop-interchange helps phase
 *     7 but can hurt phase 8's strided access; unrolling helps most phases
 *     but increases code size). The optimum is a non-obvious trade-off.
 *   * Large working sets and high iteration counts so flag effects dominate
 *     noise and measurement error.
 *
 * Correctness (nothing risky):
 *   * Fixed-seed xorshift32 inputs -> fully deterministic.
 *   * All arrays statically allocated, every index provably in bounds.
 *   * No signed overflow, no uninitialized reads, no undefined behavior.
 *   * FNV-1a checksum over every phase's output -> single pass/fail criterion.
 *   * -O0 native build is the reference; any flag configuration must produce
 *     the identical checksum or the run is flagged incorrect.
 *
 * -O3 is the reference baseline (already supported by run_carbs.py).
 * Run with the full 12-flag space (--nflags 12, 4096 configs/kernel).
 * Binary deduplication in run_carbs.py means only unique .text hashes are
 * traced and simulated; parallel compilation via --jobs.
 */

#include "common.h"
#include <string.h>

/* ---- phase 1: 2D stencil (128x128 x 60 iters) ----
 * Sensitive: tree-loop-vectorize, tree-slp-vectorize, unroll-loops, ivopts.
 * Wants these ON. Two arrays of 64 KiB each -> L2-resident. */
#define SW1 128
#define SH1 128
#define IT1 60
static float sa1[SH1][SW1], sb1[SH1][SW1];

/* ---- phase 2: large reduction + prefix sum (512K ints) ----
 * Sensitive: unroll-loops, tree-loop-vectorize, schedule-insns, ivopts.
 * Wants these ON. 2 MiB array -> exceeds L2, memory-bandwidth bound. */
#define RN2 524288
static int32_t arr2[RN2];

/* ---- phase 3: pointer chase (control kernel, ~flag-insensitive) ----
 * 16K nodes x 64 B = 1 MiB, larger than L2. Memory-latency bound. */
#define PN3 16384
typedef struct node3 { struct node3 *next; uint32_t val; char pad[56]; } node3_t;
static node3_t nodes3[PN3];
static uint32_t perm3[PN3];

/* ---- phase 4: branchy bytecode interpreter ----
 * Data-dependent switch over random opcodes. Branch-prediction / cmov
 * sensitive, but GCC flags have limited effect here -> mostly control. */
#define LEN4 65536
static uint8_t code4[LEN4];

/* ---- phase 5: CRC32 bitwise (144K bytes) ----
 * Tight shift/xor loop. Sensitive: schedule-insns, unroll-loops.
 * Vectorization can hurt (scalar bitwise dependency chain) -> conflict
 * with phases 1/2/9 that want vectorize ON. */
#define LEN5 144000
static uint8_t buf5[LEN5];

/* ---- phase 6: hot helper calls (64K data x 40 rounds) ----
 * Many calls to small leaf functions. Sensitive: inline-functions,
 * ipa-cp-clone. Wants these ON. Code-size increase from inlining can
 * conflict with unrolling-heavy phases (icache pressure). */
#define N6 65536
static uint32_t data6[N6];

/* ---- phase 7: column-major matrix access (256x256 x 40 rounds) ----
 * Stride-256 stores -> terrible cache behaviour without loop interchange.
 * Sensitive: loop-interchange. Wants ON; conflicts with phase 8's
 * strided access where interchange can hurt. */
#define IR7 256
#define IC7 256
static int32_t im7[IR7][IC7], io7[IR7][IC7];

/* ---- phase 8: strided AXPY (32K x 2 floats x 200 rounds) ----
 * Runtime stride = 1 default. Sensitive: version-loops-for-strides.
 * Wants ON for non-unit strides; interchange (phase 7) can make this
 * worse if applied broadly -> another conflict. */
#define TN8 32768
static float ta8[TN8 * 2], tb8[TN8 * 2];

/* ---- phase 9: dense 64x64 FP matrix multiply ----
 * O(n^3) float multiply-accumulate. Benefits hugely from
 * tree-loop-vectorize + tree-slp-vectorize + unroll-loops.
 * This phase alone can give 4-8x speedup with the right flags ->
 * dominant contributor to cycle spread. Two 64x64 matrices = 32 KiB each. */
#define FM 64
static float ma[FM][FM], mb[FM][FM], mc[FM][FM];

/* ---- phase 10: IPA stress (many small leaf functions on 16K data) ----
 * 16 tiny functions called in a loop. Sensitive: ipa-cp-clone,
 * inline-functions, partial inlining. Wants ON; code-size pressure
 * conflicts with unroll-heavy phases. */
#define N10 16384
static uint32_t val10[N10];

/* ---------- helpers ---------- */

static uint32_t mix32(uint32_t v, uint32_t k) {
    v ^= v >> 16; v *= 0x85ebca6bu; v ^= k; v ^= v >> 13; v *= 0xc2b2ae35u; return v ^ (v >> 16);
}

static uint32_t clampsum32(uint32_t a, uint32_t b) {
    uint32_t s = a + b; /* unsigned: well-defined wrap */
    return s < a ? 0xffffffffu : s;
}

__attribute__((noinline)) static uint32_t leaf_a(uint32_t v, uint32_t k) { return mix32(v, k) + k; }
__attribute__((noinline)) static uint32_t leaf_b(uint32_t v, uint32_t k) { return mix32(v, k) ^ k; }
__attribute__((noinline)) static uint32_t leaf_c(uint32_t v, uint32_t k) { return clampsum32(v, k); }
__attribute__((noinline)) static uint32_t leaf_d(uint32_t v, uint32_t k) { return mix32(v, k) - k; }
__attribute__((noinline)) static uint32_t leaf_e(uint32_t v, uint32_t k) { return clampsum32(v ^ k, k); }
__attribute__((noinline)) static uint32_t leaf_f(uint32_t v, uint32_t k) { return mix32(v, k) * 3u; }
__attribute__((noinline)) static uint32_t leaf_g(uint32_t v, uint32_t k) { return v + mix32(k, v); }
__attribute__((noinline)) static uint32_t leaf_h(uint32_t v, uint32_t k) { return clampsum32(v, mix32(k, v)); }
__attribute__((noinline)) static uint32_t leaf_i(uint32_t v, uint32_t k) { return (v & k) | mix32(v, k); }
__attribute__((noinline)) static uint32_t leaf_j(uint32_t v, uint32_t k) { return (v | k) ^ mix32(v, k); }
__attribute__((noinline)) static uint32_t leaf_k(uint32_t v, uint32_t k) { return (v ^ k) + clampsum32(v, k); }
__attribute__((noinline)) static uint32_t leaf_l(uint32_t v, uint32_t k) { return mix32(v ^ k, k); }
__attribute__((noinline)) static uint32_t leaf_m(uint32_t v, uint32_t k) { return clampsum32(mix32(v, k), k); }
__attribute__((noinline)) static uint32_t leaf_n(uint32_t v, uint32_t k) { return v * 31u + mix32(k, v); }
__attribute__((noinline)) static uint32_t leaf_o(uint32_t v, uint32_t k) { return mix32(v, k * 2u); }

__attribute__((noinline)) static void ph_stencil(void) {
    for (int it = 0; it < IT1; it++) {
        for (int i = 1; i < SH1 - 1; i++)
            for (int j = 1; j < SW1 - 1; j++)
                sb1[i][j] = 0.25f * (sa1[i-1][j] + sa1[i+1][j] + sa1[i][j-1] + sa1[i][j+1]);
        for (int i = 1; i < SH1 - 1; i++)
            for (int j = 1; j < SW1 - 1; j++)
                sa1[i][j] = sb1[i][j];
    }
}

static int64_t g_total2;
__attribute__((noinline)) static void ph_reduce(void) {
    int64_t total = 0;
    for (int i = 0; i < RN2; i++) total += arr2[i];
    int32_t run = 0;
    for (int i = 0; i < RN2; i++) { run += arr2[i]; arr2[i] = run; }
    for (int i = 0; i < RN2; i++) total ^= (int64_t)(uint32_t)arr2[i];
    g_total2 = total;
}

static uint64_t g_chase2;
__attribute__((noinline)) static void ph_ptrchase(void) {
    node3_t *p = &nodes3[perm3[0]];
    uint64_t sum = 0;
    for (int s = 0; s < 600000; s++) {
        sum += p->val;
        p = p->next;
    }
    g_chase2 = sum;
}

static uint32_t g_branch2;
__attribute__((noinline)) static void ph_branchy(void) {
    uint32_t acc = 1, x = 7, y = 3;
    for (int pass = 0; pass < 5; pass++) {
        for (int pc = 0; pc < LEN4; pc++) {
            uint8_t op = code4[pc];
            switch (op & 7) {
            case 0: acc += x; break;
            case 1: acc ^= (acc << 3) + y; break;
            case 2: x = x * 33u + (op >> 3); break;
            case 3: if (acc & 1) y += x; else y ^= acc; break;
            case 4: acc = (acc >> 1) | (acc << 31); break;
            case 5: if ((x ^ y) & 4) acc -= y; break;
            case 6: x += (op & 0x10) ? acc : y; break;
            default: y = (y << 1) ^ (op >> 4); break;
            }
        }
    }
    g_branch2 = acc;
}

static uint32_t g_crc2;
__attribute__((noinline)) static void ph_crc(void) {
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < LEN5; i++) {
        crc ^= buf5[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    g_crc2 = ~crc;
}

static uint32_t g_acc2;
__attribute__((noinline)) static void ph_helper(void) {
    uint32_t acc = 0;
    for (int r = 0; r < 40; r++)
        for (int i = 0; i < N6; i++)
            acc = clampsum32(acc, mix32(data6[i], (uint32_t)r));
    g_acc2 = acc;
}

static int64_t g_ipa2;
__attribute__((noinline)) static void ph_interchange(void) {
    int64_t acc = 0;
    for (int r = 0; r < 40; r++)
        for (int j = 0; j < IC7; j++)
            for (int i = 0; i < IR7; i++)
                acc += (int64_t)io7[i][j] + ((int64_t)im7[i][j] * 3) + ((int64_t)im7[i][j] >> 1);
    g_ipa2 = acc;
}

static int g_stride2;
__attribute__((noinline)) static void axpy2(float *x, const float *y, int n, int s) {
    for (int i = 0; i < n; i++) x[i * s] += 0.5f * y[i * s];
}
__attribute__((noinline)) static void ph_stride(void) {
    int s = g_stride2;
    for (int r = 0; r < 200; r++) axpy2(ta8, tb8, TN8, s);
}

static int64_t g_fmat2;
__attribute__((noinline)) static void ph_fmatmul(void) {
    for (int i = 0; i < FM; i++)
        for (int j = 0; j < FM; j++)
            mc[i][j] = 0.0f;
    for (int k = 0; k < FM; k++)
        for (int i = 0; i < FM; i++)
            for (int j = 0; j < FM; j++)
                mc[i][j] += ma[i][k] * mb[k][j];
    int64_t acc = 0;
    for (int i = 0; i < FM; i++)
        for (int j = 0; j < FM; j++) {
            union { float f; uint32_t u; } v = { mc[i][j] };
            acc += (int64_t)(int32_t)v.u;
        }
    g_fmat2 = acc;
}

static uint32_t g_ipa10;
__attribute__((noinline)) static void ph_ipa_stress(void) {
    static uint32_t (*const leaves[16])(uint32_t, uint32_t) = {
        leaf_a, leaf_b, leaf_c, leaf_d, leaf_e, leaf_f, leaf_g, leaf_h,
        leaf_i, leaf_j, leaf_k, leaf_l, leaf_m, leaf_n, leaf_o, leaf_a,
    };
    uint32_t acc = 0;
    for (int i = 0; i < N10; i++) {
        uint32_t k = val10[i];
        for (int f = 0; f < 16; f++)
            acc = clampsum32(acc, leaves[f](acc, k));
    }
    g_ipa10 = acc;
}

/* ---------- main ---------- */

static uint64_t fnv64(uint64_t c, uint64_t v) { return (c ^ v) * 1099511628211ull; }

int main(void) {
    uint32_t seed = 0xC0FFEE2u;

    /* Phase 1: stencil data */
    for (int i = 0; i < SH1; i++)
        for (int j = 0; j < SW1; j++)
            sa1[i][j] = (float)(rng_next(&seed) & 1023u) * (1.0f / 64.0f);
    memcpy(sb1, sa1, sizeof sa1);

    /* Phase 2: reduction data */
    for (int i = 0; i < RN2; i++) arr2[i] = (int32_t)(rng_next(&seed) & 0xffff) - 32768;

    /* Phase 3: pointer chase */
    for (uint32_t i = 0; i < PN3; i++) perm3[i] = i;
    for (uint32_t i = PN3 - 1; i > 0; i--) {
        uint32_t j = rng_next(&seed) % (i + 1);
        uint32_t t = perm3[i]; perm3[i] = perm3[j]; perm3[j] = t;
    }
    for (uint32_t i = 0; i < PN3; i++) {
        nodes3[perm3[i]].next = &nodes3[perm3[(i + 1) % PN3]];
        nodes3[perm3[i]].val = rng_next(&seed);
    }

    /* Phase 4: branchy opcodes */
    for (int i = 0; i < LEN4; i++) code4[i] = (uint8_t)rng_next(&seed);

    /* Phase 5: CRC data */
    for (int i = 0; i < LEN5; i++) buf5[i] = (uint8_t)rng_next(&seed);

    /* Phase 6: helper data */
    for (int i = 0; i < N6; i++) data6[i] = rng_next(&seed);

    /* Phase 7: interchange matrices */
    for (int i = 0; i < IR7; i++)
        for (int j = 0; j < IC7; j++) im7[i][j] = (int32_t)(rng_next(&seed) & 0xff);
    memcpy(io7, im7, sizeof im7);

    /* Phase 8: stride data */
    volatile int vs = 1; g_stride2 = vs;
    for (int i = 0; i < TN8 * 2; i++) { ta8[i] = (float)(rng_next(&seed) & 255); tb8[i] = (float)(rng_next(&seed) & 255); }

    /* Phase 9: FP matmul data */
    for (int i = 0; i < FM; i++)
        for (int j = 0; j < FM; j++) {
            ma[i][j] = (float)(rng_next(&seed) & 255) * (1.0f / 16.0f);
            mb[i][j] = (float)(rng_next(&seed) & 255) * (1.0f / 16.0f);
        }

    /* Phase 10: IPA stress data */
    for (int i = 0; i < N10; i++) val10[i] = rng_next(&seed);

    ROI_BEGIN();
    ph_stencil();
    ph_reduce();
    ph_ptrchase();
    ph_branchy();
    ph_crc();
    ph_helper();
    ph_interchange();
    ph_stride();
    ph_fmatmul();
    ph_ipa_stress();
    ROI_END();

    uint64_t c = 1469598103934665603ull;
    for (int i = 0; i < SH1; i++)
        for (int j = 0; j < SW1; j++) { union { float f; uint32_t u; } v = { sa1[i][j] }; c = fnv64(c, v.u); }
    c = fnv64(c, (uint64_t)g_total2);
    c = fnv64(c, g_chase2);
    c = fnv64(c, g_branch2);
    c = fnv64(c, g_crc2);
    c = fnv64(c, g_acc2);
    c = fnv64(c, g_ipa2);
    c = fnv64(c, g_fmat2);
    c = fnv64(c, g_ipa10);
    for (int i = 0; i < IR7; i++)
        for (int j = 0; j < IC7; j++) c = fnv64(c, (uint32_t)io7[i][j]);
    for (int i = 0; i < TN8; i++) { union { float f; uint32_t u; } v = { ta8[i] }; c = fnv64(c, v.u); }
    print_checksum(c);
    return 0;
}
