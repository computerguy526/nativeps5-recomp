// SPDX-License-Identifier: GPL-2.0-or-later
//
// Guest memory model (docs/m1/memory.md).
//
// One host reservation holds the whole guest address space. Two modes, chosen per title at
// translate time and recorded in memory_manifest.json:
//
//   base      host = guest + bias. The reservation is 2^40 bytes at a multiple of 2^40 (2^40
//             preferred), so every 40-bit guest address has a host page the runtime owns, also
//             guest ranges macOS itself occupies (dyld shared cache at 0x1_8000_0000.., GPU
//             carveout 0x10_0000_0000-0x70_0000_0000). The bias is a run-time value (x28).
//   identity  host == guest. The reservation is exactly 0x70_0000_0000-0xFC_0000_0000 and the
//             guest layout is shifted into it at translate time. Pointer translation compiles
//             to nothing (NPS5_ADDR_MODE_IDENTITY).
//
// The reservation is made PROT_NONE with a non-destructive fixed mapping: an occupied range is
// an error that lists the host regions in the way, never an overwrite. Guest pages become
// accessible only when the guest layout or a guest mapping call commits them, and return to
// PROT_NONE zero-fill when unmapped. Guest and host pages are both 16 KiB.

#ifndef NPS5_MEM_H
#define NPS5_MEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPS5_MEM_ABI 1
#define NPS5_PAGE 0x4000ULL
#define NPS5_PAGE_MASK (NPS5_PAGE - 1)

enum { NPS5_ADDR_BASE = 0, NPS5_ADDR_IDENTITY = 1 };

// Host ranges with fixed meaning on arm64 macOS (measured, docs/m0/S1.md).
#define NPS5_IDENTITY_LO 0x7000000000ULL
#define NPS5_IDENTITY_HI 0xFC00000000ULL
#define NPS5_BASE_SPAN 0x10000000000ULL // 2^40: the guest address space in base mode
#define NPS5_HOST_CARVEOUT_LO 0x1000000000ULL
#define NPS5_HOST_CARVEOUT_HI 0x7000000000ULL

// The translate-time memory model of a title. The generated main.c defines nps5_mem_config;
// host tests get the weak default (base mode, console layout).
typedef struct Nps5MemConfig {
  uint32_t abi;       // NPS5_MEM_ABI
  uint32_t addr_mode; // NPS5_ADDR_BASE or NPS5_ADDR_IDENTITY
  uint64_t host_base; // preferred bias (base mode); 0 in identity mode
  uint64_t guest_lo;  // first valid guest address; [origin, guest_lo) is the null guard
  uint64_t guest_hi;  // end of the guest address space
  uint64_t image_end; // end of the static images (page aligned); system area starts here
  uint64_t dyn_lo;    // lowest address handed to an unhinted guest mapping
  uint64_t pool_lo;   // user area: memory pools and other large reservations
  uint64_t stack_top; // top of the main thread stack
  uint64_t stack_len;
} Nps5MemConfig;

extern const Nps5MemConfig nps5_mem_config;

// The arena of this process. Written once by nps5_mem_reserve before any guest thread exists;
// read without locking afterwards (also from the crash handler).
typedef struct Nps5Arena {
  uint8_t *bias;      // host = bias + guest; NULL in identity mode
  uint64_t host_lo;   // host start of the reservation
  uint64_t host_len;  // its length; 0 until reserved
  uint64_t guest_org; // guest address of host_lo (0 in base mode, NPS5_IDENTITY_LO in identity)
  uint64_t guest_lo;  // first valid guest address
  uint64_t guest_hi;  // end of valid guest addresses
  uint32_t mode;
  bool flat; // host-test arena: [0, guest_hi) stays read-write, nothing is ever decommitted
} Nps5Arena;

extern Nps5Arena g_nps5_arena;

// Reserves the arena for cfg. flat_size != 0 asks for the host-test arena: guest [0, flat_size)
// committed read-write up front (the layout PS4Recomp's unit tests were written against).
// Returns 0, or -1 with a message in err that names every host region in the way.
int nps5_mem_reserve(const Nps5MemConfig *cfg, uint64_t flat_size, char *err, size_t errlen);
void nps5_mem_release(void);

// Commit/decommit guest [addr, addr+len), widened to whole 16 KiB pages. Both refuse any range
// that is not inside the reservation, so they can never touch memory the runtime does not own.
// Commit makes the pages read-write (guest protection is bookkeeping in the VM extent list;
// see memory.md "Protection"). Decommit drops the pages and any alias mapped over them and
// leaves PROT_NONE zero-fill behind.
int nps5_mem_commit(uint64_t addr, uint64_t len);
int nps5_mem_decommit(uint64_t addr, uint64_t len);
// Host protection for guest [addr, addr+len), shrunk to the whole pages inside it.
int nps5_mem_protect_inner(uint64_t addr, uint64_t len, int prot);

// True when [addr, addr+len) is inside the valid guest window.
static inline bool nps5_guest_range_ok(uint64_t addr, uint64_t len) {
  return addr >= g_nps5_arena.guest_lo && addr <= g_nps5_arena.guest_hi && len <= g_nps5_arena.guest_hi - addr;
}

// ---- Fault classification (§3.10): host address -> guest address and what lives there ----
typedef enum {
  NPS5_FA_NOT_GUEST = 0,   // outside the reservation
  NPS5_FA_NULL_GUARD,      // guest [origin, guest_lo): a null or near-null guest pointer
  NPS5_FA_IMAGE,           // inside a loaded image region
  NPS5_FA_SYSTEM,          // main thread TLS/TCB/args block
  NPS5_FA_STACK_GUARD,     // guard page below a guest stack
  NPS5_FA_STACK,           // main thread stack
  NPS5_FA_UNMAPPED,        // valid guest address, nothing mapped there now
  NPS5_FA_MAPPED,          // inside a guest mapping (protection or racing unmap)
  NPS5_FA_BEYOND,          // inside the reservation but past guest_hi
} Nps5FaultClass;

typedef struct Nps5FaultInfo {
  Nps5FaultClass cls;
  uint64_t guest;  // guest address (valid unless NOT_GUEST)
  uint64_t lo, hi; // the classified range, when known
  char name[32];   // mapping name, when known
  bool truncated;  // cls is UNMAPPED but the mapping snapshot is incomplete (out of memory): it may be mapped
} Nps5FaultInfo;

// Async-signal-safe: reads tables published with atomics, takes no lock, does not allocate.
void nps5_mem_classify(uint64_t host_addr, Nps5FaultInfo *out);
const char *nps5_fault_class_name(Nps5FaultClass c);

// Copies len bytes from host address `host` into dst without faulting: false when any part of the
// source is unmapped or inaccessible. Async-signal-safe in practice (one Mach call on the own
// task); the crash handler uses it to read guest memory that may be a guard page (a stack
// overflow leaves rsp there: a plain load would re-fault inside the handler, with the signal
// blocked, and the thread would spin forever), HLE code uses it for guest arrays it must not crash on.
bool nps5_mem_safe_read(const void *host, void *dst, size_t len);
// The same for guest address g (translated through the arena): false outside the guest window.
bool nps5_guest_safe_read(uint64_t g, void *dst, size_t len);
// True when every byte of host [p, p+len) is mapped and readable now (no copy). A check, not a lease: pair it with a bound
// on how long the range is used, or prefer the copying helpers above.
bool nps5_mem_range_readable(const void *host, size_t len);

// ---- Host access hooks (docs/m3/writewatch.md) -------------------------------------------------------------------
// A host library may keep guest pages write-protected or inaccessible on the host to learn which pages the guest
// writes or reads (AnyPS5's AGC write watch: page write watch and read traps). CPU accesses from guest code and host C
// code fault and are resolved in the signal handler (nps5_diag access resolvers). Everything else goes through these:
//  - protect: every host protection change of committed guest pages (it replaces mprotect, so the library can keep
//    its own protection on top of the guest's and record the guest's);
//  - write_begin/write_end: around a write into guest memory that cannot fault into the handler: a system call that
//    fills a guest buffer (read, pread, recv ...: it would fail with EFAULT on a protected page) or a library that
//    writes from threads of its own. The pages are open (writable, recorded as written) between the two calls;
//  - read_prepare: before a read of guest memory that cannot fault into the handler (write, send ...).
// All four are optional. Not async-signal-safe: never call them from a signal handler.
typedef struct Nps5HostAccessHooks {
  int (*protect)(void *host, size_t len, int prot); // 0 or an errno value
  void (*write_begin)(void *host, size_t len);
  void (*write_end)(void *host, size_t len);
  void (*read_prepare)(const void *host, size_t len);
} Nps5HostAccessHooks;
// Installs the hooks (NULL removes them). Call before guest threads run; the table is copied.
void nps5_mem_set_host_access_hooks(const Nps5HostAccessHooks *hooks);
// The bracket for kernel-mode and foreign-thread writes into host [p, p+len). No-ops without hooks and for memory
// outside the guest reservation (host stacks, heap). len 0 is a no-op. Calls may nest and overlap.
void nps5_host_write_begin(void *p, size_t len);
void nps5_host_write_end(void *p, size_t len);
// A kernel-mode write into host [p, p+len) failed with EFAULT although nps5_host_write_begin had opened it: the library
// closed a page again before the kernel copied (it re-arms its watches from its own threads at any time, e.g. once a
// frame for a buffer the GPU reads; a file read that waits for the disk leaves it milliseconds to do so). Opens the
// range again and returns true when the caller should repeat the call (bounded by *tries, which the caller starts at
// 0); false = not ours (no hooks, memory outside the guest reservation, or still failing after NPS5_KIO_RETRIES
// attempts: a real bad pointer). Unreported, such a failure handed the title an error or a short read for a valid
// buffer and it went on with unwritten memory (a title crashed with SIGBUS on its world map that way).
#define NPS5_KIO_RETRIES 64u
bool nps5_host_write_refault(void *p, size_t len, unsigned *tries);
// Before a kernel-mode read of host [p, p+len) (the source buffer of write/send/...).
void nps5_host_read_prepare(const void *p, size_t len);
// Whether hooks are installed (and this thread is not reporting a crash).
bool nps5_host_access_active(void);
// mprotect for host pages of the guest reservation, through the protect hook when one is installed. 0 or -1 (errno).
int nps5_mem_host_protect(void *host, size_t len, int prot);
// Set (per thread) while the crash handler reports a fault: the helpers above and nps5_mem_safe_read then never call hooks.
extern _Thread_local int g_nps5_in_crash_handler;

// Host protection for the pages of loaded images inside guest [lo, hi) (the guest's own mprotect of
// its code or data); pages outside every noted image range are left alone. Returns bytes changed.
uint64_t nps5_mem_protect_image(uint64_t lo, uint64_t hi, int prot);

// Image regions and fixed layout ranges, recorded at load for classification and the map dump.
void nps5_mem_note_range(Nps5FaultClass cls, uint64_t lo, uint64_t hi, const char *name);
// What an extent of the guest address space is. Set when the extent is created and never changed
// by sceKernelSetVirtualRangeName or anything else the guest controls; the VM manager's
// semantics (reservations, pinned stack, GPU unmapped ranges, fault classes) key on this.
enum {
  NPS5_EXT_MAPPED = 0,   // ordinary mapping (mmap, runtime objects)
  NPS5_EXT_RESERVED,     // sceKernelReserveVirtualRange: address space only, no pages
  NPS5_EXT_POOL,         // sceKernelMemoryPoolReserve
  NPS5_EXT_FLEXIBLE,     // sceKernelMap(Named)FlexibleMemory
  NPS5_EXT_DIRECT,       // sceKernelMapDirectMemory
  NPS5_EXT_MAIN_STACK,   // pinned: never unmapped or replaced
  NPS5_EXT_STACK_GUARD,  // pinned: the main stack's guard page
  NPS5_EXT_THREAD_STACK, // guard page + stack + TLS (first page is PROT_NONE)
};
// A compact snapshot of guest mappings for the crash handler, republished by the VM extent
// manager after every change (recomp_vm_note_change).
// Writer side (one writer at a time: the caller holds the VM mutex): begin, add every mapping
// in ascending order, end.
void nps5_mem_map_begin(void);
// kind is the extent's NPS5_EXT_* kind (immutable, unlike the guest-settable name): it, not the
// name, decides the stack guard classification. Adjacent entries of the same kind and name are
// merged, except thread stacks and guards, which stay distinct (each has its own guard page).
void nps5_mem_map_add(uint64_t lo, uint64_t hi, const char *name, int kind);
void nps5_mem_map_end(void);

// Writes the arena map (reservation, fixed ranges, guest mappings) to fd. Used for
// NPS5_DUMP_MAP=1 at exit and SIGUSR1.
void nps5_mem_dump_map(int fd);

#ifdef __cplusplus
}
#endif

#endif
