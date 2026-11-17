// nps5 diagnostics runtime (SPEC 8.2). See docs/m1/diag.md.
//
// Everything that runs in a signal handler uses only write(), open() and plain memory
// accesses to pre-sized static buffers; it never allocates, takes locks or calls stdio.
#include "nps5_shadow.h"
#include "nps5_diag.h"
#include "nps5_lean.h"
#include "nps5_perf.h"
#include "nps5_mem.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "recomp_context.h"

extern bool recomp_pc_in_guest_code(uint64_t pc);
extern GuestContext *recomp_get_current_ctx(void);

// ---- stamped by the translator (nps5_buildinfo.c); weak so host tests still link ----
__attribute__((weak)) const char nps5_build_commit[] = "";
__attribute__((weak)) const int nps5_build_dirty = 0;
__attribute__((weak)) const char nps5_input_sha256[] = "";
__attribute__((weak)) const char nps5_input_path[] = "";
__attribute__((weak)) const char nps5_addrmap_path[] = "";
__attribute__((weak)) const char nps5_translate_bundle[] = "";
__attribute__((weak)) const unsigned long long nps5_guest_image_size = 0;

extern const struct mach_header_64 _mh_execute_header;
extern const char g_pcmap_start[] __asm("section$start$__DATA$__nps5_pcmap");
extern const char g_pcmap_end[] __asm("section$end$__DATA$__nps5_pcmap");

uint8_t g_nps5_level[NPS5_S_COUNT];
void (*g_nps5_hle_hook)(void *ctx, void *shim_fn) = NULL;
// nps5_perf.c (docs/m3/perf.md) fills these; null when it is not linked (unit tests) or disabled.
void (*g_nps5_hle_exit_hook)(void *ctx, void *shim_fn) = NULL;
void (*g_nps5_perf_hle_enter)(void *shim_fn) = NULL;
void (*g_nps5_perf_finish)(const char *bundle_dir) = NULL;

static const char *const k_subsys[NPS5_S_COUNT] = {"loader", "lifter", "dispatch", "tier1", "hle", "kernel",
                                                   "mem", "gpu", "audio", "pad", "sys", "vm", "runtime"};
static const char *const k_level[] = {"trace", "debug", "info", "warn", "error"};

static char g_bundle_dir[1024];
// NPS5_CRASH_MEM ranges (parsed at start; the crash handler only reads the table).
static struct { int reg, deref; uint64_t off, off2, len; } g_crash_mem[8];
static int g_crash_mem_n;
static void crash_mem_parse(const char *s) {
  static const char *const nm[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
  while (s && *s && g_crash_mem_n < 8) {
    int reg = -1;
    size_t l = 0;
    for (int i = 0; i < 16; i++) {
      size_t n = strlen(nm[i]);
      if (!strncmp(s, nm[i], n) && n > l && (s[n] == '+' || s[n] == ':' || s[n] == '@')) { reg = i; l = n; }
    }
    if (reg < 0) return;
    s += l;
    uint64_t off = 0, len = 0;
    char *e;
    uint64_t off2 = 0;
    int deref = 0;
    if (*s == '+') { off = strtoull(s + 1, &e, 16); s = e; }
    if (*s == '@') { deref = 1; off2 = strtoull(s + 1, &e, 16); s = e; } // base = [reg+off] + off2
    if (*s != ':') return;
    len = strtoull(s + 1, &e, 16); s = e;
    if (len > 0x40000) len = 0x40000;
    g_crash_mem[g_crash_mem_n].reg = reg; g_crash_mem[g_crash_mem_n].off = off; g_crash_mem[g_crash_mem_n].len = len; g_crash_mem[g_crash_mem_n].deref = deref; g_crash_mem[g_crash_mem_n].off2 = off2;
    g_crash_mem_n++;
    if (*s == ',') s++;
  }
}
static char g_title[64];
static int g_log_fd = -1;
static int g_crash_fd = -1;
static struct timespec g_t0;
static struct timeval g_wall0;
static int g_started;
static _Atomic int g_crashed;
static _Atomic uintptr_t g_crash_owner; // the thread writing the crash report (set right after it wins g_crashed)
static _Atomic int g_faults;
static _Atomic int g_exiting;
static char g_note[512];
static char g_t1_kind[40], g_t1_module[128], g_t1_reason[16], g_t1_detail[1024];
static uint64_t g_t1_rip, g_t1_off;
static _Atomic int g_t1_set;
static uint64_t g_unsup_rip, g_unsup_fn;
static int g_exit_code = -1; // set by nps5_diag_set_exit_code / term_handler; -1 = not reported

// ------------------------------------------------------------------ small helpers
static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static double since_start(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)(ts.tv_sec - g_t0.tv_sec) + (double)(ts.tv_nsec - g_t0.tv_nsec) / 1e9;
}

static uint64_t cur_tid(void) {
  static _Thread_local uint64_t tid;
  if (!tid) pthread_threadid_np(NULL, &tid);
  return tid;
}

static int parse_level(const char *s) {
  for (int i = 0; i < 5; i++)
    if (strcasecmp(s, k_level[i]) == 0) return i;
  if (strcasecmp(s, "warning") == 0) return NPS5_L_WARN;
  if (strcasecmp(s, "off") == 0 || strcasecmp(s, "none") == 0) return NPS5_L_ERROR + 1;
  return -1;
}

// NPS5_LOG=hle=debug,loader=trace  (a bare level sets every subsystem; "*" too)
static void parse_log_env(void) {
  for (int i = 0; i < NPS5_S_COUNT; i++) g_nps5_level[i] = NPS5_L_INFO;
  const char *env = getenv("NPS5_LOG");
  if (!env || !*env) return;
  char buf[512];
  strlcpy(buf, env, sizeof buf);
  char *save = NULL;
  for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
    char *eq = strchr(tok, '=');
    const char *name = "*", *lv = tok;
    if (eq) { *eq = 0; name = tok; lv = eq + 1; }
    int l = parse_level(lv);
    if (l < 0) continue;
    for (int i = 0; i < NPS5_S_COUNT; i++)
      if (strcmp(name, "*") == 0 || strcmp(name, "all") == 0 || strcmp(name, k_subsys[i]) == 0)
        g_nps5_level[i] = (uint8_t)l;
  }
}

static size_t json_escape(char *dst, size_t cap, const char *s) {
  size_t n = 0;
  for (; *s && n + 8 < cap; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') { dst[n++] = '\\'; dst[n++] = (char)c; }
    else if (c == '\n') { dst[n++] = '\\'; dst[n++] = 'n'; }
    else if (c == '\r') { dst[n++] = '\\'; dst[n++] = 'r'; }
    else if (c == '\t') { dst[n++] = '\\'; dst[n++] = 't'; }
    else if (c < 0x20) { n += (size_t)snprintf(dst + n, cap - n, "\\u%04x", c); }
    else dst[n++] = (char)c;
  }
  dst[n] = 0;
  return n;
}

// ------------------------------------------------------------------ rate limiting
#define RL_SLOTS 256
#define RL_BURST 5
#define RL_WINDOW_NS 1000000000ull
typedef struct {
  const void *key;
  uint64_t window_start;
  uint32_t in_window;
  uint32_t suppressed;
  int subsys, level;
  char last[256];
} RateSlot;
static RateSlot g_rl[RL_SLOTS];
static pthread_mutex_t g_rl_mu = PTHREAD_MUTEX_INITIALIZER;

static void emit_record(int subsys, int level, const char *msg, uint32_t repeat);

// Returns true if the record should be emitted now. A rolled-over window with suppressed
// repeats flushes a coalesced record first.
static bool rate_check(const void *key, int subsys, int level, const char *msg) {
  uint64_t t = now_ns();
  size_t h = ((uintptr_t)key >> 3) % RL_SLOTS;
  uint32_t flush = 0;
  char flush_msg[256];
  bool emit = true;
  pthread_mutex_lock(&g_rl_mu);
  RateSlot *s = &g_rl[h];
  if (s->key != key) {
    if (s->suppressed) { flush = s->suppressed; strlcpy(flush_msg, s->last, sizeof flush_msg); }
    memset(s, 0, sizeof *s);
    s->key = key;
    s->window_start = t;
  }
  if (t - s->window_start > RL_WINDOW_NS) {
    if (s->suppressed) { flush = s->suppressed; strlcpy(flush_msg, s->last, sizeof flush_msg); }
    s->window_start = t;
    s->in_window = 0;
    s->suppressed = 0;
  }
  if (s->in_window >= RL_BURST) {
    s->suppressed++;
    s->subsys = subsys;
    s->level = level;
    strlcpy(s->last, msg, sizeof s->last);
    emit = false;
  } else {
    s->in_window++;
  }
  pthread_mutex_unlock(&g_rl_mu);
  if (flush) emit_record(subsys, level, flush_msg, flush);
  return emit;
}

static void rate_flush_all(void) {
  pthread_mutex_lock(&g_rl_mu);
  for (int i = 0; i < RL_SLOTS; i++) {
    if (g_rl[i].suppressed) {
      uint32_t n = g_rl[i].suppressed;
      g_rl[i].suppressed = 0;
      emit_record(g_rl[i].subsys, g_rl[i].level, g_rl[i].last, n);
    }
  }
  pthread_mutex_unlock(&g_rl_mu);
}

static void emit_record(int subsys, int level, const char *msg, uint32_t repeat) {
  if (g_log_fd < 0) return;
  char esc[1200];
  json_escape(esc, sizeof esc, msg);
  GuestContext *ctx = recomp_get_current_ctx();
  char pc[48] = "";
  if (ctx && ctx->rip) snprintf(pc, sizeof pc, ",\"guest_pc\":\"0x%llx\"", (unsigned long long)ctx->rip);
  char rep[32] = "";
  if (repeat > 1) snprintf(rep, sizeof rep, ",\"repeat\":%u", repeat);
  char line[1600];
  int n = snprintf(line, sizeof line, "{\"t\":%.6f,\"level\":\"%s\",\"subsys\":\"%s\",\"tid\":%llu%s,\"msg\":\"%s\"%s}\n",
                   since_start(), k_level[level], k_subsys[subsys], (unsigned long long)cur_tid(), pc, esc, rep);
  if (n > 0) (void)!write(g_log_fd, line, (size_t)(n < (int)sizeof line ? n : (int)sizeof line - 1));
}

void nps5_log(int subsys, int level, const char *fmt, ...) {
  if (subsys < 0 || subsys >= NPS5_S_COUNT) subsys = NPS5_S_RUNTIME;
  if (level < g_nps5_level[subsys]) return;
  char msg[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  size_t l = strlen(msg);
  while (l && (msg[l - 1] == '\n')) msg[--l] = 0;
  if (!rate_check(fmt, subsys, level, msg)) return;
  emit_record(subsys, level, msg, 1);
  if (level >= NPS5_L_WARN) fprintf(stderr, "[%s] %s\n", k_subsys[subsys], msg);
}

void nps5_log_compat(int subsys, int level, const char *tag, const char *fmt, ...) {
  char msg[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  FILE *out = level >= NPS5_L_WARN ? stderr : stdout; // as the printf/fprintf(stderr) it replaced
  size_t l = strlen(msg);
  fprintf(out, "[%s] %s%s", tag, msg, (l && msg[l - 1] == '\n') ? "" : "\n");
  while (l && msg[l - 1] == '\n') msg[--l] = 0;
  if (subsys < 0 || subsys >= NPS5_S_COUNT) subsys = NPS5_S_RUNTIME;
  if (level < g_nps5_level[subsys]) return;
  if (rate_check(fmt, subsys, level, msg)) emit_record(subsys, level, msg, 1);
}

// ------------------------------------------------------------------ HLE counters
#define HLE_SLOTS 8192
typedef struct { _Atomic(void *) fn; _Atomic uint64_t count; } HleSlot;
static HleSlot g_hle[HLE_SLOTS];
static _Atomic uint64_t g_hle_total;
static _Thread_local void *t_hle_current;
static _Atomic(void *) g_hle_last; // process-wide fallback for the crash report
// Weak default for builds without nps5_lean.c (host tests that link this file alone).
__attribute__((weak)) void nps5_lean_require_hooks(void) {}

static char g_trace_hle[256];

static const char *hle_name(void *fn, char *buf, size_t cap) {
  Dl_info di;
  if (dladdr(fn, &di) && di.dli_sname) {
    const char *n = di.dli_sname;
    if (n[0] == '_') n++;
    strlcpy(buf, n, cap);
  } else {
    snprintf(buf, cap, "shim@%p", fn);
  }
  return buf;
}

static void hle_hook(void *ctx, void *fn) {
  t_hle_current = fn;
  if (g_nps5_perf_hle_enter) g_nps5_perf_hle_enter(fn);
  atomic_store_explicit(&g_hle_last, fn, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_hle_total, 1, memory_order_relaxed);
  size_t h = ((uintptr_t)fn >> 2) % HLE_SLOTS;
  for (size_t i = 0; i < HLE_SLOTS; i++) {
    HleSlot *s = &g_hle[(h + i) % HLE_SLOTS];
    void *cur = atomic_load_explicit(&s->fn, memory_order_acquire);
    if (!cur) {
      void *exp = NULL;
      if (!atomic_compare_exchange_strong(&s->fn, &exp, fn)) cur = exp; else cur = fn;
    }
    if (cur == fn) {
      atomic_fetch_add_explicit(&s->count, 1, memory_order_relaxed);
      break;
    }
  }
  if (g_trace_hle[0] && g_nps5_level[NPS5_S_HLE] <= NPS5_L_TRACE + 1) {
    char nm[160];
    hle_name(fn, nm, sizeof nm);
    if (fnmatch(g_trace_hle, nm, 0) == 0 || strstr(g_trace_hle, nm)) {
      GuestContext *c = (GuestContext *)ctx;
      nps5_log(NPS5_S_HLE, NPS5_L_INFO, "hle %s rdi=0x%llx rsi=0x%llx rdx=0x%llx rcx=0x%llx r8=0x%llx r9=0x%llx", nm,
               (unsigned long long)c->rdi, (unsigned long long)c->rsi, (unsigned long long)c->rdx,
               (unsigned long long)c->rcx, (unsigned long long)c->r8, (unsigned long long)c->r9);
    }
  }
}

// ------------------------------------------------------------------ unresolved / unsupported tables
typedef struct { char name[96]; char nid[32]; char lib[48]; uint64_t first_rip; uint64_t caller; _Atomic uint64_t hits; } HitRow;
#define HIT_ROWS 64
static HitRow g_unres[HIT_ROWS];
static int g_unres_n;
static pthread_mutex_t g_unres_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_unsupported_pc;
static uint64_t g_unsupported_hits;

void nps5_diag_unresolved_hit(const char *name, const char *nid, const char *lib, uint64_t rip, uint64_t caller) {
  const char *key = (name && *name) ? name : (nid ? nid : "?");
  pthread_mutex_lock(&g_unres_mu);
  HitRow *r = NULL;
  for (int i = 0; i < g_unres_n; i++)
    if (strcmp(g_unres[i].name, key) == 0) { r = &g_unres[i]; break; }
  int first = 0;
  if (!r && g_unres_n < HIT_ROWS) {
    r = &g_unres[g_unres_n++];
    strlcpy(r->name, key, sizeof r->name);
    strlcpy(r->nid, nid ? nid : "", sizeof r->nid);
    strlcpy(r->lib, lib ? lib : "", sizeof r->lib);
    r->first_rip = rip;
    r->caller = caller;
    first = 1;
  }
  if (r) atomic_fetch_add(&r->hits, 1);
  pthread_mutex_unlock(&g_unres_mu);
  if (first)
    nps5_log(NPS5_S_HLE, NPS5_L_ERROR, "unresolved import called: %s nid=%s lib=%s rip=0x%llx caller=0x%llx", key,
             nid ? nid : "", lib ? lib : "", (unsigned long long)rip, (unsigned long long)caller);
}

// ------------------------------------------------------------------ pc map
// The translator emits a compact table (pkg/emitter/pcmap.go for the record layout): one record
// per guest function with a delta-coded stream of its guest instructions. It is indexed by
// function at startup (16 bytes per function) and decoded on demand, so a title with tens of
// millions of guest instructions pays nothing per instruction.
typedef struct { uint64_t host; uint64_t guest; uint64_t fn; } PcEnt; // guest: pc | flags << 56
typedef struct {
  uint64_t host;
  const uint8_t *rec;
} PcFn;
typedef struct {
  uint64_t host, guest;
  uint32_t hostlen, namelen, streamlen, nent;
} PcHdr; // the 32-byte record header, read with memcpy (the table is only 8-byte aligned)
#define PC_HDR 32
#define PCF_UNSUPPORTED 1
#define PCF_INDIRECT 2
#define PCF_ENTRY 4
static PcFn *g_fns;
static size_t g_fn_n;
static size_t g_pc_n; // guest instructions in the map
static uint64_t g_image_base;

static void pc_hdr(const uint8_t *rec, PcHdr *h) { memcpy(h, rec, PC_HDR); }
static size_t pc_rec_size(const PcHdr *h) {
  return (PC_HDR + (size_t)h->namelen + h->streamlen + 7) & ~(size_t)7;
}

static int pcfn_cmp(const void *a, const void *b) {
  uint64_t x = ((const PcFn *)a)->host, y = ((const PcFn *)b)->host;
  return x < y ? -1 : x > y;
}

static void load_pcmap(void) {
  g_image_base = (uint64_t)(uintptr_t)&_mh_execute_header;
  const uint8_t *p = (const uint8_t *)g_pcmap_start, *end = (const uint8_t *)g_pcmap_end;
  size_t n = 0;
  for (const uint8_t *q = p; q + PC_HDR <= end; n++) {
    PcHdr h;
    pc_hdr(q, &h);
    q += pc_rec_size(&h);
  }
  if (!n) return;
  g_fns = (PcFn *)malloc(n * sizeof(PcFn));
  if (!g_fns) return;
  int sorted = 1;
  size_t i = 0, ents = 0;
  for (const uint8_t *q = p; q + PC_HDR <= end && i < n; i++) {
    PcHdr h;
    pc_hdr(q, &h);
    g_fns[i].host = h.host;
    g_fns[i].rec = q;
    ents += h.nent;
    if (i && g_fns[i].host < g_fns[i - 1].host) sorted = 0;
    q += pc_rec_size(&h);
  }
  if (!sorted) qsort(g_fns, n, sizeof(PcFn), pcfn_cmp);
  g_fn_n = n;
  g_pc_n = ents;
}

static uint64_t pc_uleb(const uint8_t **sp) {
  uint64_t v = 0;
  unsigned sh = 0;
  const uint8_t *s = *sp;
  for (;;) {
    uint8_t c = *s++;
    v |= (uint64_t)(c & 0x7f) << sh;
    if (!(c & 0x80)) break;
    sh += 7;
  }
  *sp = s;
  return v;
}

// Decodes the entries of one record in host order. next() yields host, guest and flags of each.
typedef struct {
  const uint8_t *s, *send;
  uint64_t host, guest;
  uint32_t left;
  int first;
} PcIt;

static void pcit_init(PcIt *it, const uint8_t *rec, const PcHdr *h) {
  it->s = rec + PC_HDR + h->namelen;
  it->send = it->s + h->streamlen;
  it->host = h->host;
  it->guest = h->guest;
  it->left = h->nent;
  it->first = 1;
}

static int pcit_next(PcIt *it, uint64_t *host, uint64_t *guest, unsigned *flags) {
  if (!it->left || it->s >= it->send) return 0;
  uint8_t b = *it->s++;
  uint64_t hw, fl = 0;
  int64_t gd;
  if (b) {
    hw = b >> 4;
    gd = b & 15;
  } else {
    fl = pc_uleb(&it->s);
    hw = pc_uleb(&it->s);
    uint64_t z = pc_uleb(&it->s);
    gd = (int64_t)(z >> 1) ^ -(int64_t)(z & 1);
  }
  it->host += hw * 4;
  it->guest = (uint64_t)((int64_t)it->guest + gd);
  it->left--;
  *host = it->host;
  *guest = it->guest;
  *flags = (unsigned)fl | (it->first ? PCF_ENTRY : 0);
  it->first = 0;
  return 1;
}

// Tier-1 fragments (nps5_tier1.c) are dylibs loaded at run time; each brings its own pc map (the
// same records as the main image's). Registered tables are immutable and never freed, so the
// crash handler reads them lock-free.
typedef struct { uint64_t lo, hi; PcFn *fns; size_t n; char *path; } FragMap;
#define MAX_FRAGMAPS 4096
static FragMap g_fm[MAX_FRAGMAPS];
static _Atomic size_t g_fm_n;

static pthread_mutex_t g_fm_mu = PTHREAD_MUTEX_INITIALIZER; // writers only; readers are lock-free

void nps5_diag_register_fragment(uint64_t lo, uint64_t hi, const uint8_t *pcmap, size_t bytes, const char *path) {
  pthread_mutex_lock(&g_fm_mu);
  size_t i = atomic_load_explicit(&g_fm_n, memory_order_relaxed);
  if (i >= MAX_FRAGMAPS) { pthread_mutex_unlock(&g_fm_mu); return; }
  FragMap *f = &g_fm[i];
  f->lo = lo; f->hi = hi; f->fns = NULL; f->n = 0;
  f->path = path ? strdup(path) : NULL;
  if (pcmap && bytes) {
    const uint8_t *end = pcmap + bytes;
    size_t n = 0;
    for (const uint8_t *q = pcmap; q + PC_HDR <= end; n++) {
      PcHdr h;
      pc_hdr(q, &h);
      q += pc_rec_size(&h);
    }
    f->fns = n ? (PcFn *)malloc(n * sizeof(PcFn)) : NULL;
    if (f->fns) {
      size_t k = 0;
      for (const uint8_t *q = pcmap; q + PC_HDR <= end && k < n; k++) {
        PcHdr h;
        pc_hdr(q, &h);
        f->fns[k].host = h.host;
        f->fns[k].rec = q;
        q += pc_rec_size(&h);
      }
      qsort(f->fns, k, sizeof(PcFn), pcfn_cmp);
      f->n = k;
    }
  }
  atomic_store_explicit(&g_fm_n, i + 1, memory_order_release);
  pthread_mutex_unlock(&g_fm_mu);
}

static const FragMap *frag_of(uint64_t pc) {
  size_t n = atomic_load_explicit(&g_fm_n, memory_order_acquire);
  for (size_t i = 0; i < n; i++)
    if (pc >= g_fm[i].lo && pc < g_fm[i].hi) return &g_fm[i];
  return NULL;
}

int nps5_diag_pc_in_fragment(uint64_t pc) { return frag_of(pc) != NULL; }

const char *nps5_diag_fragment_path(uint64_t pc) {
  const FragMap *f = frag_of(pc);
  return f ? (f->path ? f->path : "") : NULL;
}

// Host pc -> the guest instruction it belongs to, or NULL outside the map. The result is valid
// until the 8th following call (the crash handler holds a few at a time).
static const PcEnt *pc_lookup(uint64_t host) {
  static PcEnt ring[8];
  static unsigned ri;
  const FragMap *fm = frag_of(host);
  const PcFn *fns = fm ? fm->fns : g_fns;
  size_t lo = 0, hi = fm ? fm->n : g_fn_n;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (fns[mid].host <= host) lo = mid + 1; else hi = mid;
  }
  if (!lo) return NULL;
  const uint8_t *rec = fns[lo - 1].rec;
  PcHdr h;
  pc_hdr(rec, &h);
  if (host >= h.host + h.hostlen) return NULL;
  PcIt it;
  pcit_init(&it, rec, &h);
  uint64_t eh, eg, bh = 0, bg = 0;
  unsigned ef, bf = 0;
  int have = 0;
  while (pcit_next(&it, &eh, &eg, &ef)) {
    if (eh > host && have) break;
    bh = eh; bg = eg; bf = ef; have = 1;
  }
  (void)bh;
  if (!have) return NULL;
  PcEnt *e = &ring[ri++ & 7];
  e->host = bh;
  e->guest = bg | (uint64_t)bf << 56;
  e->fn = h.guest;
  return e;
}

// name of a guest function by its entry address; unnamed functions get fn_0x<entry>
static const char *pc_fn_name(uint64_t fn_entry) {
  static char synth[4][32];
  static unsigned si;
  size_t nf = atomic_load_explicit(&g_fm_n, memory_order_acquire);
  for (size_t t = 0; t <= nf; t++) {
    const PcFn *fns = t ? g_fm[t - 1].fns : g_fns;
    size_t n = t ? g_fm[t - 1].n : g_fn_n;
    for (size_t i = 0; i < n; i++) {
      PcHdr h;
      pc_hdr(fns[i].rec, &h);
      if (h.guest != fn_entry) continue;
      if (h.namelen) return (const char *)fns[i].rec + PC_HDR;
      char *b = synth[si++ & 3];
      static const char hex[] = "0123456789abcdef";
      int k = 0;
      b[k++] = 'f'; b[k++] = 'n'; b[k++] = '_'; b[k++] = '0'; b[k++] = 'x';
      int started = 0;
      for (int sh = 60; sh >= 0; sh -= 4) {
        int d = (int)((fn_entry >> sh) & 15);
        if (d || started || sh == 0) { b[k++] = hex[d]; started = 1; }
      }
      b[k] = 0;
      return b;
    }
  }
  return NULL;
}

// Writes pcmap.bin (the format of pkg/diag ReadPCMap): header {magic, image base, count}, then
// {host offset, guest|flags<<56, function entry} per guest instruction, plus one sentinel
// (flag 8) after each function marking the end of its host code. Streams through fd without
// allocating, so the crash handler can use it.
static void write_pcmap_bin(int fd) {
  uint64_t hdr[3];
  memcpy(hdr, "NPS5PCM1", 8);
  hdr[1] = g_image_base;
  hdr[2] = g_pc_n + g_fn_n;
  (void)!write(fd, hdr, sizeof hdr);
  static uint64_t chunk[3 * 512];
  size_t k = 0;
  for (size_t i = 0; i < g_fn_n; i++) {
    PcHdr h;
    pc_hdr(g_fns[i].rec, &h);
    PcIt it;
    pcit_init(&it, g_fns[i].rec, &h);
    uint64_t eh, eg;
    unsigned ef;
    for (;;) {
      int more = pcit_next(&it, &eh, &eg, &ef);
      if (k + 1 > 512) { (void)!write(fd, chunk, k * 24); k = 0; }
      if (more) {
        chunk[k * 3] = eh - g_image_base;
        chunk[k * 3 + 1] = eg | (uint64_t)ef << 56;
      } else {
        chunk[k * 3] = h.host + h.hostlen - g_image_base;
        chunk[k * 3 + 1] = (uint64_t)8 << 56;
      }
      chunk[k * 3 + 2] = h.guest;
      k++;
      if (!more) break;
    }
  }
  if (k) (void)!write(fd, chunk, k * 24);
}

// ------------------------------------------------------------------ async-signal-safe writer
typedef struct { char buf[8192]; size_t n; int fd; } Sw;
static void sw_flush(Sw *w) {
  size_t off = 0;
  while (off < w->n) {
    ssize_t r = write(w->fd, w->buf + off, w->n - off);
    if (r <= 0) { if (errno == EINTR) continue; break; }
    off += (size_t)r;
  }
  w->n = 0;
}
static void sw_str(Sw *w, const char *s) {
  while (*s) {
    if (w->n == sizeof w->buf) sw_flush(w);
    w->buf[w->n++] = *s++;
  }
}
static void sw_hex(Sw *w, uint64_t v) {
  char t[19];
  int i = 18;
  t[i] = 0;
  do { t[--i] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
  t[--i] = 'x';
  t[--i] = '0';
  sw_str(w, t + i);
}
static void sw_hex16(Sw *w, uint64_t v) {
  char t[19];
  t[0] = '0'; t[1] = 'x';
  for (int i = 0; i < 16; i++) t[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 15];
  t[18] = 0;
  sw_str(w, t);
}
static void sw_dec(Sw *w, int64_t v) {
  char t[24];
  int i = 23;
  t[i] = 0;
  uint64_t u = v < 0 ? (uint64_t)(-v) : (uint64_t)v;
  do { t[--i] = (char)('0' + u % 10); u /= 10; } while (u);
  if (v < 0) t[--i] = '-';
  sw_str(w, t + i);
}
static void sw_kv(Sw *w, const char *k, const char *v) { sw_str(w, k); sw_str(w, ": "); sw_str(w, v); sw_str(w, "\n"); }
static void sw_kx(Sw *w, const char *k, uint64_t v) { sw_str(w, k); sw_str(w, ": "); sw_hex(w, v); sw_str(w, "\n"); }

// Paths written to the run bundle (manifest.json, crash.txt) have the user's home directory replaced by "~"
// (nps5_diag_redact_path_home in nps5_diag.h; NPS5_DIAG_FULL_PATHS=1 keeps them whole). HOME is captured at startup:
// the crash handler cannot call getenv.
static char g_home[512];
static int g_full_paths;
size_t nps5_diag_redact_path(const char *in, char *out, size_t cap) {
  return nps5_diag_redact_path_home(in, out, cap, g_full_paths ? NULL : g_home);
}
static void sw_kpath(Sw *w, const char *k, const char *v) {
  char b[1100];
  nps5_diag_redact_path(v, b, sizeof b);
  sw_kv(w, k, b);
}

// ------------------------------------------------------------------ crash handler
#define NSIG_HANDLED 5
static const int k_sigs[NSIG_HANDLED] = {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGABRT};
static struct sigaction g_prev[NSIG_HANDLED];
static char g_altstack[64 * 1024] __attribute__((aligned(16)));

static const char *sig_name(int s) {
  switch (s) {
  case SIGSEGV: return "SIGSEGV";
  case SIGBUS: return "SIGBUS";
  case SIGILL: return "SIGILL";
  case SIGTRAP: return "SIGTRAP";
  case SIGABRT: return "SIGABRT";
  default: return "SIG?";
  }
}

// Fault address -> what it is (SPEC §3.10, docs/m1/memory.md "Fault classification"). The arena
// is the single source of truth for host<->guest: guest = guest_org + (host - host_lo), which is
// host - bias in base mode and host itself in identity mode. Everything used here is published
// with atomics by nps5_mem.c; nothing locks or allocates.
static void classify_fault(Sw *w, uint64_t fa, uint64_t sp, GuestContext *ctx) {
  (void)ctx;
  sw_kx(w, "fault_addr", fa);
  Nps5FaultInfo fi;
  nps5_mem_classify(fa, &fi);
  const char *cls = nps5_fault_class_name(fi.cls);
  if (fi.cls == NPS5_FA_NOT_GUEST) {
    cls = "unmapped host address";
    if (fa < 0x100000000ull && g_nps5_arena.mode == NPS5_ADDR_IDENTITY)
      // identity: guest == host, so a null or near-null guest pointer lands in the host's page zero
      cls = "guest null guard (null or near-null guest pointer; identity mode: the host's PAGEZERO)";
    else if (fa < 0x100000000ull)
      cls = "host null page / PAGEZERO (a host pointer was null, or a guest address was used without the bias)";
    else if (fa >= NPS5_HOST_CARVEOUT_LO && fa < NPS5_HOST_CARVEOUT_HI)
      cls = "host GPU carveout 0x10_0000_0000-0x70_0000_0000 (a guest address used as a host pointer?)";
    else if (fa + (1ull << 20) > sp && fa < sp + (8ull << 20))
      cls = "host stack";
    else if (g_nps5_arena.host_len && fa >= g_nps5_arena.host_lo + g_nps5_arena.host_len)
      cls = "above the guest arena (guest address beyond 40 bits)";
  }
  sw_kv(w, "fault_class", cls);
  sw_kv(w, "addr_mode", g_nps5_arena.mode == NPS5_ADDR_IDENTITY ? "identity" : "base");
  sw_kx(w, "host_bias", (uint64_t)(uintptr_t)g_nps5_arena.bias);
  if (fi.cls != NPS5_FA_NOT_GUEST) {
    sw_kx(w, "fault_guest_addr", fi.guest);
    if (fi.hi > fi.lo) {
      sw_kx(w, "fault_range_lo", fi.lo);
      sw_kx(w, "fault_range_hi", fi.hi);
    }
    if (fi.name[0]) sw_kv(w, "fault_range_name", fi.name);
  } else if (g_nps5_arena.mode == NPS5_ADDR_BASE && g_nps5_arena.host_len && fa < g_nps5_arena.guest_hi) {
    // Looks like a guest address that reached a host access without translation.
    sw_kx(w, "fault_addr_as_guest_would_be_host", fa + (uint64_t)(uintptr_t)g_nps5_arena.bias);
  }
}

static void crash_report(int sig, siginfo_t *si, void *uctx) {
  ucontext_t *uc = (ucontext_t *)uctx;
  arm_thread_state64_t *ss = &uc->uc_mcontext->__ss;
  static Sw w;
  w.fd = g_crash_fd;
  w.n = 0;
  if (w.fd < 0) return;
  uint64_t pc = ss->__pc;
  GuestContext *ctx = recomp_get_current_ctx();
  int in_guest = recomp_pc_in_guest_code(pc);

  uint64_t frames[40];
  uint64_t recs[40]; // address of the frame record that holds frames[i] as its return address (0 = none)
  int nf = 0;
  sw_str(&w, "nps5 crash report v1\n");
  sw_str(&w, "signal: "); sw_dec(&w, sig); sw_str(&w, " ("); sw_str(&w, sig_name(sig)); sw_str(&w, ")\n");
  sw_str(&w, "si_code: "); sw_dec(&w, si ? si->si_code : 0); sw_str(&w, "\n");
  sw_str(&w, "t: "); sw_dec(&w, (int64_t)since_start()); sw_str(&w, " s since start\n");
  sw_str(&w, "tid: "); sw_dec(&w, (int64_t)cur_tid()); sw_str(&w, "\n");
  sw_kx(&w, "image_base", g_image_base);
  sw_kpath(&w, "addrmap", nps5_addrmap_path);
  sw_kpath(&w, "translate_bundle", nps5_translate_bundle);
  sw_kx(&w, "host_pc", pc);
  sw_kx(&w, "host_pc_image_offset", pc - g_image_base);
  sw_kx(&w, "host_lr", ss->__lr);
  sw_kx(&w, "host_sp", ss->__sp);
  sw_flush(&w); // the header is on disk before any stack walk: a walk that dies leaves it behind

  // Backtrace by walking the frame-pointer chain (the shim bridge keeps a frame record).
  uint64_t fp = ss->__fp;
  const int flat = recomp_call_model_flat();
  // Flat call model: in guest code x29 is the shadow stack pointer, not a frame pointer; the
  // host frames start at the context's recomp_enter_guest frame, which is where sp points.
  if (flat && in_guest) fp = ctx && ctx->host_boundary >= ss->__sp ? ctx->host_boundary : ss->__sp;
  recs[nf] = 0;
  frames[nf++] = pc;
  if (ss->__lr) { recs[nf] = 0; frames[nf++] = ss->__lr; }
  uint64_t bad_fp = 0; // a frame record that could not be read (a corrupt frame pointer)
  for (int i = 0; i < 36 && fp && (fp & 15) == 0; i++) {
    // Stay within 64 MiB above the current sp to avoid wild reads.
    if (fp < ss->__sp || fp > ss->__sp + (64ull << 20)) break;
    uint64_t rec[2]; // fault-safe: a corrupt fp is the usual crash, the walk must not fault inside the handler
    if (!nps5_mem_safe_read((const void *)(uintptr_t)fp, rec, sizeof rec)) { bad_fp = fp; break; }
    uint64_t ret = rec[1], next = rec[0];
    if (ret) { recs[nf] = fp; frames[nf++] = ret; }
    if (next <= fp) break;
    fp = next;
  }
  // Reason.
  const char *reason = "fatal signal";
  const PcEnt *pe = in_guest ? pc_lookup(pc) : NULL;
  int unsupported = 0;
  if (sig == SIGTRAP || sig == SIGILL) {
    if (in_guest && pe && (pe->guest >> 56) & 3) {
      unsupported = 1;
      reason = (pe->guest >> 56) & 2 ? "UNSUPPORTED indirect jmp/call stub (brk #0) reached" : "UNSUPPORTED guest instruction stub (brk #0) reached";
    } else if (in_guest) {
      reason = "brk/illegal instruction in guest code (guest UD2 or other trap)";
    } else {
      reason = "trap/illegal instruction outside guest code";
    }
  } else if (sig == SIGSEGV || sig == SIGBUS) {
    reason = in_guest ? "memory fault in guest code" : "memory fault in runtime/HLE code";
  } else if (sig == SIGABRT && g_unsup_rip) {
    reason = "UNSUPPORTED guest instruction executed (lifter stub called recomp_unsupported_instruction)";
  } else if (sig == SIGABRT && atomic_load(&g_t1_set)) {
    reason = "tier-1 miss fallback failed: the guest transferred control to an address that has no translation and none could be made";
  } else if (sig == SIGABRT && g_note[0]) {
    reason = "abort() after a runtime-reported error (see note)";
  } else if (sig == SIGABRT) {
    reason = "abort() (uncaught C++ exception, assert, or runtime abort); see stderr/log for the message";
  }
  sw_kv(&w, "reason", reason);
  if (g_note[0]) sw_kv(&w, "note", g_note);
  if (atomic_load(&g_t1_set)) {
    sw_kv(&w, "tier1_kind", g_t1_kind);
    sw_kx(&w, "tier1_guest_rip", g_t1_rip);
    sw_kv(&w, "tier1_module", g_t1_module);
    sw_kx(&w, "tier1_module_offset", g_t1_off);
    sw_kv(&w, "tier1_miss_reason", g_t1_reason);
    sw_kv(&w, "tier1_detail", g_t1_detail);
    sw_kv(&w, "tier1_misses_log", "tier1_misses.jsonl in the run bundle");
  }
  {
    const char *fp = nps5_diag_fragment_path(pc);
    if (fp) sw_kv(&w, "tier1_fragment", fp[0] ? fp : "(unnamed)");
  }

  // Guest location.
  if (in_guest && pe) {
    uint64_t grip = pe->guest & ((1ull << 56) - 1);
    sw_kx(&w, "guest_rip", grip);
    sw_kx(&w, "guest_function_entry", pe->fn);
    sw_str(&w, "guest_function_offset: "); sw_hex(&w, grip - pe->fn); sw_str(&w, "\n");
    const char *nm = pc_fn_name(pe->fn);
    sw_kv(&w, "guest_function", nm ? nm : "(unnamed)");
    if (unsupported) {
      g_unsupported_pc = grip;
      g_unsupported_hits++;
    }
  } else if (in_guest) {
    sw_kv(&w, "guest_rip", "(host pc in guest code but not in pcmap: translate with diagnostics enabled)");
  } else {
    const PcEnt *ce = NULL;
    for (int i = 0; i < nf && !ce; i++)
      if (recomp_pc_in_guest_code(frames[i])) ce = pc_lookup(frames[i]);
    if (g_unsup_rip) {
      sw_kx(&w, "guest_rip", g_unsup_rip);
      const char *un = pc_fn_name(g_unsup_fn);
      sw_kx(&w, "guest_function_entry", g_unsup_fn);
      sw_kv(&w, "guest_function", un ? un : "(unnamed)");
      sw_kx(&w, "guest_function_offset", g_unsup_rip - g_unsup_fn);
    } else if (ce) {
      uint64_t grip = ce->guest & ((1ull << 56) - 1);
      sw_kx(&w, "guest_rip", grip);
      sw_kv(&w, "guest_rip_kind", "return address of the guest call into the runtime/HLE (innermost guest frame)");
      sw_kx(&w, "guest_function_entry", ce->fn);
      const char *cn = pc_fn_name(ce->fn);
      sw_kv(&w, "guest_function", cn ? cn : "(unnamed)");
      sw_kx(&w, "guest_function_offset", grip - ce->fn);
    } else {
      sw_kv(&w, "guest_rip", "(no guest frame found on the host stack)");
    }
    if (ctx) sw_kx(&w, "last_shim_guest_addr (ctx.rip)", ctx->rip);
  }
  void *hle = t_hle_current ? t_hle_current : atomic_load(&g_hle_last);
  sw_kx(&w, in_guest ? "last_hle_call_host_fn" : "hle_call_in_progress_host_fn", (uint64_t)(uintptr_t)hle);

  // Fault address.
  if (sig == SIGSEGV || sig == SIGBUS) classify_fault(&w, (uint64_t)(uintptr_t)si->si_addr, ss->__sp, ctx);

  // Guest registers.
  const uint64_t *x = ss->__x;
  if (in_guest) {
    sw_str(&w, "guest_registers: (live, from pinned host registers)\n");
    static const char *const nm[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
    uint64_t v[16] = {x[19], x[21], x[22], x[20], x[26], x[25], x[23], x[24], x[8], x[9], x[10], x[11], x[12], x[13], x[14], x[15]};
    for (int i = 0; i < 16; i++) { sw_str(&w, "  "); sw_str(&w, nm[i]); sw_str(&w, " = "); sw_hex16(&w, v[i]); sw_str(&w, i % 2 ? "\n" : "   "); }
    sw_kx(&w, "  fs_base", x[27]);
    sw_kx(&w, "  mem_base", x[28]);
    uint32_t nzcv = ss->__cpsr;
    sw_str(&w, "  flags (from host NZCV): "); sw_hex(&w, nzcv);
    sw_str(&w, " SF="); sw_dec(&w, (nzcv >> 31) & 1);
    sw_str(&w, " ZF="); sw_dec(&w, (nzcv >> 30) & 1);
    sw_str(&w, " hostC="); sw_dec(&w, (nzcv >> 29) & 1);
    sw_str(&w, " OF="); sw_dec(&w, (nzcv >> 28) & 1);
    sw_str(&w, "  (x86 CF = hostC or its inverse depending on static carry polarity at this point; PF/AF not modelled)\n");
    uint64_t sp[8];
    // rsp may point into the stack guard page (a stack overflow): read it without faulting, a
    // fault inside this handler would spin the thread forever.
    if (ctx && RECOMP_MEM_OK(ctx) && x[26] + 64 <= ctx->mem_size && nps5_guest_safe_read(x[26], sp, sizeof sp)) {
      sw_str(&w, "guest_stack_words (rsp+0..):\n");
      for (int i = 0; i < 8; i++) { sw_str(&w, "  [rsp+"); sw_hex(&w, (uint64_t)i * 8); sw_str(&w, "] = "); sw_hex16(&w, sp[i]); sw_str(&w, "\n"); }
    }
    // NPS5_CRASH_MEM=<reg>[+hexoff][@hexoff2]:<hexlen>[,...]: extra guest memory at reg+off (with @: at
    // [reg+off]+off2), as qwords.
    for (int k = 0; k < g_crash_mem_n; k++) {
      uint64_t base = v[g_crash_mem[k].reg] + g_crash_mem[k].off;
      if (g_crash_mem[k].deref) {
        if (!nps5_guest_safe_read(base, &base, sizeof base)) continue;
        base += g_crash_mem[k].off2;
      }
      sw_str(&w, "crash_mem "); sw_str(&w, nm[g_crash_mem[k].reg]); sw_str(&w, "+"); sw_hex(&w, g_crash_mem[k].off);
      sw_str(&w, " (guest "); sw_hex(&w, base); sw_str(&w, "):\n");
      for (uint64_t o = 0; o < g_crash_mem[k].len; o += 8) {
        uint64_t q;
        if (!nps5_guest_safe_read(base + o, &q, sizeof q)) { sw_str(&w, "  (unreadable at +"); sw_hex(&w, o); sw_str(&w, ")\n"); break; }
        sw_str(&w, "  +"); sw_hex(&w, o); sw_str(&w, " = "); sw_hex16(&w, q); sw_str(&w, "\n");
      }
    }
  } else if (ctx) {
    sw_str(&w, "guest_registers: (as last stored by the shim bridge)\n");
    sw_kx(&w, "  rip", ctx->rip); sw_kx(&w, "  rsp", ctx->rsp); sw_kx(&w, "  rbp", ctx->rbp);
    sw_kx(&w, "  rax", ctx->rax); sw_kx(&w, "  rdi", ctx->rdi); sw_kx(&w, "  rsi", ctx->rsi);
    sw_kx(&w, "  rdx", ctx->rdx); sw_kx(&w, "  rcx", ctx->rcx); sw_kx(&w, "  fs_base", ctx->fs_base);
  }
  sw_str(&w, "host_x0-x28:\n");
  for (int i = 0; i < 29; i++) { sw_str(&w, "  x"); sw_dec(&w, i); sw_str(&w, i < 10 ? "  = " : " = "); sw_hex16(&w, x[i]); sw_str(&w, i % 2 ? "\n" : "   "); }
  sw_str(&w, "\n");

  // Vector state (xmm0-15 = v0-v15) and control registers.
  arm_neon_state64_t *ns = &uc->uc_mcontext->__ns;
  sw_str(&w, "xmm0-15 (v0-v15):\n");
  for (int i = 0; i < 16; i++) {
    uint64_t lo = (uint64_t)ns->__v[i], hi = (uint64_t)(ns->__v[i] >> 64);
    sw_str(&w, "  xmm"); sw_dec(&w, i); sw_str(&w, i < 10 ? "  = " : " = "); sw_hex16(&w, hi); sw_str(&w, " "); sw_hex16(&w, lo); sw_str(&w, "\n");
  }
  sw_kx(&w, "fpcr", ns->__fpcr);
  sw_kx(&w, "fpsr", ns->__fpsr);
  sw_str(&w, "guest_mxcsr_ctx: "); sw_hex(&w, ctx ? ctx->mxcsr : 0); sw_str(&w, "\n");

  sw_str(&w, "backtrace (frame-pointer walk, host pc -> guest if in guest code):\n");
  if (bad_fp) { sw_str(&w, "  (walk truncated: unreadable frame record at "); sw_hex(&w, bad_fp); sw_str(&w, ")\n"); }
  for (int i = 0; i < nf; i++) {
    sw_str(&w, "  #"); sw_dec(&w, i); sw_str(&w, " "); sw_hex(&w, frames[i]);
    sw_str(&w, " (image+"); sw_hex(&w, frames[i] - g_image_base); sw_str(&w, ")");
    if (recomp_pc_in_guest_code(frames[i])) {
      const PcEnt *e = pc_lookup(frames[i]);
      if (e) {
        sw_str(&w, " guest "); sw_hex(&w, e->guest & ((1ull << 56) - 1));
        const char *n = pc_fn_name(e->fn);
        sw_str(&w, " in "); sw_str(&w, n ? n : "fn"); sw_str(&w, " (entry "); sw_hex(&w, e->fn); sw_str(&w, ")");
      }
    }
    sw_str(&w, "\n");
  }
  // Guest call chain. The guest frames are not on the frame-pointer chain: each recompiled function
  // keeps [host lr, entry guest rsp] in a 16-byte host slot (asm_emitter.go), contiguous above the
  // shim bridge's 48-byte frame record. Walk them from the innermost guest frame found above.
  sw_flush(&w);
  if (flat) {
    // The shadow return stack is the guest call chain: one entry per live guest call, each with
    // the host continuation of its call site. In guest code the live pointer is x29, elsewhere
    // the context stored it; entries the guest stack has already popped are skipped.
    static uint64_t conts[64];
    uint64_t top = in_guest ? ss->__fp : (ctx ? ctx->shadow_sp : 0);
    uint64_t grsp = in_guest ? x[26] : (ctx ? ctx->rsp : 0);
    size_t cn = top ? recomp_shadow_chain(top, grsp, conts, 64) : 0;
    if (cn) {
      sw_str(&w, "guest_call_chain: (shadow return stack, innermost first; each line is a call site inside the named guest function)\n");
      int after_throw = 0;
      for (size_t k = 0; k < cn; k++) {
        sw_str(&w, "  #"); sw_dec(&w, (int64_t)k); sw_str(&w, " ");
        if (conts[k] == RECOMP_CHAIN_BOUNDARY) {
          sw_str(&w, "<host boundary: HLE callback or thread entry>\n");
          continue;
        }
        const PcEnt *ce = pc_lookup(conts[k] - 4);
        if (ce) {
          const char *n = pc_fn_name(ce->fn);
          sw_hex(&w, ce->guest & ((1ull << 56) - 1)); sw_str(&w, " in "); sw_str(&w, n ? n : "fn");
          if (after_throw == 1) {
            sw_str(&w, "   <- thrower of the uncaught exception");
            after_throw = 2;
          }
          if (n && (!strcmp(n, "__cxa_throw") || !strcmp(n, "__cxa_rethrow")) && !after_throw) after_throw = 1;
        } else {
          sw_str(&w, "(no pc map entry for host "); sw_hex(&w, conts[k]); sw_str(&w, ")");
        }
        sw_str(&w, "\n");
      }
      if (after_throw == 2) sw_kv(&w, "uncaught_exception", "yes: a guest C++ exception reached std::terminate/abort (see the line marked thrower in guest_call_chain)");
    } else {
      sw_kv(&w, "guest_call_chain", "(shadow return stack empty or unavailable)");
    }
    if (ctx && ctx->shadow) {
      sw_str(&w, "shadow_stack: depth="); sw_dec(&w, top ? (int64_t)(((uint64_t)(uintptr_t)ctx->shadow->bottom - top) / 32) : -1);
      sw_str(&w, " slow_rets="); sw_dec(&w, (int64_t)atomic_load(&ctx->shadow->slow_rets));
      sw_str(&w, " overflows="); sw_dec(&w, (int64_t)atomic_load(&ctx->shadow->overflows));
      sw_str(&w, "\n");
    }
  } else if (!in_guest) {
    int bi = -1;
    for (int i = 0; i < nf && bi < 0; i++)
      if (recs[i] && recomp_pc_in_guest_code(frames[i])) bi = i;
    if (bi >= 0) {
      sw_str(&w, "guest_call_chain: (innermost first; each line is a call site inside the named guest function)\n");
      uint64_t prev_e = 0;
      int after_throw = 0;
      uint64_t lr = frames[bi];
      for (uint64_t p = recs[bi] + 48, k = 0; k < 64; k++, p += 16) {
        if (!recomp_pc_in_guest_code(lr)) break;
        const PcEnt *ce = pc_lookup(lr - 4);
        sw_str(&w, "  #"); sw_dec(&w, (int64_t)k); sw_str(&w, " ");
        if (ce) {
          const char *n = pc_fn_name(ce->fn);
          sw_hex(&w, ce->guest & ((1ull << 56) - 1)); sw_str(&w, " in "); sw_str(&w, n ? n : "fn");
          if (after_throw == 1) {
            sw_str(&w, "   <- thrower of the uncaught exception");
            after_throw = 2;
          }
          if (n && (!strcmp(n, "__cxa_throw") || !strcmp(n, "__cxa_rethrow")) && !after_throw) after_throw = 1;
        } else {
          sw_str(&w, "(no pc map entry)");
        }
        sw_str(&w, "\n");
        uint64_t slot[2];
        if (!nps5_mem_safe_read((const void *)(uintptr_t)(p), slot, sizeof slot)) { sw_str(&w, "  (chain truncated: unreadable frame slot)\n"); break; }
        uint64_t e = slot[1];
        lr = slot[0];
        if (e <= prev_e) break;
        prev_e = e;
      }
      if (after_throw == 2) sw_kv(&w, "uncaught_exception", "yes: a guest C++ exception reached std::terminate/abort (see the line marked thrower in guest_call_chain)");
    }
  }
  sw_flush(&w);

  // The core report is on disk. Dump the pc map next to it so `nps5 addr2guest` works offline.
  if (g_pc_n && g_bundle_dir[0]) {
    char path[1100];
    size_t bl = strlen(g_bundle_dir);
    memcpy(path, g_bundle_dir, bl);
    memcpy(path + bl, "/pcmap.bin", 11);
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) {
      write_pcmap_bin(fd);
      close(fd);
    }
  }
}

static void diag_atexit(void);
static void diag_atexit_fwd(void) { diag_atexit(); }

__attribute__((weak)) _Thread_local int g_nps5_in_crash_handler; // nps5_mem.h; weak in both files so each links alone

// For a crash handler that ends the process itself (the stderr report in recomp_runtime.c calls exit): write the
// bundle's summary and manifest NOW, before any library teardown runs, so they exist whatever exit() runs into.
// Idempotent (the atexit call that follows is then a no-op); the alarm bounds everything that comes after.
void nps5_diag_crash_finalize(void) {
  signal(SIGALRM, SIG_DFL);
  alarm(5);
  diag_atexit_fwd();
}

#define NPS5_MAX_RESOLVERS 4
typedef struct {
  Nps5FaultResolver plain;
  Nps5AccessResolver access;
  Nps5ContextResolver context;
} Resolver;
static Resolver g_resolvers[NPS5_MAX_RESOLVERS];
static _Atomic int g_nresolvers;
static _Atomic uint64_t g_resolved_faults;
static pthread_mutex_t g_resolver_mu = PTHREAD_MUTEX_INITIALIZER; // registration only; the handler takes no lock

static bool add_resolver(Resolver r) {
  pthread_mutex_lock(&g_resolver_mu);
  const int n = atomic_load(&g_nresolvers);
  const bool ok = n < NPS5_MAX_RESOLVERS;
  if (ok) {
    g_resolvers[n] = r; // published by the count: the handler reads [0, count)
    atomic_store_explicit(&g_nresolvers, n + 1, memory_order_release);
  }
  pthread_mutex_unlock(&g_resolver_mu);
  return ok;
}
bool nps5_diag_add_fault_resolver(Nps5FaultResolver fn) { return fn && add_resolver((Resolver){.plain = fn}); }
bool nps5_diag_add_access_resolver(Nps5AccessResolver fn) { return fn && add_resolver((Resolver){.access = fn}); }
bool nps5_diag_add_context_resolver(Nps5ContextResolver fn) { return fn && add_resolver((Resolver){.context = fn}); }
uint64_t nps5_diag_resolved_faults(void) { return atomic_load_explicit(&g_resolved_faults, memory_order_relaxed); }

// Whether the faulting access was a store: ESR_EL1 of a data abort has WnR in bit 6 (exception class 0x24/0x25).
static int fault_is_write(void *uctx) {
#if defined(__aarch64__)
  if (!uctx || !((ucontext_t *)uctx)->uc_mcontext) return 0;
  const uint32_t esr = ((ucontext_t *)uctx)->uc_mcontext->__es.__esr;
  const uint32_t ec = esr >> 26;
  return (ec == 0x24 || ec == 0x25) && (esr & (1u << 6)) != 0;
#else
  (void)uctx;
  return 0;
#endif
}

bool (*g_nps5_hostcall_fault_hook)(void *ucontext, uint64_t fault_addr);

// Livelock guard: a resolver that claims a fault without changing what made it fault (its table out of step with the host page
// protection, a misclassified atomic) leaves the thread retrying the same instruction forever at 100% CPU with no report. More than
// the limit of claimed faults at one address on one thread, each within 50 ms of the previous, is treated as unresolved and ends in
// the crash report (with a line that says so). The gap rule keeps a paced re-arm loop (one fault per frame) from counting.
static _Atomic uint32_t g_livelock_limit = 20000;
void nps5_diag_set_livelock_limit(uint32_t n) { atomic_store(&g_livelock_limit, n ? n : 1); }
static _Thread_local uint64_t t_ll_addr, t_ll_last_ns;
static _Thread_local uint32_t t_ll_count;
static bool livelock_tripped(uint64_t at) {
  const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  if (at == t_ll_addr && t_ll_last_ns && now - t_ll_last_ns < 50ull * 1000 * 1000) t_ll_count++;
  else { t_ll_addr = at; t_ll_count = 1; }
  t_ll_last_ns = now;
  return t_ll_count > atomic_load_explicit(&g_livelock_limit, memory_order_relaxed);
}

static void crash_handler(int sig, siginfo_t *si, void *uctx) {
  // Not a crash: a shadow stack push into its guard page (flat call model). Compacted, retried.
  if ((sig == SIGSEGV || sig == SIGBUS) && si && recomp_shadow_handle_fault(uctx, (uint64_t)(uintptr_t)si->si_addr)) return;
  // Not a crash: a host library called a guest function pointer (nps5_hostcall.c). Bridged.
  if ((sig == SIGSEGV || sig == SIGBUS) && si && si->si_code > 0 && g_nps5_hostcall_fault_hook &&
      g_nps5_hostcall_fault_hook(uctx, (uint64_t)(uintptr_t)si->si_addr)) return;
  if ((sig == SIGSEGV || sig == SIGBUS) && si->si_code > 0) { // a memory fault, not a signal sent to the process
    const int n = atomic_load_explicit(&g_nresolvers, memory_order_acquire);
    const uint64_t at = (uint64_t)(uintptr_t)si->si_addr;
    const int saved_errno = errno; // the interrupted code may be between a system call and its errno check
    for (int i = 0; i < n; i++) {
      const int verdict = g_resolvers[i].context ? g_resolvers[i].context(uctx, at, fault_is_write(uctx))
                          : g_resolvers[i].access ? g_resolvers[i].access(at, fault_is_write(uctx)) : g_resolvers[i].plain(at);
      const bool claimed = verdict != 0;
      if (verdict == 1 && livelock_tripped(at)) {
        static const char m[] = "nps5: fault resolver livelock: a claimed fault at the same address keeps repeating without the page changing; treating it as unresolved\n";
        (void)!write(2, m, sizeof m - 1);
        break; // falls into the crash report below
      }
      if (claimed) { // resolved: not a crash, nothing is reported
        atomic_fetch_add_explicit(&g_resolved_faults, 1, memory_order_relaxed);
        errno = saved_errno;
        return;
      }
    }
    errno = saved_errno;
  }
  g_nps5_in_crash_handler = 1; // from here on no host access hook runs on this thread (nps5_mem.h)
  // A real crash from here on: bound the time to death before anything that can block (dladdr and the report take
  // dyld/stdio locks that the crashed thread may hold).
  signal(SIGALRM, SIG_DFL);
  alarm(2);
  int exp = 0;
  if (!atomic_compare_exchange_strong(&g_crashed, &exp, 1)) {
    if (atomic_load(&g_crash_owner) != (uintptr_t)pthread_self()) {
      // ANOTHER thread crashed while the first crash is still being reported (typical: the reporting thread is in
      // exit(), library teardown destroys a mutex, a thread that still runs throws from it and std::terminate
      // aborts). Ending the process here would cut the first report short (seen: no summary, "recursive crash").
      // Park this thread: the reporting thread ends the process, and its alarm bounds the wait.
      static const char m[] = "nps5: a second thread failed while the crash report is being written; it waits for the report\n";
      (void)!write(2, m, sizeof m - 1);
      for (;;) pause();
    }
    // A fault inside the handler on the reporting thread itself: give up quickly.
    static const char m[] = "nps5: recursive crash, aborting\n";
    (void)!write(2, m, sizeof m - 1);
    _exit(128 + sig);
  }
  atomic_store(&g_crash_owner, (uintptr_t)pthread_self());
  atomic_fetch_add(&g_faults, 1);
  crash_report(sig, si, uctx);
  // Symbolise with dladdr in a second pass; it is not async-signal-safe, so the report above is
  // already complete if this hangs or faults.
  if (g_crash_fd >= 0) {
    arm_thread_state64_t *ss = &((ucontext_t *)uctx)->uc_mcontext->__ss;
    Dl_info di;
    if (!recomp_pc_in_guest_code(ss->__pc) && dladdr((void *)(uintptr_t)ss->__pc, &di) && di.dli_sname) {
      Sw w = {.fd = g_crash_fd};
      sw_str(&w, "host_symbol: "); sw_str(&w, di.dli_sname); sw_str(&w, "+"); sw_hex(&w, ss->__pc - (uint64_t)(uintptr_t)di.dli_saddr); sw_str(&w, "\n");
      void *hf = t_hle_current ? t_hle_current : atomic_load(&g_hle_last);
      char nb[160];
      if (hf) { sw_str(&w, "hle_function: "); sw_str(&w, hle_name(hf, nb, sizeof nb)); sw_str(&w, "\n"); }
      sw_flush(&w);
    }
  }
  if (sig == SIGABRT) {
    // abort() does not run atexit handlers and the handler we chain to may re-raise: finalise the
    // bundle now (best effort; not async-signal-safe, so an alarm bounds it).
    alarm(2);
    signal(SIGALRM, SIG_DFL);
    diag_atexit_fwd();
  }
  // Chain to the handler installed before us (the legacy stderr report); for abort restore default.
  for (int i = 0; i < NSIG_HANDLED; i++) {
    if (k_sigs[i] != sig) continue;
    if (g_prev[i].sa_flags & SA_SIGINFO && g_prev[i].sa_sigaction) { g_prev[i].sa_sigaction(sig, si, uctx); return; }
    if (g_prev[i].sa_handler != SIG_DFL && g_prev[i].sa_handler != SIG_IGN && g_prev[i].sa_handler) { g_prev[i].sa_handler(sig); return; }
  }
  // No earlier handler: finalise the bundle (best effort; not async-signal-safe) and die by the signal.
  alarm(2);
  signal(SIGALRM, SIG_DFL);
  diag_atexit_fwd();
  signal(sig, SIG_DFL);
  raise(sig);
}

// Per-thread alternate signal stack: the handlers use SA_ONSTACK, so a thread without one dies silently on a stack
// overflow (the handler has no stack to run on). The stack is released through a pthread key destructor at thread exit.
#define NPS5_THREAD_ALTSTACK (64 * 1024)
static pthread_key_t g_altstack_key;
static pthread_once_t g_altstack_once = PTHREAD_ONCE_INIT;
static void altstack_release(void *p) {
  stack_t off = {.ss_flags = SS_DISABLE};
  sigaltstack(&off, NULL);
  free(p);
}
static void altstack_key_init(void) { pthread_key_create(&g_altstack_key, altstack_release); }
void nps5_diag_thread_init(void) {
  stack_t cur;
  if (sigaltstack(NULL, &cur) == 0 && !(cur.ss_flags & SS_DISABLE)) return; // already installed (idempotent)
  pthread_once(&g_altstack_once, altstack_key_init);
  void *mem = malloc(NPS5_THREAD_ALTSTACK);
  if (!mem) return;
  stack_t ss = {.ss_sp = mem, .ss_size = NPS5_THREAD_ALTSTACK, .ss_flags = 0};
  if (sigaltstack(&ss, NULL) != 0) { free(mem); return; }
  pthread_setspecific(g_altstack_key, mem);
}

void nps5_diag_install_crash_handlers(void) {
  stack_t ss = {.ss_sp = g_altstack, .ss_size = sizeof g_altstack, .ss_flags = 0};
  sigaltstack(&ss, NULL);
  for (int i = 0; i < NSIG_HANDLED; i++) {
    struct sigaction cur;
    if (sigaction(k_sigs[i], NULL, &cur) == 0 && (cur.sa_flags & SA_SIGINFO) && cur.sa_sigaction == crash_handler) continue;
    struct sigaction sa = {0};
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sa.sa_sigaction = crash_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(k_sigs[i], &sa, &g_prev[i]);
  }
}

// ------------------------------------------------------------------ summary + manifest
static int cmp_hle(const void *a, const void *b) {
  uint64_t x = ((const HitRow *)a)->hits, y = ((const HitRow *)b)->hits;
  return x < y ? 1 : x > y ? -1 : 0;
}

typedef struct { void *fn; uint64_t count; } HleRow;
static int cmp_hle_row(const void *a, const void *b) {
  uint64_t x = ((const HleRow *)a)->count, y = ((const HleRow *)b)->count;
  return x < y ? 1 : x > y ? -1 : 0;
}

static void write_summary(void) {
  if (!g_bundle_dir[0]) return;
  char path[1100];
  snprintf(path, sizeof path, "%s/summary.txt", g_bundle_dir);
  FILE *f = fopen(path, "w");
  if (!f) return;
  struct timeval t1;
  gettimeofday(&t1, NULL);
  double wall = (double)(t1.tv_sec - g_wall0.tv_sec) + (double)(t1.tv_usec - g_wall0.tv_usec) / 1e6;
  fprintf(f, "wall_time_s: %.3f\n", wall);
  if (g_exit_code >= 0) fprintf(f, "exit_code: %d\n", g_exit_code);
  fprintf(f, "faults: %d\ncrashed: %d\n", atomic_load(&g_faults), atomic_load(&g_crashed));
  if (recomp_call_model_flat()) {
    RecompShadowStats st;
    recomp_shadow_stats(&st);
    fprintf(f, "call_model: flat\nshadow_stacks: %llu\nshadow_slow_rets: %llu (resynced %llu, host boundary %llu, by address %llu)\n"
               "shadow_overflows: %llu\ndispatch_slow: %llu\n",
            (unsigned long long)st.stacks, (unsigned long long)st.slow_rets, (unsigned long long)st.slow_hits,
            (unsigned long long)st.slow_boundary, (unsigned long long)st.slow_by_address, (unsigned long long)st.overflows,
            (unsigned long long)st.dispatch_slow);
  } else {
    fprintf(f, "call_model: nested\n");
  }
  fprintf(f, "hle_calls_total: %llu\n", (unsigned long long)atomic_load(&g_hle_total));
  HleRow *rows = (HleRow *)malloc(HLE_SLOTS * sizeof(HleRow));
  size_t nr = 0;
  if (rows) {
    for (size_t i = 0; i < HLE_SLOTS; i++) {
      void *fn = atomic_load(&g_hle[i].fn);
      if (fn) { rows[nr].fn = fn; rows[nr].count = atomic_load(&g_hle[i].count); nr++; }
    }
    qsort(rows, nr, sizeof(HleRow), cmp_hle_row);
    fprintf(f, "hle_calls_top20:\n");
    for (size_t i = 0; i < nr && i < 20; i++) {
      char nm[160];
      fprintf(f, "  %10llu  %s\n", (unsigned long long)rows[i].count, hle_name(rows[i].fn, nm, sizeof nm));
    }
    free(rows);
  }
  fprintf(f, "unresolved_import_hits:\n");
  pthread_mutex_lock(&g_unres_mu);
  HitRow tmp[HIT_ROWS];
  int n = g_unres_n;
  for (int i = 0; i < n; i++) { memcpy(&tmp[i], &g_unres[i], sizeof(HitRow) - sizeof(_Atomic uint64_t)); tmp[i].hits = atomic_load(&g_unres[i].hits); }
  pthread_mutex_unlock(&g_unres_mu);
  qsort(tmp, (size_t)n, sizeof(HitRow), cmp_hle);
  for (int i = 0; i < n; i++)
    fprintf(f, "  %10llu  %s nid=%s lib=%s first_rip=0x%llx\n", (unsigned long long)tmp[i].hits, tmp[i].name, tmp[i].nid, tmp[i].lib, (unsigned long long)tmp[i].first_rip);
  fprintf(f, "unsupported_instruction_hits:\n");
  if (g_unsupported_hits) fprintf(f, "  %10llu  guest_pc=0x%llx\n", (unsigned long long)g_unsupported_hits, (unsigned long long)g_unsupported_pc);
  fclose(f);
  // Echo to stderr only when something is wrong.
  if (n || g_unsupported_hits || atomic_load(&g_crashed)) {
    fprintf(stderr, "[nps5] exit summary (%s/summary.txt): faults=%d unresolved_imports=%d unsupported_hits=%llu\n", g_bundle_dir,
            atomic_load(&g_faults), n, (unsigned long long)g_unsupported_hits);
  }
}

static const char *env_subset_prefixes[] = {"NPS5_", "PS4_", "PS4RECOMP_", "VK_", "MTL_", NULL};

static void write_manifest(int finished) {
  if (!g_bundle_dir[0]) return;
  char path[1100], tmp[1110];
  snprintf(path, sizeof path, "%s/manifest.json", g_bundle_dir);
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  FILE *f = fopen(tmp, "w");
  if (!f) return;
  char model[128] = "", osb[64] = "";
  size_t l = sizeof model;
  sysctlbyname("hw.model", model, &l, NULL, 0);
  l = sizeof osb;
  sysctlbyname("kern.osversion", osb, &l, NULL, 0);
  char exe[1024] = "";
  uint32_t el = sizeof exe;
  _NSGetExecutablePath(exe, &el);
  char e1[1100];
  fprintf(f, "{\n  \"phase\": \"run\",\n");
  fprintf(f, "  \"title\": \"%s\",\n", g_title);
  struct tm tm;
  time_t s = g_wall0.tv_sec;
  gmtime_r(&s, &tm);
  char ts[40];
  strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tm);
  fprintf(f, "  \"started\": \"%s.%03dZ\",\n", ts, (int)(g_wall0.tv_usec / 1000));
  if (finished) {
    struct timeval t1;
    gettimeofday(&t1, NULL);
    double wall = (double)(t1.tv_sec - g_wall0.tv_sec) + (double)(t1.tv_usec - g_wall0.tv_usec) / 1e6;
    fprintf(f, "  \"wall_ms\": %lld,\n", (long long)(wall * 1000));
    if (g_exit_code >= 0) fprintf(f, "  \"exit_code\": %d,\n", g_exit_code);
    fprintf(f, "  \"result\": \"%s\",\n", atomic_load(&g_crashed) ? "crash" : (g_exit_code > 0 ? "error" : "ok"));
  }
  fprintf(f, "  \"tool\": {\"name\": \"nps5 runtime\", \"git_commit\": \"%s\", \"git_dirty\": %s},\n", nps5_build_commit,
          nps5_build_dirty ? "true" : "false");
  char rd[1100];
  nps5_diag_redact_path(exe, rd, sizeof rd);
  json_escape(e1, sizeof e1, rd);
  fprintf(f, "  \"executable\": \"%s\",\n", e1);
  nps5_diag_redact_path(nps5_input_path, rd, sizeof rd);
  json_escape(e1, sizeof e1, rd);
  fprintf(f, "  \"inputs\": [{\"path\": \"%s\", \"sha256\": \"%s\"}],\n", e1, nps5_input_sha256);
  nps5_diag_redact_path(nps5_translate_bundle, rd, sizeof rd);
  json_escape(e1, sizeof e1, rd);
  fprintf(f, "  \"translate_bundle\": \"%s\",\n", e1);
  fprintf(f, "  \"host\": {\"model\": \"%s\", \"os_build\": \"%s\", \"os\": \"darwin\", \"arch\": \"arm64\"},\n", model, osb);
  fprintf(f, "  \"env\": {");
  int first = 1;
  extern char **environ;
  for (char **e = environ; e && *e; e++) {
    for (const char **p = env_subset_prefixes; *p; p++) {
      if (strncmp(*e, *p, strlen(*p)) == 0) {
        char k[128], v[512];
        const char *eq = strchr(*e, '=');
        if (!eq) break;
        size_t kl = (size_t)(eq - *e);
        if (kl >= sizeof k) break;
        memcpy(k, *e, kl);
        k[kl] = 0;
        char rv[512];
        nps5_diag_redact_path(eq + 1, rv, sizeof rv);
        json_escape(v, sizeof v, rv);
        fprintf(f, "%s\"%s\": \"%s\"", first ? "" : ", ", k, v);
        first = 0;
        break;
      }
    }
  }
  fprintf(f, "},\n  \"artifacts\": [");
  {
    static const char *const names[] = {"log.jsonl", "crash.txt", "summary.txt", "pcmap.bin"};
    int firstA = 1;
    for (int i = 0; i < 4; i++) {
      char ap[1200];
      snprintf(ap, sizeof ap, "%s/%s", g_bundle_dir, names[i]);
      struct stat ast;
      // crash.txt is pre-opened empty: it only counts once something was written to it
      if (stat(ap, &ast) == 0 && (i != 1 || ast.st_size > 0 || !finished)) {
        fprintf(f, "%s\"%s\"", firstA ? "" : ", ", names[i]);
        firstA = 0;
      }
    }
  }
  fprintf(f, "]\n}\n");
  fclose(f);
  rename(tmp, path);
}

static void diag_atexit(void) {
  if (atomic_exchange(&g_exiting, 1)) return;
  if (!g_started) return;
  rate_flush_all();
  nps5_log(NPS5_S_RUNTIME, NPS5_L_INFO, "exit: code=%d (-1 = not reported) faults=%d hle_calls=%llu", g_exit_code, atomic_load(&g_faults),
           (unsigned long long)atomic_load(&g_hle_total));
  write_summary();
  if (g_nps5_perf_finish) g_nps5_perf_finish(g_bundle_dir);
  write_manifest(1);
  if (g_crash_fd >= 0) {
    struct stat st;
    if (fstat(g_crash_fd, &st) == 0 && st.st_size == 0 && g_bundle_dir[0]) {
      char p[1100];
      snprintf(p, sizeof p, "%s/crash.txt", g_bundle_dir);
      unlink(p);
    }
    close(g_crash_fd);
    g_crash_fd = -1;
  }
  if (getenv("NPS5_DUMP_PCMAP") && g_pc_n && g_bundle_dir[0]) {
    char p[1100];
    snprintf(p, sizeof p, "%s/pcmap.bin", g_bundle_dir);
    int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) {
      write_pcmap_bin(fd);
      close(fd);
    }
  }
  if (g_log_fd >= 0) { close(g_log_fd); g_log_fd = -1; }
}

void nps5_diag_set_exit_code(int code) { g_exit_code = code; }

void nps5_diag_unsupported_hit(uint64_t rip, uint64_t fn_entry, const char *text) {
  g_unsup_rip = rip;
  g_unsup_fn = fn_entry;
  g_unsupported_pc = rip;
  g_unsupported_hits++;
  char t[160] = "";
  if (text && (uintptr_t)text > 0x1000) strlcpy(t, text, sizeof t);
  nps5_diag_set_note("UNSUPPORTED guest instruction at guest 0x%llx (function 0x%llx): %s", (unsigned long long)rip,
                     (unsigned long long)fn_entry, t);
  nps5_log(NPS5_S_LIFTER, NPS5_L_ERROR, "unsupported guest instruction executed: rip=0x%llx fn=0x%llx %s",
           (unsigned long long)rip, (unsigned long long)fn_entry, t);
}

void nps5_diag_set_tier1_failure(const char *kind, uint64_t rip, const char *module, uint64_t off, const char *reason,
                                 const char *detail) {
  strlcpy(g_t1_kind, kind ? kind : "", sizeof g_t1_kind);
  strlcpy(g_t1_module, module && *module ? module : "(no module)", sizeof g_t1_module);
  strlcpy(g_t1_reason, reason ? reason : "", sizeof g_t1_reason);
  nps5_diag_redact_path(detail ? detail : "", g_t1_detail, sizeof g_t1_detail);   // free text with paths (manifest, work dir)
  g_t1_rip = rip;
  g_t1_off = off;
  atomic_store(&g_t1_set, 1);
  nps5_diag_set_note("tier-1 miss at guest 0x%llx (%s+0x%llx): %s: %s", (unsigned long long)rip, g_t1_module,
                     (unsigned long long)off, g_t1_kind, g_t1_detail);
}

void nps5_diag_set_note(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char raw[sizeof g_note];
  vsnprintf(raw, sizeof raw, fmt, ap);
  va_end(ap);
  nps5_diag_redact_path(raw, g_note, sizeof g_note);   // notes quote paths too: home -> ~ like the rest of the bundle
}

const char *nps5_diag_bundle_dir(void) { return g_bundle_dir; }

// Host pc in recompiled guest code -> guest function (needs the pc map; 0 when absent).
int nps5_diag_host_pc_to_guest(uint64_t host_pc, uint64_t *guest_pc, uint64_t *fn_entry, const char **fn_name) {
  const PcEnt *e = (g_pc_n || atomic_load(&g_fm_n)) ? pc_lookup(host_pc) : NULL;
  if (!e) return 0;
  if (guest_pc) *guest_pc = e->guest & ((1ull << 56) - 1);
  if (fn_entry) *fn_entry = e->fn;
  if (fn_name) *fn_name = pc_fn_name(e->fn);
  return 1;
}

int nps5_diag_guest_fn_name(uint64_t fn_entry, const char **fn_name) {
  const char *n = pc_fn_name(fn_entry);
  if (fn_name) *fn_name = n;
  return n != NULL;
}

static void make_bundle(void) {
  if (getenv("NPS5_NO_BUNDLE")) return;
  const char *root = getenv("NPS5_RUNS_DIR");
  if (!root || !*root) root = "runs";
  char title[64] = "";
  const char *t = getenv("NPS5_TITLE");
  if (t && *t) {
    strlcpy(title, t, sizeof title);
  } else if (nps5_input_path[0]) {
    const char *b = strrchr(nps5_input_path, '/');
    strlcpy(title, b ? b + 1 : nps5_input_path, sizeof title);
    char *dot = strrchr(title, '.');
    if (dot) *dot = 0;
  } else {
    char exe[1024];
    uint32_t el = sizeof exe;
    if (_NSGetExecutablePath(exe, &el) == 0) {
      const char *b = strrchr(exe, '/');
      strlcpy(title, b ? b + 1 : exe, sizeof title);
    }
  }
  for (char *p = title; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-')) *p = '_';
  if (!title[0]) strlcpy(title, "untitled", sizeof title);
  strlcpy(g_title, title, sizeof g_title);
  mkdir(root, 0755);
  struct tm tm;
  time_t s = g_wall0.tv_sec;
  localtime_r(&s, &tm);
  char ts[32];
  strftime(ts, sizeof ts, "%Y%m%dT%H%M%S", &tm);
  for (int i = 0; i < 100; i++) {
    if (i == 0)
      snprintf(g_bundle_dir, sizeof g_bundle_dir, "%s/%s.%03d-%s-run", root, ts, (int)(g_wall0.tv_usec / 1000), title);
    else
      snprintf(g_bundle_dir, sizeof g_bundle_dir, "%s/%s.%03d-%s-run-%d", root, ts, (int)(g_wall0.tv_usec / 1000), title, i);
    if (mkdir(g_bundle_dir, 0755) == 0) break;
    if (errno != EEXIST) { g_bundle_dir[0] = 0; return; }
    g_bundle_dir[0] = 0;
  }
  if (!g_bundle_dir[0]) return;
  char p[1200];
  snprintf(p, sizeof p, "%s/log.jsonl", g_bundle_dir);
  g_log_fd = open(p, O_CREAT | O_WRONLY | O_APPEND, 0644);
  snprintf(p, sizeof p, "%s/crash.txt", g_bundle_dir);
  g_crash_fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  // latest symlinks: relative to root
  const char *base = strrchr(g_bundle_dir, '/');
  base = base ? base + 1 : g_bundle_dir;
  const char *links[2] = {"latest", "latest-run"};
  for (int i = 0; i < 2; i++) {
    char lp[1200], lt[1210];
    snprintf(lp, sizeof lp, "%s/%s", root, links[i]);
    snprintf(lt, sizeof lt, "%s.tmp%d", lp, (int)getpid());
    unlink(lt);
    if (symlink(base, lt) == 0) rename(lt, lp);
  }
}

// SIGTERM (e.g. from timeout(1)) and SIGINT: finalise the bundle, then die by the same signal.
// Best effort: this is not async-signal-safe, so an alarm forces the exit if it hangs.
static void term_handler(int sig) {
  // A second delivery (timeout(1) signals the process and its group; another thread takes it) must not
  // kill the process while the first is still writing the summary: it waits for the first to finish.
  static _Atomic int entered;
  if (atomic_exchange(&entered, 1)) {
    for (;;) pause();
  }
  alarm(2);
  signal(SIGALRM, SIG_DFL);
  nps5_log(NPS5_S_RUNTIME, NPS5_L_WARN, "terminated by signal %d", sig);
  g_exit_code = 128 + sig;
  diag_atexit();
  signal(sig, SIG_DFL);
  raise(sig);
}

__attribute__((constructor)) static void nps5_diag_init(void) {
  clock_gettime(CLOCK_MONOTONIC, &g_t0);
  gettimeofday(&g_wall0, NULL);
  { const char *h = getenv("HOME"), *fp = getenv("NPS5_DIAG_FULL_PATHS");
    if (h) strlcpy(g_home, h, sizeof g_home);
    g_full_paths = fp && *fp == '1'; }
  parse_log_env();
  crash_mem_parse(getenv("NPS5_CRASH_MEM"));
  const char *th = getenv("NPS5_TRACE_HLE");
  if (th) {
    strlcpy(g_trace_hle, th, sizeof g_trace_hle);
    nps5_lean_require_hooks(); // the trace is a bridge hook: lean calls must run it
  }
  make_bundle();
  g_started = 1;
  load_pcmap();
  if (!getenv("NPS5_NO_HLE_COUNT")) g_nps5_hle_hook = hle_hook;
  write_manifest(0);
  atexit(diag_atexit);
  if (!getenv("NPS5_NO_TERM_HANDLER")) {
    signal(SIGTERM, term_handler);
    signal(SIGINT, term_handler);
  }
  nps5_diag_install_crash_handlers(); // recomp_install_crash_handler chains to whatever it replaces
  nps5_log(NPS5_S_RUNTIME, NPS5_L_INFO, "run started: pcmap_entries=%zu image_base=0x%llx", g_pc_n, (unsigned long long)g_image_base);
}
