// SPDX-License-Identifier: GPL-2.0-or-later
//
// [apediag] shims (included at the end of nps5_physsub.c: they use its PsWrap / ps_save / ps_call_raw helpers). Pure logic and the
// switches' meaning: nps5_apediag_logic.h. Off by default: with NPS5_APEDIAG unset every shim is one flag test and a pass-through
// call of the kept guest body. ASTRO BOT (PPSA21564 01.007) only: the wraps exist only in a program translated with the entries of
// profiles/funcrepl/PPSA21564-physsub.json named "apediag ...", and the field offsets below are that build's.
//
//   NPS5_APEDIAG=1            on: log transitions of the watched objects (default off)
//   NPS5_APEDIAG_FILE=<path>  log file (default <run dir>/apediag.log, the run dir being the parent of KYTY_FLIGHTREC_DIR; else ./apediag.log)
//   NPS5_APEDIAG_IDS=<file>   level-object id hashes to tag objects with ("<hex fnv1a32 of the id string> <label>" per line)
//   NPS5_APEDIAG_MAXLINES=N   whole log (default 60000); 200 lines per untagged, 4000 per tagged object
//
// Watched (guest classes, wrapped update functions; "bot" objects are collected from the create function's return value):
//   counter  (counter class, update): count +0x5f8, type +0x630, ready count recomputed from the bots (state 8 for > 0.1 s)
//   delay    (delay trigger class, update): FSM state id +0x5f0 (Wait, EnterDelay, Enter, Triggering, LeaveDelay, Leave), state time +0x5fc
//   pst      (player state trigger class, update): window +0x5d0..0x650
//   pbb      (partial break board class, update): window +0x5d0..0x7d0
//   bot      (bot class, collected from its create function): window +0x1680..0x16c0, FSM object *(+0x1690) window +0x60..0xa0 (state id +0x78); polled once per frame
#include "nps5_apediag_logic.h"
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

#define AD_NCLS 5
enum { AD_COUNTER, AD_DELAY, AD_PST, AD_PBB, AD_BOT };
static const char *const g_ad_cls_name[AD_NCLS] = {"counter", "delay", "pst", "pbb", "bot"};
static const uint32_t g_ad_win_lo[AD_NCLS] = {0x5d0, 0x5d0, 0x5d0, 0x5d0, 0x1680};
static const uint32_t g_ad_win_len[AD_NCLS] = {0x70, 0xd0, 0x80, 0x200, 0x40};
#define AD_FSM_PTR 0x1690u
#define AD_BOT_TIME 0x16a0u
#define AD_FSM_STATE 0x78u
#define AD_BOT_READY_STATE 8u
#define AD_BOT_READY_SECS 0.1f
#define AD_MAXBOTS 32
#define AD_SLOTS 1024

static int g_ad_on = -1;
static pthread_once_t g_ad_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_ad_mu = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_ad_f;
static Nps5AdId g_ad_ids[NPS5_AD_MAX_IDS];
static int g_ad_nids;
static Nps5AdBudget g_ad_budget = {60000, 0, 200, 4000};
static uint64_t g_ad_t0_ns;
static Nps5AdObj *g_ad_obj; // AD_SLOTS objects (heap: ~1.3 KB each)
static uint8_t g_ad_cls[AD_SLOTS];
static uint32_t g_ad_ndump[AD_NCLS];
static uint64_t g_ad_bot[AD_MAXBOTS]; // guest addresses of the bots seen
static int g_ad_nbot;
static uint32_t g_ad_bot_state[AD_MAXBOTS];
static float g_ad_bot_tin[AD_MAXBOTS];
static uint64_t g_ad_poll_ns;
static struct {
    uint64_t addr;
    int32_t count, ready;
    uint32_t type;
    int seen;
} g_ad_cnt[16];
static struct {
    uint64_t addr;
    uint32_t st;
    int seen;
} g_ad_dt[AD_SLOTS];
static PsWrap g_w_ad[AD_NCLS] = {{.shim = "nps5_apediag_counter"}, {.shim = "nps5_apediag_delay"},
                                  {.shim = "nps5_apediag_pst"},     {.shim = "nps5_apediag_pbb"},
                                  {.shim = "nps5_apediag_botcreate"}};

static uint64_t ad_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void ad_init(void) {
    const char *e = getenv("NPS5_APEDIAG");
    g_ad_on = e && *e == '1';
    if (!g_ad_on) return;
    char path[1024];
    const char *f = getenv("NPS5_APEDIAG_FILE");
    if (f && *f) {
        snprintf(path, sizeof path, "%s", f);
    } else {
        const char *fr = getenv("KYTY_FLIGHTREC_DIR");
        if (fr && *fr) {
            char d[900];
            snprintf(d, sizeof d, "%s", fr);
            char *sl = strrchr(d, '/');
            if (sl) *sl = 0;
            snprintf(path, sizeof path, "%s/apediag.log", sl ? d : ".");
        } else {
            snprintf(path, sizeof path, "apediag.log");
        }
    }
    g_ad_f = fopen(path, "w");
    if (!g_ad_f) {
        fprintf(stderr, "[apediag] W cannot open %s: off\n", path);
        g_ad_on = 0;
        return;
    }
    const char *ml = getenv("NPS5_APEDIAG_MAXLINES");
    if (ml && atoi(ml) > 0) g_ad_budget.total = (uint32_t)atoi(ml);
    const char *idf = getenv("NPS5_APEDIAG_IDS");
    if (idf && *idf) {
        FILE *fi = fopen(idf, "r");
        if (fi) {
            char *buf = calloc(1, 64 * 1024);
            const size_t n = buf ? fread(buf, 1, 64 * 1024 - 1, fi) : 0;
            fclose(fi);
            if (buf) {
                buf[n] = 0;
                g_ad_nids = nps5_ad_ids_parse(buf, g_ad_ids, NPS5_AD_MAX_IDS);
                free(buf);
            }
        }
    }
    g_ad_obj = calloc(AD_SLOTS, sizeof(Nps5AdObj));
    g_ad_t0_ns = ad_now_ns();
    if (!g_ad_obj) g_ad_on = 0;
    fprintf(g_ad_f, "[apediag] on: %d ids, budget %u lines\n", g_ad_nids, g_ad_budget.total);
    fflush(g_ad_f);
    fprintf(stderr, "[apediag] on: log %s, %d ids\n", path, g_ad_nids);
}

// Guest memory is sparse: a value that merely looks like a pointer (the identity scan follows every qword of an object) can name
// an unmapped page below mem_size. The read goes through the kernel (vm_read_overwrite), which fails instead of faulting.
static int ad_rd(const GuestContext *ctx, uint64_t g, void *dst, size_t n) {
    if (g < 0x10000 || g + n > ctx->mem_size) return 0;
#ifdef __APPLE__
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)NPS5_G2H(ctx, g), n,
                                  (mach_vm_address_t)(uintptr_t)dst, &got) == KERN_SUCCESS && got == n;
#else
    memcpy(dst, NPS5_G2H(ctx, g), n);
    return 1;
#endif
}

static void ad_line(const char *cls, uint64_t addr, int id, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
// caller holds g_ad_mu
static void ad_line(const char *cls, uint64_t addr, int id, const char *fmt, ...) {
    char body[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    fprintf(g_ad_f, "t=%.3f f=%llu %s 0x%llx%s%s%s\n", (double)(ad_now_ns() - g_ad_t0_ns) / 1e9,
            (unsigned long long)atomic_load_explicit(&g_ps_world_n, memory_order_relaxed), cls, (unsigned long long)addr,
            id >= 0 ? " id=" : "", id >= 0 ? g_ad_ids[id].label : "", body);
    fflush(g_ad_f);
}

// identity: the id hash directly in the object's first 0x5c0 bytes, or in the first 0x60 bytes of a record it points to
static int ad_identify(const GuestContext *ctx, uint64_t this, char *how, size_t hcap) {
    if (!g_ad_nids) return -1;
    uint8_t buf[0x5c0];
    if (!ad_rd(ctx, this, buf, sizeof buf)) return -1;
    size_t off = 0;
    int k = nps5_ad_scan_ids(buf, sizeof buf, g_ad_ids, g_ad_nids, &off);
    if (k >= 0) {
        snprintf(how, hcap, "direct +0x%zx", off);
        return k;
    }
    for (size_t o = 8; o + 8 <= sizeof buf; o += 8) {
        uint64_t p;
        memcpy(&p, buf + o, 8);
        uint8_t rec[0x60];
        if (p < 0x100000 || !ad_rd(ctx, p, rec, sizeof rec)) continue;
        k = nps5_ad_scan_ids(rec, sizeof rec, g_ad_ids, g_ad_nids, &off);
        if (k >= 0) {
            snprintf(how, hcap, "ptr +0x%zx -> +0x%zx", o, off);
            return k;
        }
    }
    return -1;
}

static Nps5AdObj *ad_slot(uint64_t addr, int cls, uint32_t lo, uint32_t len, int *created) {
    uint32_t h = (uint32_t)((addr >> 4) * 2654435761u) % AD_SLOTS;
    for (int i = 0; i < AD_SLOTS; i++) {
        const uint32_t k = (h + (uint32_t)i) % AD_SLOTS;
        if (g_ad_obj[k].addr == addr && g_ad_cls[k] == cls && g_ad_obj[k].lo == lo) return &g_ad_obj[k];
        if (!g_ad_obj[k].addr) {
            nps5_ad_obj_init(&g_ad_obj[k], addr, lo, len);
            g_ad_cls[k] = (uint8_t)cls;
            *created = 1;
            return &g_ad_obj[k];
        }
    }
    return NULL;
}

static void ad_hex(const uint8_t *b, uint32_t lo, uint32_t len, char *out, size_t cap) {
    size_t u = 0;
    out[0] = 0;
    for (uint32_t i = 0; i < len && u + 12 < cap; i += 4) {
        uint32_t w;
        memcpy(&w, b + i, 4);
        if (w) u += (size_t)snprintf(out + u, cap - u, " %x:%x", lo + i, w);
    }
}

// observe one object window (caller holds the mutex); returns the object slot or NULL
static Nps5AdObj *ad_watch(GuestContext *ctx, int cls, uint64_t addr, uint32_t lo, uint32_t len, int tag_from) {
    uint8_t cur[NPS5_AD_WIN_MAX];
    if (len > sizeof cur || !ad_rd(ctx, addr + lo, cur, len)) return NULL;
    int created = 0;
    Nps5AdObj *o = ad_slot(addr, cls, lo, len, &created);
    if (!o) return NULL;
    if (created) {
        char how[64] = "";
        o->id = tag_from >= 0 ? tag_from : ad_identify(ctx, addr, how, sizeof how);
        if (nps5_ad_budget_take(&g_ad_budget, o)) {
            char hex[1400];
            if (g_ad_ndump[cls] < 40) {
                g_ad_ndump[cls]++;
                ad_hex(cur, lo, len, hex, sizeof hex);
            } else {
                hex[0] = 0;
            }
            ad_line(g_ad_cls_name[cls], addr, o->id, " NEW %s%s |%s", o->id >= 0 ? "via " : "untagged", how, hex);
        }
        char dummy[8];
        nps5_ad_diff(o, cur, dummy, sizeof dummy, 12); // prime
        return o;
    }
    if (o->id < 0 && g_ad_nids && (o->lines & 7u) == 0) {
        char how[64];
        o->id = ad_identify(ctx, addr, how, sizeof how);
        if (o->id >= 0 && nps5_ad_budget_take(&g_ad_budget, o)) ad_line(g_ad_cls_name[cls], addr, o->id, " TAGGED via %s", how);
    }
    char d[700];
    if (nps5_ad_diff(o, cur, d, sizeof d, 12) && nps5_ad_budget_take(&g_ad_budget, o)) {
        char extra[120] = "";
        if (cls == AD_DELAY) {
            uint32_t st = 0;
            float tm = 0;
            ad_rd(ctx, addr + 0x5f0, &st, 4);
            ad_rd(ctx, addr + 0x5fc, &tm, 4);
            snprintf(extra, sizeof extra, " state=%s(%u) tin=%.3f", nps5_ad_dt_state_name(st), st, (double)tm);
        }
        ad_line(g_ad_cls_name[cls], addr, o->id, "%s |%s", extra, d);
    }
    return o;
}

// bots: poll their state once per ~16 ms; log state transitions
static void ad_poll_bots(GuestContext *ctx) {
    const uint64_t now = ad_now_ns();
    if (now - g_ad_poll_ns < 16000000ull) return;
    g_ad_poll_ns = now;
    for (int i = 0; i < g_ad_nbot; i++) {
        const uint64_t b = g_ad_bot[i];
        uint64_t fsm = 0;
        uint32_t st = 0xffffffffu;
        float tin = 0;
        if (!ad_rd(ctx, b + AD_FSM_PTR, &fsm, 8)) continue;
        if (fsm) ad_rd(ctx, fsm + AD_FSM_STATE, &st, 4);
        ad_rd(ctx, b + AD_BOT_TIME, &tin, 4);
        Nps5AdObj *o = ad_watch(ctx, AD_BOT, b, g_ad_win_lo[AD_BOT], g_ad_win_len[AD_BOT], -1);
        if (fsm) ad_watch(ctx, AD_BOT, fsm, 0x60, 0x40, o ? o->id : -1);
        if (st != g_ad_bot_state[i]) {
            if (o && nps5_ad_budget_take(&g_ad_budget, o))
                ad_line("bot", b, o->id, " FSM %u->%u tin=%.3f", g_ad_bot_state[i], st, (double)tin);
            g_ad_bot_state[i] = st;
        }
        g_ad_bot_tin[i] = tin;
    }
}

static void ad_counter_note(GuestContext *ctx, uint64_t this, Nps5AdObj *o) {
    int32_t count = 0;
    uint32_t type = 0;
    ad_rd(ctx, this + 0x5f8, &count, 4);
    ad_rd(ctx, this + 0x630, &type, 4);
    uint8_t valid[AD_MAXBOTS];
    for (int i = 0; i < g_ad_nbot; i++) valid[i] = g_ad_bot_state[i] != 0xffffffffu;
    const int ready = nps5_ad_ready_count(g_ad_bot_state, g_ad_bot_tin, valid, g_ad_nbot, AD_BOT_READY_STATE, AD_BOT_READY_SECS);
    int k = -1;
    for (int i = 0; i < 16; i++)
        if (g_ad_cnt[i].addr == this) k = i;
    if (k < 0)
        for (int i = 0; i < 16 && k < 0; i++)
            if (!g_ad_cnt[i].addr) {
                k = i;
                g_ad_cnt[i].addr = this;
            }
    if (k < 0) return;
    if (!g_ad_cnt[k].seen || g_ad_cnt[k].count != count || g_ad_cnt[k].ready != ready || g_ad_cnt[k].type != type) {
        if (o && nps5_ad_budget_take(&g_ad_budget, o))
            ad_line("counter", this, o->id, " count=%d type=%u ready=%d/%d bots=%d%s", count, type, ready, count, g_ad_nbot,
                    ready >= count && count > 0 ? " FIRES" : "");
        g_ad_cnt[k].seen = 1;
        g_ad_cnt[k].count = count;
        g_ad_cnt[k].ready = ready;
        g_ad_cnt[k].type = type;
    }
}

static void ad_shim(GuestContext *ctx, int cls) {
    pthread_once(&g_ad_once, ad_init);
    PsArgs a;
    ps_save(ctx, &a);
    ps_call_raw(ctx, &g_w_ad[cls], &a, 0, 0.0f);
    if (g_ad_on > 0) {
        const uint64_t ret = ctx->rax;
        const uint64_t xmm0 = ctx->xmm[0].u64[0];
        pthread_mutex_lock(&g_ad_mu);
        ad_poll_bots(ctx);
        Nps5AdObj *o = ad_watch(ctx, cls, a.rdi, g_ad_win_lo[cls], g_ad_win_len[cls], -1);
        if (cls == AD_COUNTER && o) ad_counter_note(ctx, a.rdi, o);
        pthread_mutex_unlock(&g_ad_mu);
        ctx->rax = ret;
        ctx->xmm[0].u64[0] = xmm0;
    }
    SHIM_RETURN();
}

void nps5_apediag_counter(GuestContext *ctx) { ad_shim(ctx, AD_COUNTER); }
void nps5_apediag_delay(GuestContext *ctx) { ad_shim(ctx, AD_DELAY); }
void nps5_apediag_pst(GuestContext *ctx) { ad_shim(ctx, AD_PST); }
void nps5_apediag_pbb(GuestContext *ctx) { ad_shim(ctx, AD_PBB); }
// bot create: object* create(void); the returned object is a bot
void nps5_apediag_botcreate(GuestContext *ctx) {
    pthread_once(&g_ad_once, ad_init);
    PsArgs a;
    ps_save(ctx, &a);
    ps_call_raw(ctx, &g_w_ad[AD_BOT], &a, 0, 0.0f);
    if (g_ad_on > 0 && ctx->rax) {
        const uint64_t ret = ctx->rax;
        pthread_mutex_lock(&g_ad_mu);
        int have = 0;
        for (int i = 0; i < g_ad_nbot; i++) have |= g_ad_bot[i] == ret;
        if (!have && g_ad_nbot < AD_MAXBOTS) {
            g_ad_bot[g_ad_nbot] = ret;
            g_ad_bot_state[g_ad_nbot++] = 0xffffffffu;
            fprintf(g_ad_f, "t=%.3f bot create 0x%llx (%d bots)\n", (double)(ad_now_ns() - g_ad_t0_ns) / 1e9,
                    (unsigned long long)ret, g_ad_nbot);
            fflush(g_ad_f);
        }
        pthread_mutex_unlock(&g_ad_mu);
        ctx->rax = ret;
    }
    SHIM_RETURN();
}
