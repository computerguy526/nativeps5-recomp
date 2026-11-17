// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef RECOMP_RUNTIME_H
#define RECOMP_RUNTIME_H

#include "recomp_context.h"
#include "nps5_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

// Runtime initialization and memory layout
GuestContext *recomp_init_runtime(size_t guest_mem_sz, const uint8_t *elf_image, size_t image_size, const char *prog_name);
GuestContext *recomp_init_runtime_file(const char *image_filename, size_t requested_mem_sz, const char *prog_name);
void recomp_init_process_param(GuestContext *ctx, uint64_t proc_param_addr);
// One module's PT_TLS block in the process's static TLS area, as the generator laid it out
// (elfloader.TLSModule, FreeBSD rtld order, TLS variant II): module id's block is at
// FS - offset; its .tdata template is [vaddr, vaddr + filesz), the rest up to memsz is zero.
typedef struct RecompTlsModule {
  uint64_t id; // tls_index.module: 1, 2, ... (the executable is 1 when it has PT_TLS)
  uint64_t vaddr;
  uint64_t filesz;
  uint64_t memsz;
  uint64_t align;
  uint64_t offset;
} RecompTlsModule;
#define RECOMP_TLS_MAX_MODULES 64
// Registers the process's static TLS modules (mods[i].id == i + 1; the table is copied) and sets
// up the main thread's static TLS area below its TCB. Aborts on a malformed table.
void recomp_init_tls_modules(GuestContext *ctx, const RecompTlsModule *mods, size_t count);
// The offset below FS of module_id's static TLS block, or 0 when the process has no such module.
uint64_t recomp_tls_module_offset(uint64_t module_id);
void recomp_setup_entry_args(GuestContext *ctx, int argc, char **argv, uint64_t entry_addr);
void recomp_dispatch_external(GuestContext *ctx, uint64_t target);
// Flat call model runtime (shadow return stack, continuation table); also pulls in the dispatch
// API shared with tier 1 (recomp_dispatch_api.h).
#include "nps5_shadow.h"
// True when host pc is in recompiled guest code (the __recomp_guest section: recompiled
// functions, shim stubs, indirect-branch entry points). There the guest registers are live in
// the pinned host registers; in host code (shims, the bridge, helpers) the GuestContext holds
// what the last guest -> host transition stored. Async-signal-safe.
bool recomp_pc_in_guest_code(uint64_t pc);
// Non-local control transfers (nps5_eh.c, docs/m1/setjmp-eh.md).
void shim_guest_setjmp(GuestContext *ctx);
void shim_guest_longjmp(GuestContext *ctx);
void shim_guest_unwind_jumpto(GuestContext *ctx);
void shim_guest_abort(GuestContext *ctx);
void recomp_resume_guest(GuestContext *ctx, uintptr_t host_sp, uintptr_t host_pc, uint64_t fpcr) __attribute__((noreturn));
void recomp_free_runtime(GuestContext *ctx);
GuestContext *recomp_create_thread_context(GuestContext *parent, uint64_t stack_size);
uint64_t recomp_vm_alloc(GuestContext *ctx, size_t size);
uint64_t recomp_vm_alloc_named(GuestContext *ctx, size_t size, int prot, int flags, const char *name);
uint64_t recomp_vm_alloc_named_aligned(GuestContext *ctx, size_t size, size_t alignment, int prot, int flags, const char *name);
uint64_t recomp_vm_alloc_fixed(GuestContext *ctx, uint64_t desired_addr, size_t size, int prot, int flags, const char *name);
// Guest-requested mappings (nps5_mem.h, docs/m1/memory.md): unhinted ones go to the user zone
// from 0x2_0000_0000; a MAP_FIXED request replaces what the guest had mapped in the range.
uint64_t recomp_vm_map_guest(GuestContext *ctx, size_t size, size_t alignment, int prot, int flags, const char *name);
uint64_t recomp_vm_map(GuestContext *ctx, uint64_t addr, size_t size, size_t alignment, int prot, int flags, const char *name);
// recomp_vm_map with the extent kind (NPS5_EXT_*, nps5_mem.h) and the reason for a failure: *err
// (may be NULL) gets EINVAL for a malformed request (fixed address 0 or not 16 KiB aligned, range
// outside the guest window, size overflow) and ENOMEM when the range or space is not available.
uint64_t recomp_vm_map_ex(GuestContext *ctx, uint64_t addr, size_t size, size_t alignment, int prot, int flags,
                          const char *name, int kind, int *err);
uint64_t recomp_vm_map_fixed_replace(GuestContext *ctx, uint64_t addr, size_t size, int prot, int flags, const char *name);
int recomp_vm_free(GuestContext *ctx, uint64_t addr, size_t size);
bool recomp_vm_split_extent_at(GuestContext *proc, uint64_t at);
// sceKernelMprotect / sceKernelMtypeprotect: sets the guest protection (and the memory type unless
// mtype < 0) of the mapped part of [addr, addr+size), splitting extents at the range edges, and
// applies the CPU view of it to the host pages. Returns 0 or an errno: EINVAL for a range that wraps,
// leaves the guest window or maps nothing (a range wholly inside the image/system area, which has
// no extents, succeeds without effect), ENOMEM when bookkeeping memory is unavailable.
int recomp_vm_protect(GuestContext *ctx, uint64_t addr, uint64_t size, int prot, int mtype);
// Copies the extent containing addr into *out (under vm_mutex; next/prev are NULLed) and returns
// true, or returns false when none does. A pointer into the live list would dangle once another
// thread folds or unmaps the extent.
bool recomp_vm_query(GuestContext *ctx, uint64_t addr, GuestVMExtent *out);
// Provably unmapped guest ranges (free extents and untouched reservations), sorted and merged,
// for readers that must not take vm_mutex per query: [out[i][0], out[i][1]). Writes at most max
// ranges and returns how many there are; *gen gets the generation they belong to.
size_t recomp_vm_unmapped_ranges(GuestContext *ctx, uint64_t (*out)[2], size_t max, uint64_t *gen);
// Changes whenever an extent becomes free or mapped or is renamed (recomp_vm_note_change, called
// with vm_mutex held by every such change).
uint64_t recomp_vm_generation(void);
void recomp_vm_note_change(void);

// Observers of guest mappings (the AGC driver's allocation registry, docs/m3/gpu-followups.md). Hooks may be NULL.
// Ranges are guest addresses, page aligned, one call per extent. A hook must not call back into the VM.
//   on_map      after the host pages exist and are final (see recomp_vm_hooks_report_map); vm_mutex HELD: keep it short
//               and non-blocking
//   on_protect  an extent's guest protection changes; vm_mutex HELD; called before the host mprotect
//   on_unmap    BEFORE the host pages are released or reused (munmap, MAP_FIXED over a mapping); vm_mutex NOT held
//               (the extent is busy, so nothing else can touch it) and the hook may block, e.g. wait for the GPU.
// Reservations (no pages) are not reported. Lock order: direct-memory lock -> vm_mutex -> hook table.
typedef struct Nps5VmHooks {
  void (*on_map)(void *user, uint64_t addr, uint64_t size, int prot, int kind, const char *name);
  void (*on_protect)(void *user, uint64_t addr, uint64_t size, int prot, int kind, const char *name);
  void (*on_unmap)(void *user, uint64_t addr, uint64_t size);
  void *user;
  // Optional. Guest [a, a+size) and [b, b+size) are (about to be) backed by the same pages: two direct mappings of one
  // physical range. Called by the direct-memory layer before the on_map of the newer one, with its lock held and
  // without vm_mutex. A page write watch cannot cover such memory (a store through one address does not trip the
  // protection of the other).
  void (*on_alias)(void *user, uint64_t a, uint64_t b, uint64_t size);
} Nps5VmHooks;
// Registers (up to 4) and replays on_map for every extent mapped now, atomically with respect to changes
// (an on_map for a range the hook already has must be accepted). Returns an id >= 0, or -1.
int recomp_vm_hooks_register(const Nps5VmHooks *h);
// on_map for the extents in [addr, addr+size), for a mapping whose pages became final after the VM layer mapped it:
// a direct mapping (NPS5_EXT_DIRECT) is not reported when its extent is created but by the direct-memory layer, once
// the physical block is aliased over it (the alias replaces the host pages).
void recomp_vm_hooks_report_map(GuestContext *ctx, uint64_t addr, uint64_t size);
// on_alias for every hook that has one (see Nps5VmHooks.on_alias).
void recomp_vm_hooks_report_alias(uint64_t a, uint64_t b, uint64_t size);
// Waits for calls in flight, then no further call is made. Must not be called from a hook.
void recomp_vm_hooks_unregister(int id);
// The process context whose extent list the crash snapshot publishes (set by recomp_init_runtime).
extern GuestContext *g_nps5_vm_root;

// Unresolved Symbol Diagnostics & Core Stubs
typedef struct RecompUnresolvedSym {
    uint64_t addr;
    char *nid;
    char *sym_name;
    char *lib_name;
    struct RecompUnresolvedSym *next;
} RecompUnresolvedSym;

void recomp_register_unresolved(uint64_t addr, const char *nid, const char *sym_name, const char *lib_name);
const RecompUnresolvedSym *recomp_lookup_unresolved(uint64_t addr);
void recomp_unimplemented_shim(GuestContext *ctx, const char *shim_name);
void shim_unresolved_stub(GuestContext *ctx);
void recomp_unsupported_instruction(uint64_t rip, uint64_t fn_entry, const char *inst_str);

// Main thread and TLS context helpers
void recomp_init_main_thread(GuestContext *ctx);
GuestContext *recomp_get_current_ctx(void);
void recomp_set_current_ctx(GuestContext *ctx);

// Subsystem teardown and lifecycle
void recomp_free_thread_context(GuestContext *ctx);
void ps4_direct_mem_destroy(void);
void ps4_event_flag_destroy(void);
void ps4_sync_destroy(void);
void ps4_videoout_destroy(void);
void ps4_equeue_destroy(void);
void ps4_metal_screen_destroy(void);
void ps4_host_run_main_loop(void);

// Ends the process from an exit path (window close, Cmd+Q) without running static
// destructors: game threads and Metal completion handlers still use global state.
void ps4_host_quit(int code) __attribute__((noreturn));
// Nonzero once the crash handler runs (it ends the process with exit(1) so the logs flush): exit
// handlers and destructors that would wait for other threads or the GPU skip that work then.
int ps4_host_crashing(void);
// The crash handler (SIGSEGV, SIGBUS, SIGTRAP, SIGILL): installed by recomp_init_runtime; host
// tests install it on their own.
void recomp_install_crash_handler(void);
// Startup settings (libSceSystemService/ps4_settings.m): resolved once before the guest starts.
void ps4_host_settings_init(void);
void ps4_stall_watchdog_note_present(void); // frame stall diagnostics (PS4_STALL_WATCHDOG)
void ps4_stall_watchdog_arm(void);          // starts it at sceVideoOutOpen: the stall counts from the open
int ps4_host_setting_pro_mode(void);
void ps4_keyboard_destroy(void);
void ps4_vfs_destroy(void);
void ps4_aio_destroy(void);
void ps4_gnmdriver_destroy(void);
void ps4_ajm_destroy(void);

// SIMD String helpers: see recomp_context.h

// AES-NI and PCLMULQDQ helpers
xmm_reg_t recomp_pclmulqdq(uint64_t a, uint64_t b);
xmm_reg_t recomp_vaesenc(xmm_reg_t s1, xmm_reg_t s2);
xmm_reg_t recomp_vaesenclast(xmm_reg_t s1, xmm_reg_t s2);
xmm_reg_t recomp_vaesdec(xmm_reg_t s1, xmm_reg_t s2);
xmm_reg_t recomp_vaesdeclast(xmm_reg_t s1, xmm_reg_t s2);
xmm_reg_t recomp_vaesimc(xmm_reg_t s);

#ifdef __cplusplus
}
#endif

#endif // RECOMP_RUNTIME_H
