// Host unit test of the crash/diagnostics machinery (links pkg/runtime/core/nps5_diag.c with stubs; no guest code):
//  - nps5_diag_redact_path_home: home-directory redaction of paths written to the run bundle
//  - nps5_diag_thread_init: a private alternate signal stack on a second thread
//  - a crash with a corrupt frame pointer still leaves the report header and a truncated-walk note in crash.txt
//  - a stack overflow on a thread that called nps5_diag_thread_init is reported
// Modes: no argument = run everything (re-executing itself for the crashing cases); "fp" / "overflow" = crash.
// Built and run by tools/diag_unit.sh, which sets NPS5_RUNS_DIR.
#include "nps5_diag.h"
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// ---- stubs for what nps5_diag.c links against but a host-only test has no use for
char g_nps5_arena[512];
int g_recomp_call_model;
const char *nps5_fault_class_name(int c) { (void)c; return "?"; }
void nps5_mem_classify(uint64_t a, void *o) { (void)a; memset(o, 0, 128); }
bool nps5_guest_safe_read(uint64_t g, void *d, size_t n) { (void)g; (void)d; (void)n; return false; }
bool nps5_mem_safe_read(const void *h, void *d, size_t n) {
  mach_vm_size_t got = 0;
  return mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)h, n, (mach_vm_address_t)(uintptr_t)d, &got) == KERN_SUCCESS && got == n;
}
void *recomp_get_current_ctx(void) { return NULL; }
bool recomp_pc_in_guest_code(uint64_t pc) { (void)pc; return false; }
size_t recomp_shadow_chain(uint64_t t, uint64_t r, uint64_t *c, size_t m) { (void)t; (void)r; (void)c; (void)m; return 0; }
bool recomp_shadow_handle_fault(void *u, uint64_t a) { (void)u; (void)a; return false; }
void recomp_shadow_stats(void *o) { memset(o, 0, 64); }
__attribute__((used, section("__DATA,__nps5_pcmap"))) static const char g_dummy_pcmap[16];

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void redact(const char *in, const char *home, const char *want) {
  char out[256];
  nps5_diag_redact_path_home(in, out, sizeof out, home);
  if (strcmp(out, want)) { printf("FAIL redact(\"%s\", home=%s) = \"%s\", want \"%s\"\n", in, home ? home : "NULL", out, want); fails++; }
}

static void test_redact(void) {
  const char *h = "/Users/alice";
  redact("/Users/alice/Downloads/x.elf", h, "~/Downloads/x.elf");
  redact("/Users/alice", h, "~");
  redact("/Users/alice/", h, "~/");
  redact("/Users/bob/x", h, "~/x");                       // any /Users/<name>/
  redact("/home/carol/x", h, "~/x");
  redact("/Users/aliceX/x", h, "~/x");                    // not HOME, still a user directory
  redact("/opt/homebrew/lib", h, "/opt/homebrew/lib");    // untouched
  redact("/opt/Users/alice/x", h, "/opt/Users/alice/x");  // the home string in the middle of a path
  redact("relative/Users/alice/x", h, "relative/Users/alice/x");
  redact("", h, "");
  redact("/Users/alice/a.json:/usr/share/b.json:/Users/bob/c.json", h, "~/a.json:/usr/share/b.json:~/c.json");
  redact("-I/Users/alice/inc", h, "-I/Users/alice/inc");  // not at a boundary: left alone
  redact("DIR=/Users/alice/x", h, "DIR=~/x");
  redact("/home/dave/x", NULL, "~/x");                    // HOME unset: the /home/<name> rule still applies
  redact("/data/x", NULL, "/data/x");
  redact("/Users/alice/x", "", "~/x");
  redact("/srv/me/x", "/srv/me/", "~/x");                 // HOME with a trailing slash
  redact("/Users/alice/x", "/", "~/x");                   // HOME "/" is not a home to strip
  redact("/Users/", h, "/Users/");
  // truncation: always NUL-terminated, never beyond cap
  char big[3000];
  memset(big, 'a', sizeof big - 1); big[0] = '/'; big[sizeof big - 1] = 0;
  char small[16];
  memset(small, 'Z', sizeof small);
  size_t n = nps5_diag_redact_path_home(big, small, sizeof small, h);
  CHECK(n == 15 && small[15] == 0 && strlen(small) == 15);
  char one[1]; one[0] = 'Z';
  nps5_diag_redact_path_home(big, one, 1, h); CHECK(one[0] == 0);
  char s2[8]; memset(s2, 'Z', sizeof s2);
  nps5_diag_redact_path_home("/Users/alice/abcdefghijkl", s2, sizeof s2, h);
  CHECK(s2[7] == 0 && !strncmp(s2, "~/abcde", 7));
  CHECK(nps5_diag_redact_path_home(NULL, s2, sizeof s2, h) == 0 && s2[0] == 0);
  CHECK(nps5_diag_redact_path_home("x", NULL, 4, h) == 0);
}

static void *altstack_thread(void *arg) {
  stack_t st;
  *(int *)arg = sigaltstack(NULL, &st) == 0 && (st.ss_flags & SS_DISABLE) ? 1 : 0; // 1: none before
  nps5_diag_thread_init();
  void *sp1 = NULL;
  *((int *)arg + 1) = sigaltstack(NULL, &st) == 0 && !(st.ss_flags & SS_DISABLE) && st.ss_size >= 32 * 1024;
  sp1 = st.ss_sp;
  nps5_diag_thread_init(); // idempotent: the same stack stays installed
  *((int *)arg + 2) = sigaltstack(NULL, &st) == 0 && st.ss_sp == sp1;
  return NULL;
}
static void test_thread_init(void) {
  int r[3] = {0, 0, 0};
  pthread_t t;
  pthread_create(&t, NULL, altstack_thread, r);
  pthread_join(t, NULL);
  CHECK(r[0] == 1);
  CHECK(r[1] == 1);
  CHECK(r[2] == 1);
}

static void __attribute__((noreturn)) crash_bad_fp(void) {
  // an aligned frame pointer inside [sp, sp+64MiB] whose page is unmapped
  uint64_t sp = (uint64_t)(uintptr_t)__builtin_frame_address(0), bad = 0, probe;
  for (uint64_t off = 24ull << 20; off < 60ull << 20; off += 1ull << 20) {
    uint64_t c = (sp + off) & ~15ull;
    if (!nps5_mem_safe_read((const void *)(uintptr_t)c, &probe, 8)) { bad = c; break; }
  }
  if (!bad) { fprintf(stderr, "no unmapped address found\n"); _exit(3); }
  __asm__ volatile("mov x29, %0\n mov x2, #0\n ldr x1, [x2]\n" : : "r"(bad) : "x1", "x2", "x29", "memory");
  _exit(4);
}

static int overflow(volatile int n) {
  volatile char pad[1024];
  pad[0] = (char)n;
  return overflow(n + 1) + pad[0];
}
static void *overflow_thread(void *a) { (void)a; nps5_diag_thread_init(); overflow(0); return NULL; }

// ---- a second thread fails while the first crash is being reported (the release play-test crash: the reporting
// thread was in exit(), a render thread threw from a destroyed mutex and aborted -> "recursive crash", no summary).
static _Atomic int g_second_go, g_second_in;
static void *second_thread(void *a) {
  (void)a;
  while (!atomic_load(&g_second_go)) usleep(1000);
  atomic_store(&g_second_in, 1);
  abort(); // what std::terminate does
}
static void legacy_two(int sig, siginfo_t *si, void *u) { // stands for the stderr report + exit(1) of recomp_runtime.c
  (void)sig; (void)si; (void)u;
  atomic_store(&g_second_go, 1);
  while (!atomic_load(&g_second_in)) usleep(1000);
  usleep(300 * 1000); // the second thread is in its abort by now
  nps5_diag_crash_finalize();
  char p[700];
  snprintf(p, sizeof p, "%s/legacy_done", getenv("NPS5_RUNS_DIR"));
  FILE *f = fopen(p, "w");
  if (f) fclose(f);
  exit(7);
}
static void legacy_rec(int sig, siginfo_t *si, void *u) { (void)sig; (void)si; (void)u; abort(); } // same thread again
static void legacy_recfin(int sig, siginfo_t *si, void *u) { (void)sig; (void)si; (void)u; nps5_diag_crash_finalize(); abort(); } // abort in teardown
static void chain_before_diag(void (*fn)(int, siginfo_t *, void *)) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sa.sa_sigaction = fn;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  nps5_diag_install_crash_handlers(); // ours in front again, fn = the handler it chains to
}
static void __attribute__((noreturn)) crash_null(void) {
  __asm__ volatile("mov x2, #0\n ldr x1, [x2]\n" : : : "x1", "x2", "memory");
  _exit(4);
}

// ---- a crash on top of a handler that already runs deep on the alternate stack (a fault being served, then a crash
// or a sent signal): the old 64 KiB stacks overflowed into neighbouring data; now there is room and a guard page.
static void deep_handler(int sig) {
  (void)sig;
  volatile char pad[100 * 1024];
  for (size_t i = 0; i < sizeof pad; i += 1024) pad[i] = (char)i;
  __asm__ volatile("mov x2, #0\n ldr x1, [x2]\n" : : : "x1", "x2", "memory");
  _exit(4 + pad[1024]);
}
static void deep_arm(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_flags = SA_ONSTACK;
  sa.sa_handler = deep_handler;
  sigaction(SIGUSR1, &sa, NULL);
}
static void *deep_thread(void *a) { (void)a; nps5_diag_thread_init(); pthread_kill(pthread_self(), SIGUSR1); return NULL; }

static int run_self(const char *mode, const char *runs) {
  char cmd[2048];
  extern char **environ;
  (void)environ;
  snprintf(cmd, sizeof cmd, "NPS5_RUNS_DIR='%s' NPS5_AUDIO_NULL=1 exec '%s' %s >/dev/null 2>&1", runs, getenv("DIAG_UNIT_SELF"), mode);
  return system(cmd);
}
static int file_has(const char *dir, const char *what) { // any crash.txt below dir contains the text
  char cmd[2048];
  snprintf(cmd, sizeof cmd, "grep -q -- '%s' '%s'/*/crash.txt 2>/dev/null", what, dir);
  return system(cmd) == 0;
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "fp")) crash_bad_fp();
  if (argc > 1 && !strcmp(argv[1], "t1")) {   // a tier-1 miss whose detail quotes paths under HOME, then abort
    const char *h = getenv("HOME");
    char det[700];
    snprintf(det, sizeof det, "open %s/x/lifter_manifest.json: no such file (work dir kept: %s/y)", h ? h : "", h ? h : "");
    nps5_diag_set_tier1_failure("bad_manifest", 0x1234, "eboot.bin", 0x34, "jump", det);
    abort();
  }
  if (argc > 1 && !strcmp(argv[1], "deepmain")) { deep_arm(); raise(SIGUSR1); _exit(6); }
  if (argc > 1 && !strcmp(argv[1], "deepthread")) { deep_arm(); pthread_t t; pthread_create(&t, NULL, deep_thread, NULL); pthread_join(t, NULL); _exit(6); }
  if (argc > 1 && !strcmp(argv[1], "two")) { pthread_t t; pthread_create(&t, NULL, second_thread, NULL); chain_before_diag(legacy_two); crash_null(); }
  if (argc > 1 && !strcmp(argv[1], "rec")) { chain_before_diag(legacy_rec); crash_null(); }
  if (argc > 1 && !strcmp(argv[1], "recfin")) { chain_before_diag(legacy_recfin); crash_null(); }
  if (argc > 1 && !strcmp(argv[1], "overflow")) { pthread_t t; pthread_create(&t, NULL, overflow_thread, NULL); pthread_join(t, NULL); _exit(5); }
  test_redact();
  test_thread_init();
  const char *self = argv[0];
  setenv("DIAG_UNIT_SELF", self, 1);
  char base[512];
  snprintf(base, sizeof base, "%s/diag_unit.%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
  char d1[600], d2[600], cmd[1400];
  snprintf(d1, sizeof d1, "%s.fp", base); snprintf(d2, sizeof d2, "%s.ov", base);
  mkdir(d1, 0755); mkdir(d2, 0755);
  int st = run_self("fp", d1);
  CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV);
  CHECK(file_has(d1, "nps5 crash report v1"));
  CHECK(file_has(d1, "^host_sp: "));
  CHECK(file_has(d1, "walk truncated: unreadable frame record"));
  st = run_self("overflow", d2);
  CHECK(WIFSIGNALED(st));
  CHECK(file_has(d2, "^signal: "));
  char d3[600]; snprintf(d3, sizeof d3, "%s.t1", base); mkdir(d3, 0755);
  st = run_self("t1", d3);
  CHECK(WIFSIGNALED(st));
  CHECK(file_has(d3, "^tier1_detail: open ~/x/lifter_manifest.json: no such file (work dir kept: ~/y)"));
  CHECK(file_has(d3, "^note: tier-1 miss at guest 0x1234 (eboot.bin+0x34): bad_manifest: open ~/x/"));
  if (getenv("HOME") && strlen(getenv("HOME")) > 1) CHECK(!file_has(d3, getenv("HOME")));
  // second thread aborts during the report: the first report still finishes (legacy handler's exit code, summary
  // written, no "recursive crash" exit), repeated because it is a race
  char d4[600], d5[600];
  for (int i = 0; i < 20; i++) {
    snprintf(d4, sizeof d4, "%s.two%d", base, i); mkdir(d4, 0755);
    st = run_self("two", d4);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 7);
    CHECK(file_has(d4, "nps5 crash report v1"));
    CHECK(file_has(d4, "^fault_addr: 0x0"));
    snprintf(cmd, sizeof cmd, "test -f '%s/legacy_done' && grep -q '^crashed: 1' '%s'/*/summary.txt && grep -q '\"result\": \"crash\"' '%s'/*/manifest.json", d4, d4, d4);
    CHECK(system(cmd) == 0);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", d4); system(cmd);
  }
  const char *deep[2] = {"deepmain", "deepthread"};
  for (int i = 0; i < 2; i++) {
    char d6[600]; snprintf(d6, sizeof d6, "%s.%s", base, deep[i]); mkdir(d6, 0755);
    st = run_self(deep[i], d6);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV);
    CHECK(file_has(d6, "^fault_addr: 0x0"));
    CHECK(file_has(d6, "^backtrace"));
    snprintf(cmd, sizeof cmd, "grep -q '^crashed: 1' '%s'/*/summary.txt", d6);
    CHECK(system(cmd) == 0);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", d6); system(cmd);
  }
  // a fault on the reporting thread itself is still cut short (exit 128 + SIGABRT), with the report on disk
  snprintf(d5, sizeof d5, "%s.rec", base); mkdir(d5, 0755);
  st = run_self("rec", d5);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 128 + SIGABRT);
  CHECK(file_has(d5, "nps5 crash report v1"));
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", d5); system(cmd);
  // the same after the bundle was finalised (abort from library teardown inside exit()): a plain exit 1, bundle complete
  snprintf(d5, sizeof d5, "%s.recfin", base); mkdir(d5, 0755);
  st = run_self("recfin", d5);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 1);
  snprintf(cmd, sizeof cmd, "grep -q '^crashed: 1' '%s'/*/summary.txt", d5);
  CHECK(system(cmd) == 0);
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", d5); system(cmd);
  snprintf(cmd, sizeof cmd, "rm -rf '%s' '%s' '%s'", d1, d2, d3); system(cmd);
  printf(fails ? "diag_unit: %d FAILURES\n" : "diag_unit: all passed\n", fails);
  return fails ? 1 : 0;
}
