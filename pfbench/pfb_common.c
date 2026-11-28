/* CLI parsing, allocation, ROI timing and perf counters. */
#define _GNU_SOURCE
#include "pfb.h"
#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#endif
#ifdef PFB_M5
#include <gem5/m5ops.h>
#endif

pfb_cfg G;
volatile uint64_t pfb_sink;

int swpf_available(void) {
#ifdef PFB_NO_SWPF
    return 0;
#else
    return 1;
#endif
}

/* ---- CLI -------------------------------------------------------------------------- */
#define MAX_OPTS 96
static int    g_argc;
static char **g_argv;
static int    g_used[256];
static int    g_help;
static struct { const char *name, *def, *help; } g_opts[MAX_OPTS];
static int    g_nopts;

void cli_init(int argc, char **argv) {
    g_argc = argc; g_argv = argv;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { g_help = 1; g_used[i] = 1; }
}
int cli_help_requested(void) { return g_help; }

static void reg(const char *name, const char *def, const char *help) {
    for (int i = 0; i < g_nopts; i++) if (!strcmp(g_opts[i].name, name)) return;
    if (g_nopts < MAX_OPTS) { g_opts[g_nopts].name = name; g_opts[g_nopts].def = def; g_opts[g_nopts].help = help; g_nopts++; }
}

static const char *lookup(const char *name) {
    size_t n = strlen(name);
    const char *val = NULL;
    for (int i = 1; i < g_argc; i++) {
        const char *a = g_argv[i];
        if (a[0] == '-' && a[1] == '-' && !strncmp(a + 2, name, n) && a[2 + n] == '=') {
            val = a + 3 + n; g_used[i] = 1;    /* last one wins */
        }
    }
    return val;
}

long cli_long(const char *name, long def, const char *help) {
    static char bufs[MAX_OPTS][24]; static int nb;
    char *b = bufs[nb++ % MAX_OPTS];
    snprintf(b, 24, "%ld", def);
    reg(name, b, help);
    const char *v = lookup(name);
    if (!v) return def;
    char *end; errno = 0;
    long x = strtol(v, &end, 0);               /* accepts 0x.. hex */
    if (errno || end == v) die("--%s: bad integer '%s'", name, v);
    switch (*end) {                             /* K/M/G suffixes (binary) */
        case 'k': case 'K': x <<= 10; end++; break;
        case 'm': case 'M': x <<= 20; end++; break;
        case 'g': case 'G': x <<= 30; end++; break;
    }
    if (*end) die("--%s: bad integer '%s'", name, v);
    return x;
}

const char *cli_str(const char *name, const char *def, const char *help) {
    reg(name, def, help);
    const char *v = lookup(name);
    return v ? v : def;
}

void cli_done(void) {
    if (g_help) {
        printf("options for %s (all --name=value; integers accept 0x.., K/M/G suffixes):\n",
               G.kernel ? G.kernel : "pfbench");
        for (int i = 0; i < g_nopts; i++)
            printf("  --%-22s %-10s %s\n", g_opts[i].name, g_opts[i].def ? g_opts[i].def : "", g_opts[i].help);
        exit(0);
    }
    int bad = 0;
    for (int i = 1; i < g_argc; i++)
        if (!g_used[i] && g_argv[i] != G.kernel) { fprintf(stderr, "error: unknown option '%s' for kernel %s\n", g_argv[i], G.kernel); bad = 1; }
    if (bad) { fprintf(stderr, "run '%s %s --help' for its options\n", g_argv[0], G.kernel); exit(2); }
}

/* ---- memory ------------------------------------------------------------------------ */
void *pfb_alloc(size_t bytes, const char *name) {
    const size_t align = 2u << 20;
    if (bytes == 0) bytes = 1;
    size_t len = (bytes + align - 1) / align * align;
#ifdef _WIN32
    uint8_t *raw = (uint8_t *)VirtualAlloc(NULL, len + align, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!raw) die("VirtualAlloc %zu bytes for %s failed", len, name);
#else
    uint8_t *raw = mmap(NULL, len + align, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) die("mmap %zu bytes for %s failed", len, name);
#endif
    uint8_t *p = (uint8_t *)(((uintptr_t)raw + align - 1) & ~(uintptr_t)(align - 1));
    memset(p, 0, len);                          /* pre-touch: no first-touch faults in the ROI */
    for (size_t off = 0; off + 8 <= bytes; off += (size_t)G.line)   /* non-zero data -> meaningful checksums */
        *(uint64_t *)(p + off) = off / (size_t)G.line + 1;
    if (!G.quiet)
        fprintf(stderr, "REGION %s 0x%lx 0x%lx lines=%zu\n", name, (unsigned long)(uintptr_t)p,
                (unsigned long)((uintptr_t)p + bytes), bytes / (size_t)G.line);
    return p;
}

/* ---- result key/values ------------------------------------------------------------- */
static char g_kv[2048];
static size_t g_kvlen;
void result_kv(const char *key, const char *fmt, ...) {
    if (g_kvlen >= sizeof g_kv - 1) return;
    g_kvlen += snprintf(g_kv + g_kvlen, sizeof g_kv - g_kvlen, " %s=", key);
    if (g_kvlen >= sizeof g_kv - 1) return;
    va_list ap; va_start(ap, fmt);
    g_kvlen += vsnprintf(g_kv + g_kvlen, sizeof g_kv - g_kvlen, fmt, ap);
    va_end(ap);
}

/* ---- perf counters ----------------------------------------------------------------- */
#define MAX_EV 12
static struct { const char *name; int fd; uint32_t type; uint64_t config; } g_ev[MAX_EV];
static int g_nev;
static int g_perf_on;
static char g_perf_msg[160];

#ifdef _WIN32
static void perf_setup(void) {
    (void)cli_str("perf", "off", "perf counters: auto (use if permitted) | off");
    (void)cli_str("pmu-raw", "", "extra raw PMU events name=0xCONFIG,... (Linux only)");
    (void)g_ev;
}
#else
static void add_ev(const char *name, uint32_t type, uint64_t config) {
    if (g_nev < MAX_EV) { g_ev[g_nev].name = name; g_ev[g_nev].type = type; g_ev[g_nev].config = config; g_ev[g_nev].fd = -1; g_nev++; }
}
#define HWC(cache, op, res) (PERF_COUNT_HW_CACHE_##cache | (PERF_COUNT_HW_CACHE_OP_##op << 8) | (PERF_COUNT_HW_CACHE_RESULT_##res << 16))

static void perf_setup(void) {
    const char *mode = cli_str("perf", "auto", "perf counters: auto (use if permitted) | off");
    const char *raw  = cli_str("pmu-raw", "", "extra raw PMU events name=0xCONFIG,... (e.g. L2 events, see README)");
    if (!strcmp(mode, "off")) return;
    add_ev("cycles",       PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES);
    add_ev("instructions", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS);
    add_ev("l1d_rd_access", PERF_TYPE_HW_CACHE, HWC(L1D, READ, ACCESS));
    add_ev("l1d_rd_miss",   PERF_TYPE_HW_CACHE, HWC(L1D, READ, MISS));
    add_ev("llc_rd_access", PERF_TYPE_HW_CACHE, HWC(LL, READ, ACCESS));
    add_ev("llc_rd_miss",   PERF_TYPE_HW_CACHE, HWC(LL, READ, MISS));
    /* raw events: name=config pairs */
    static char rbuf[512];
    snprintf(rbuf, sizeof rbuf, "%s", raw);
    for (char *tok = strtok(rbuf, ","); tok; tok = strtok(NULL, ",")) {
        char *eq = strchr(tok, '=');
        if (!eq) die("--pmu-raw: expected name=0xconfig, got '%s'", tok);
        *eq = 0;
        add_ev(tok, PERF_TYPE_RAW, strtoull(eq + 1, NULL, 0));
    }
    int opened = 0;
    for (int i = 0; i < g_nev; i++) {
        struct perf_event_attr a;
        memset(&a, 0, sizeof a);
        a.size = sizeof a; a.type = g_ev[i].type; a.config = g_ev[i].config;
        a.disabled = 1; a.exclude_kernel = 1; a.exclude_hv = 1;
        a.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
        g_ev[i].fd = (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
        if (g_ev[i].fd >= 0) opened++;
    }
    if (!opened) {
        FILE *f = fopen("/proc/sys/kernel/perf_event_paranoid", "r");
        int lvl = -99; if (f) { if (fscanf(f, "%d", &lvl) != 1) lvl = -99; fclose(f); }
        snprintf(g_perf_msg, sizeof g_perf_msg, "perf counters unavailable (perf_event_paranoid=%d); timing only. "
                 "Enable with: sudo sysctl kernel.perf_event_paranoid=1", lvl);
        return;
    }
    g_perf_on = 1;
}
#endif

/* ---- ROI ---------------------------------------------------------------------------- */
#ifdef _WIN32
static LARGE_INTEGER g_qpc_freq, g_t0_qpc;
#else
static struct timespec g_t0;
#endif
static int g_roi_state;

PFB_EXPORT __attribute__((noinline, used)) void pfb_roi_begin(void) {
    if (g_roi_state++) die("pfb_roi_begin called twice");
    if (g_perf_msg[0]) info("%s", g_perf_msg);
    fflush(NULL);
#ifndef _WIN32
    for (int i = 0; g_perf_on && i < g_nev; i++)
        if (g_ev[i].fd >= 0) { ioctl(g_ev[i].fd, PERF_EVENT_IOC_RESET, 0); ioctl(g_ev[i].fd, PERF_EVENT_IOC_ENABLE, 0); }
#endif
#ifdef PFB_M5
    m5_work_begin(0, 0);
#endif
#ifdef _WIN32
    QueryPerformanceFrequency(&g_qpc_freq);
    QueryPerformanceCounter(&g_t0_qpc);
#else
    clock_gettime(CLOCK_MONOTONIC, &g_t0);
#endif
    __asm__ volatile("" ::: "memory");
}

PFB_EXPORT __attribute__((noinline, used)) void pfb_roi_end(uint64_t accesses, uint64_t checksum) {
    __asm__ volatile("" ::: "memory");
#ifdef _WIN32
    LARGE_INTEGER t1_qpc;
    QueryPerformanceCounter(&t1_qpc);
#else
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
#endif
#ifdef PFB_M5
    m5_work_end(0, 0);
#endif
    uint64_t vals[MAX_EV] = {0};
    int ok[MAX_EV] = {0};
#ifndef _WIN32
    for (int i = 0; g_perf_on && i < g_nev; i++) {
        if (g_ev[i].fd < 0) continue;
        ioctl(g_ev[i].fd, PERF_EVENT_IOC_DISABLE, 0);
        uint64_t r[3];
        if (read(g_ev[i].fd, r, sizeof r) == sizeof r && r[2]) {
            vals[i] = (r[2] < r[1]) ? (uint64_t)((double)r[0] * r[1] / r[2]) : r[0]; /* scale if multiplexed */
            ok[i] = 1;
        }
    }
#endif
    pfb_sink = checksum;
    g_roi_state++;
#ifdef _WIN32
    double ns = (double)(t1_qpc.QuadPart - g_t0_qpc.QuadPart) * 1e9 / (double)g_qpc_freq.QuadPart;
#else
    double ns = (t1.tv_sec - g_t0.tv_sec) * 1e9 + (t1.tv_nsec - g_t0.tv_nsec);
#endif
    printf("RESULT kernel=%s%s accesses=%llu time_ns=%.0f ns_per_access=%.3f",
           G.kernel, g_kv, (unsigned long long)accesses, ns, accesses ? ns / accesses : 0.0);
    for (int i = 0; i < g_nev; i++)
        if (ok[i]) printf(" %s=%llu", g_ev[i].name, (unsigned long long)vals[i]);
    /* derived miss ratios */
    for (int i = 0; i + 1 < g_nev; i++) {
        size_t n = strlen(g_ev[i].name);
        if (ok[i] && ok[i + 1] && n > 7 && !strcmp(g_ev[i].name + n - 7, "_access") && vals[i])
            printf(" %.*s_miss_rate=%.4f", (int)(n - 7), g_ev[i].name, (double)vals[i + 1] / vals[i]);
    }
    printf(" checksum=%llu\n", (unsigned long long)checksum);
    fflush(stdout);
}

/* Called from main before the kernel parses its own options. */
void pfb_common_setup(void);
void pfb_common_setup(void) { perf_setup(); }
