// SPDX-License-Identifier: GPL-2.0-or-later
//
// Native allocator behind the function-level HLE of Unreal Engine's FMallocBinned3
// (docs/m3/function-hle.md). It hands out GUEST addresses inside memory it gets from a provider
// (the runtime's VM manager in the title, an mmap arena in the unit test), so a returned pointer
// is an ordinary guest pointer: host = guest + bias.
//
// Design: thread caches over per-class central lists over 64 KiB-aligned spans carved from 256 MiB
// slabs (blocks up to 128 KiB, 48 size classes, no per-block header), and one mapping per block
// above that, with a small cache of recently freed ones. Thread safe; no allocation from the
// guest-visible heap for metadata (everything else lives in host memory).

#ifndef NPS5_UEMALLOC_CORE_H
#define NPS5_UEMALLOC_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct UemProvider {
  // Maps size bytes of zero-filled read-write guest memory aligned to align (a power of two, at
  // least 16 KiB); returns the guest address, or 0 when there is none.
  uint64_t (*map)(void *user, size_t size, size_t align);
  // Gives a mapping from map() back (the whole mapping).
  void (*unmap)(void *user, uint64_t addr, size_t size);
  void *user;
  uint8_t *bias; // host address of guest address 0 (host = bias + guest)
} UemProvider;

// Size-class geometry (also what UE's QuantizeSize / GetAllocationSize observe).
#define UEM_MAX_SMALL 0x20000u // largest block served from spans (the same limit FMallocBinned3 uses)
#define UEM_MIN_ALIGN 16u
#define UEM_MAX_SMALL_ALIGN 0x10000u // larger alignments get their own mapping
#define UEM_LARGE_GRANULE 0x4000u    // large blocks are rounded to the 16 KiB guest page

typedef struct UemStats {
  uint64_t allocs, frees, reallocs;                 // calls (not bytes)
  uint64_t small_allocs, large_allocs;              // served from spans / own mapping
  uint64_t tcache_hits;                             // small allocations served by the thread cache
  uint64_t slab_bytes;                              // address space taken for spans
  uint64_t large_live_bytes, large_cached_bytes;    // mapped for live large blocks / kept in the cache
  uint64_t unknown_frees;                           // free() of a pointer this allocator does not own
} UemStats;

// One-time setup (idempotent: the first call wins). Returns 0, or -1 when the metadata cannot be
// reserved.
int uem_init(const UemProvider *p);

// Allocates size bytes (0 is served as 1) aligned to align (0 = 16; rounded up to a power of
// two). Returns the guest address, or 0 when out of memory.
uint64_t uem_alloc(size_t size, size_t align);

// UE realloc semantics: p == 0 allocates; size == 0 frees p and returns 0 with *oom = 0. On out of
// memory returns 0 with *oom = 1 and p stays valid. A block that already is large enough (same
// size class / within a quarter of a large mapping) and suitably aligned is returned as is.
uint64_t uem_realloc(uint64_t p, size_t size, size_t align, int *oom);

// Frees p (0 is a no-op). Returns 0, or -1 when p is not a block of this allocator (nothing is
// touched, the unknown_frees counter is bumped).
int uem_free(uint64_t p);

// The size a request of (size, align) really gets: what uem_usable_size() reports for the block
// uem_alloc(size, align) returns. Out of range requests return size.
size_t uem_quantize(size_t size, size_t align);

// Usable size of the block at p. Returns 0 and stores it, or -1 when p is not one of ours.
int uem_usable_size(uint64_t p, size_t *out);

// Returns the calling thread's cached blocks to the central lists (thread exit does it
// automatically).
void uem_thread_flush(void);

// Fills freed small blocks with 0xDD (after the free-list link) so use-after-free shows up. Off
// by default; NPS5_UEMALLOC_POISON=1 turns it on in the runtime.
void uem_set_poison(int on);

void uem_stats(UemStats *out);

#ifdef __cplusplus
}
#endif
#endif
