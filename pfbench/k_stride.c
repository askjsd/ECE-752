/* Stride pathologies. All strides here are in bytes or cache lines, never array
 * elements: prefetchers train on line addresses, so an element stride smaller than a
 * line just looks like a sequential stream.
 *
 * stride, stride_overflow, stride_alt and stride_zigzag share ONE loop with ONE load
 * instruction, so a PC-indexed prefetcher sees the whole pattern on a single PC
 * (an unrolled loop would give each PC a constant stride and hide the pattern). */
#include "pfb.h"
#include <string.h>

static long walk_params(long *region_lines, long *shift_lines) {
    long acc      = cli_long("accesses", big_lines(), "accesses in the ROI");
    *region_lines = cli_long("region-lines", big_lines(), "address range walked before wrapping (default --big-lines)");
    *shift_lines  = cli_long("wrap-shift-lines", 3, "lines the start shifts by on each wrap, so a wrapped walk touches new lines");
    return acc;
}

/* Walk with per-step deltas alternating d0, d1 (bytes; equal for a constant stride). */
static int walk(long acc, long region_lines, long shift_lines, int64_t d0, int64_t d1) {
    const int64_t R = (int64_t)region_lines * G.line;
    const int64_t span = (d0 < 0 ? -d0 : d0) + (d1 < 0 ? -d1 : d1);
    const int64_t start = (d0 < 0) ? -d0 : 0;   /* keep a leading negative step in range */
    int64_t period = d0 + d1; if (period < 0) period = -period;
    if (period == 0) period = 1;
    if (R <= start + span) die("--region-lines too small for these strides");
    const uint64_t lim = (uint64_t)(R - span);
    const int64_t shift = (int64_t)shift_lines * G.line;
    uint8_t *base = pfb_alloc((size_t)R, "region");

    result_kv("d0", "%lld", (long long)d0); result_kv("d1", "%lld", (long long)d1);
    result_kv("region_lines", "%ld", region_lines);
    int64_t off = start, w = 0; uint64_t sum = 0;
    pfb_roi_begin();
    for (long i = 0; i < acc; i++) {
        sum += LD(base + off);
        off += (i & 1) ? d1 : d0;
        if ((uint64_t)off >= lim) { w++; off = start + (w * shift) % period; }
    }
    pfb_roi_end((uint64_t)acc, sum);
    return 0;
}

/* Constant stride: the control for the other stride variants. */
int k_stride(void) {
    long rl, sl, acc = walk_params(&rl, &sl);
    long sb = cli_long("stride-bytes", 4 * G.line, "stride in bytes (default 4 lines)");
    cli_done();
    if (sb == 0) die("--stride-bytes must be non-zero");
    result_kv("stride_bytes", "%ld", sb);
    return walk(acc, rl, sl, sb, sb);
}

/* stride = 2^n + alias (in --unit). Truncated to an n-bit field (mod 2^n) the stride
 * becomes `alias`, so a table with an n-bit stride field does not just miss: it
 * learns the WRONG stride and issues useless prefetches at +alias. A signed n-bit
 * field already overflows at 2^(n-1). */
int k_stride_overflow(void) {
    long rl, sl, acc = walk_params(&rl, &sl);
    long bits  = cli_long("bits", 11, "width n of the modelled stride field");
    const char *unit = cli_str("unit", "byte", "byte | line: unit the stride field counts in");
    long alias = cli_long("alias", 64, "value (in --unit) the truncated stride aliases to");
    long sign  = cli_long("signed", 0, "1 = the field is two's-complement signed");
    long sb    = cli_long("stride-bytes", 0, "override the stride (bytes); 0 = 2^n + alias");
    cli_done();
    if (bits < 1 || bits > 40) die("--bits must be in [1, 40]");
    long ub = 1;
    if (!strcmp(unit, "line")) ub = G.line;
    else if (strcmp(unit, "byte")) die("--unit must be byte or line");
    long stride_u = sb ? sb / ub : (1L << bits) + alias;
    if (!sb) sb = stride_u * ub;
    long trunc = stride_u & ((1L << bits) - 1);
    long strunc = (sign && trunc >= (1L << (bits - 1))) ? trunc - (1L << bits) : trunc;
    long maxrep = sign ? (1L << (bits - 1)) - 1 : (1L << bits) - 1;
    info("stride %ld %s(s) = %ld B; n=%ld %s field holds <= %ld; truncated value %ld %s(s) = %ld B",
         stride_u, unit, sb, bits, sign ? "signed" : "unsigned", maxrep, strunc, unit, strunc * ub);
    if (stride_u <= maxrep) warn("stride %ld fits in the field: no overflow", stride_u);
    if (labs(strunc * ub) < G.line && strunc) info("aliased stride is < 1 line: it points into the same line");
    result_kv("bits", "%ld", bits); result_kv("unit", "%s", unit); result_kv("signed", "%ld", sign);
    result_kv("stride_bytes", "%ld", sb); result_kv("alias_bytes", "%ld", strunc * ub);
    return walk(acc, rl, sl, sb, sb);
}

/* +s1, +s2, +s1, +s2 ... lines. A last-stride/confirm prefetcher never sees two equal
 * strides in a row; a delta-history (VLDP/SPP-style) prefetcher should learn it. */
int k_stride_alt(void) {
    long rl, sl, acc = walk_params(&rl, &sl);
    long s1 = cli_long("s1-lines", 4, "first stride in lines");
    long s2 = cli_long("s2-lines", 8, "second stride in lines");
    cli_done();
    if (s1 == s2) warn("s1 == s2: this is a constant stride");
    result_kv("s1_lines", "%ld", s1); result_kv("s2_lines", "%ld", s2);
    return walk(acc, rl, sl, s1 * G.line, s2 * G.line);
}

/* +fwd, -back, +fwd, -back ... lines. With fwd == back the walk only ping-pongs
 * between two lines (everything hits; nothing to prefetch), so the default is
 * +3/-1: every line is visited once, out of order, with net forward progress. */
int k_stride_zigzag(void) {
    long rl, sl, acc = walk_params(&rl, &sl);
    long f = cli_long("fwd-lines", 3, "forward step in lines");
    long b = cli_long("back-lines", 1, "backward step in lines");
    cli_done();
    if (f <= 0 || b <= 0) die("--fwd-lines and --back-lines must be positive");
    if (f == b) warn("fwd == back: the walk alternates between two lines only (degenerate)");
    if (f < b) info("net progress is backward");
    result_kv("fwd_lines", "%ld", f); result_kv("back_lines", "%ld", b);
    if (f < b) return walk(acc, rl, sl, -f * G.line, b * G.line); /* mirror so the cursor stays in range */
    return walk(acc, rl, sl, f * G.line, -b * G.line);
}

/* Short bursts of --burst-lines accesses (stride --burst-stride-lines) at random
 * starting lines, each followed by --jumps single-use random accesses. All accesses
 * come from one load instruction. A prefetcher that ramps up on the burst wastes its
 * prefetches past the burst end; one that trains on the jumps wastes more. */
int k_bursts(void) {
    long acc = cli_long("accesses", big_lines(), "accesses in the ROI");
    long rl  = cli_long("region-lines", big_lines(), "region the bursts and jumps land in");
    long bl  = cli_long("burst-lines", 4, "accesses per burst");
    long bs  = cli_long("burst-stride-lines", 1, "stride inside a burst, in lines");
    long nj  = cli_long("jumps", 1, "random single-use accesses between bursts");
    cli_done();
    if (bl < 1 || nj < 0 || bs == 0) die("need --burst-lines >= 1, --jumps >= 0, --burst-stride-lines != 0");
    long span = (bl - 1) * labs(bs) + 1;
    if (rl <= span) die("--region-lines too small");
    uint8_t *base = pfb_alloc((size_t)rl * G.line, "region");
    result_kv("burst_lines", "%ld", bl); result_kv("burst_stride_lines", "%ld", bs);
    result_kv("jumps", "%ld", nj); result_kv("region_lines", "%ld", rl);

    const long per = bl + nj;
    const uint64_t nstart = (uint64_t)(rl - span);
    const int64_t first = bs < 0 ? (span - 1) : 0;     /* burst may run downward */
    uint64_t s = G.seed, sum = 0;
    int64_t line = 0; long pos = per;                  /* force a new burst first */
    pfb_roi_begin();
    for (long i = 0; i < acc; i++) {
        if (pos >= per) { pos = 0; line = first + (int64_t)(xs64(&s) % nstart); }
        else if (pos < bl) line += bs;
        else line = (int64_t)(xs64(&s) % (uint64_t)rl);
        pos++;
        sum += LD(base + line * G.line);
    }
    pfb_roi_end((uint64_t)acc, sum);
    return 0;
}
