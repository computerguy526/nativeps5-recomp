// SPDX-License-Identifier: GPL-2.0-or-later
//
// Page watch table of the Kyty GPU seam (docs/m3/kyty-seam.md "16 KiB pages").
//
// Kyty's PageManager tracks 4 KiB pages and wants a host protection per tracker page; an arm64 macOS host page is
// 16 KiB. This table keeps the watch mode of every 4 KiB tracker page (2 bits) and the guest's own protection of every
// host page, and sets each host page to the strictest of its four tracker pages and the guest's protection:
//
//     host protection = guest protection  minus WRITE when any tracker page is watched for writes
//                                         minus READ and WRITE when any tracker page is watched for access
//
// A fault on the host page is offered to the owner for every armed tracker page of that host page (the faulting one
// first, with the real access kind), so the access can always be retried: the price of the coarser host page is that
// up to three neighbours are reported written or read although they were not (counted in the statistics).
//
// Self-contained (no runtime headers) so it is unit-tested alone. Lookups are lock-free (one byte per host page in a
// lazily committed array); updates take one of 64 spin locks chosen by address. The fault path takes no lock while it
// calls the owner. Nothing here allocates after nps5_kyty_watch_init.

#ifndef NPS5_KYTY_WATCH_H
#define NPS5_KYTY_WATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NPS5_KW_NONE 0u   // KYTY_GPU_WATCH_NONE
#define NPS5_KW_WRITE 1u  // KYTY_GPU_WATCH_WRITE
#define NPS5_KW_ACCESS 2u // KYTY_GPU_WATCH_ACCESS

#define NPS5_KW_TRACK_SHIFT 12 // tracker page: 4 KiB
#define NPS5_KW_HOST_SHIFT 14  // host page: 16 KiB

typedef struct Nps5KytyWatchConfig {
  uint64_t guest_lo, guest_hi;                       // watched addresses lie in [guest_lo, guest_hi)
  uint64_t bias;                                     // host = guest + bias
  int (*host_protect)(void *host, size_t len, int prot); // mprotect; 0 or -1
  // The owner's fault entry (KytyGpuApi.handle_fault): a guest address inside a tracker page (the faulting byte for
  // the page that faulted, the page's first byte for a neighbour or a prepared buffer), is_write. Returns 1 when the
  // page was its. It disarms through nps5_kyty_watch_set before it returns.
  int (*handle_fault)(uint64_t guest_page, int is_write);
  // False sharing on the 16 KiB host page (nps5_kyty_emu.h). emulate: 0 = every armed tracker page of the faulting
  // host page that traps the access is reported to the owner (the faulting one with the truth, the neighbours as if
  // they had been accessed); 1 (the seam's default) = a neighbour that is ACCESS-watched is never reported: the
  // trapped instruction is carried out on the alias and the host page stays closed (write-watched neighbours of a
  // store are still reported: that opens the page for good and costs the owner one re-read of an unchanged page);
  // 2 = no neighbour is ever reported. alias_of: an always-accessible view of guest [guest, guest+size) or NULL;
  // called in the signal handler (lock-free). Without it nothing is emulated.
  int emulate;
  void *(*alias_of)(uint64_t guest, uint64_t size);
  // Diagnostics, both optional, both called in the signal handler. on_slow: a handle_fault call took longer than
  // slow_ns (kind as in Nps5KytyWatchStats.kind_calls). on_declined: an access next to an access-watched page that the
  // emulator could not carry out (ucontext of the fault).
  uint64_t slow_ns;
  void (*on_slow)(uint64_t guest, int is_write, unsigned kind, uint64_t ns);
  void (*on_declined)(void *ucontext, uint64_t guest, int is_write);
} Nps5KytyWatchConfig;

typedef struct Nps5KytyWatchStats {
  uint64_t sets;             // nps5_kyty_watch_set calls
  uint64_t host_protects;    // host protection changes made (calls of host_protect)
  uint64_t faults;           // faults claimed
  uint64_t fault_pages;      // tracker pages offered to the owner for those faults
  uint64_t collateral_pages; // of those, pages other than the faulting one (false reports caused by 16 KiB pages)
  uint64_t unresolved;       // faults on an armed page that stayed armed after the owner ran
  uint64_t prepares;         // tracker pages opened by write_begin / read_prepare
  uint64_t write_faults;     // of `faults`: stores
  uint64_t raced;            // faults on a page with no armed tracker page that were retried (raced disarm)
  uint64_t owner_ns;         // time inside the owner's handle_fault for `fault_pages` (mach_absolute_time based)
  uint64_t owner_max_ns;     // the longest single handle_fault call
  uint64_t slow_calls;       // handle_fault calls that took longer than 100 us
  // handle_fault calls and time by what the page was: [0] the faulting page, write-watched; [1] the faulting page,
  // access-watched; [2] a neighbour (collateral), write-watched; [3] a neighbour, access-watched.
  uint64_t kind_calls[4], kind_ns[4];
  uint64_t emulated;         // trapped accesses to an unwatched tracker page carried out on the alias (the owner not called)
  uint64_t loads_served;     // loads the owner completed with its own bytes (nps5_kyty_watch_fault_load_apply)
  uint64_t emu_declined;     // such accesses the emulator could not carry out (instruction not covered, no alias): reported the old way
  uint64_t straddles;        // such accesses that reach into an armed tracker page: that page was offered as the faulting one
} Nps5KytyWatchStats;

// 0, or -1 when the state arrays cannot be reserved. Call once before any other function.
int nps5_kyty_watch_init(const Nps5KytyWatchConfig *cfg);
bool nps5_kyty_watch_ready(void);

// Kyty's protect_guest: watch mode of the tracker pages in guest [addr, addr+size). 0, or -1 for a range outside
// the guest window or not a multiple of 4 KiB.
int nps5_kyty_watch_set(uint64_t guest, uint64_t size, unsigned watch);
// The runtime changes the guest's own protection of host pages (Nps5HostAccessHooks.protect: replaces mprotect).
// prot is PROT_READ|PROT_WRITE bits. Returns 0 or an errno value.
int nps5_kyty_watch_guest_protect(void *host, size_t len, int prot);
// The range is unmapped or no longer GPU-visible: every watch in it is dropped without telling the owner, the guest
// protection record is reset (read-write) and the host protection is NOT touched (the pages are about to change).
void nps5_kyty_watch_forget(uint64_t guest, uint64_t size);

// Fault on host address `host` (signal handler). True when a watch on that host page restricted the access and the
// owner was run for every armed tracker page: the access is retried. False: not a watch fault.
bool nps5_kyty_watch_fault(uint64_t host, int is_write);
// The same with the thread's ucontext_t, which allows the emulation above. Returns 0 not a watch fault, 1 handled:
// retry the access, 2 handled: the access was carried out and the thread continues behind the instruction.
int nps5_kyty_watch_fault_ctx(void *ucontext, uint64_t host, int is_write);
// For the owner, only inside its handle_fault for the FAULTING tracker page of a load (the library can then serve
// the load from bytes it has, e.g. the page's last published copy, instead of waiting for the GPU and opening the
// page): what the trapped instruction loads (guest address, 1..16 bytes); 0 when it is not a covered load or there
// is no instruction to complete (a prepared system-call buffer, a neighbour's report).
int nps5_kyty_watch_fault_load(uint64_t *guest, uint32_t *size);
// Completes that load with `bytes` as the loaded memory and lets the thread continue behind it; the page stays armed.
// 1 when done (handle_fault then just returns), 0 when refused (wrong size, not inside such a fault).
int nps5_kyty_watch_fault_load_apply(const void *bytes, uint32_t size);

// Before host code or a system call writes (is_write) or reads host [host, host+len) without being able to fault into
// the handler: runs the owner for every armed tracker page that would trap. A range with no armed page costs one byte
// compare per 16 KiB.
void nps5_kyty_watch_prepare(const void *host, size_t len, int is_write);

// The watch mode of the tracker page holding guest address g, and the host protection the table last set for its
// host page (PROT_* bits). Tests and diagnostics.
unsigned nps5_kyty_watch_get(uint64_t guest);
int nps5_kyty_watch_host_prot(uint64_t guest);
void nps5_kyty_watch_stats(Nps5KytyWatchStats *out);
// Releases the arrays (tests).
void nps5_kyty_watch_shutdown(void);

#endif
