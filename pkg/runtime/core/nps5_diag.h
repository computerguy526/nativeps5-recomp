// nps5 diagnostics (SPEC 8.2): run bundle, structured JSONL log, crash handler, exit summary.
#ifndef NPS5_DIAG_H
#define NPS5_DIAG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { NPS5_L_TRACE = 0, NPS5_L_DEBUG, NPS5_L_INFO, NPS5_L_WARN, NPS5_L_ERROR } nps5_level_t;

typedef enum {
  NPS5_S_LOADER = 0, NPS5_S_LIFTER, NPS5_S_DISPATCH, NPS5_S_TIER1, NPS5_S_HLE, NPS5_S_KERNEL,
  NPS5_S_MEM, NPS5_S_GPU, NPS5_S_AUDIO, NPS5_S_PAD, NPS5_S_SYS, NPS5_S_VM, NPS5_S_RUNTIME,
  NPS5_S_COUNT
} nps5_subsys_t;

// Current threshold per subsystem (NPS5_LOG=hle=debug,loader=trace; default info).
extern uint8_t g_nps5_level[NPS5_S_COUNT];

// Logs one JSONL record {t, level, subsys, tid, guest_pc?, msg} to the run bundle's log.jsonl.
// Repeats from the same call site are coalesced ("repeat": N). Warn and above also echo to
// stderr as "[subsys] msg".
void nps5_log(int subsys, int level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#define NPS5_LOG(subsys, level, ...)                                                        \
  do {                                                                                      \
    if ((level) >= g_nps5_level[subsys]) nps5_log((subsys), (level), __VA_ARGS__);          \
  } while (0)

// Compat for the existing "[ps4-xxx] ..." printf-style runtime logging: keeps the stderr line
// exactly as before and also records it in log.jsonl at the given level. tag is e.g. "ps4-sys".
void nps5_log_compat(int subsys, int level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
#define PS4_LOGF(subsys, level, tag, ...) nps5_log_compat((subsys), (level), (tag), __VA_ARGS__)

// Called by the shim bridge (_recomp_call_shim) before every HLE call when set.
extern void (*g_nps5_hle_hook)(void *ctx, void *shim_fn);

// Unresolved import hit: log once per NID and count every hit.
void nps5_diag_unresolved_hit(const char *name, const char *nid, const char *lib, uint64_t rip, uint64_t caller);

// Installs the crash handlers (SIGSEGV/SIGBUS/SIGILL/SIGTRAP/SIGABRT), chaining to the handlers
// installed before, and loads the pc map. Safe to call more than once.
void nps5_diag_install_crash_handlers(void);
// Fault resolvers: host libraries that resolve some SIGSEGV/SIGBUS themselves (the AGC driver's write watch protects
// guest pages and unprotects them on the first write) register a function here. The crash handler calls the
// resolvers first for a real memory fault (not kill/raise) and returns without a report when one returns true; only an
// unresolved fault is a crash. Without this the handler (which sits above the library's own in the sigaction chain)
// reported every resolved fault as fatal. Async-signal context: the resolver must be async-signal-safe. Up to 4.
typedef bool (*Nps5FaultResolver)(uint64_t host_fault_addr);
bool nps5_diag_add_fault_resolver(Nps5FaultResolver fn);
// The same with the kind of access (is_write: 1 a store, 0 a load or an instruction fetch), for a library that also
// makes pages inaccessible to trap reads. The resolver may block (it can wait for another thread to bring the page up
// to date) but must not take a lock the faulting thread can hold. Shares the 4 slots with the plain resolvers.
typedef bool (*Nps5AccessResolver)(uint64_t host_fault_addr, int is_write);
bool nps5_diag_add_access_resolver(Nps5AccessResolver fn);
// The same with the faulting thread's ucontext_t, for a resolver that may carry the trapped instruction out itself.
// Returns 0 not claimed, 1 claimed: the access is retried, 2 claimed: the resolver moved the thread past the
// instruction (the fault cannot repeat by itself, so it does not count towards the livelock guard below).
typedef int (*Nps5ContextResolver)(void *ucontext, uint64_t host_fault_addr, int is_write);
bool nps5_diag_add_context_resolver(Nps5ContextResolver fn);
// Faults a resolver claimed since the start (all threads).
uint64_t nps5_diag_resolved_faults(void);
// Livelock guard of the fault resolvers (default 20000 claimed faults at one address within 50 ms gaps; settable for tests).
void nps5_diag_set_livelock_limit(uint32_t n);

// Set by nps5_hostcall.c on first use: the crash handler offers every memory fault to it before the resolvers
// (a host library branching to a guest function pointer; the hook redirects the thread and returns true).
extern bool (*g_nps5_hostcall_fault_hook)(void *ucontext, uint64_t fault_addr);
// Name of a guest function by its entry address (a linear scan of the pc map; nps5_diag_host_pc_to_guest skips it
// when its fn_name is NULL).
int nps5_diag_guest_fn_name(uint64_t fn_entry, const char **fn_name);

// A one-line note that the crash report prints under "note:" (e.g. which import was missing).
void nps5_diag_set_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void nps5_diag_set_exit_code(int code);
// The lifter stub for an unsupported guest instruction ran (called before it aborts).
void nps5_diag_unsupported_hit(uint64_t rip, uint64_t fn_entry, const char *text);

// Maps a host pc inside recompiled guest code to its guest pc / function (name may be NULL).
// Returns 0 when there is no pc map (translated with NPS5_NO_PCMAP) or no entry.
int nps5_diag_host_pc_to_guest(uint64_t host_pc, uint64_t *guest_pc, uint64_t *fn_entry, const char **fn_name);

// Tier 1 (nps5_tier1.c). A miss that cannot be served: recorded for crash.txt (structured
// tier1_* fields plus the note) before the caller aborts.
void nps5_diag_set_tier1_failure(const char *kind, uint64_t rip, const char *module, uint64_t off, const char *reason,
                                 const char *detail);
// A tier-1 dylib was loaded: host code range [lo, hi), its pc map (`bytes` bytes of the records of
// pkg/emitter/pcmap.go, the same as the main image's __nps5_pcmap section) and path. The map must
// stay valid; it is indexed once here.
void nps5_diag_register_fragment(uint64_t lo, uint64_t hi, const uint8_t *pcmap, size_t bytes, const char *path);
int nps5_diag_pc_in_fragment(uint64_t pc);
const char *nps5_diag_fragment_path(uint64_t pc); // NULL when pc is not in a fragment

// Path of the run bundle directory ("" when bundles are disabled).
const char *nps5_diag_bundle_dir(void);

#ifdef __cplusplus
}
#endif
#endif
