// Host test of the guest clocks (docs/m3/clock.md): includes ps4_kernel_sys.c itself to reach its statics.
// Native arm64 only; no guest code runs. Build and run with tools/clock_unit.sh.
//   monotonic    process-time counter never goes backwards across 8 threads (a reader that starts after a value was
//                published must see at least that value), without and with a stepping GPU-stall credit
//   frequency    counter delta / frequency, the microsecond clock and CLOCK_MONOTONIC against the host over 1 s: 0.1%
//   overflow     the scale against a 128-bit reference up to 2^63 ticks; the clocks with a counter skewed by 25 and
//                250 years (the 64-bit product v*355 would wrap near 2^64/355 = 5.2e16 ticks = 68 years)
//   bench        ns per call, the old implementation (copied below) against the new one
#define NPS5_CLOCK_TEST 1
#include "../../pkg/runtime/modules/libkernel/ps4_kernel_sys.c"
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>
#include <signal.h>
static uint64_t recomp_rdtsc_old(void);

// ---- runtime stubs
uint8_t g_nps5_level[NPS5_S_COUNT];
void nps5_log(int s, int l, const char *fmt, ...) { (void)s; (void)l; (void)fmt; }
_Thread_local GuestContext *g_current_ctx;
uint64_t g_recomp_call_model;
recomp_fn_t **g_dispatch_l1[DISPATCH_LEVEL_SIZE];

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t host_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static GuestContext *C;
static uint64_t L_ptc(void) { return lean_sceKernelGetProcessTimeCounter(0, 0, 0, 0, 0, 0, C); }

// ---- fake GPU-stall credit: what AgcDriverStallMicros does (a mutex and the host clock), growing in steps
static pthread_mutex_t g_stall_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_stall_us, g_stall_since;
static _Atomic int g_stall_on;
static uint64_t fake_stall_micros(void) {
    pthread_mutex_lock(&g_stall_mu);
    uint64_t c = g_stall_us + (g_stall_since ? (host_ns() - g_stall_since) / 1000 : 0);
    pthread_mutex_unlock(&g_stall_mu);
    return c;
}
static void *stall_driver(void *arg) {
    (void)arg;
    while (atomic_load(&g_stall_on)) { // alternate: 3 ms of stall, 2 ms idle
        pthread_mutex_lock(&g_stall_mu); g_stall_since = host_ns(); pthread_mutex_unlock(&g_stall_mu);
        usleep(3000);
        pthread_mutex_lock(&g_stall_mu); g_stall_us += (host_ns() - g_stall_since) / 1000; g_stall_since = 0; pthread_mutex_unlock(&g_stall_mu);
        usleep(2000);
    }
    return NULL;
}

// ---- monotonicity across threads
static _Atomic uint64_t g_pub;
static _Atomic int g_go, g_stop;
static _Atomic uint64_t g_viol, g_reads, g_viol_own, g_viol_max;
typedef uint64_t (*clockfn)(void);
static clockfn g_fn;
static void *mono_thread(void *arg) {
    (void)arg;
    uint64_t prev = 0, n = 0;
    while (!atomic_load(&g_go)) {}
    while (!atomic_load(&g_stop)) {
        const uint64_t m = atomic_load_explicit(&g_pub, memory_order_acquire); // published before this read starts
        const uint64_t v = g_fn();
        if (v < prev) atomic_fetch_add(&g_viol_own, 1);
        if (v < m) { atomic_fetch_add(&g_viol, 1); uint64_t d = m - v, o = atomic_load(&g_viol_max); while (d > o && !atomic_compare_exchange_weak(&g_viol_max, &o, d)) {} }
        prev = v; n++;
        uint64_t old = m;
        while (v > old && !atomic_compare_exchange_weak_explicit(&g_pub, &old, v, memory_order_release, memory_order_relaxed)) {}
    }
    atomic_fetch_add(&g_reads, n);
    return NULL;
}
static void run_mono(const char *name, clockfn fn, double secs) {
    pthread_t t[8];
    g_fn = fn; atomic_store(&g_pub, 0); atomic_store(&g_go, 0); atomic_store(&g_stop, 0); atomic_store(&g_viol, 0); atomic_store(&g_reads, 0); atomic_store(&g_viol_own, 0); atomic_store(&g_viol_max, 0);
    for (int i = 0; i < 8; i++) pthread_create(&t[i], NULL, mono_thread, NULL);
    atomic_store(&g_go, 1);
    usleep((useconds_t)(secs * 1e6));
    atomic_store(&g_stop, 1);
    for (int i = 0; i < 8; i++) pthread_join(t[i], NULL);
    printf("monotonic %-34s 8 threads, %llu reads: %llu backwards within a thread, %llu behind a value another thread published (worst %llu ticks)\n", name, (unsigned long long)atomic_load(&g_reads), (unsigned long long)atomic_load(&g_viol_own), (unsigned long long)atomic_load(&g_viol), (unsigned long long)atomic_load(&g_viol_max));
    CHECK(atomic_load(&g_viol_own) == 0, "%s: went backwards within a thread", name);
#ifdef NPS5_CLOCK_ISB
    CHECK(atomic_load(&g_viol) == 0, "%s: behind a published value", name);
#endif
}
static uint64_t L_gpt(void) { return lean_sceKernelGetProcessTime(0, 0, 0, 0, 0, 0, C); }
static uint64_t L_mono(void) {
    uint64_t ts[2];
    static _Atomic int nslot; static __thread int slot = -1;
    if (slot < 0) slot = atomic_fetch_add(&nslot, 1) % 64;
    const uint64_t at = 0x1000 + 16 * (uint64_t)slot;
    lean_clock_gettime(4, at, 0, 0, 0, 0, C);
    memcpy(ts, C->mem_base + at, 16);
    return ts[0] * 1000000000ULL + ts[1];
}

// ---- the old implementation (before docs/m3/clock.md), for the benchmark
static uint64_t old_initial_tsc;
static _Atomic uint64_t old_last;
static uint64_t old_ptc(void) {
    uint64_t now = recomp_rdtsc_old();
    uint64_t ticks = now >= old_initial_tsc ? now - old_initial_tsc : 0;
    if (!atomic_load(&g_clock_credit_fn)) return ticks;
    const uint64_t us = (uint64_t)(((unsigned __int128)ticks * 1000000ULL) / RECOMP_TSC_FREQ);
    uint64_t c = atomic_load(&g_clock_credit_fn)();
    if (c > NPS5_MAX_CLOCK_CREDIT_US) c = NPS5_MAX_CLOCK_CREDIT_US;
    if (c > us) c = us;
    const uint64_t credit_ticks = (uint64_t)(((unsigned __int128)c * RECOMP_TSC_FREQ) / 1000000ULL);
    uint64_t v = ticks - credit_ticks, o = atomic_load(&old_last);
    while (v > o && !atomic_compare_exchange_weak(&old_last, &o, v)) {}
    return v > o ? v : o;
}

static uint64_t recomp_rdtsc_old(void) { // the implementation before this change: a 128-bit division per read
    uint64_t val, frq;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
    if (frq == 0) return val;
    return (uint64_t)(((unsigned __int128)val * RECOMP_TSC_FREQ) / frq);
}

// ---- frequency
static void test_frequency(void) {
    const uint64_t f = lean_sceKernelGetProcessTimeCounterFrequency(0, 0, 0, 0, 0, 0, C);
    CHECK(f == RECOMP_TSC_FREQ && f == lean_sceKernelGetTscFrequency(0, 0, 0, 0, 0, 0, C), "frequency %llu", (unsigned long long)f);
    const uint64_t h0 = host_ns(), c0 = L_ptc(), u0 = L_gpt(), m0 = L_mono(), t0 = lean_sceKernelReadTsc(0, 0, 0, 0, 0, 0, C);
    usleep(1000000);
    const uint64_t h1 = host_ns(), c1 = L_ptc(), u1 = L_gpt(), m1 = L_mono(), t1 = lean_sceKernelReadTsc(0, 0, 0, 0, 0, 0, C);
    const double wall = (double)(h1 - h0) / 1e9;
    const double e_c = ((double)(c1 - c0) / (double)f) / wall - 1, e_u = ((double)(u1 - u0) / 1e6) / wall - 1;
    const double e_m = ((double)(m1 - m0) / 1e9) / wall - 1, e_t = ((double)(t1 - t0) / (double)f) / wall - 1;
    printf("frequency 1 s wall: counter %+.5f%%  process_us %+.5f%%  tsc %+.5f%%  clock_gettime mono %+.5f%%\n", e_c * 100, e_u * 100, e_t * 100, e_m * 100);
    CHECK(fabs(e_c) < 0.001 && fabs(e_u) < 0.001 && fabs(e_t) < 0.001 && fabs(e_m) < 0.001, "rate outside 0.1%%");
    // the monotonic clock agrees with the host's (same epoch within the resync error)
    const int64_t d = (int64_t)L_mono() - (int64_t)clock_gettime_nsec_np(CLOCK_MONOTONIC);
    printf("          CLOCK_MONOTONIC minus the host's, 1 s after start: %lld ns\n", (long long)d);
    CHECK(llabs(d) < 20000000, "monotonic offset %lld ns", (long long)d); // the host clock is slewed by NTP: it drifts from the counter by up to 0.05%
    // microseconds vs ticks: exact relation
    for (int i = 0; i < 1000; i++) { const uint64_t a = L_gpt(), b = L_ptc(); CHECK(b / 2130 >= a, "us %llu ticks %llu", (unsigned long long)a, (unsigned long long)b); }
}

// ---- overflow
static void test_overflow(void) {
    // the scale against the exact 128-bit value
    const uint64_t freqs[] = {24000000ULL, 25000000ULL, 1000000000ULL, 3000000000ULL};
    uint64_t x = 0x9e3779b97f4a7c15ULL;
    long n = 0;
    for (int fi = 0; fi < 4; fi++)
        for (int i = 0; i < 200000; i++) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            uint64_t v = x >> (i % 40);
            if (i % 1000 == 0) v = (UINT64_MAX / 355) + (uint64_t)(i / 1000) - 100; // around the 64-bit product wrap
            const unsigned __int128 ref = (unsigned __int128)v * RECOMP_TSC_FREQ / freqs[fi];
            if (ref >> 64) continue;
            n++;
            CHECK(recomp_cnt_to_tsc(v, freqs[fi]) == (uint64_t)ref, "scale v=%llu frq=%llu", (unsigned long long)v, (unsigned long long)freqs[fi]);
        }
    // monotonic and exact straight through the wrap of a 64-bit v*355
    uint64_t prev = 0;
    for (uint64_t v = UINT64_MAX / 355 - 5000; v < UINT64_MAX / 355 + 5000; v++) {
        const uint64_t r = recomp_cnt_to_tsc(v, 24000000ULL);
        CHECK(r >= prev && r == (uint64_t)((unsigned __int128)v * RECOMP_TSC_FREQ / 24000000ULL), "wrap v=%llu", (unsigned long long)v);
        prev = r;
    }
    printf("overflow  scale: %ld values exact (24 MHz and divmod frequencies, around 2^64/355)\n", n);
    // the clocks of a machine that has been up for years: the counter skewed by 25 and 200 years, at the real counter
    // frequency and at 24 MHz (where a 64-bit v*355 would wrap near 68 years of ticks)
    const uint64_t real_frq = g_cntfrq;
    const uint64_t frqs[] = {real_frq, 24000000ULL};
    for (int fi = 0; fi < 2; fi++)
        for (int pass = 0; pass < 2; pass++) {
            g_cntfrq = frqs[fi];
            g_cnt_skew = (pass ? 200ULL : 25ULL) * 365 * 86400 * g_cntfrq;
            uint64_t last = 0, lastm = 0;
            for (int i = 0; i < 100000; i++) {
                const uint64_t v0 = cntvct_now();
                const uint64_t c = L_ptc(), m = L_mono();
                const uint64_t v1 = cntvct_now();
                const unsigned __int128 lo = (unsigned __int128)(v0 - g_cnt_base) * RECOMP_TSC_FREQ / g_cntfrq;
                const unsigned __int128 hi = (unsigned __int128)(v1 - g_cnt_base) * RECOMP_TSC_FREQ / g_cntfrq;
                CHECK(c >= (uint64_t)lo && c <= (uint64_t)hi && c >= last, "skew: counter %llu not in [%llu, %llu]", (unsigned long long)c, (unsigned long long)lo, (unsigned long long)hi);
                CHECK(m >= lastm, "skew: mono went backwards");
                CHECK(lo >> 64 == 0, "test setup: reference overflows");
                last = c; lastm = m;
            }
            printf("overflow  counter skewed by %3d years at %.0f MHz: process counter %.1f years of ticks, exact and monotonic\n",
                   pass ? 200 : 25, (double)g_cntfrq / 1e6, (double)last / RECOMP_TSC_FREQ / 86400 / 365);
        }
    g_cnt_skew = 0;
    g_cntfrq = real_frq;

}


// ---- the lean entries against the full shims' contract (kernel/syscalls shim_clock_gettime, shim_gettimeofday)
static void test_semantics(void) {
    C->fs_base = 0x2000; // errno slot: fs_base + 0x100
    uint64_t r = lean_clock_gettime(4, 0, 0, 0, 0, 0, C); // NULL timespec: -1, EFAULT
    CHECK(r == (uint64_t)-1 && *(int *)(C->mem_base + 0x2100) == EFAULT, "clock_gettime(NULL)");
    struct timespec h;
    clock_gettime(CLOCK_REALTIME, &h);
    r = lean_clock_gettime(0, 0x3000, 0, 0, 0, 0, C); // CLOCK_REALTIME
    int64_t *g = (int64_t *)(C->mem_base + 0x3000);
    CHECK(r == 0 && llabs((g[0] - h.tv_sec) * 1000000000LL + (g[1] - h.tv_nsec)) < 50000000LL && g[1] >= 0 && g[1] < 1000000000, "clock_gettime(REALTIME) %lld.%09lld vs %ld", (long long)g[0], (long long)g[1], (long)h.tv_sec);
    lean_clock_gettime(4, 0x3000, 0, 0, 0, 0, C);
    const int64_t a0 = g[0], a1 = g[1];
    lean_clock_gettime(5, 0x3000, 0, 0, 0, 0, C); // CLOCK_UPTIME shares the monotonic clock
    CHECK(a1 >= 0 && a1 < 1000000000 && (g[0] > a0 || (g[0] == a0 && g[1] >= a1)) && g[0] - a0 < 2, "CLOCK_MONOTONIC / CLOCK_UPTIME");
    r = lean_sceKernelClockGettime(4, 0x3000, 0, 0, 0, 0, C);
    CHECK(r == 0, "sceKernelClockGettime");
    // gettimeofday: tv and tz both written; tz NULL allowed; failing arguments are the host's business
    struct timeval htv; gettimeofday(&htv, NULL);
    memset(C->mem_base + 0x4000, 0xAA, 64);
    r = lean_gettimeofday(0x4000, 0x4100, 0, 0, 0, 0, C);
    g = (int64_t *)(C->mem_base + 0x4000);
    CHECK(r == 0 && llabs(g[0] - htv.tv_sec) <= 1 && g[1] >= 0 && g[1] < 1000000, "gettimeofday");
    r = lean_gettimeofday(0x4000, 0, 0, 0, 0, 0, C);
    CHECK(r == 0, "gettimeofday(tv, NULL)");
    // sceKernelGettimeofday: NULL is EINVAL, otherwise the same seconds
    r = lean_sceKernelGettimeofday(0, 0, 0, 0, 0, 0, C);
    CHECK((int64_t)r == -EINVAL, "sceKernelGettimeofday(NULL)");
    r = lean_sceKernelGettimeofday(0x4200, 0, 0, 0, 0, 0, C);
    CHECK(r == 0 && llabs(*(int64_t *)(C->mem_base + 0x4200) - htv.tv_sec) <= 1, "sceKernelGettimeofday");
    // frequency, time and tsc
    CHECK(lean_sceKernelGetProcessTimeCounterFrequency(0, 0, 0, 0, 0, 0, C) == 2130000000ULL, "process counter frequency");
    const uint64_t a = lean_sceKernelReadTsc(0, 0, 0, 0, 0, 0, C), b = recomp_rdtsc(), c = lean_sceKernelReadTsc(0, 0, 0, 0, 0, 0, C);
    CHECK(a <= b && b <= c, "ReadTsc and recomp_rdtsc count the same counter");
    CHECK(sceKernelGetProcessTimeCounter() <= L_ptc() && sceKernelGetProcessTime() <= L_gpt(), "full and lean agree");
}

// ---- benchmark
static double bench(const char *name, uint64_t (*fn)(void), double base_ns) {
    const long N = 20000000;
    uint64_t s = 0;
    for (long i = 0; i < 100000; i++) s += fn();
    const uint64_t t0 = host_ns();
    for (long i = 0; i < N; i++) s += fn();
    const double ns = (double)(host_ns() - t0) / N;
    printf("bench     %-46s %7.2f ns/call%s\n", name, ns, base_ns > 0 ? "" : "");
    (void)base_ns;
    if (s == 42) printf("!");
    return ns;
}
static uint64_t b_rdtsc_old(void) { return recomp_rdtsc_old(); }
static uint64_t b_rdtsc_new(void) { return recomp_rdtsc(); }
static uint64_t b_mach(void) { return mach_absolute_time(); }
static uint64_t b_cgt(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_nsec; }
static uint64_t b_gtod(void) { struct timeval tv; gettimeofday(&tv, NULL); return (uint64_t)tv.tv_usec; }
static uint64_t b_L_mono(void) { return mono_ns(); }
static uint64_t b_old_gpt(void) { return old_ptc() / 2130; }

static void test_bench(void) {
    old_initial_tsc = recomp_rdtsc_old();
    printf("-- no GPU-stall credit installed (homebrew, or NPS5_NO_STALL_CREDIT=1)\n");
    bench("old sceKernelGetProcessTimeCounter", old_ptc, 0);
    bench("new sceKernelGetProcessTimeCounter", L_ptc, 0);
    bench("old sceKernelGetProcessTime", b_old_gpt, 0);
    bench("new sceKernelGetProcessTime", L_gpt, 0);
    bench("old RDTSC lowering core (128-bit divide)", b_rdtsc_old, 0);
    bench("new RDTSC / sceKernelReadTsc (mul + umulh)", b_rdtsc_new, 0);
    bench("host mach_absolute_time()", b_mach, 0);
    bench("host clock_gettime(CLOCK_MONOTONIC)", b_cgt, 0);
    bench("new CLOCK_MONOTONIC (counter + offset)", b_L_mono, 0);
    bench("host gettimeofday (still a host call)", b_gtod, 0);
    printf("-- GPU-stall credit installed (AnyPS5 backend: mutex + steady_clock behind the credit function)\n");
    atomic_store(&g_stall_on, 1);
    pthread_t d; pthread_create(&d, NULL, stall_driver, NULL);
    nps5_set_guest_clock_credit(fake_stall_micros);
    bench("old sceKernelGetProcessTimeCounter", old_ptc, 0);
    bench("new sceKernelGetProcessTimeCounter", L_ptc, 0);
    atomic_store(&g_stall_on, 0);
    pthread_join(d, NULL);
    nps5_set_guest_clock_credit(NULL);
}

// A signal handler on the thread that is inside credit_refresh (odd sequence) reads the clock: the reader used to spin
// forever waiting for the writer it had interrupted. The write section now blocks signals; this hammers refreshes while
// signals with a clock-reading handler are aimed at the refreshing thread. alarm() turns a hang into a failed run.
static volatile uint64_t g_sig_reads;
static void sig_reader(int s) { (void)s; (void)sceKernelGetProcessTimeCounter(); g_sig_reads++; }
static _Atomic int g_sig_stop;
static void *sig_writer(void *a) {
    (void)a;
    while (!atomic_load(&g_sig_stop)) { atomic_store(&g_cr_deadline, 0); (void)sceKernelGetProcessTimeCounter(); }
    return NULL;
}
static void test_signal_midwrite(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = sig_reader; sigemptyset(&sa.sa_mask); sigaction(SIGUSR1, &sa, NULL);
    nps5_set_guest_clock_credit(fake_stall_micros);
    pthread_t w; atomic_store(&g_sig_stop, 0); pthread_create(&w, NULL, sig_writer, NULL);
    alarm(30);
    struct timespec t0, t; clock_gettime(CLOCK_MONOTONIC, &t0);
    long sent = 0;
    do { pthread_kill(w, SIGUSR1); sent++; usleep(20); clock_gettime(CLOCK_MONOTONIC, &t); } while ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000 < 1500);
    atomic_store(&g_sig_stop, 1); pthread_join(w, NULL);
    alarm(0);
    // the refreshing thread's signal mask was restored
    sigset_t cur; pthread_sigmask(SIG_BLOCK, NULL, &cur);
    CHECK(!sigismember(&cur, SIGUSR1), "signal mask not restored");
    CHECK(sent > 100, "only %ld signals sent", sent);
    printf("signal-in-refresh stress: %ld signals, %llu handler clock reads, no hang\n", sent, (unsigned long long)g_sig_reads);
    nps5_set_guest_clock_credit(NULL);
}

// A handler that interrupts a frame owning the credit function's mutex reads the clock with a stale deadline: it must use the
// cached credit and not call the function (which locks that mutex: a self-deadlock). alarm() turns a hang into a failed run.
static volatile uint64_t g_sig_clock_val;
static void sig_reader_locked(int s) { (void)s; g_clock_in_signal++; g_sig_clock_val = sceKernelGetProcessTimeCounter(); g_clock_in_signal--; }
static void test_signal_no_credit_call(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = sig_reader_locked; sigemptyset(&sa.sa_mask); sigaction(SIGUSR1, &sa, NULL);
    nps5_set_guest_clock_credit(fake_stall_micros);
    alarm(20);
    for (int i = 0; i < 50; i++) {
        pthread_mutex_lock(&g_stall_mu);              // the interrupted frame owns the credit mutex
        atomic_store(&g_cr_deadline, 0);              // stale cache: an unguarded reader would refresh (and lock) now
        g_sig_clock_val = 0;
        raise(SIGUSR1);
        pthread_mutex_unlock(&g_stall_mu);
        CHECK(g_sig_clock_val != 0, "handler did not read the clock");
    }
    alarm(0);
    printf("signal-path clock read with the credit mutex held: no self-deadlock\n");
    nps5_set_guest_clock_credit(NULL);
}

int main(int argc, char **argv) {
    static char arena[1 << 20];
    C = calloc(1, sizeof(GuestContext));
    C->mem_base = (uint8_t *)arena; // guest address 0x1000 = arena + 0x1000
    const int bench_only = argc > 1 && !strcmp(argv[1], "bench");
    if (!bench_only) {
        test_semantics();
        test_frequency();
        test_overflow();
        run_mono("process_time_counter", L_ptc, 1.0);
        run_mono("process_time_us", L_gpt, 0.5);
        // with a stepping credit: the floor must keep it monotonic across the steps
        atomic_store(&g_stall_on, 1);
        pthread_t d; pthread_create(&d, NULL, stall_driver, NULL);
        nps5_set_guest_clock_credit(fake_stall_micros);
        run_mono("counter + stepping credit", L_ptc, 3.0);
        run_mono("process_time_us + stepping credit", L_gpt, 1.0);
        const uint64_t ec = atomic_load(&g_cr_ec);
        printf("          credit applied: %llu us, floor %llu ticks\n", (unsigned long long)(ec / 2130), (unsigned long long)atomic_load(&g_cr_floor));
        CHECK(ec > 0, "credit never applied");
        atomic_store(&g_stall_on, 0);
        pthread_join(d, NULL);
        test_signal_midwrite();
        test_signal_no_credit_call();
        nps5_set_guest_clock_credit(NULL);
        run_mono("CLOCK_MONOTONIC", L_mono, 1.0);
    }
    test_bench();
    printf(fails ? "clock_unit: %d FAILURES\n" : "clock_unit: all checks passed\n", fails);
    return fails != 0;
}
