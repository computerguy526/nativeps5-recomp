#include "recomp_runtime.h"
#include "ps4_direct_mem.h"
#include "ps4_sysmodule.h"
#include "ps4_vfs.h"
#include "ps4_log.h"
#include "nps5_diag.h"
#include "nps5_mem.h"

#include <errno.h>
#include <stdatomic.h>
#include <signal.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <dlfcn.h>
#include <execinfo.h>
#include <libgen.h>
#include <mach-o/dyld.h>
#endif
#if defined(__arm64__) || defined(__aarch64__)
#include <arm_neon.h>
#endif

recomp_fn_t **g_dispatch_l1[DISPATCH_LEVEL_SIZE] = {0};
_Thread_local GuestContext *g_current_ctx = NULL;
pthread_mutex_t g_heap_mutex = PTHREAD_MUTEX_INITIALIZER;

// NPS5_DUMP_MAP=1: the guest arena map (reservation, images, fixed ranges, guest mappings) on
// stderr when the process ends through the runtime (SPEC §8.2).
static void dump_map_if_asked(void) {
  const char *v = getenv("NPS5_DUMP_MAP");
  if (v && v[0] == '1') nps5_mem_dump_map(STDERR_FILENO);
}


GuestContext *recomp_get_current_ctx(void) {
  return g_current_ctx;
}

void recomp_set_current_ctx(GuestContext *ctx) {
  g_current_ctx = ctx;
}

static _Atomic int g_crashing = 0;
__attribute__((weak)) void nps5_diag_crash_finalize(void); // nps5_diag.c

int ps4_host_crashing(void) { return g_crashing; }

// The host thread the crash handler runs on, and what brought the signal. A fault is delivered to
// the thread that faulted. A signal sent with kill() goes to any thread that does not block it,
// usually the main thread in its run loop, and macOS fills its siginfo like a fault at address 0
// (SIGSEGV: si_code SEGV_ACCERR, si_addr 0, si_pid 0). The exception state tells them apart: a
// fault's syndrome (ESR) has the class of the fault, and for an abort its fault address (FAR) is
// si_addr; a sent signal finds the syndrome of the thread's last exception, a system call when it
// was blocked in one.
static void crash_report_source(int sig, const siginfo_t *si, const void *ucontext) {
  char name[64] = "";
  pthread_getname_np(pthread_self(), name, sizeof name);
  uint64_t tid = 0;
  pthread_threadid_np(NULL, &tid);
  fprintf(stderr, "Signal %d on host thread %llu '%s'%s\n", sig, (unsigned long long)tid, name,
          pthread_main_np() ? " (the main thread)" : "");
#if defined(__APPLE__) && defined(__arm64__)
  const arm_exception_state64_t *es = &((const ucontext_t *)ucontext)->uc_mcontext->__es;
  const uint32_t esr = es->__esr;
  const uint32_t ec = esr >> 26;
  const uint64_t addr = (uint64_t)(uintptr_t)si->si_addr;
  int fault = 0;
  const char *kind = "";
  switch (ec) {
  case 0x20: case 0x21: kind = "instruction abort"; fault = (sig == SIGSEGV || sig == SIGBUS) && es->__far == addr; break;
  case 0x24: case 0x25: kind = "data abort"; fault = (sig == SIGSEGV || sig == SIGBUS) && es->__far == addr; break;
  case 0x22: kind = "PC alignment"; fault = sig == SIGBUS; break;
  case 0x26: kind = "SP alignment"; fault = sig == SIGBUS; break;
  case 0x00: kind = "undefined instruction"; fault = sig == SIGILL; break;
  case 0x3c: kind = "brk"; fault = sig == SIGTRAP || sig == SIGILL; break;
  case 0x15: kind = "system call"; break;
  default: kind = "other"; break;
  }
  if (fault) {
    fprintf(stderr, "Cause: a fault of this thread (ESR 0x%x: %s, FAR 0x%llx)\n", esr, kind,
            (unsigned long long)es->__far);
  } else {
    fprintf(stderr,
            "Cause: sent to the process (kill or pthread_kill), not a fault of this thread: its last exception "
            "is a %s (ESR 0x%x, FAR 0x%llx)\n",
            kind, esr, (unsigned long long)es->__far);
  }
#else
  (void)ucontext;
  fprintf(stderr, "Cause: si_code %d, si_pid %d\n", si->si_code, (int)si->si_pid);
#endif
}

// The host registers of the interrupted thread.
static void crash_report_host_registers(const void *ucontext) {
#if defined(__APPLE__) && defined(__arm64__)
  const arm_thread_state64_t *state = &((const ucontext_t *)ucontext)->uc_mcontext->__ss;
  Dl_info di;
  const int named = dladdr((void *)state->__pc, &di) && di.dli_sname;
  fprintf(stderr, "Host PC: 0x%llx (ARM64)%s%s", (unsigned long long)state->__pc, named ? " " : "",
          named ? di.dli_sname : "");
  if (named) fprintf(stderr, "+0x%llx", (unsigned long long)(state->__pc - (uint64_t)(uintptr_t)di.dli_saddr));
  fprintf(stderr, "\n");
  for (int i = 0; i < 29; i++) {
    fprintf(stderr, "x%d=0x%llx ", i, (unsigned long long)state->__x[i]);
    if ((i + 1) % 4 == 0) fprintf(stderr, "\n");
  }
  fprintf(stderr, "\nfp=0x%llx lr=0x%llx sp=0x%llx\n", (unsigned long long)state->__fp,
          (unsigned long long)state->__lr, (unsigned long long)state->__sp);
#else
  (void)ucontext;
#endif
}

// One guest word for the crash report, read without faulting: rsp may be in a stack guard page
// (stack overflow), and a fault inside the handler (signal blocked) would spin the thread forever.
static bool crash_read_u64(uint64_t g, uint64_t *v) { return nps5_guest_safe_read(g, v, sizeof *v); }

// NPS5_CRASH_REGDUMP=1 (saros2, guest heap corruption hunt): for every live guest register that
// holds a guest address, print its VM extent and the first 16 qwords there; for each of those
// qwords that is itself a guest address, the 4 qwords it points at. Shows which table/object a
// garbage value was loaded from (e.g. an allocator page table entry of all ones).
// Kyty guest-write journal (KYTY_GUEST_WRITE_JOURNAL=1 in the Kyty lib): which Kyty writes into
// guest memory overlap [a, a+len). Resolved at crash time by name; absent = no output.
typedef int (*KytyJournalLookup)(uint64_t guest_addr, uint64_t len, char *out, size_t out_len);
static void crash_journal(uint64_t a, uint64_t len, const char *indent) {
  static KytyJournalLookup fn;
  static int tried;
  if (!tried) {
    tried = 1;
    // kyty_gpu.dylib is opened RTLD_LOCAL (nps5_kyty.c): ask its handle first.
    extern void *nps5_kyty_lib_handle(void) __attribute__((weak));
    void *h = nps5_kyty_lib_handle ? nps5_kyty_lib_handle() : NULL;
    fn = (KytyJournalLookup)(h ? dlsym(h, "kyty_guest_write_journal_lookup") : NULL);
    if (!fn) fn = (KytyJournalLookup)dlsym(RTLD_DEFAULT, "kyty_guest_write_journal_lookup");
    Dl_info di;
    if (fn && dladdr((void *)fn, &di))
      fprintf(stderr, "Kyty guest-write journal: %s loaded at %p (atos -o <dylib> -l %p <caller>)\n", di.dli_fname,
              di.dli_fbase, di.dli_fbase);
  }
  if (!fn) return;
  char buf[2048];
  buf[0] = 0;
  const int n = fn(a, len, buf, sizeof buf);
  if (n > 0) fprintf(stderr, "%s  Kyty wrote here (%d journal entries, newest first):\n%s%s", indent, n, buf,
                     (buf[0] && buf[strlen(buf) - 1] != '\n') ? "\n" : "");
}

static void crash_dump_one(const char *tag, uint64_t v, int depth) {
  if (!g_current_ctx || v < 0x10000 || v >= g_current_ctx->mem_size) return;
  GuestVMExtent e;
  const bool have = recomp_vm_query(g_current_ctx, v, &e);
  const int n = depth ? 4 : 16;
  fprintf(stderr, "%s%s=0x%llx", depth ? "    " : "  ", tag, (unsigned long long)v);
  if (have) fprintf(stderr, " [%s kind=%d 0x%llx+0x%llx%s]", e.name, e.kind, (unsigned long long)e.addr,
                    (unsigned long long)e.size, e.is_free ? " FREE" : "");
  else fprintf(stderr, " [no extent]");
  uint64_t q[16];
  int got = 0;
  for (; got < n; got++) if (!crash_read_u64(v + (uint64_t)got * 8, &q[got])) break;
  fprintf(stderr, ":");
  for (int i = 0; i < got; i++) fprintf(stderr, " %016llx", (unsigned long long)q[i]);
  fprintf(stderr, "\n");
  if (got) crash_journal(v, (uint64_t)got * 8, depth ? "    " : "  ");
  if (!depth)
    for (int i = 0; i < got; i++) {
      char t[32];
      snprintf(t, sizeof t, "+0x%x", i * 8);
      crash_dump_one(t, q[i], 1);
    }
}
#if defined(__APPLE__) && defined(__arm64__)
static void crash_dump_regs(const uint64_t *x) {
  static const char *names[] = {"RAX", "RBX", "RCX", "RDX", "RSI", "RDI", "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15", "RBP"};
  static const int idx[] = {19, 20, 21, 22, 23, 24, 8, 9, 10, 11, 12, 13, 14, 15, 25};
  fprintf(stderr, "Guest register dump (NPS5_CRASH_REGDUMP):\n");
  for (int r = 0; r < 15; r++) crash_dump_one(names[r], x[idx[r]], 0);
}
#endif

static void crash_handler(int sig, siginfo_t *si, void *ucontext) {
  // A shadow stack push into its guard page (flat call model) is handled and retried.
  if ((sig == SIGSEGV || sig == SIGBUS) && si && recomp_shadow_handle_fault(ucontext, (uint64_t)(uintptr_t)si->si_addr)) return;
  g_crashing = 1; // before exit(1): the exit handlers it runs skip their waits
  ps4_log_drain_sync(1.0); // pending output first, so the FATAL report below is not lost
  if (g_current_ctx) {
    fprintf(stderr,
            "\nFATAL: Signal %d at host address %p (mem_base=%p)\n"
            "RIP=0x%llx RSP=0x%llx RBP=0x%llx RAX=0x%llx RBX=0x%llx\n"
            "RCX=0x%llx RDX=0x%llx RSI=0x%llx RDI=0x%llx\n"
            "R8=0x%llx R9=0x%llx R10=0x%llx R11=0x%llx\n"
            "R12=0x%llx R13=0x%llx R14=0x%llx R15=0x%llx\n",
            sig, si->si_addr, g_current_ctx->mem_base,
            (unsigned long long)g_current_ctx->rip,
            (unsigned long long)g_current_ctx->rsp,
            (unsigned long long)g_current_ctx->rbp,
            (unsigned long long)g_current_ctx->rax,
            (unsigned long long)g_current_ctx->rbx,
            (unsigned long long)g_current_ctx->rcx,
            (unsigned long long)g_current_ctx->rdx,
            (unsigned long long)g_current_ctx->rsi,
            (unsigned long long)g_current_ctx->rdi,
            (unsigned long long)g_current_ctx->r8,
            (unsigned long long)g_current_ctx->r9,
            (unsigned long long)g_current_ctx->r10,
            (unsigned long long)g_current_ctx->r11,
            (unsigned long long)g_current_ctx->r12,
            (unsigned long long)g_current_ctx->r13,
            (unsigned long long)g_current_ctx->r14,
            (unsigned long long)g_current_ctx->r15);
    if (RECOMP_MEM_OK(g_current_ctx) && g_current_ctx->rsp < g_current_ctx->mem_size) {
      fprintf(stderr, "Guest Stack at RSP=0x%llx (96 words):\n", (unsigned long long)g_current_ctx->rsp);
      for (int i = 0; i < 96; i++) {
        uint64_t w;
        if ((uint64_t)(g_current_ctx->rsp + i * 8 + 8) > g_current_ctx->mem_size) break;
        if (!crash_read_u64(g_current_ctx->rsp + (uint64_t)i * 8, &w)) {
          fprintf(stderr, "  [RSP+0x%03x] = (unreadable: guard page or unmapped)\n", i * 8);
          break;
        }
        fprintf(stderr, "  [RSP+0x%03x] = 0x%016llx\n", i * 8, (unsigned long long)w);
      }
    }
    if (RECOMP_MEM_OK(g_current_ctx)) {
      // Guest backtrace: likely return addresses found on the stack, plus the
      // frame-pointer chain.
      fprintf(stderr, "Guest thread_id=%llu; code addresses on stack:", (unsigned long long)g_current_ctx->thread_id);
      int shown = 0;
      for (int i = 0; i < 512 && shown < 24; i++) {
        if ((uint64_t)(g_current_ctx->rsp + i * 8 + 8) > g_current_ctx->mem_size) break;
        uint64_t v;
        if (!crash_read_u64(g_current_ctx->rsp + (uint64_t)i * 8, &v)) break;
        if (recomp_module_text_contains(v)) {
          // Return addresses point into the middle of a function.
          fprintf(stderr, " %llx", (unsigned long long)v);
          shown++;
        }
      }
      fprintf(stderr, "\nGuest RBP chain:");
      uint64_t bp = g_current_ctx->rbp;
      for (int k = 0; k < 16 && bp && bp + 16 <= g_current_ctx->mem_size; k++) {
        uint64_t ret, next;
        if (!crash_read_u64(bp + 8, &ret) || !crash_read_u64(bp, &next)) break;
        fprintf(stderr, " %llx", (unsigned long long)ret);
        bp = next;
      }
      fprintf(stderr, "\n");
    }
#if defined(__APPLE__) && defined(__arm64__)
    arm_thread_state64_t *state = (arm_thread_state64_t *)&(((ucontext_t *)ucontext)->uc_mcontext->__ss);
    if (recomp_pc_in_guest_code(state->__pc)) {
      // The registers above are what the last shim call stored; in guest code the pinned host
      // registers are the live guest registers (contract at recomp_enter_guest).
      Dl_info di;
      const int named = dladdr((void *)state->__pc, &di) && di.dli_sname;
      const uint64_t *x = state->__x;
      fprintf(stderr,
              "In recompiled guest code (%s+0x%llx); live guest registers:\n"
              "RSP=0x%llx RBP=0x%llx RAX=0x%llx RBX=0x%llx RCX=0x%llx RDX=0x%llx RSI=0x%llx RDI=0x%llx\n"
              "R8=0x%llx R9=0x%llx R10=0x%llx R11=0x%llx R12=0x%llx R13=0x%llx R14=0x%llx R15=0x%llx FS=0x%llx\n",
              named ? di.dli_sname : "?", named ? (unsigned long long)(state->__pc - (uint64_t)(uintptr_t)di.dli_saddr) : 0ULL,
              (unsigned long long)x[26], (unsigned long long)x[25], (unsigned long long)x[19], (unsigned long long)x[20],
              (unsigned long long)x[21], (unsigned long long)x[22], (unsigned long long)x[23], (unsigned long long)x[24],
              (unsigned long long)x[8], (unsigned long long)x[9], (unsigned long long)x[10], (unsigned long long)x[11],
              (unsigned long long)x[12], (unsigned long long)x[13], (unsigned long long)x[14], (unsigned long long)x[15],
              (unsigned long long)x[27]);
      if (x[28] == (uint64_t)(uintptr_t)g_current_ctx->mem_base && x[26] < g_current_ctx->mem_size) {
        fprintf(stderr, "Guest code addresses on the live stack:");
        for (int i = 0, shown = 0; i < 512 && shown < 24 && x[26] + (uint64_t)i * 8 + 8 <= g_current_ctx->mem_size; i++) {
          uint64_t v;
          if (!crash_read_u64(x[26] + (uint64_t)i * 8, &v)) break; // a stack overflow leaves rsp in the guard page
          if (recomp_module_text_contains(v)) {
            fprintf(stderr, " %llx", (unsigned long long)v);
            shown++;
          }
        }
        fprintf(stderr, "\n");
      }
      if (getenv("NPS5_CRASH_REGDUMP")) crash_dump_regs(x);
    }
#endif
    crash_report_source(sig, si, ucontext);
    crash_report_host_registers(ucontext);
    void *callstack[64];
    int frames = backtrace(callstack, 64);
    fprintf(stderr, "Call stack (%d frames):\n", frames);
    backtrace_symbols_fd(callstack, frames, 2);
  } else {
    fprintf(stderr,
            "\nFATAL: Signal %d at host address %p (no guest context)\n", sig,
            si->si_addr);
    crash_report_source(sig, si, ucontext);
    crash_report_host_registers(ucontext);
    void *callstack[64];
    int frames = backtrace(callstack, 64);
    fprintf(stderr, "Call stack (%d frames):\n", frames);
    backtrace_symbols_fd(callstack, frames, 2);
  }
  fflush(stderr);
  if (nps5_diag_crash_finalize) nps5_diag_crash_finalize(); // run bundle complete before exit() runs library teardown
  exit(1);
}

#define UNRESOLVED_BUCKETS 1024
// Overridden by modules/libc/nps5_libc_core.c (strong definition).
__attribute__((weak)) bool nps5_libc_bind_data_import(GuestContext *ctx, uint64_t slot_addr, const char *name) {
  (void)ctx; (void)slot_addr; (void)name;
  return false;
}
static RecompUnresolvedSym *g_unresolved_table[UNRESOLVED_BUCKETS];
static pthread_mutex_t g_unresolved_mutex = PTHREAD_MUTEX_INITIALIZER;

void recomp_register_unresolved(uint64_t addr, const char *nid,
                                const char *sym_name, const char *lib_name) {
  if (!addr)
    return;
  uint32_t bucket = (uint32_t)((addr ^ (addr >> 12)) % UNRESOLVED_BUCKETS);
  pthread_mutex_lock(&g_unresolved_mutex);
  RecompUnresolvedSym *entry =
      (RecompUnresolvedSym *)malloc(sizeof(RecompUnresolvedSym));
  if (entry) {
    entry->addr = addr;
    entry->nid = nid ? strdup(nid) : NULL;
    entry->sym_name = sym_name ? strdup(sym_name) : NULL;
    entry->lib_name = lib_name ? strdup(lib_name) : NULL;
    entry->next = g_unresolved_table[bucket];
    g_unresolved_table[bucket] = entry;
  }
  pthread_mutex_unlock(&g_unresolved_mutex);
}

const RecompUnresolvedSym *recomp_lookup_unresolved(uint64_t addr) {
  uint32_t bucket = (uint32_t)((addr ^ (addr >> 12)) % UNRESOLVED_BUCKETS);
  pthread_mutex_lock(&g_unresolved_mutex);
  RecompUnresolvedSym *curr = g_unresolved_table[bucket];
  while (curr) {
    if (curr->addr == addr) {
      pthread_mutex_unlock(&g_unresolved_mutex);
      return curr;
    }
    curr = curr->next;
  }
  pthread_mutex_unlock(&g_unresolved_mutex);
  return NULL;
}

__attribute__((weak)) bool ps4_nptoolkit2_dispatch(GuestContext *ctx, const char *nid) {
  (void)ctx;
  (void)nid;
  return false;
}

void shim_unresolved_stub(GuestContext *ctx) {
  uint64_t caller_rip = 0;
  if (ctx && RECOMP_MEM_OK(ctx) && ctx->rsp + 8 <= ctx->mem_size) {
    caller_rip = *(uint64_t *)(ctx->mem_base + ctx->rsp);
  }
  const RecompUnresolvedSym *sym = recomp_lookup_unresolved(ctx ? ctx->rip : 0);
  nps5_diag_unresolved_hit(sym ? sym->sym_name : NULL, sym ? sym->nid : NULL, sym ? sym->lib_name : NULL, ctx ? ctx->rip : 0, caller_rip);
  nps5_diag_set_note("unresolved import %s (NID %s, lib %s) called from guest 0x%llx",
                     sym && sym->sym_name && sym->sym_name[0] ? sym->sym_name : (sym && sym->nid ? sym->nid : "?"), sym && sym->nid ? sym->nid : "?",
                     sym && sym->lib_name ? sym->lib_name : "?", (unsigned long long)caller_rip);
  if (sym && sym->lib_name && strncmp(sym->lib_name, "excluded:", 9) == 0) {
    // Import of a module left out of the translation (-exclude-module): the hit was logged once
    // and counted above; return 0 (NULL / success) to the caller.
    if (ctx) {
      ctx->rax = 0;
      ctx->rsp += 8;
    }
    return;
  }
  if (sym && sym->lib_name) {
    if (strcmp(sym->lib_name, "libSceNpToolkit2") == 0) {
      extern bool ps4_nptoolkit2_dispatch(GuestContext *ctx, const char *nid);
      if (ps4_nptoolkit2_dispatch(ctx, sym->nid)) {
        return;
      }
    }
    if (strcmp(sym->lib_name, "libSceVoiceQoS") == 0 ||
        strcmp(sym->lib_name, "libSceCoredump") == 0 ||
        strcmp(sym->lib_name, "libSceAudiodec") == 0 ||
        strcmp(sym->lib_name, "libSceNgs2") == 0) {
      fprintf(stderr,
              "[ps4-recomp] Stub: %s (NID: %s, Lib: %s) called from RIP=0x%llx -> 0\n",
              sym->sym_name ? sym->sym_name : "unknown",
              sym->nid ? sym->nid : "unknown",
              sym->lib_name,
              (unsigned long long)caller_rip);
      if (ctx) {
        ctx->rax = 0;
        ctx->rsp += 8;
      }
      return;
    }
  }
  if (sym && sym->sym_name && sym->sym_name[0]) {
    fprintf(stderr,
            "[ps4-recomp] WARN: Called unimplemented function '%s' (NID: %s, "
            "Lib: %s) at RIP=0x%llx (caller RIP=0x%llx, RSP=0x%llx)\n",
            sym->sym_name, sym->nid ? sym->nid : "unknown",
            sym->lib_name ? sym->lib_name : "unknown",
            (unsigned long long)(ctx ? ctx->rip : 0),
            (unsigned long long)caller_rip,
            (unsigned long long)(ctx ? ctx->rsp : 0));
  } else if (sym && sym->nid && sym->nid[0]) {
    fprintf(stderr,
            "[ps4-recomp] WARN: Called unimplemented function NID '%s' (Lib: "
            "%s) at RIP=0x%llx (caller RIP=0x%llx, RSP=0x%llx)\n",
            sym->nid, sym->lib_name ? sym->lib_name : "unknown",
            (unsigned long long)(ctx ? ctx->rip : 0),
            (unsigned long long)caller_rip,
            (unsigned long long)(ctx ? ctx->rsp : 0));
  } else {
    fprintf(stderr,
            "[ps4-recomp] WARN: Called unresolved function at RIP=0x%llx "
            "(caller RIP=0x%llx, RSP=0x%llx)\n",
            (unsigned long long)(ctx ? ctx->rip : 0),
            (unsigned long long)caller_rip,
            (unsigned long long)(ctx ? ctx->rsp : 0));
  }
  // ue2boot: NPS5_UNRESOLVED_RET0=1 (bring-up of a new title, opt-in): return 0 to the caller instead of
  // aborting, so a run gets past imports nobody has implemented yet. Each name is reported above every time
  // it is called until NPS5_UNRESOLVED_RET0_QUIET=1; the run summary still counts unresolved hits.
  {
    static int ret0 = -1;
    if (ret0 < 0) { const char *e = getenv("NPS5_UNRESOLVED_RET0"); ret0 = (e && e[0] == '1') ? 1 : 0; }
    if (ret0 == 1) {
      fflush(stderr);
      if (ctx) { ctx->rax = 0; ctx->rsp += 8; }
      return;
    }
  }
  recomp_dump_guest_context(ctx);
  fflush(stderr);
  abort();
  if (ctx) {
    ctx->rax = 0;
    ctx->rsp += 8;
  }
}

void recomp_unimplemented_shim(GuestContext *ctx, const char *shim_name) {
  uint64_t caller_rip = 0;
  if (ctx && RECOMP_MEM_OK(ctx) && ctx->rsp + 8 <= ctx->mem_size) {
    caller_rip = *(uint64_t *)(ctx->mem_base + ctx->rsp);
  }
  fprintf(stderr,
          "[ps4-recomp] WARN: Called unimplemented host shim '%s' at "
          "RIP=0x%llx (caller RIP=0x%llx, RSP=0x%llx)\n",
          shim_name ? shim_name : "unknown",
          (unsigned long long)(ctx ? ctx->rip : 0),
          (unsigned long long)caller_rip,
          (unsigned long long)(ctx ? ctx->rsp : 0));
  fflush(stderr);
  if (ctx) {
    ctx->rax = 0;
    ctx->rsp += 8;
  }
}

extern void recomp_ic_flush_all(void) __attribute__((weak));

// ---- lazy dispatch table (NPS5_LAZY_DISPATCH=1) -------------------------------------------------
// The 3-level table spends 8 bytes per guest code BYTE (a 64 Ki-entry level-3 table per 64 KiB of
// code, 16 KiB host pages that hold a function pointer every few hundred bytes are all dirtied by
// the boot-time registration): ~700 MB of dirty host memory for a 260 MB eboot, most of it for
// functions that are only ever reached by direct calls. With the flag, the registrations made
// before the first slow-path lookup go into a compact sorted array (24 B per function) and a
// function is published into the table the first time something misses on it (recomp_lazy_resolve,
// called by every table consumer that gets NULL: the continuation slow paths, tier 1, the miss
// entry). The registration order semantics are kept (the last registration of an address wins) and
// registrations after the first lookup go straight to the table as before.
typedef struct { uint64_t guest; recomp_fn_t fn; uint64_t seq; } LazyEnt;
static LazyEnt *g_lazy;
static size_t g_lazy_n, g_lazy_cap;
static int g_lazy_mode = -1; // -1 unread, 0 off, 1 collecting, 2 sealed
static pthread_mutex_t g_lazy_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_lazy_log; // NPS5_LAZY_DISPATCH=2: log every published address (measurement)

static int lazy_cmp(const void *a, const void *b) {
  const LazyEnt *x = (const LazyEnt *)a, *y = (const LazyEnt *)b;
  if (x->guest != y->guest) return x->guest < y->guest ? -1 : 1;
  return x->seq < y->seq ? -1 : x->seq > y->seq;
}

// Publishes fn into the table slot of a if it is empty (compare-and-swap at every level, so it
// races safely with other publishers). Returns 1 when stored, 0 when the slot was taken, -1 on OOM.
int recomp_dispatch_publish(uint64_t a, recomp_fn_t fn) {
  if (a == 0 || a >= (1ULL << 48)) return 0;
  recomp_fn_t ***s1 = &g_dispatch_l1[(a >> DISPATCH_L1_SHIFT) & DISPATCH_LEVEL_MASK];
  recomp_fn_t **l2 = __atomic_load_n(s1, __ATOMIC_ACQUIRE);
  if (!l2) {
    recomp_fn_t **n = (recomp_fn_t **)calloc(DISPATCH_LEVEL_SIZE, sizeof(recomp_fn_t *));
    if (!n) return -1;
    recomp_fn_t **exp = NULL;
    if (__atomic_compare_exchange_n(s1, &exp, n, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) l2 = n;
    else { free(n); l2 = exp; }
  }
  recomp_fn_t ***s2 = (recomp_fn_t ***)&l2[(a >> DISPATCH_L2_SHIFT) & DISPATCH_LEVEL_MASK];
  recomp_fn_t *l3 = __atomic_load_n((recomp_fn_t **)s2, __ATOMIC_ACQUIRE);
  if (!l3) {
    recomp_fn_t *n = (recomp_fn_t *)calloc(DISPATCH_LEVEL_SIZE, sizeof(recomp_fn_t));
    if (!n) return -1;
    recomp_fn_t *exp = NULL;
    if (__atomic_compare_exchange_n((recomp_fn_t **)s2, &exp, n, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) l3 = n;
    else { free(n); l3 = exp; }
  }
  recomp_fn_t exp = NULL;
  return __atomic_compare_exchange_n(&l3[a & DISPATCH_LEVEL_MASK], &exp, fn, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ? 1 : 0;
}

static void lazy_seal(void) {
  pthread_mutex_lock(&g_lazy_mu);
  if (g_lazy_mode == 1) {
    qsort(g_lazy, g_lazy_n, sizeof(LazyEnt), lazy_cmp);
    size_t k = 0; // keep the last registration of each address
    for (size_t i = 0; i < g_lazy_n; i++) {
      if (i + 1 < g_lazy_n && g_lazy[i + 1].guest == g_lazy[i].guest) continue;
      g_lazy[k++] = g_lazy[i];
    }
    g_lazy_n = k;
    LazyEnt *t = (LazyEnt *)realloc(g_lazy, (k ? k : 1) * sizeof(LazyEnt));
    if (t) g_lazy = t;
    g_lazy_cap = k;
    __atomic_store_n(&g_lazy_mode, 2, __ATOMIC_RELEASE);
  }
  pthread_mutex_unlock(&g_lazy_mu);
}

// The registered function of guest address a that is not in the table yet: publishes it and returns
// it (NULL when the address was never registered, or the mode is off). Cheap when off.
recomp_fn_t recomp_lazy_resolve(uint64_t a) {
  int m = __atomic_load_n(&g_lazy_mode, __ATOMIC_ACQUIRE);
  if (m < 1) return NULL;
  if (m == 1) { lazy_seal(); }
  size_t lo = 0, hi = g_lazy_n;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (g_lazy[mid].guest < a) lo = mid + 1; else hi = mid;
  }
  if (lo >= g_lazy_n || g_lazy[lo].guest != a) return NULL;
  recomp_fn_t fn = g_lazy[lo].fn;
  if (recomp_dispatch_publish(a, fn) == 1 && g_lazy_log) fprintf(stderr, "[lazy-dispatch] 0x%llx\n", (unsigned long long)a);
  return fn;
}

static void lazy_free(void) {
  pthread_mutex_lock(&g_lazy_mu);
  free(g_lazy);
  g_lazy = NULL;
  g_lazy_n = g_lazy_cap = 0;
  pthread_mutex_unlock(&g_lazy_mu);
}

void recomp_register_fn(uint64_t guest_addr, recomp_fn_t fn) {
  if (guest_addr >= (1ULL << 48)) {
    return;
  }
  int m = __atomic_load_n(&g_lazy_mode, __ATOMIC_ACQUIRE);
  if (m == -1) {
    const char *e = getenv("NPS5_LAZY_DISPATCH");
    int want = e && e[0] && e[0] != '0';
    g_lazy_log = e && e[0] == '2';
    pthread_mutex_lock(&g_lazy_mu);
    if (g_lazy_mode == -1) __atomic_store_n(&g_lazy_mode, want ? 1 : 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_lazy_mu);
    m = __atomic_load_n(&g_lazy_mode, __ATOMIC_ACQUIRE);
  }
  if (m == 1) {
    pthread_mutex_lock(&g_lazy_mu);
    if (g_lazy_mode == 1) {
      if (g_lazy_n == g_lazy_cap) {
        size_t nc = g_lazy_cap ? g_lazy_cap * 2 : 65536;
        LazyEnt *t = (LazyEnt *)realloc(g_lazy, nc * sizeof(LazyEnt));
        if (!t) { perror("realloc lazy dispatch"); abort(); }
        g_lazy = t;
        g_lazy_cap = nc;
      }
      g_lazy[g_lazy_n] = (LazyEnt){guest_addr, fn, g_lazy_n};
      g_lazy_n++;
      pthread_mutex_unlock(&g_lazy_mu);
      return;
    }
    pthread_mutex_unlock(&g_lazy_mu);
  }
  uint64_t l1_idx = (guest_addr >> DISPATCH_L1_SHIFT) & DISPATCH_LEVEL_MASK;
  if (!g_dispatch_l1[l1_idx]) {
    g_dispatch_l1[l1_idx] =
        (recomp_fn_t **)calloc(DISPATCH_LEVEL_SIZE, sizeof(recomp_fn_t *));
    if (!g_dispatch_l1[l1_idx]) {
      perror("calloc dispatch L2 table");
      abort();
    }
  }
  uint64_t l2_idx = (guest_addr >> DISPATCH_L2_SHIFT) & DISPATCH_LEVEL_MASK;
  if (!g_dispatch_l1[l1_idx][l2_idx]) {
    g_dispatch_l1[l1_idx][l2_idx] =
        (recomp_fn_t *)calloc(DISPATCH_LEVEL_SIZE, sizeof(recomp_fn_t));
    if (!g_dispatch_l1[l1_idx][l2_idx]) {
      perror("calloc dispatch L3 table");
      abort();
    }
  }
  recomp_fn_t *slot = &g_dispatch_l1[l1_idx][l2_idx][guest_addr & DISPATCH_LEVEL_MASK];
  recomp_fn_t old = *slot;
  *slot = fn;
  // A replaced entry (function replacement, HLE override) may be cached by call-site inline
  // caches (docs/m3/callic.md): drop them. First registrations need nothing: caches only
  // hold targets the dispatch table resolved.
  if (old && old != fn && recomp_ic_flush_all) recomp_ic_flush_all();
}

static size_t parse_mem_size_str(const char *str) {
  if (!str || !*str)
    return 0;
  char *end = NULL;
  unsigned long long val = strtoull(str, &end, 0);
  if (end && *end) {
    if (*end == 'g' || *end == 'G')
      val *= 1024ULL * 1024 * 1024;
    else if (*end == 'm' || *end == 'M')
      val *= 1024ULL * 1024;
    else if (*end == 'k' || *end == 'K')
      val *= 1024ULL;
  }
  return (size_t)val;
}

void recomp_install_crash_handler(void) {
  struct sigaction sa = {0};
  sa.sa_flags = SA_SIGINFO;
  sa.sa_sigaction = crash_handler;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGTRAP, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  nps5_diag_install_crash_handlers(); // writes crash.txt, then chains to the handler above
}

// Inserts a new extent after `after` (NULL: at the head). Only used while building the
// initial list, before any other thread exists.
static GuestVMExtent *vm_init_append(GuestContext *ctx, GuestVMExtent *after, uint64_t addr, uint64_t size,
                                     bool is_free, const char *name) {
  if (size == 0) return after;
  GuestVMExtent *e = (GuestVMExtent *)calloc(1, sizeof(GuestVMExtent));
  if (!e) {
    perror("calloc GuestVMExtent");
    abort();
  }
  e->addr = addr;
  e->size = size;
  e->is_free = is_free;
  e->prot = is_free ? 0 : (PROT_READ | PROT_WRITE);
  if (name) strncpy(e->name, name, sizeof(e->name) - 1);
  e->prev = after;
  if (after) after->next = e;
  else ctx->vm_extents = e;
  return e;
}

// Guest layout (docs/m1/memory.md), relative to the layout origin (0 in base mode,
// 0x70_0000_0000 in identity mode):
//
//   [0, 4 MiB)              null guard, never mapped
//   0x40_0000 ...           executable, then the bundled modules (placed at translate time)
//   image_end ...           system block: main thread static TLS (64 KiB), TCB (64 KiB),
//                           process arguments (64 KiB)
//   ... 0x2_0000_0000       system zone: thread stacks and TLS, runtime-owned guest objects
//   0x2_0000_0000 ...       user zone: mappings the guest asks for without an address
//   0x7_EAFF_8000           guard page, then the 64 MiB main stack up to 0x7_EEFF_C000
//   0x10_0000_0000 ...      memory pools; fixed mappings may be anywhere that is free
//   0xFC_0000_0000          end of the application area
//
// guest_mem_sz != 0 (host unit tests, PS4_RECOMP_MEM) selects the flat arena instead: guest
// [0, guest_mem_sz) is committed read-write up front and nothing is ever made inaccessible,
// which is the memory PS4Recomp's own tests were written against.
// ---- AnyPS5 AGC driver binding (docs/m1/memory.md "AnyPS5 integration") ----
// The driver (core/libs/prx/libSceAgcDriver, AgcGuestMemory.h) translates every GPU-visible guest
// address through host = guest + bias. A build that links it defines NPS5_HAVE_ANYPS5_AGC=1; the
// runtime then configures it once, before any guest code runs: the bias of this arena, the guest
// window, and a scratch window for driver-owned memory that needs a guest address (kept
// inaccessible and mapped in the extent list, so the guest can never be handed it; the driver maps
// pages into it itself). Without the define nothing is referenced (an undefined symbol would fail
// the link of every other build, weak or not).
#if NPS5_HAVE_ANYPS5_AGC
void AgcSetGuestBias(int64_t bias);
void AgcSetGuestArena(uint64_t guest_lo, uint64_t guest_hi);
void AgcSetGuestScratch(uint64_t guest_lo, uint64_t bytes);

#define NPS5_AGC_SCRATCH_OFFSET 0xF000000000ULL // guest 0xF0_0000_0000 relative to the layout origin
#define NPS5_AGC_SCRATCH_BYTES 0x10000000ULL    // 256 MiB

void nps5_anyps5_startup(GuestContext *ctx); // modules/libSceAnyPS5: driver page claim, host environment

static void nps5_agc_bind_guest_memory(GuestContext *ctx) {
  const Nps5Arena *a = &g_nps5_arena;
  nps5_anyps5_startup(ctx);
  AgcSetGuestBias((int64_t)(uintptr_t)a->bias); // 0 in identity mode: guest == host
  AgcSetGuestArena(a->guest_lo, a->guest_hi);
  // Identity mode has bias 0: the driver's scratch memory is ordinary heap memory there (its guest
  // address is its host address), so nothing is reserved.
  if (!a->flat && a->mode == NPS5_ADDR_BASE) {
    const uint64_t lo = a->guest_org + NPS5_AGC_SCRATCH_OFFSET;
    if (nps5_guest_range_ok(lo, NPS5_AGC_SCRATCH_BYTES) &&
        recomp_vm_alloc_fixed(ctx, lo, NPS5_AGC_SCRATCH_BYTES, 0, 0, "agc_scratch") == lo) {
      nps5_mem_decommit(lo, NPS5_AGC_SCRATCH_BYTES); // address space only: the driver maps pages into it
      AgcSetGuestScratch(lo, NPS5_AGC_SCRATCH_BYTES);
    } else {
      fprintf(stderr, "[ps4-recomp] WARNING: cannot reserve the AGC driver scratch window at guest 0x%llx\n",
              (unsigned long long)lo);
    }
  }
}
#else
static void nps5_agc_bind_guest_memory(GuestContext *ctx) { (void)ctx; }
#endif

GuestContext *recomp_init_runtime(size_t guest_mem_sz, const uint8_t *elf_image,
                                  size_t image_size, const char *prog_name) {
  GuestContext *ctx = (GuestContext *)calloc(1, sizeof(GuestContext));
  if (!ctx) {
    perror("calloc GuestContext");
    return NULL;
  }

  g_current_ctx = ctx;
  recomp_install_crash_handler();

  if (guest_mem_sz == 0) {
    const char *env_mem = getenv("PS4_RECOMP_MEM");
    if (env_mem) {
      guest_mem_sz = parse_mem_size_str(env_mem);
    }
  }
  if (guest_mem_sz != 0 && image_size > 0 && guest_mem_sz < image_size + 0x20000ULL) {
    guest_mem_sz = image_size + 0x20000ULL;
  }

  const Nps5MemConfig *cfg = &nps5_mem_config;
  char err[2048];
  if (nps5_mem_reserve(cfg, guest_mem_sz, err, sizeof(err)) != 0) {
    fprintf(stderr, "[ps4-recomp] FATAL: guest memory: %s\n", err);
    NPS5_LOG(NPS5_S_MEM, NPS5_L_ERROR, "arena reservation failed: %s", err);
    free(ctx);
    return NULL;
  }
  const Nps5Arena *arena = &g_nps5_arena;

  // host = mem_base + guest for every guest address (mem_base is NULL in identity mode);
  // mem_size is the end of the guest address space, the bound HLE range checks use.
  ctx->mem_base = arena->bias;
  ctx->mem_size = arena->guest_hi;
  fprintf(stderr, "[ps4-recomp] guest memory: %s mode, host 0x%llx-0x%llx, bias 0x%llx, guest 0x%llx-0x%llx%s\n",
          arena->mode == NPS5_ADDR_IDENTITY ? "identity" : "base", (unsigned long long)arena->host_lo,
          (unsigned long long)(arena->host_lo + arena->host_len), (unsigned long long)(uintptr_t)arena->bias,
          (unsigned long long)arena->guest_lo, (unsigned long long)arena->guest_hi, arena->flat ? " (flat test arena)" : "");
  NPS5_LOG(NPS5_S_MEM, NPS5_L_INFO, "arena mode=%s host_lo=0x%llx host_len=0x%llx bias=0x%llx guest_lo=0x%llx guest_hi=0x%llx flat=%d",
           arena->mode == NPS5_ADDR_IDENTITY ? "identity" : "base", (unsigned long long)arena->host_lo,
           (unsigned long long)arena->host_len, (unsigned long long)(uintptr_t)arena->bias,
           (unsigned long long)arena->guest_lo, (unsigned long long)arena->guest_hi, (int)arena->flat);

  ctx->fpu_cw = 0x037F; // Standard x87 control word default

  // Dense image passed in memory: host tests only (flat arena, guest address 0 upwards).
  if (elf_image && image_size > 0) {
    if (!arena->flat || image_size > arena->guest_hi) {
      fprintf(stderr, "[ps4-recomp] FATAL: an in-memory dense image needs the flat test arena\n");
      free(ctx);
      return NULL;
    }
    memcpy(ctx->mem_base, elf_image, image_size);
  }

  // System block right after the images.
  uint64_t image_end = cfg->image_end;
  if (arena->flat) {
    image_end = (image_size + 0xFFFFULL) & ~0xFFFFULL;
    if (image_end < 0x10000ULL) image_end = 0x10000ULL;
  }
  if (image_end < arena->guest_lo) image_end = arena->guest_lo;
  uint64_t tls_area = image_end;
  uint64_t tcb_addr = tls_area + 0x10000ULL;
  uint64_t args_addr = tcb_addr + 0x10000ULL;
  uint64_t sys_end = args_addr + 0x10000ULL;

  // Main stack. The flat arena puts it at the top of its window, as before.
  uint64_t stack_top = cfg->stack_top, stack_len = cfg->stack_len;
  if (arena->flat) {
    stack_top = arena->guest_hi < 0x800000000ULL ? arena->guest_hi : 0x800000000ULL;
    if (stack_len > stack_top / 4) stack_len = (stack_top / 4) & ~NPS5_PAGE_MASK;
  }
  uint64_t stack_lo = stack_top - stack_len;
  uint64_t guard_lo = stack_lo - NPS5_PAGE;
  if (stack_top > arena->guest_hi || stack_len == 0 || guard_lo < sys_end) {
    fprintf(stderr, "[ps4-recomp] FATAL: guest layout does not fit: images end 0x%llx, stack 0x%llx-0x%llx, space ends 0x%llx\n",
            (unsigned long long)image_end, (unsigned long long)stack_lo, (unsigned long long)stack_top,
            (unsigned long long)arena->guest_hi);
    nps5_mem_release();
    free(ctx);
    return NULL;
  }
  if (nps5_mem_commit(tls_area, sys_end - tls_area) != 0 || nps5_mem_commit(stack_lo, stack_len) != 0) {
    fprintf(stderr, "[ps4-recomp] FATAL: cannot commit the guest system block or main stack: %s\n", strerror(errno));
    nps5_mem_release();
    free(ctx);
    return NULL;
  }
  nps5_mem_note_range(NPS5_FA_SYSTEM, tls_area, sys_end, "system block");
  nps5_mem_note_range(NPS5_FA_STACK_GUARD, guard_lo, stack_lo, "main stack guard");
  nps5_mem_note_range(NPS5_FA_STACK, stack_lo, stack_top, "main stack");

  ctx->fs_base = tcb_addr;
  MEM_U64(tcb_addr) = tcb_addr;
  MEM_U64(tcb_addr + 0x28ULL) = 0x595e9fbd94fda766ULL;
  MEM_U64(tcb_addr + RECOMP_TCB_CTX_SLOT) = (uint64_t)ctx;
  ctx->mxcsr = 0x1f80;
  ctx->fpu_cw = 0x037f;

  // Process arguments for _start_ps4_c
  ctx->args_addr = args_addr;
  uint64_t prog_name_addr = args_addr + 0x100ULL;
  const char *pname = "/app0/eboot.bin";
  if (prog_name && strncmp(prog_name, "/app0/", 6) == 0) {
    pname = prog_name;
  }
  strncpy((char *)(ctx->mem_base + prog_name_addr), pname, 255);

  // argc = 1
  MEM_U64(args_addr) = 1;
  // argv[0] = prog_name_addr
  MEM_U64(args_addr + 8) = prog_name_addr;
  // argv[1] = NULL
  MEM_U64(args_addr + 16) = 0;
  // envp[0] = NULL
  MEM_U64(args_addr + 24) = 0;
  // auxv[0] = AT_NULL (type = 0, val = 0)
  MEM_U64(args_addr + 32) = 0;
  MEM_U64(args_addr + 40) = 0;

  // RDI points to the argument structure
  ctx->rdi = args_addr;

  ctx->heap_ptr = sys_end;

  // Process root context and VM extent manager. The extent list is sorted by address and
  // covers [sys_end, guest_hi): free space below and above the main stack, the stack and its
  // guard page. Images and the system block are outside it (they are never unmapped).
  ctx->process_ctx = ctx;
  g_nps5_vm_root = ctx;
  pthread_mutex_init(&ctx->vm_mutex, NULL);
  GuestVMExtent *tail = NULL;
  tail = vm_init_append(ctx, tail, sys_end, guard_lo - sys_end, true, NULL);
  tail = vm_init_append(ctx, tail, guard_lo, NPS5_PAGE, false, "stack_guard");
  tail->prot = 0;
  tail->kind = NPS5_EXT_STACK_GUARD;
  tail = vm_init_append(ctx, tail, stack_lo, stack_len, false, "main_stack");
  tail->kind = NPS5_EXT_MAIN_STACK;
  tail = vm_init_append(ctx, tail, stack_top, arena->guest_hi - stack_top, true, NULL);

  ctx->stack_base = stack_lo;
  ctx->stack_alloc_size = stack_len;
  ctx->rsp = (stack_top - 0x1000ULL) & ~0xFFULL;
  ctx->rbp = ctx->rsp;
  recomp_vm_note_change();

  ctx->thread_id = 1000;
  recomp_init_main_thread(ctx);
  nps5_agc_bind_guest_memory(ctx);

  return ctx;
}

// Debug aid: PS4_WATCH=<hex guest addr>[:<len>] polls that guest range from a host thread
// (~every 50 us) and logs every change with the video flip count, to find out when (and by
// whom: CPU or GPU) memory gets clobbered.
#include <pthread.h>
#include <time.h>
extern volatile uint64_t g_ps4_videoout_flip_count;
static uint8_t *g_watch_ptr;
static size_t g_watch_len;
static void *watch_thread(void *arg) {
  (void)arg;
  uint8_t prev[256], cur[256];
  memcpy(prev, g_watch_ptr, g_watch_len);
  for (;;) {
    struct timespec ts = {0, 50000};
    nanosleep(&ts, NULL);
    memcpy(cur, g_watch_ptr, g_watch_len);
    if (memcmp(cur, prev, g_watch_len) != 0) {
      fprintf(stderr, "[watch] flip %llu: changed:", (unsigned long long)g_ps4_videoout_flip_count);
      for (size_t i = 0; i < g_watch_len; i += 4)
        fprintf(stderr, " %08x", *(uint32_t *)(cur + i));
      fprintf(stderr, "\n");
      memcpy(prev, cur, g_watch_len);
    }
  }
  return NULL;
}
static void start_watch(GuestContext *ctx) {
  const char *w = getenv("PS4_WATCH");
  if (!w || !ctx || !RECOMP_MEM_OK(ctx)) return;
  char *end;
  uint64_t addr = strtoull(w, &end, 16);
  size_t len = 16;
  if (*end == ':') len = (size_t)strtoul(end + 1, NULL, 0);
  if (len > 256) len = 256;
  len &= ~(size_t)3;
  if (!len || addr >= ctx->mem_size) return;
  g_watch_ptr = ctx->mem_base + addr;
  g_watch_len = len;
  pthread_t t;
  pthread_create(&t, NULL, watch_thread, NULL);
  fprintf(stderr, "[watch] watching guest 0x%llx len %zu\n", (unsigned long long)addr, len);
}

// guest_image.bin (emitter.EncodeGuestImage): magic, region table {guest addr, size, file
// offset}, segment table {guest addr, memsz, ELF PF_* flags}, region bytes. Regions are
// committed and filled; guest code segments are then made read-only (to the host they are
// data: jump tables and constants are read from them, nothing executes or writes them).
static int load_guest_image(GuestContext *ctx, FILE *fp, const char *path) {
  static const char magic[8] = {'N', 'P', 'S', '5', 'I', 'M', 'G', 1};
  struct {
    char magic[8];
    uint32_t nregions, nsegments;
  } hdr;
  if (fread(&hdr, sizeof(hdr), 1, fp) != 1 || memcmp(hdr.magic, magic, sizeof(magic)) != 0) {
    fprintf(stderr, "[ps4-recomp] FATAL: %s is not a sparse guest image (NPS5IMG v1); retranslate the title\n", path);
    return -1;
  }
  if (hdr.nregions == 0 || hdr.nregions > 4096 || hdr.nsegments > 65536) {
    fprintf(stderr, "[ps4-recomp] FATAL: %s: implausible table sizes (%u regions, %u segments)\n", path, hdr.nregions,
            hdr.nsegments);
    return -1;
  }
  typedef struct {
    uint64_t addr, size, off;
  } Region;
  typedef struct {
    uint64_t addr, memsz;
    uint32_t flags, reserved;
  } Seg;
  Region *regions = (Region *)calloc(hdr.nregions, sizeof(Region));
  Seg *segs = (Seg *)calloc(hdr.nsegments ? hdr.nsegments : 1, sizeof(Seg));
  int rc = -1;
  if (!regions || !segs || fread(regions, sizeof(Region), hdr.nregions, fp) != hdr.nregions ||
      fread(segs, sizeof(Seg), hdr.nsegments, fp) != hdr.nsegments) {
    fprintf(stderr, "[ps4-recomp] FATAL: %s: truncated image tables\n", path);
    goto out;
  }
  const uint64_t image_end = g_nps5_arena.flat ? g_nps5_arena.guest_hi : nps5_mem_config.image_end;
  size_t total = 0;
  for (uint32_t i = 0; i < hdr.nregions; i++) {
    const Region *r = &regions[i];
    if (!nps5_guest_range_ok(r->addr, r->size) || r->addr + r->size > image_end) {
      fprintf(stderr,
              "[ps4-recomp] FATAL: %s: image region 0x%llx-0x%llx is outside the image area 0x%llx-0x%llx of this "
              "title's memory layout\n",
              path, (unsigned long long)r->addr, (unsigned long long)(r->addr + r->size),
              (unsigned long long)g_nps5_arena.guest_lo, (unsigned long long)image_end);
      goto out;
    }
    if (nps5_mem_commit(r->addr, r->size) != 0) {
      fprintf(stderr, "[ps4-recomp] FATAL: cannot commit image region 0x%llx (+0x%llx): %s\n", (unsigned long long)r->addr,
              (unsigned long long)r->size, strerror(errno));
      goto out;
    }
    if (fseeko(fp, (off_t)r->off, SEEK_SET) != 0 || fread(ctx->mem_base + r->addr, 1, r->size, fp) != r->size) {
      fprintf(stderr, "[ps4-recomp] FATAL: %s: short read of image region 0x%llx (+0x%llx)\n", path,
              (unsigned long long)r->addr, (unsigned long long)r->size);
      goto out;
    }
    nps5_mem_note_range(NPS5_FA_IMAGE, r->addr & ~NPS5_PAGE_MASK, (r->addr + r->size + NPS5_PAGE_MASK) & ~NPS5_PAGE_MASK,
                        "image");
    total += r->size;
  }
  size_t ro = 0;
  for (uint32_t i = 0; i < hdr.nsegments; i++) {
    const Seg *sg = &segs[i];
    // PF_X = 1, PF_W = 2. Only whole pages inside an executable, non-writable segment.
    if ((sg->flags & 1) && !(sg->flags & 2) && nps5_guest_range_ok(sg->addr, sg->memsz) &&
        nps5_mem_protect_inner(sg->addr, sg->memsz, PROT_READ) == 0)
      ro += sg->memsz;
  }
  printf("[ps4-recomp] Loaded guest image: %u regions, %zu bytes at guest 0x%llx-0x%llx; %zu bytes of guest code read-only\n",
         hdr.nregions, total, (unsigned long long)regions[0].addr,
         (unsigned long long)(regions[hdr.nregions - 1].addr + regions[hdr.nregions - 1].size), ro);
  rc = 0;
out:
  free(regions);
  free(segs);
  return rc;
}

GuestContext *recomp_init_runtime_file(const char *image_filename,
                                       size_t requested_mem_sz,
                                       const char *prog_name) {
  char path[1024] = {0};
  FILE *fp = NULL;

  // 1. Try relative to app_root (e.g. from VFS discovery)
  const char *app_root = ps4_vfs_get_app_root();
  if (app_root && *app_root && image_filename && image_filename[0] != '/') {
    snprintf(path, sizeof(path), "%s/%s", app_root, image_filename);
    if (access(path, R_OK) == 0) {
      fp = fopen(path, "rb");
    }
  }

  // 2. Try macOS .app bundle SharedSupport directory directly
  if (!fp && image_filename) {
#if defined(__APPLE__)
    char exec_path[1024] = {0};
    uint32_t size = sizeof(exec_path);
    if (_NSGetExecutablePath(exec_path, &size) == 0) {
      char canonical_exec[1024];
      if (realpath(exec_path, canonical_exec) != NULL) {
        char dir_buf[1024];
        strncpy(dir_buf, canonical_exec, sizeof(dir_buf) - 1);
        dir_buf[sizeof(dir_buf) - 1] = '\0';
        char *dir = dirname(dir_buf);
        snprintf(path, sizeof(path), "%s/../SharedSupport/%s", dir, image_filename);
        if (access(path, R_OK) == 0) {
          fp = fopen(path, "rb");
        }
        if (!fp) {
          snprintf(path, sizeof(path), "%s/../Resources/%s", dir, image_filename);
          if (access(path, R_OK) == 0) {
            fp = fopen(path, "rb");
          }
        }
        if (!fp) {
          snprintf(path, sizeof(path), "%s/%s", dir, image_filename);
          if (access(path, R_OK) == 0) {
            fp = fopen(path, "rb");
          }
        }
      }
    }
#endif
  }

  // 3. Try directly (e.g. current working directory or full path)
  if (!fp && image_filename && access(image_filename, R_OK) == 0) {
    strncpy(path, image_filename, sizeof(path) - 1);
    fp = fopen(path, "rb");
  }

  if (!fp) {
    fprintf(stderr, "[ps4-recomp] Failed to open guest image file: %s\n",
            image_filename ? image_filename : "(null)");
    return NULL;
  }

  GuestContext *ctx = recomp_init_runtime(requested_mem_sz, NULL, 0, prog_name);
  if (!ctx) {
    fclose(fp);
    return NULL;
  }
  if (load_guest_image(ctx, fp, path) != 0) {
    fclose(fp);
    return NULL;
  }
  fclose(fp);

  // HLE libc data imports (__stdoutp, __isthreaded, ...): a data import's GOT slot must hold the
  // address of the variable, so give each one a guest cell (modules/libc/nps5_libc_core.c).
  {
    pthread_mutex_lock(&g_unresolved_mutex);
    for (int b = 0; b < UNRESOLVED_BUCKETS; b++)
      for (RecompUnresolvedSym *curr = g_unresolved_table[b]; curr; curr = curr->next) {
        const char *nm = curr->sym_name && *curr->sym_name ? curr->sym_name : curr->nid;
        if (nm) nps5_libc_bind_data_import(ctx, curr->addr, nm);
      }
    pthread_mutex_unlock(&g_unresolved_mutex);
  }

  start_watch(ctx);
  return ctx;
}

void recomp_init_process_param(GuestContext *ctx, uint64_t proc_param_addr) {
  if (!ctx)
    return;
  ctx->proc_param_addr = proc_param_addr;
  if (ctx->process_ctx) {
    ctx->process_ctx->proc_param_addr = proc_param_addr;
  }
  // The kernel sizes direct memory from the process's memory parameters at process start,
  // before the game's malloc init below allocates any.
  ps4_direct_mem_configure(ctx, proc_param_addr);
  if (!RECOMP_MEM_OK(ctx) || proc_param_addr == 0)
    return;
  if (proc_param_addr + 0x40 > ctx->mem_size)
    return;

  // SceKernelProcessParam structure has libc_param at offset 0x38
  uint64_t libc_param_addr =
      *(uint64_t *)(ctx->mem_base + proc_param_addr + 0x38ULL);
  if (libc_param_addr != 0 && libc_param_addr + 0x40 <= ctx->mem_size) {
    uint64_t lp_size = *(uint64_t *)(ctx->mem_base + libc_param_addr);
    uint32_t lp_maj = *(uint32_t *)(ctx->mem_base + libc_param_addr + 0x8ULL);
    uint32_t lp_min = *(uint32_t *)(ctx->mem_base + libc_param_addr + 0xcULL);
    uint64_t malloc_replace_addr =
        *(uint64_t *)(ctx->mem_base + libc_param_addr + 0x30ULL);
    uint64_t new_replace_addr =
        *(uint64_t *)(ctx->mem_base + libc_param_addr + 0x38ULL);
    PS4_LOGF(NPS5_S_MEM, NPS5_L_DEBUG, "ps4-mem", "SceLibcParam at 0x%llx: size=0x%llx ver=%u.%u malloc_replace=0x%llx new_replace=0x%llx\n",
           (unsigned long long)libc_param_addr, (unsigned long long)lp_size, lp_maj, lp_min,
           (unsigned long long)malloc_replace_addr, (unsigned long long)new_replace_addr);

    // If libc.prx is linked as a guest module with inits, its init functions
    // (specifically __libc_init -> _malloc_init) will run in recomp_modules_start_preloaded()
    // and initialize the heap API and replacements according to the PS4 ABI.
    // We only perform fallback initialization here if libc.prx has no preloaded inits.
    if (!recomp_module_has_preloaded_inits("libc.prx")) {
      uint64_t malloc_init_addr = recomp_resolve_symbol("libc.prx", "z8GPiQwaAEY");
      if (!malloc_init_addr) {
        malloc_init_addr = recomp_resolve_symbol("libc.prx", "_malloc_init");
      }
      if (malloc_init_addr != 0) {
        printf("[ps4-recomp] Initializing libc malloc subsystem via _malloc_init at 0x%llx...\n",
               (unsigned long long)malloc_init_addr);
        recomp_call_guest(ctx, malloc_init_addr);
      } else if (malloc_replace_addr != 0 &&
                 malloc_replace_addr + 0x20 <= ctx->mem_size) {
        uint64_t mr_size = *(uint64_t *)(ctx->mem_base + malloc_replace_addr);
        uint64_t init_func =
            *(uint64_t *)(ctx->mem_base + malloc_replace_addr + 0x10ULL);
        uint64_t malloc_func = *(uint64_t *)(ctx->mem_base + malloc_replace_addr + 0x20ULL);
        PS4_LOGF(NPS5_S_MEM, NPS5_L_DEBUG, "ps4-mem", "SceLibcMallocReplace at 0x%llx: size=0x%llx init=0x%llx malloc=0x%llx\n",
               (unsigned long long)malloc_replace_addr, (unsigned long long)mr_size,
               (unsigned long long)init_func, (unsigned long long)malloc_func);
        if (init_func != 0 && init_func < ctx->mem_size) {
          printf("[ps4-recomp] Initializing custom memory allocator via "
                 "SceLibcParam at 0x%llx...\n",
                 (unsigned long long)init_func);
          recomp_call_guest(ctx, init_func);
        }
      } else if (new_replace_addr != 0) {
        // Fallback: directly invoke _new_setup (NID KNNNbyRieqQ) if _malloc_init was not in libc exports
        uint64_t new_setup_addr = recomp_resolve_symbol("libc.prx", "KNNNbyRieqQ");
        if (!new_setup_addr) {
          new_setup_addr = recomp_resolve_symbol("libc.prx", "_new_setup");
        }
        if (new_setup_addr != 0) {
          printf("[ps4-recomp] Initializing C++ operator new replacements via _new_setup at 0x%llx...\n",
                 (unsigned long long)new_setup_addr);
          ctx->rdi = new_replace_addr;
          recomp_call_guest(ctx, new_setup_addr);
        }
      }
    }
  }
}

// x86-64 static TLS is TLS variant II: module id's block is at FS - offset (the generator lays
// the blocks out as FreeBSD's rtld does: the executable first, its block ending at FS, as its
// fs-relative accesses expect). The table is process-wide and written once, before any guest
// thread exists.
static RecompTlsModule g_tls_modules[RECOMP_TLS_MAX_MODULES];
static size_t g_tls_module_count;

uint64_t recomp_tls_module_offset(uint64_t module_id) {
  if (module_id == 0 || module_id > g_tls_module_count) return 0;
  return g_tls_modules[module_id - 1].offset;
}

// Zeroes the static TLS area [tcb - static_tls_size, tcb) and copies every module's .tdata
// template into its block.
static void tls_init_area(uint8_t *mem_base, uint64_t tcb, uint64_t static_tls_size) {
  memset(mem_base + tcb - static_tls_size, 0, static_tls_size);
  for (size_t i = 0; i < g_tls_module_count; i++) {
    const RecompTlsModule *m = &g_tls_modules[i];
    if (m->filesz) memcpy(mem_base + tcb - m->offset, mem_base + m->vaddr, m->filesz);
  }
}

void recomp_init_tls_modules(GuestContext *ctx, const RecompTlsModule *mods, size_t count) {
  if (!ctx) return;
  if (count > RECOMP_TLS_MAX_MODULES) {
    fprintf(stderr, "[ps4-recomp] FATAL: %zu static TLS modules, the runtime holds %d\n", count, RECOMP_TLS_MAX_MODULES);
    abort();
  }
  uint64_t end = 0;
  for (size_t i = 0; i < count; i++) {
    const RecompTlsModule *m = &mods[i];
    const uint64_t align = m->align ? m->align : 1;
    if (m->id != i + 1 || m->offset < m->memsz || m->offset % align != 0 || m->filesz > m->memsz ||
        (m->filesz && m->vaddr + m->filesz > ctx->mem_size)) {
      fprintf(stderr,
              "[ps4-recomp] FATAL: malformed static TLS module %zu: id %llu vaddr 0x%llx filesz 0x%llx memsz 0x%llx "
              "align 0x%llx offset 0x%llx\n",
              i, (unsigned long long)m->id, (unsigned long long)m->vaddr, (unsigned long long)m->filesz,
              (unsigned long long)m->memsz, (unsigned long long)m->align, (unsigned long long)m->offset);
      abort();
    }
    g_tls_modules[i] = *m;
    if (m->offset > end) end = m->offset;
  }
  g_tls_module_count = count;
  // At least 0x100 bytes, 16-byte aligned.
  uint64_t static_tls_size = (end + 15) & ~15ULL;
  if (static_tls_size < 0x100ULL) static_tls_size = 0x100ULL;
  ctx->static_tls_size = static_tls_size;
  if (ctx->process_ctx) ctx->process_ctx->static_tls_size = static_tls_size;

  // recomp_init_runtime reserves 64 KB below the main thread's TCB for its static TLS.
  if (static_tls_size > 0x10000ULL) {
    fprintf(stderr, "[ps4-recomp] FATAL: static TLS 0x%llx exceeds the 64 KB main thread TLS area\n",
            (unsigned long long)static_tls_size);
    abort();
  }
  if (RECOMP_MEM_OK(ctx) && ctx->fs_base >= static_tls_size) {
    tls_init_area(ctx->mem_base, ctx->fs_base, static_tls_size);
    for (size_t i = 0; i < count; i++)
      printf("[ps4-recomp] TLS module %llu: vaddr=0x%llx filesz=0x%llx memsz=0x%llx align=%llu at fs-0x%llx\n",
             (unsigned long long)mods[i].id, (unsigned long long)mods[i].vaddr, (unsigned long long)mods[i].filesz,
             (unsigned long long)mods[i].memsz, (unsigned long long)mods[i].align, (unsigned long long)mods[i].offset);
    printf("[ps4-recomp] TLS initialized: %zu modules, static_tls_size=0x%llx (fs_base=0x%llx)\n", count,
           (unsigned long long)static_tls_size, (unsigned long long)ctx->fs_base);
  }
}

void recomp_setup_entry_args(GuestContext *ctx, int argc, char **argv, uint64_t entry_addr) {
  if (!ctx || !RECOMP_MEM_OK(ctx)) return;

  uint64_t args_addr = ctx->args_addr;
  if (!args_addr) {
    args_addr = ctx->fs_base + 0x10000ULL;
    ctx->args_addr = args_addr;
    if (ctx->process_ctx) {
      ctx->process_ctx->args_addr = args_addr;
    }
  }

  int guest_argc = 0;
  uint64_t str_buf = args_addr + 0x120ULL;

  const char *p0 = "/app0/eboot.bin";
  if (argc > 0 && argv && argv[0] && strncmp(argv[0], "/app0/", 6) == 0) {
    p0 = argv[0];
  }

  uint64_t p0_addr = str_buf;
  strncpy((char *)(ctx->mem_base + p0_addr), p0, 255);
  *(char *)(ctx->mem_base + p0_addr + 255) = '\0';
  str_buf += (strlen(p0) + 1 + 7) & ~7ULL;

  MEM_U64(args_addr + 8ULL) = p0_addr;
  guest_argc = 1;

  for (int i = 1; i < argc && guest_argc < 32; i++) {
    if (argv && argv[i]) {
      uint64_t arg_addr = str_buf;
      strncpy((char *)(ctx->mem_base + arg_addr), argv[i], 255);
      *(char *)(ctx->mem_base + arg_addr + 255) = '\0';
      str_buf += (strlen(argv[i]) + 1 + 7) & ~7ULL;
      MEM_U64(args_addr + 8ULL + (uint64_t)guest_argc * 8ULL) = arg_addr;
      guest_argc++;
    }
  }

  MEM_U64(args_addr + 8ULL + (uint64_t)guest_argc * 8ULL) = 0;
  *(int32_t *)(ctx->mem_base + args_addr) = guest_argc;
  *(uint32_t *)(ctx->mem_base + args_addr + 4ULL) = 0;
  MEM_U64(args_addr + 0x110ULL) = entry_addr;

  // Set up RDI and RSI according to PS4 System V ABI:
  ctx->rdi = args_addr;
  ctx->rsi = 0;

  // Prepare stack according to PS4 Orbis OS entry conventions:
  // [rsp] = argc
  // [rsp + 8] = argv[0]
  uint64_t sp = ctx->rsp;
  sp &= ~15ULL;
  sp -= 8;
  sp -= 8;
  MEM_U64(sp) = p0_addr;
  sp -= 8;
  MEM_U64(sp) = (uint64_t)guest_argc;

  ctx->rsp = sp;
  ctx->rbp = 0;
}

void shim_sceKernelGetProcParam(GuestContext *ctx) {
  GuestContext *proc = ctx->process_ctx ? ctx->process_ctx : ctx;
  ctx->rax = proc->proc_param_addr;
  SHIM_RETURN();
}

void recomp_dispatch_external(GuestContext *ctx, uint64_t target) {
  recomp_dispatch(ctx, target);
}

#if defined(__APPLE__)
// Bounds of the __TEXT,__recomp_guest section, provided by the linker (empty when there is no
// guest code, as in host tests).
extern const char g_recomp_guest_text_start[] __asm("section$start$__TEXT$__recomp_guest");
extern const char g_recomp_guest_text_end[] __asm("section$end$__TEXT$__recomp_guest");

bool recomp_pc_in_guest_code(uint64_t pc) {
  if (pc >= (uint64_t)(uintptr_t)g_recomp_guest_text_start && pc < (uint64_t)(uintptr_t)g_recomp_guest_text_end) return true;
  return nps5_diag_pc_in_fragment(pc) != 0; // tier-1 fragment dylibs (nps5_tier1.c)
}
#else
bool recomp_pc_in_guest_code(uint64_t pc) {
  (void)pc;
  return false;
}
#endif

void recomp_free_runtime(GuestContext *ctx) {
  if (!ctx)
    return;

  ps4_metal_screen_destroy();
  ps4_keyboard_destroy();
  ps4_videoout_destroy();
  ps4_equeue_destroy();
  ps4_direct_mem_destroy();
  ps4_event_flag_destroy();
  ps4_sync_destroy();
  ps4_vfs_destroy();
  ps4_aio_destroy();
  ps4_gnmdriver_destroy();
  ps4_ajm_destroy();

  dump_map_if_asked();
  nps5_mem_release();
  g_nps5_vm_root = NULL;
  GuestVMExtent *ext = ctx->vm_extents;
  while (ext) {
    GuestVMExtent *next = ext->next;
    free(ext);
    ext = next;
  }
  pthread_mutex_destroy(&ctx->vm_mutex);
  for (size_t i = 0; i < DISPATCH_LEVEL_SIZE; i++) {
    if (g_dispatch_l1[i]) {
      for (size_t j = 0; j < DISPATCH_LEVEL_SIZE; j++) {
        if (g_dispatch_l1[i][j]) {
          free(g_dispatch_l1[i][j]);
        }
      }
      free(g_dispatch_l1[i]);
      g_dispatch_l1[i] = NULL;
    }
  }
  lazy_free();
  free(ctx);
}

// Guest non-local control transfers (setjmp/longjmp, C++ landing pads) live in nps5_eh.c.

// C backend only: recompiled C functions push UnwindFrames; assembly code never does.
void recomp_unwind_to(GuestContext *ctx, uint64_t target_ip) {
  ctx->rip = target_ip;
  UnwindFrame *f = ctx->unwind_frame;
  while (f) {
    if (target_ip >= f->fn_start && target_ip < f->fn_end) {
      ctx->unwind_frame = f;
      longjmp(f->buf, 1);
    }
    f = f->prev;
  }
  fprintf(stderr,
          "FATAL: Unwind target 0x%llx not found in active unwind frames!\n",
          (unsigned long long)target_ip);
  abort();
}

GuestContext *recomp_create_thread_context(GuestContext *parent,
                                           uint64_t stack_size) {
  if (!parent)
    return NULL;
  if (stack_size == 0)
    stack_size = 2 * 1024 * 1024; // 2MB default stack
  GuestContext *proc = parent->process_ctx ? parent->process_ctx : parent;

  uint64_t tls_size = proc->static_tls_size ? proc->static_tls_size : 0x1000ULL;
  tls_size = (tls_size + 0xFFFULL) & ~0xFFFULL;

  // Allocate guard page, stack, TLS area and TCB (64KB) via the VM extent allocator. The
  // lowest page is given back to the host as PROT_NONE: a stack overflow faults there and the
  // crash report names it instead of the thread silently writing into its neighbour.
  // A size that does not fit the guest window must fail, not wrap (rounding ~0 up gave 0).
  const uint64_t limit = g_nps5_arena.guest_hi ? g_nps5_arena.guest_hi : (1ULL << 40);
  if (stack_size > limit || tls_size > limit) {
    fprintf(stderr, "[ps4-recomp] thread stack of 0x%llx bytes does not fit the guest address space\n",
            (unsigned long long)stack_size);
    return NULL;
  }
  stack_size = (stack_size + NPS5_PAGE_MASK) & ~NPS5_PAGE_MASK;
  size_t alloc_total = NPS5_PAGE + stack_size + tls_size + 0x10000ULL;
  uint64_t alloc_base = recomp_vm_alloc_named(proc, alloc_total, PROT_READ | PROT_WRITE, 0, "thread_stack");
  if (alloc_base == (uint64_t)-1) {
    fprintf(stderr,
            "[ps4-recomp] FATAL: out of guest memory for new thread stack\n");
    return NULL;
  }
  if (!g_nps5_arena.flat) nps5_mem_protect_inner(alloc_base, NPS5_PAGE, PROT_NONE);
  uint64_t stack_base = alloc_base + NPS5_PAGE;
  uint64_t tcb_base = stack_base + stack_size + tls_size;

  // Static TLS area for the new thread [tcb_base - tls_size, tcb_base): zeroed, with every
  // module's .tdata template in its block (recomp_init_tls_modules).
  tls_init_area(parent->mem_base, tcb_base, tls_size);

  // Initialize FS TCB base and stack canary
  *(uint64_t *)(parent->mem_base + tcb_base) = tcb_base;
  *(uint64_t *)(parent->mem_base + tcb_base + 0x28ULL) = 0x595e9fbd94fda766ULL;

  GuestContext *t_ctx = (GuestContext *)calloc(1, sizeof(GuestContext));
  if (!t_ctx)
    return NULL;

  *(uint64_t *)(parent->mem_base + tcb_base + RECOMP_TCB_CTX_SLOT) = (uint64_t)t_ctx;

  t_ctx->mem_base = parent->mem_base;
  t_ctx->mem_size = parent->mem_size;
  t_ctx->heap_ptr = parent->heap_ptr;
  t_ctx->fs_base = tcb_base;
  t_ctx->process_ctx = proc;
  t_ctx->stack_base = alloc_base;
  t_ctx->stack_alloc_size = alloc_total;
  t_ctx->rsp = (stack_base + stack_size - 0x100ULL) & ~0xFFULL;
  t_ctx->rbp = t_ctx->rsp;
  // The creator's control words, as FreeBSD's cpu_copy_thread copies the user FPU state at
  // thr_new; the guest entry trampoline applies mxcsr to FPCR (RMode, FZ).
  t_ctx->fpu_cw = parent->fpu_cw;
  t_ctx->mxcsr = parent->mxcsr;
  t_ctx->static_tls_size = proc->static_tls_size;

  return t_ctx;
}

void recomp_free_thread_context(GuestContext *ctx) {
  if (!ctx)
    return;
  GuestContext *proc = ctx->process_ctx ? ctx->process_ctx : ctx;
  if (ctx->stack_base && ctx->stack_alloc_size > 0 && ctx != proc) {
    recomp_vm_free(proc, ctx->stack_base, ctx->stack_alloc_size);
  }
  recomp_shadow_release(ctx);
  free(ctx);
}


// recomp_vpcmpistri_impl / recomp_vpcmpe_stri_impl ((V)PCMPISTRI, (V)PCMPESTRI): nps5_pcmpstr.c.

// ============================================================================
// AES-NI and PCLMULQDQ Helpers
// ============================================================================

xmm_reg_t recomp_pclmulqdq(uint64_t a, uint64_t b) {
  xmm_reg_t res = {0};
#if (defined(__aarch64__) || defined(__arm64__))
  poly64_t pa = (poly64_t)a;
  poly64_t pb = (poly64_t)b;
  poly128_t r = vmull_p64(pa, pb);
  memcpy(&res, &r, 16);
#else
  uint64_t lo = 0, hi = 0;
  for (int i = 0; i < 64; i++) {
    if ((b >> i) & 1) {
      lo ^= (a << i);
      if (i > 0) {
        hi ^= (a >> (64 - i));
      }
    }
  }
  res.u64[0] = lo;
  res.u64[1] = hi;
#endif
  return res;
}

static const uint8_t aes_sbox[256] = {
  0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
  0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
  0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
  0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
  0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
  0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
  0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
  0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
  0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
  0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
  0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
  0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
  0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
  0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
  0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
  0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};

static const uint8_t aes_inv_sbox[256] = {
  0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
  0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
  0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
  0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
  0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
  0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
  0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
  0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
  0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
  0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
  0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
  0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
  0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
  0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
  0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
  0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d
};

static inline uint8_t aes_xtime(uint8_t x) {
  return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0));
}

static inline void aes_mix_column(uint8_t *col) {
  uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
  col[0] = aes_xtime(a0 ^ a1) ^ a1 ^ a2 ^ a3;
  col[1] = aes_xtime(a1 ^ a2) ^ a2 ^ a3 ^ a0;
  col[2] = aes_xtime(a2 ^ a3) ^ a3 ^ a0 ^ a1;
  col[3] = aes_xtime(a3 ^ a0) ^ a0 ^ a1 ^ a2;
}

static inline uint8_t aes_mul_gf(uint8_t a, uint8_t b) {
  uint8_t p = 0;
  for (int i = 0; i < 8; i++) {
    if (b & 1) p ^= a;
    uint8_t hi = a & 0x80;
    a <<= 1;
    if (hi) a ^= 0x1B;
    b >>= 1;
  }
  return p;
}

static inline void aes_inv_mix_column(uint8_t *col) {
  uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
  col[0] = aes_mul_gf(a0, 0x0E) ^ aes_mul_gf(a1, 0x0B) ^ aes_mul_gf(a2, 0x0D) ^ aes_mul_gf(a3, 0x09);
  col[1] = aes_mul_gf(a0, 0x09) ^ aes_mul_gf(a1, 0x0E) ^ aes_mul_gf(a2, 0x0B) ^ aes_mul_gf(a3, 0x0D);
  col[2] = aes_mul_gf(a0, 0x0D) ^ aes_mul_gf(a1, 0x09) ^ aes_mul_gf(a2, 0x0E) ^ aes_mul_gf(a3, 0x0B);
  col[3] = aes_mul_gf(a0, 0x0B) ^ aes_mul_gf(a1, 0x0D) ^ aes_mul_gf(a2, 0x09) ^ aes_mul_gf(a3, 0x0E);
}

xmm_reg_t recomp_vaesenc(xmm_reg_t s1, xmm_reg_t s2) {
  xmm_reg_t state;
  state.u8[0]  = aes_sbox[s1.u8[0]];
  state.u8[4]  = aes_sbox[s1.u8[4]];
  state.u8[8]  = aes_sbox[s1.u8[8]];
  state.u8[12] = aes_sbox[s1.u8[12]];

  state.u8[1]  = aes_sbox[s1.u8[5]];
  state.u8[5]  = aes_sbox[s1.u8[9]];
  state.u8[9]  = aes_sbox[s1.u8[13]];
  state.u8[13] = aes_sbox[s1.u8[1]];

  state.u8[2]  = aes_sbox[s1.u8[10]];
  state.u8[6]  = aes_sbox[s1.u8[14]];
  state.u8[10] = aes_sbox[s1.u8[2]];
  state.u8[14] = aes_sbox[s1.u8[6]];

  state.u8[3]  = aes_sbox[s1.u8[15]];
  state.u8[7]  = aes_sbox[s1.u8[3]];
  state.u8[11] = aes_sbox[s1.u8[7]];
  state.u8[15] = aes_sbox[s1.u8[11]];

  aes_mix_column(&state.u8[0]);
  aes_mix_column(&state.u8[4]);
  aes_mix_column(&state.u8[8]);
  aes_mix_column(&state.u8[12]);

  state.u64[0] ^= s2.u64[0];
  state.u64[1] ^= s2.u64[1];
  return state;
}

xmm_reg_t recomp_vaesenclast(xmm_reg_t s1, xmm_reg_t s2) {
  xmm_reg_t state;
  state.u8[0]  = aes_sbox[s1.u8[0]];
  state.u8[4]  = aes_sbox[s1.u8[4]];
  state.u8[8]  = aes_sbox[s1.u8[8]];
  state.u8[12] = aes_sbox[s1.u8[12]];

  state.u8[1]  = aes_sbox[s1.u8[5]];
  state.u8[5]  = aes_sbox[s1.u8[9]];
  state.u8[9]  = aes_sbox[s1.u8[13]];
  state.u8[13] = aes_sbox[s1.u8[1]];

  state.u8[2]  = aes_sbox[s1.u8[10]];
  state.u8[6]  = aes_sbox[s1.u8[14]];
  state.u8[10] = aes_sbox[s1.u8[2]];
  state.u8[14] = aes_sbox[s1.u8[6]];

  state.u8[3]  = aes_sbox[s1.u8[15]];
  state.u8[7]  = aes_sbox[s1.u8[3]];
  state.u8[11] = aes_sbox[s1.u8[7]];
  state.u8[15] = aes_sbox[s1.u8[11]];

  state.u64[0] ^= s2.u64[0];
  state.u64[1] ^= s2.u64[1];
  return state;
}

xmm_reg_t recomp_vaesdec(xmm_reg_t s1, xmm_reg_t s2) {
  xmm_reg_t state;
  state.u8[0]  = aes_inv_sbox[s1.u8[0]];
  state.u8[4]  = aes_inv_sbox[s1.u8[4]];
  state.u8[8]  = aes_inv_sbox[s1.u8[8]];
  state.u8[12] = aes_inv_sbox[s1.u8[12]];

  state.u8[1]  = aes_inv_sbox[s1.u8[13]];
  state.u8[5]  = aes_inv_sbox[s1.u8[1]];
  state.u8[9]  = aes_inv_sbox[s1.u8[5]];
  state.u8[13] = aes_inv_sbox[s1.u8[9]];

  state.u8[2]  = aes_inv_sbox[s1.u8[10]];
  state.u8[6]  = aes_inv_sbox[s1.u8[14]];
  state.u8[10] = aes_inv_sbox[s1.u8[2]];
  state.u8[14] = aes_inv_sbox[s1.u8[6]];

  state.u8[3]  = aes_inv_sbox[s1.u8[7]];
  state.u8[7]  = aes_inv_sbox[s1.u8[11]];
  state.u8[11] = aes_inv_sbox[s1.u8[15]];
  state.u8[15] = aes_inv_sbox[s1.u8[3]];

  aes_inv_mix_column(&state.u8[0]);
  aes_inv_mix_column(&state.u8[4]);
  aes_inv_mix_column(&state.u8[8]);
  aes_inv_mix_column(&state.u8[12]);

  state.u64[0] ^= s2.u64[0];
  state.u64[1] ^= s2.u64[1];
  return state;
}

xmm_reg_t recomp_vaesdeclast(xmm_reg_t s1, xmm_reg_t s2) {
  xmm_reg_t state;
  state.u8[0]  = aes_inv_sbox[s1.u8[0]];
  state.u8[4]  = aes_inv_sbox[s1.u8[4]];
  state.u8[8]  = aes_inv_sbox[s1.u8[8]];
  state.u8[12] = aes_inv_sbox[s1.u8[12]];

  state.u8[1]  = aes_inv_sbox[s1.u8[13]];
  state.u8[5]  = aes_inv_sbox[s1.u8[1]];
  state.u8[9]  = aes_inv_sbox[s1.u8[5]];
  state.u8[13] = aes_inv_sbox[s1.u8[9]];

  state.u8[2]  = aes_inv_sbox[s1.u8[10]];
  state.u8[6]  = aes_inv_sbox[s1.u8[14]];
  state.u8[10] = aes_inv_sbox[s1.u8[2]];
  state.u8[14] = aes_inv_sbox[s1.u8[6]];

  state.u8[3]  = aes_inv_sbox[s1.u8[7]];
  state.u8[7]  = aes_inv_sbox[s1.u8[11]];
  state.u8[11] = aes_inv_sbox[s1.u8[15]];
  state.u8[15] = aes_inv_sbox[s1.u8[3]];

  state.u64[0] ^= s2.u64[0];
  state.u64[1] ^= s2.u64[1];
  return state;
}

xmm_reg_t recomp_vaesimc(xmm_reg_t s) {
  xmm_reg_t res = s;
  aes_inv_mix_column(&res.u8[0]);
  aes_inv_mix_column(&res.u8[4]);
  aes_inv_mix_column(&res.u8[8]);
  aes_inv_mix_column(&res.u8[12]);
  return res;
}

static inline uint32_t aes_subword(uint32_t w) {
  return ((uint32_t)aes_sbox[w & 0xFF]) |
         (((uint32_t)aes_sbox[(w >> 8) & 0xFF]) << 8) |
         (((uint32_t)aes_sbox[(w >> 16) & 0xFF]) << 16) |
         (((uint32_t)aes_sbox[(w >> 24) & 0xFF]) << 24);
}

static inline uint32_t aes_rotword(uint32_t w) {
  return (w >> 8) | ((w & 0xFF) << 24);
}

xmm_reg_t recomp_vaeskeygenassist_impl(xmm_reg_t s, uint8_t rcon) {
  xmm_reg_t res = {0};
  uint32_t x1 = s.u32[1];
  uint32_t x3 = s.u32[3];
  uint32_t sw1 = aes_subword(x1);
  uint32_t sw3 = aes_subword(x3);
  res.u32[0] = sw1;
  res.u32[1] = aes_rotword(sw1) ^ (uint32_t)rcon;
  res.u32[2] = sw3;
  res.u32[3] = aes_rotword(sw3) ^ (uint32_t)rcon;
  return res;
}

void recomp_unsupported_instruction(uint64_t rip, uint64_t fn_entry, const char *inst_str) {
  nps5_diag_unsupported_hit(rip, fn_entry, inst_str);
  fprintf(stderr, "\n=======================================================\n");
  fprintf(stderr, "FATAL: Unsupported guest instruction executed!\n");
  fprintf(stderr, "  Guest RIP:   0x%llx\n", (unsigned long long)rip);
  if (fn_entry != 0) {
    fprintf(stderr, "  Function:    fn_0x%llx (offset +0x%llx)\n",
            (unsigned long long)fn_entry,
            (unsigned long long)(rip >= fn_entry ? rip - fn_entry : 0));
  }
  if (inst_str && (uintptr_t)inst_str > 0x1000 && inst_str[0]) {
    fprintf(stderr, "  Instruction: %s\n", inst_str);
  }
  fprintf(stderr, "=======================================================\n\n");
  fflush(stderr);
  abort();
}

// ---- Long-frame counters (ps4_hitch.h) ---------------------------------------------------
#include "ps4_hitch.h"
#include <stdatomic.h>

static _Atomic uint64_t g_hitch_ns[PS4_HITCH_KINDS];

uint64_t ps4_hitch_now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

void ps4_hitch_add_ns(int kind, uint64_t ns) {
  if ((unsigned)kind < PS4_HITCH_KINDS) atomic_fetch_add_explicit(&g_hitch_ns[kind], ns, memory_order_relaxed);
}

uint64_t ps4_hitch_take_ns(int kind) {
  return (unsigned)kind < PS4_HITCH_KINDS ? atomic_exchange_explicit(&g_hitch_ns[kind], 0, memory_order_relaxed) : 0;
}

// ---- Quit --------------------------------------------------------------------------------
// exit() would run static destructors while game threads and Metal completion handlers keep
// using the objects being destroyed (crash in RunFences on a destroyed mutex). Nothing needs to
// be written on exit except the pipeline archive; stdio buffers are flushed.
// Mach-O static links cannot leave a weak reference undefined, so the default is an empty weak
// definition; the pipeline archive module provides the strong one.
__attribute__((weak)) void ps4_gnm_pso_archive_save_for_quit(double timeout_s) { (void)timeout_s; }
// A GNM frame capture in progress (PS4_GNM_CAPTURE_FRAME) writes what it recorded; the Metal
// driver provides the strong definition.
__attribute__((weak)) void ps4_gnm_capture_save_for_quit(double timeout_s) { (void)timeout_s; }

void ps4_host_quit(int code) {
  dump_map_if_asked();
  ps4_gnm_pso_archive_save_for_quit(2.0);
  ps4_gnm_capture_save_for_quit(2.0);
  fflush(NULL);
  ps4_log_drain_sync(1.0);
  _exit(code);
}
