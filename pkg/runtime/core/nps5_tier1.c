// Tier-1 miss fallback (SPEC 3.4 / 3.10, docs/m1/tier1.md).
//
// recomp_dispatch_miss() is the slow-path entry of recomp_dispatch_api.h. A guest address with no
// translation is validated against the loaded modules, the translator (`ps4-recomp fragment`) is
// run out of process on a bounded fragment starting there, the resulting dylib is loaded, its
// entries are published in the dispatch table, and execution continues at the address.
//
// Rules kept here: misses are deduplicated on (generation, address) and waiters get the same
// result; the translator and clang run with no runtime lock held; the number of concurrent
// compiles, the time per compile, the dylib size, the number of fragments and the disk cache are
// capped; a cached dylib is validated (size and hash) before it is loaded; publication happens
// only after the library loaded and its descriptor checked, one release store per entry, the
// requested address last; nothing is ever unloaded; nothing here runs in signal context.
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <poll.h>
#include <sys/wait.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>

#include "recomp_context.h"
#include "recomp_dispatch_api.h"
#include "nps5_shadow.h"
#include "nps5_diag.h"
#include "ps4_sysmodule.h"

extern char **environ;

// ---- stamped by the translator (nps5_tier1_info.c); weak so host tests still link --------------
__attribute__((weak)) const char nps5_tier1_manifest[] = "";
__attribute__((weak)) const char nps5_tier1_translator[] = "";
__attribute__((weak)) const char nps5_tier1_xver[] = "";
__attribute__((weak)) const char nps5_tier1_elf_sha[] = "";
__attribute__((weak)) const char nps5_tier1_outdir[] = "";
__attribute__((weak)) const char nps5_tier1_call_model[] = "nested";
__attribute__((weak)) const char nps5_tier1_cache_root[] = "";

// The code generation counter belongs to the call/return model (recomp_dispatch_api.h); this weak
// definition only serves builds without it (it never changes: tier 1 never unloads code).
__attribute__((weak)) uint64_t recomp_code_generation(void) { return 0; }

typedef struct NPS5FragEntry {
  uint64_t guest;
  void (*fn)(void *);
} NPS5FragEntry;
typedef struct NPS5FragDesc {
  uint64_t magic;
  uint32_t abi;
  uint32_t n;
  const uint8_t *(*pcmap)(size_t *bytes); // records of pkg/emitter/pcmap.go
  const uint64_t *(*pads)(size_t *pairs); // {guest pc, host address}: landing pads, and in the flat model every block and return site
  NPS5FragEntry e[];
} NPS5FragDesc;
#define NPS5_FRAG_MAGIC 0x3147524650535035ull
#define NPS5_FRAG_ABI 2 // keep in step with cli.Tier1ABI
#define MAX_FRAG_ENTRIES 8192

// ---- configuration ----------------------------------------------------------------------------
static struct {
  int enabled;        // translator + manifest known and NPS5_MISS != fatal
  int fatal_mode;     // NPS5_MISS=fatal
  char cache[1024];   // <cache>/<elf sha>-<xver>
  int timeout_ms;     // per translator run (translate + clang + link)
  int max_concurrent;
  int max_fragments;
  long long max_dylib_bytes;
  long long cache_max_bytes;
  int flat;           // call model flat: the continuation of a function is its jump entry
} g_cfg;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static int env_int(const char *name, int dflt) {
  const char *v = getenv(name);
  if (!v || !*v) return dflt;
  char *end;
  long n = strtol(v, &end, 10);
  return (*end == 0 && n >= 0 && n < 1000000000) ? (int)n : dflt;
}

static void mkdir_p(const char *path) {
  char tmp[1100];
  strlcpy(tmp, path, sizeof tmp);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
      mkdir(tmp, 0755);
      *p = '/';
    }
  }
  mkdir(tmp, 0755);
}

static void cfg_init(void) {
  const char *miss = getenv("NPS5_MISS");
  g_cfg.fatal_mode = miss && !strcmp(miss, "fatal");
  g_cfg.timeout_ms = env_int("NPS5_TIER1_TIMEOUT_MS", 120000);
  g_cfg.max_concurrent = env_int("NPS5_TIER1_MAX_CONCURRENT", 2);
  if (g_cfg.max_concurrent < 1) g_cfg.max_concurrent = 1;
  g_cfg.max_fragments = env_int("NPS5_TIER1_MAX_FRAGMENTS", 1024);
  g_cfg.max_dylib_bytes = (long long)env_int("NPS5_TIER1_MAX_DYLIB_MB", 64) << 20;
  g_cfg.cache_max_bytes = (long long)env_int("NPS5_TIER1_CACHE_MB", 512) << 20;
  g_cfg.flat = recomp_call_model_flat(); // the model of the running image; the fragment is lifted in the manifest's, which the translator built this image with
  if (!nps5_tier1_manifest[0] || !nps5_tier1_translator[0] || !nps5_tier1_elf_sha[0]) return;
  const char *root = getenv("NPS5_TIER1_CACHE");
  char base[900];
  if (root && *root) strlcpy(base, root, sizeof base);
  else if (nps5_tier1_cache_root[0]) strlcpy(base, nps5_tier1_cache_root, sizeof base); // shared across retranslations
  else snprintf(base, sizeof base, "%s/tier1_cache", nps5_tier1_outdir);
  snprintf(g_cfg.cache, sizeof g_cfg.cache, "%s/%.16s-%.16s", base, nps5_tier1_elf_sha, nps5_tier1_xver);
  mkdir_p(g_cfg.cache);
  g_cfg.enabled = !g_cfg.fatal_mode;
}

// ---- time, ids, json --------------------------------------------------------------------------
static double mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static double g_t0_ms;

static size_t jesc(char *dst, size_t cap, const char *s) {
  size_t n = 0;
  for (; s && *s && n + 7 < cap; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') { dst[n++] = '\\'; dst[n++] = (char)c; }
    else if (c == '\n') { dst[n++] = '\\'; dst[n++] = 'n'; }
    else if (c < 0x20) { n += (size_t)snprintf(dst + n, cap - n, "\\u%04x", c); }
    else dst[n++] = (char)c;
  }
  dst[n] = 0;
  return n;
}

// ---- miss log ---------------------------------------------------------------------------------
typedef struct {
  uint64_t rip;
  const char *module;
  uint64_t offset;
  const char *reason;
  const char *outcome; // compiled, cache_hit, waited, raced, failed
  const char *kind;    // failure kind or ""
  double compile_ms;
  long long frag_bytes;
  int entries, insts;
  uint64_t gen;
  const char *detail;
} MissRec;

static const char *reason_name(uint32_t r) {
  switch (r) {
  case RECOMP_MISS_RET: return "ret";
  case RECOMP_MISS_CALL: return "call";
  case RECOMP_MISS_JUMP: return "jump";
  case RECOMP_MISS_RESUME: return "resume";
  }
  return "unknown";
}

static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_log_fd = -2;

static void miss_log(const MissRec *m) {
  char mod[300], det[1200], kind[100];
  jesc(mod, sizeof mod, m->module ? m->module : "");
  jesc(det, sizeof det, m->detail ? m->detail : "");
  jesc(kind, sizeof kind, m->kind ? m->kind : "");
  char line[2400];
  uint64_t tid = 0;
  pthread_threadid_np(NULL, &tid);
  int n = snprintf(line, sizeof line,
                   "{\"t\":%.3f,\"rip\":\"0x%llx\",\"module\":\"%s\",\"offset\":\"0x%llx\",\"reason\":\"%s\","
                   "\"outcome\":\"%s\",\"kind\":\"%s\",\"compile_ms\":%.1f,\"fragment_bytes\":%lld,\"entries\":%d,"
                   "\"insts\":%d,\"generation\":%llu,\"tid\":%llu,\"detail\":\"%s\"}\n",
                   (mono_ms() - g_t0_ms) / 1000.0, (unsigned long long)m->rip, mod, (unsigned long long)m->offset,
                   m->reason, m->outcome, kind, m->compile_ms, m->frag_bytes, m->entries, m->insts,
                   (unsigned long long)m->gen, (unsigned long long)tid, det);
  if (n > 0 && (size_t)n < sizeof line) {
    pthread_mutex_lock(&g_log_mu);
    if (g_log_fd == -2) {
      const char *dir = nps5_diag_bundle_dir();
      g_log_fd = -1;
      if (dir && *dir) {
        char p[1200];
        snprintf(p, sizeof p, "%s/tier1_misses.jsonl", dir);
        g_log_fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
      }
    }
    if (g_log_fd >= 0) (void)!write(g_log_fd, line, (size_t)n);
    pthread_mutex_unlock(&g_log_mu);
  }
  NPS5_LOG(NPS5_S_TIER1, m->kind && *m->kind ? NPS5_L_ERROR : NPS5_L_INFO,
           "miss rip=0x%llx %s+0x%llx %s: %s%s%s (%.1f ms)", (unsigned long long)m->rip, m->module ? m->module : "?",
           (unsigned long long)m->offset, m->reason, m->outcome, m->kind && *m->kind ? " " : "", m->kind ? m->kind : "",
           m->compile_ms);
}

// ---- dispatch table publication ---------------------------------------------------------------
static pthread_mutex_t g_pub_mu = PTHREAD_MUTEX_INITIALIZER;

extern recomp_fn_t recomp_lazy_resolve(uint64_t a);              // recomp_runtime.c (NPS5_LAZY_DISPATCH)
extern int recomp_dispatch_publish(uint64_t a, recomp_fn_t fn);  // recomp_runtime.c: CAS into an empty slot

// The registered entry of a: from the table, else (NPS5_LAZY_DISPATCH) from the lazy AOT registrations,
// which this publishes into the table.
static recomp_fn_t table_get(uint64_t a) {
  if (a == 0 || a >= (1ULL << 48)) return NULL;
  recomp_fn_t **l2 = __atomic_load_n(&g_dispatch_l1[(a >> DISPATCH_L1_SHIFT) & DISPATCH_LEVEL_MASK], __ATOMIC_ACQUIRE);
  recomp_fn_t *l3 = l2 ? __atomic_load_n(&l2[(a >> DISPATCH_L2_SHIFT) & DISPATCH_LEVEL_MASK], __ATOMIC_ACQUIRE) : NULL;
  recomp_fn_t fn = l3 ? __atomic_load_n(&l3[a & DISPATCH_LEVEL_MASK], __ATOMIC_ACQUIRE) : NULL;
  return fn ? fn : recomp_lazy_resolve(a);
}

// Sets the slot if it is empty (never replaces an AOT or earlier entry, lazy or published). Caller holds g_pub_mu.
// Returns 1 when published, 0 when the slot was taken, -1 on allocation failure.
static int table_put(uint64_t a, recomp_fn_t fn) {
  if (a == 0 || a >= (1ULL << 48)) return 0;
  if (table_get(a)) return 0;
  return recomp_dispatch_publish(a, fn);
}

// ---- in-flight misses (dedupe) ------------------------------------------------------------------
typedef struct Inflight {
  uint64_t gen, rip;
  int done, refs;
  uintptr_t result;
  char kind[40];
  char detail[1024];
  struct Inflight *next;
} Inflight;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static Inflight *g_inflight;
static int g_active_compiles;
static int g_fragments_total;
static unsigned g_seq;

// ---- results ----------------------------------------------------------------------------------
typedef struct {
  char kind[40];
  char detail[1024];
  char dylib[1100];
  int entries, insts;
  long long frag_bytes;
  double compile_ms;
  int cache_hit;
} Outcome;

static void out_fail(Outcome *o, const char *kind, const char *fmt, ...) {
  strlcpy(o->kind, kind, sizeof o->kind);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(o->detail, sizeof o->detail, fmt, ap);
  va_end(ap);
}

static uint64_t fnv_file(const char *path, long long *size) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  uint64_t h = 1469598103934665603ull;
  unsigned char buf[65536];
  size_t n;
  long long total = 0;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
    for (size_t i = 0; i < n; i++) { h ^= buf[i]; h *= 1099511628211ull; }
    total += (long long)n;
  }
  fclose(f);
  if (size) *size = total;
  return h;
}

static int read_file(const char *path, char *buf, size_t cap) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  size_t n = fread(buf, 1, cap - 1, f);
  fclose(f);
  buf[n] = 0;
  return (int)n;
}

// Value of "key=" at the start of a line.
static int kv_get(const char *text, const char *key, char *out, size_t cap) {
  size_t kl = strlen(key);
  for (const char *p = text; *p;) {
    if (!strncmp(p, key, kl) && p[kl] == '=') {
      const char *v = p + kl + 1;
      size_t n = strcspn(v, "\n");
      if (n >= cap) n = cap - 1;
      memcpy(out, v, n);
      out[n] = 0;
      return 1;
    }
    p += strcspn(p, "\n");
    if (*p) p++;
  }
  return 0;
}

static void tail_of(const char *path, char *out, size_t cap) {
  out[0] = 0;
  char buf[4096];
  int n = read_file(path, buf, sizeof buf);
  if (n <= 0) return;
  const char *p = buf + (n > (int)cap - 1 ? n - ((int)cap - 1) : 0);
  strlcpy(out, p, cap);
  for (char *q = out; *q; q++) if (*q == '\n') *q = '|';
}

// ---- running the translator ----------------------------------------------------------------------
// ---- persistent translator server (docs/m3/iteration-speed.md) ------------------------------------
// `ps4-recomp fragment-serve` loads the title once; a miss then costs the lift and the clang/link of
// the fragment (about 0.2 s) instead of 2-3 s of loading the ELF. One request per connection. Any
// problem with the server itself (cannot start, connection lost, no answer) disables it and the
// miss is served by the process-per-miss path below; a failure of the fragment is reported through
// result.txt exactly as there. NPS5_TIER1_SERVER=0 turns it off.
static pthread_mutex_t g_srv_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_srv_state; // 0 not started, 1 running, -1 disabled
static pid_t g_srv_pid;
static char g_srv_sock[104];

static int srv_connect(void) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_un a;
  memset(&a, 0, sizeof a);
  a.sun_family = AF_UNIX;
  strlcpy(a.sun_path, g_srv_sock, sizeof a.sun_path);
  if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static void srv_atexit(void) {
  if (g_srv_pid > 0) kill(g_srv_pid, SIGTERM);
}

// mkdir -p-less creation of <base>/nps5t1-<uid> with mode 0700, verified: a directory (not a symlink), owned by us, no
// group/other access. Candidates: $TMPDIR (when short enough for sun_path), the tier-1 cache directory, /tmp.
static int srv_private_dir_at(const char *base, char *out, size_t cap) {
  char d[512];
  if (strlen(base) > 60) return 0;
  snprintf(d, sizeof d, "%s/nps5t1-%u", base, (unsigned)getuid());
  if (mkdir(d, 0700) != 0 && errno != EEXIST) return 0;
  struct stat st;
  if (lstat(d, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077) != 0) return 0;
  if (strlen(d) >= cap) return 0;
  strcpy(out, d);
  return 1;
}
static int srv_private_dir(char *out, size_t cap) {
  const char *tmp = getenv("TMPDIR");
  if (tmp && *tmp && srv_private_dir_at(tmp, out, cap)) return 1;
  if (srv_private_dir_at(g_cfg.cache, out, cap)) return 1;
  return srv_private_dir_at("/tmp", out, cap);
}

static int srv_ensure(void) {

  pthread_mutex_lock(&g_srv_mu);
  if (g_srv_state != 0) {
    int r = g_srv_state == 1 ? 0 : -1;
    pthread_mutex_unlock(&g_srv_mu);
    return r;
  }
  g_srv_state = -1;
  const char *en = getenv("NPS5_TIER1_SERVER");
  if (en && !strcmp(en, "0")) goto out;
  // The socket lives in a private (0700, ours) directory: never directly in a shared /tmp, where the name would be
  // predictable. The server also checks the peer uid of every connection.
  char sdir[120];
  if (!srv_private_dir(sdir, sizeof sdir)) goto out;
  snprintf(g_srv_sock, sizeof g_srv_sock, "%s/s.%d", sdir, (int)getpid());
  char par[24], logp[1200];
  snprintf(par, sizeof par, "%d", (int)getpid());
  snprintf(logp, sizeof logp, "%s/server.log", g_cfg.cache);
  char *argv[] = {(char *)nps5_tier1_translator, "fragment-serve", "--manifest", (char *)nps5_tier1_manifest, "--socket", g_srv_sock,
                  "--parent", par, NULL};
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 1, logp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  posix_spawn_file_actions_adddup2(&fa, 1, 2);
  posix_spawnattr_t at;
  posix_spawnattr_init(&at);
  sigset_t none, all;
  sigemptyset(&none);
  sigfillset(&all);
  posix_spawnattr_setsigmask(&at, &none);
  posix_spawnattr_setsigdefault(&at, &all);
  posix_spawnattr_setpgroup(&at, 0);
  posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);
  int rc = posix_spawn(&g_srv_pid, nps5_tier1_translator, &fa, &at, argv, environ);
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&at);
  if (rc != 0) {
    g_srv_pid = 0;
    goto out;
  }
  atexit(srv_atexit);
  double deadline = mono_ms() + 60000.0; // loading a 26 M instruction title takes a few seconds
  for (;;) {
    int fd = srv_connect();
    if (fd >= 0) {
      close(fd);
      g_srv_state = 1;
      break;
    }
    int status;
    if (waitpid(g_srv_pid, &status, WNOHANG) == g_srv_pid) {
      g_srv_pid = 0;
      break;
    }
    if (mono_ms() > deadline) {
      kill(g_srv_pid, SIGKILL);
      waitpid(g_srv_pid, &status, 0);
      g_srv_pid = 0;
      break;
    }
    usleep(20000);
  }
out:;
  int r = g_srv_state == 1 ? 0 : -1;
  pthread_mutex_unlock(&g_srv_mu);
  return r;
}

// 0: the server built the fragment (read result.txt); -1: the server is not usable for this request.
static int run_via_server(uint64_t rip, const char *work) {
  if (srv_ensure() != 0) return -1;
  int fd = srv_connect();
  if (fd < 0) {
    pthread_mutex_lock(&g_srv_mu);
    g_srv_state = -1;
    pthread_mutex_unlock(&g_srv_mu);
    return -1;
  }
  char req[1500];
  int n = snprintf(req, sizeof req, "entry=0x%llx out=%s timeout-ms=%d\n", (unsigned long long)rip, work, g_cfg.timeout_ms);
  if (write(fd, req, (size_t)n) != n) {
    close(fd);
    return -1;
  }
  double deadline = mono_ms() + g_cfg.timeout_ms + 5000.0;
  char buf[64];
  size_t got = 0;
  for (;;) {
    double left = deadline - mono_ms();
    if (left <= 0) break;
    struct pollfd p = {fd, POLLIN, 0};
    int pr = poll(&p, 1, left > 1000 ? 1000 : (int)left);
    if (pr < 0 && errno != EINTR) break;
    if (pr > 0) {
      ssize_t r = read(fd, buf + got, sizeof buf - 1 - got);
      if (r <= 0) break;
      got += (size_t)r;
      buf[got] = 0;
      if (strchr(buf, '\n')) break;
    }
  }
  close(fd);
  return (got >= 4 && !strncmp(buf, "done", 4)) ? 0 : -1;
}

static int read_result(const char *work, const char *logp, int status, int have_status, Outcome *o);
static int run_translator_proc(uint64_t rip, const char *work, Outcome *o);

static int run_translator(uint64_t rip, const char *work, Outcome *o) {
  if (run_via_server(rip, work) == 0) {
    char logp[1200];
    snprintf(logp, sizeof logp, "%s/log.txt", work);
    return read_result(work, logp, 0, 0, o);
  }
  return run_translator_proc(rip, work, o);
}

static int run_translator_proc(uint64_t rip, const char *work, Outcome *o) {
  char rip_s[32], tmo[32], logp[1200];
  snprintf(rip_s, sizeof rip_s, "0x%llx", (unsigned long long)rip);
  snprintf(tmo, sizeof tmo, "%d", g_cfg.timeout_ms);
  snprintf(logp, sizeof logp, "%s/log.txt", work);
  char *argv[] = {(char *)nps5_tier1_translator, "fragment", "--manifest", (char *)nps5_tier1_manifest, "--entry",
                  rip_s, "--out", (char *)work, "--timeout-ms", tmo, NULL};
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 1, logp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  posix_spawn_file_actions_adddup2(&fa, 1, 2);
  posix_spawnattr_t at;
  posix_spawnattr_init(&at);
  sigset_t none, all;
  sigemptyset(&none);
  sigfillset(&all);
  posix_spawnattr_setsigmask(&at, &none);
  posix_spawnattr_setsigdefault(&at, &all);
  posix_spawnattr_setpgroup(&at, 0);
  posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);
  pid_t pid;
  int rc = posix_spawn(&pid, nps5_tier1_translator, &fa, &at, argv, environ);
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&at);
  if (rc != 0) {
    out_fail(o, "translator_spawn_failed", "cannot run %s: %s", nps5_tier1_translator, strerror(rc));
    return -1;
  }
  double deadline = mono_ms() + g_cfg.timeout_ms + 5000.0;
  int status = 0;
  for (;;) {
    pid_t w = waitpid(pid, &status, WNOHANG);
    if (w == pid) break;
    if (w < 0 && errno != EINTR) {
      out_fail(o, "translator_wait_failed", "waitpid: %s", strerror(errno));
      return -1;
    }
    if (mono_ms() > deadline) {
      kill(-pid, SIGKILL);
      waitpid(pid, &status, 0);
      out_fail(o, "timeout", "translator exceeded %d ms and was killed", g_cfg.timeout_ms);
      return -1;
    }
    usleep(2000);
  }
  return read_result(work, logp, status, 1, o);
}

static int read_result(const char *work, const char *logp, int status, int have_status, Outcome *o) {
  char res[8192];
  char rp[1200];
  snprintf(rp, sizeof rp, "%s/result.txt", work);
  if (read_file(rp, res, sizeof res) > 0) {
    char ok[8] = "";
    kv_get(res, "ok", ok, sizeof ok);
    if (!strcmp(ok, "1")) {
      char v[64];
      if (kv_get(res, "dylib", o->dylib, sizeof o->dylib)) {
        if (kv_get(res, "insts", v, sizeof v)) o->insts = atoi(v);
        if (kv_get(res, "funcs", v, sizeof v)) o->entries = atoi(v);
        return 0;
      }
    }
    char kind[40] = "internal", msg[900] = "";
    kv_get(res, "kind", kind, sizeof kind);
    kv_get(res, "message", msg, sizeof msg);
    out_fail(o, kind, "%s", msg);
    return -1;
  }
  char tail[400];
  tail_of(logp, tail, sizeof tail);
  if (!have_status) out_fail(o, "translator_failed", "server answered but wrote no result.txt; log: %s", tail);
  else if (WIFSIGNALED(status)) out_fail(o, "translator_crashed", "signal %d; log: %s", WTERMSIG(status), tail);
  else out_fail(o, "translator_failed", "exit %d, no result.txt; log: %s", WEXITSTATUS(status), tail);
  return -1;
}

// ---- cache --------------------------------------------------------------------------------------
static void cache_paths(uint64_t rip, char *dylib, size_t dcap, char *meta, size_t mcap) {
  snprintf(dylib, dcap, "%s/f_%llx.dylib", g_cfg.cache, (unsigned long long)rip);
  snprintf(meta, mcap, "%s/f_%llx.meta", g_cfg.cache, (unsigned long long)rip);
}

// A cached fragment is used only if its meta and dylib agree (abi, size, hash): a torn or
// altered file is recompiled, never loaded.
static int cache_lookup(uint64_t rip, Outcome *o) {
  char dy[1100], me[1100], txt[4096], v[64];
  cache_paths(rip, dy, sizeof dy, me, sizeof me);
  if (read_file(me, txt, sizeof txt) <= 0) return 0;
  if (!kv_get(txt, "abi", v, sizeof v) || atoi(v) != NPS5_FRAG_ABI) return 0;
  long long want_size = 0, size = 0;
  if (!kv_get(txt, "size", v, sizeof v)) return 0;
  want_size = atoll(v);
  if (!kv_get(txt, "fnv", v, sizeof v)) return 0;
  uint64_t want_h = strtoull(v, NULL, 16);
  uint64_t h = fnv_file(dy, &size);
  if (h == 0 || size != want_size || h != want_h) return 0;
  strlcpy(o->dylib, dy, sizeof o->dylib);
  o->frag_bytes = size;
  if (kv_get(txt, "insts", v, sizeof v)) o->insts = atoi(v);
  if (kv_get(txt, "funcs", v, sizeof v)) o->entries = atoi(v);
  o->cache_hit = 1;
  return 1;
}

typedef struct { char path[1200]; time_t mtime; long long size; } CEnt;
static int cent_cmp(const void *a, const void *b) {
  time_t x = ((const CEnt *)a)->mtime, y = ((const CEnt *)b)->mtime;
  return x < y ? -1 : x > y;
}

// Keeps the cache under its cap by removing the oldest fragments (never the one just stored).
static void cache_evict(const char *keep) {
  DIR *d = opendir(g_cfg.cache);
  if (!d) return;
  CEnt *e = NULL;
  size_t n = 0, cap = 0;
  long long total = 0;
  struct dirent *de;
  while ((de = readdir(d))) {
    size_t l = strlen(de->d_name);
    if (l < 7 || strcmp(de->d_name + l - 6, ".dylib")) continue;
    if (n == cap) {
      cap = cap ? cap * 2 : 64;
      CEnt *ne = (CEnt *)realloc(e, cap * sizeof *e);
      if (!ne) break;
      e = ne;
    }
    snprintf(e[n].path, sizeof e[n].path, "%s/%s", g_cfg.cache, de->d_name);
    struct stat st;
    if (stat(e[n].path, &st) != 0) continue;
    e[n].mtime = st.st_mtime;
    e[n].size = st.st_size;
    total += st.st_size;
    n++;
  }
  closedir(d);
  if (total > g_cfg.cache_max_bytes && e) {
    qsort(e, n, sizeof *e, cent_cmp);
    for (size_t i = 0; i < n && total > g_cfg.cache_max_bytes; i++) {
      if (!strcmp(e[i].path, keep)) continue;
      char me[1200];
      strlcpy(me, e[i].path, sizeof me);
      strlcpy(me + strlen(me) - 6, ".meta", 7);
      unlink(me);
      if (unlink(e[i].path) == 0) total -= e[i].size;
    }
  }
  free(e);
}

// Compiles the fragment for rip into the cache. Called with no lock held.
static int compile_fragment(uint64_t rip, Outcome *o) {
  pthread_mutex_lock(&g_mu);
  if (g_fragments_total >= g_cfg.max_fragments) {
    pthread_mutex_unlock(&g_mu);
    out_fail(o, "cap_exceeded", "NPS5_TIER1_MAX_FRAGMENTS=%d fragments already compiled in this run", g_cfg.max_fragments);
    return -1;
  }
  while (g_active_compiles >= g_cfg.max_concurrent) pthread_cond_wait(&g_cv, &g_mu);
  g_active_compiles++;
  g_fragments_total++;
  unsigned seq = ++g_seq;
  pthread_mutex_unlock(&g_mu);

  char work[1200];
  snprintf(work, sizeof work, "%s/w_%llx_%d_%u", g_cfg.cache, (unsigned long long)rip, (int)getpid(), seq);
  mkdir(work, 0755);
  double t = mono_ms();
  int rc = run_translator(rip, work, o);
  o->compile_ms = mono_ms() - t;

  if (rc == 0) {
    char dy[1100], me[1100];
    cache_paths(rip, dy, sizeof dy, me, sizeof me);
    long long size = 0;
    uint64_t h = fnv_file(o->dylib, &size);
    if (!h || size <= 0) {
      out_fail(o, "compile_failed", "translator reported %s but it is missing or empty", o->dylib);
      rc = -1;
    } else if (size > g_cfg.max_dylib_bytes) {
      out_fail(o, "cap_exceeded", "fragment is %lld bytes, NPS5_TIER1_MAX_DYLIB_MB allows %lld", size, g_cfg.max_dylib_bytes);
      rc = -1;
    } else {
      char mt[1100];
      snprintf(mt, sizeof mt, "%s.tmp%u", me, seq);
      FILE *f = fopen(mt, "w");
      if (f) {
        fprintf(f, "abi=%d\nsize=%lld\nfnv=%llx\ninsts=%d\nfuncs=%d\n", NPS5_FRAG_ABI, size, (unsigned long long)h, o->insts,
                o->entries);
        fclose(f);
      }
      // dylib first, then meta: a reader needs both and checks them against each other.
      if (rename(o->dylib, dy) != 0 || !f || rename(mt, me) != 0) {
        out_fail(o, "cache_write_failed", "cannot store %s: %s", dy, strerror(errno));
        rc = -1;
      } else {
        strlcpy(o->dylib, dy, sizeof o->dylib);
        o->frag_bytes = size;
        cache_evict(dy);
      }
    }
  }
  if (rc == 0) {
    char p[1300];
    const char *names[] = {"log.txt", "result.txt", "code_000.s", "frag_desc.c", "frag.dylib", "unsupported.txt", NULL};
    for (int i = 0; names[i]; i++) {
      snprintf(p, sizeof p, "%s/%s", work, names[i]);
      unlink(p);
    }
    rmdir(work);
  } else {
    char extra[1200];
    snprintf(extra, sizeof extra, " (work dir kept: %s)", work);
    size_t l = strlen(o->detail);
    if (l + strlen(extra) < sizeof o->detail) strcat(o->detail, extra);
  }
  pthread_mutex_lock(&g_mu);
  g_active_compiles--;
  pthread_cond_broadcast(&g_cv);
  pthread_mutex_unlock(&g_mu);
  return rc;
}

// ---- loading and publishing -----------------------------------------------------------------------
static int module_text_ok(uint64_t a) { return recomp_module_text_contains(a); }

// dlopens the fragment and publishes its entries. The descriptor is validated first: ABI, size
// bounds, every entry inside executable guest code of a loaded module, the requested rip among
// them. Entries are then stored with release semantics, the requested one last.
// Unloads a fragment that was never (or no longer) reachable. Shadow continuations and the
// continuation table are purged of its range first (recomp_dispatch_api.h): a shadow entry must
// not reference unloaded code.
static void unload(void *h, uint64_t lo, uint64_t hi) {
  if (hi > lo) recomp_shadow_invalidate((uintptr_t)lo, (uintptr_t)hi);
  dlclose(h);
}

static int load_and_publish(const char *path, uint64_t rip, Outcome *o, uintptr_t *result) {
  void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    const char *e = dlerror();
    out_fail(o, "load_failed", "dlopen %s: %s", path, e ? e : "?");
    return -1;
  }
  const NPS5FragDesc *d = (const NPS5FragDesc *)dlsym(h, "nps5_frag_desc");
  if (!d || d->magic != NPS5_FRAG_MAGIC || d->abi != NPS5_FRAG_ABI || d->n == 0 || d->n > MAX_FRAG_ENTRIES) {
    out_fail(o, "bad_descriptor", "%s: missing or incompatible nps5_frag_desc (abi %u, entries %u)", path, d ? d->abi : 0,
             d ? d->n : 0);
    dlclose(h); // nothing of it was published
    return -1;
  }
  // Host range of the fragment's guest code (the __recomp_guest section of the dylib).
  uint64_t rlo = 0, rhi = 0;
  Dl_info di;
  if (dladdr((const void *)d, &di) && di.dli_fbase) {
    unsigned long sz = 0;
    uint8_t *sec = getsectiondata((const struct mach_header_64 *)di.dli_fbase, "__TEXT", "__recomp_guest", &sz);
    if (sec && sz) { rlo = (uint64_t)(uintptr_t)sec; rhi = rlo + sz; }
  }
  int has_rip = 0;
  uint64_t lo = ~0ull, hi = 0;
  for (uint32_t i = 0; i < d->n; i++) {
    if (!module_text_ok(d->e[i].guest)) {
      out_fail(o, "bad_descriptor", "%s: entry 0x%llx is not in executable guest code", path, (unsigned long long)d->e[i].guest);
      unload(h, rlo, rhi);
      return -1;
    }
    if (d->e[i].guest == rip) has_rip = 1;
    uint64_t f = (uint64_t)(uintptr_t)d->e[i].fn;
    if (f < lo) lo = f;
    if (f > hi) hi = f;
  }
  if (!has_rip) {
    out_fail(o, "bad_descriptor", "%s: the fragment does not contain 0x%llx", path, (unsigned long long)rip);
    unload(h, rlo, rhi);
    return -1;
  }
  if (!rhi) { rlo = lo; rhi = hi + 4; }
  size_t pn = 0;
  const uint8_t *pm = d->pcmap ? d->pcmap(&pn) : NULL;
  nps5_diag_register_fragment(rlo, rhi, pm, pn, path);
  // Landing pads (C++ exceptions thrown into the fragment's functions, nps5_eh.c) and, in the flat
  // model, every block and return site: published through the continuation table, which both
  // consult. A static (AOT) entry for the same address wins.
  size_t padn = 0;
  const uint64_t *pads = d->pads ? d->pads(&padn) : NULL;
  for (size_t i = 0; pads && i < padn; i++) {
    if (pads[2 * i] == ~0ull || !pads[2 * i + 1]) continue; // the section's sentinel
    if (!recomp_cont_table_lookup(pads[2 * i])) recomp_cont_register(pads[2 * i], (uintptr_t)pads[2 * i + 1]);
  }
  const int off = g_cfg.flat ? RECOMP_FLAT_JUMP_ENTRY_OFFSET : 0;

  pthread_mutex_lock(&g_pub_mu);
  int fail = 0;
  for (uint32_t i = 0; i < d->n && !fail; i++) {
    if (d->e[i].guest == rip) continue;
    if (table_put(d->e[i].guest, (recomp_fn_t)d->e[i].fn) < 0) fail = 1;
  }
  recomp_fn_t cur = NULL;
  if (!fail) {
    for (uint32_t i = 0; i < d->n; i++)
      if (d->e[i].guest == rip) {
        if (table_put(rip, (recomp_fn_t)d->e[i].fn) < 0) fail = 1;
        break;
      }
    cur = table_get(rip);
  }
  pthread_mutex_unlock(&g_pub_mu);
  if (fail || !cur) {
    out_fail(o, "publish_failed", "cannot register the entries of %s", path);
    return -1; // library stays loaded (never unloaded); nothing was published for rip
  }
  // A flat-model continuation is the jump entry of the function; published for the RET and
  // RESUME slow paths (return sites, longjmp/unwinder targets) that look addresses up there.
  // The nested model cannot enter a function body by address, so it has no such entry.
  if (g_cfg.flat) recomp_cont_register(rip, (uintptr_t)cur + (uintptr_t)off);
  o->entries = (int)d->n;
  *result = (uintptr_t)cur + (uintptr_t)off;
  return 0;
}

// ---- the entry --------------------------------------------------------------------------------------
static void module_of(uint64_t rip, const char **name, uint64_t *off) {
  *name = "";
  *off = rip;
  const RecompModuleUnwindInfo *m = recomp_module_find_by_addr(rip);
  if (m && m->name) {
    *name = m->name;
    *off = rip - m->start_addr;
  }
}

static void note_failure(uint64_t rip, uint32_t reason, const char *kind, const char *detail) {
  const char *mod;
  uint64_t off;
  module_of(rip, &mod, &off);
  nps5_diag_set_tier1_failure(kind, rip, mod, off, reason_name(reason), detail);
}

uintptr_t recomp_dispatch_miss(struct GuestContext *ctx, uint64_t rip, uint32_t reason) {
  (void)ctx;
  pthread_once(&g_once, cfg_init);
  {
    // A function the AOT image registered that the lazy dispatch table has not published yet: not a miss.
    recomp_fn_t lz = recomp_lazy_resolve(rip);
    if (lz) return (uintptr_t)lz + (g_cfg.flat ? RECOMP_FLAT_JUMP_ENTRY_OFFSET : 0);
  }
  if (g_t0_ms == 0) g_t0_ms = mono_ms();
  double t0 = mono_ms();
  const char *mod;
  uint64_t off;
  module_of(rip, &mod, &off);
  const char *rn = reason_name(reason);
  uint64_t gen = recomp_code_generation();
  MissRec rec = {.rip = rip, .module = mod, .offset = off, .reason = rn, .gen = gen, .kind = "", .detail = ""};

  // Another thread may have published it since the caller looked.
  recomp_fn_t have = table_get(rip);
  if (have) {
    rec.outcome = "raced";
    miss_log(&rec);
    return (uintptr_t)have + (g_cfg.flat ? RECOMP_FLAT_JUMP_ENTRY_OFFSET : 0);
  }
  if (g_cfg.fatal_mode) {
    rec.outcome = "failed"; rec.kind = "miss_fatal"; rec.detail = "NPS5_MISS=fatal";
    miss_log(&rec);
    note_failure(rip, reason, "miss_fatal", "NPS5_MISS=fatal: a dispatch miss is a discovery bug, fix it at the source");
    return 0;
  }
  if (!g_cfg.enabled) {
    rec.outcome = "failed"; rec.kind = "disabled";
    rec.detail = "this build has no tier-1 configuration (translated before tier 1, or the translator could not write it)";
    miss_log(&rec);
    note_failure(rip, reason, "disabled", rec.detail);
    return 0;
  }
  if (rip == 0 || rip >= (1ULL << 48) || !recomp_module_text_contains(rip)) {
    rec.outcome = "failed"; rec.kind = "outside_guest_code";
    rec.detail = "target is not in an executable segment of any loaded guest module (jump into data, a wild or null pointer, or guest-generated code, which needs tier 2)";
    miss_log(&rec);
    note_failure(rip, reason, "outside_guest_code", rec.detail);
    return 0;
  }

  // Deduplicate on (generation, rip): the first thread compiles, the others wait for its result.
  pthread_mutex_lock(&g_mu);
  Inflight *in = g_inflight;
  while (in && !(in->rip == rip && in->gen == gen)) in = in->next;
  if (in) {
    in->refs++;
    while (!in->done) pthread_cond_wait(&g_cv, &g_mu);
    uintptr_t r = in->result;
    char kind[40], detail[1024];
    strlcpy(kind, in->kind, sizeof kind);
    strlcpy(detail, in->detail, sizeof detail);
    if (--in->refs == 0) free(in);
    pthread_mutex_unlock(&g_mu);
    rec.compile_ms = mono_ms() - t0;
    rec.detail = detail;
    if (r) {
      rec.outcome = "waited";
    } else {
      rec.outcome = "failed"; rec.kind = kind;
      note_failure(rip, reason, kind, detail);
    }
    miss_log(&rec);
    return r;
  }
  in = (Inflight *)calloc(1, sizeof *in);
  if (!in) {
    pthread_mutex_unlock(&g_mu);
    rec.outcome = "failed"; rec.kind = "out_of_memory"; rec.detail = "cannot allocate the miss record";
    miss_log(&rec);
    note_failure(rip, reason, "out_of_memory", rec.detail);
    return 0;
  }
  in->rip = rip; in->gen = gen; in->refs = 1;
  in->next = g_inflight;
  g_inflight = in;
  pthread_mutex_unlock(&g_mu);

  // Outside every lock: cache lookup, translator + clang, dlopen.
  Outcome o;
  memset(&o, 0, sizeof o);
  uintptr_t result = 0;
  int rc = 0;
  if (cache_lookup(rip, &o)) rc = 0;
  else rc = compile_fragment(rip, &o);
  if (rc == 0) rc = load_and_publish(o.dylib, rip, &o, &result);
  if (rc != 0 && o.cache_hit) {
    // A cached library that no longer loads: drop it and compile again once.
    char dy[1100], me[1100];
    cache_paths(rip, dy, sizeof dy, me, sizeof me);
    unlink(me);
    unlink(dy);
    char first[1024];
    strlcpy(first, o.detail, sizeof first);
    memset(&o, 0, sizeof o);
    rc = compile_fragment(rip, &o);
    if (rc == 0) rc = load_and_publish(o.dylib, rip, &o, &result);
  }
  rec.compile_ms = mono_ms() - t0;
  rec.frag_bytes = o.frag_bytes;
  rec.entries = o.entries;
  rec.insts = o.insts;

  pthread_mutex_lock(&g_mu);
  in->result = rc == 0 ? result : 0;
  strlcpy(in->kind, rc == 0 ? "" : o.kind, sizeof in->kind);
  strlcpy(in->detail, rc == 0 ? "" : o.detail, sizeof in->detail);
  in->done = 1;
  for (Inflight **pp = &g_inflight; *pp; pp = &(*pp)->next)
    if (*pp == in) { *pp = in->next; break; }
  pthread_cond_broadcast(&g_cv);
  if (--in->refs == 0) free(in);
  pthread_mutex_unlock(&g_mu);

  if (rc == 0) {
    rec.outcome = o.cache_hit ? "cache_hit" : "compiled";
    miss_log(&rec);
    return result;
  }
  rec.outcome = "failed"; rec.kind = o.kind; rec.detail = o.detail;
  miss_log(&rec);
  note_failure(rip, reason, o.kind, o.detail);
  return 0;
}
