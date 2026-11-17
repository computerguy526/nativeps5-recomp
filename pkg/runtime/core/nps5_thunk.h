// SPDX-License-Identifier: GPL-2.0-or-later
//
// Guest <-> host pointer translation at the HLE boundary (docs/m1/memory.md "HLE pointer
// translation"). Used by the generated thunks (pkg/thunkgen) and by HLE code that has to turn a
// host pointer back into the guest address it stands for.
//
//   base mode      host = guest + bias, with the guest's NULL staying NULL. One add, one compare
//                  and one conditional select per pointer. A guest value at or above the end of
//                  the guest window (ctx->mem_size) translates to NULL: it can never be a valid
//                  guest pointer, and adding the bias would give a host address outside the
//                  reservation (the HLE would read or write somebody else's memory). The HLE then
//                  sees a NULL pointer, which every checked HLE function already refuses.
//   identity mode  host == guest. NPS5_ADDR_MODE_IDENTITY=1 makes every macro a cast: thunks
//                  and HLE code compile to what they would be without any translation layer.

#ifndef NPS5_THUNK_H
#define NPS5_THUNK_H

#include "recomp_context.h"
#include "nps5_mem.h"

// NPS5_CHECKED_PTR=1 (debug builds, NPS5_CHECKED_PTR=1 in the environment of `nps5 build`): every
// guest pointer the thunks translate must be below the end of the guest window; a wild one (at or
// above 2^40, or a "negative" ptr-8 of NULL) stops the process with the pointer named (review 1,
// m5). Without it, translation maps such a pointer to NULL (below) instead of stopping.
#ifndef NPS5_CHECKED_PTR
#define NPS5_CHECKED_PTR 0
#endif
#if NPS5_CHECKED_PTR
// Never returns (nps5_mem.c).
void nps5_bad_guest_pointer(uint64_t g) __attribute__((noreturn, cold));
#define NPS5_PTR_CHECK(g) \
  do { if (__builtin_expect((g) >= g_nps5_arena.guest_hi, 0)) nps5_bad_guest_pointer(g); } while (0)
#else
#define NPS5_PTR_CHECK(g) ((void)0)
#endif

#if NPS5_ADDR_MODE_IDENTITY

static inline void *nps5_g2h(const GuestContext *ctx, uint64_t g) {
  (void)ctx;
  NPS5_PTR_CHECK(g);
  return (void *)(uintptr_t)g;
}
static inline uint64_t nps5_h2g(const GuestContext *ctx, const void *h) {
  (void)ctx;
  return (uint64_t)(uintptr_t)h;
}
#if NPS5_CHECKED_PTR
#define NPS5_G2H(ctx, g) nps5_g2h((ctx), (uint64_t)(g))
#else
#define NPS5_G2H(ctx, g) ((void *)(uintptr_t)(g))
#endif
#define NPS5_H2G(ctx, h) ((uint64_t)(uintptr_t)(h))

#else

static inline void *nps5_g2h(const GuestContext *ctx, uint64_t g) {
  NPS5_PTR_CHECK(g);
  return g && g < ctx->mem_size ? (void *)(ctx->mem_base + g) : NULL;
}
static inline uint64_t nps5_h2g(const GuestContext *ctx, const void *h) {
  return h ? (uint64_t)((const uint8_t *)h - ctx->mem_base) : 0;
}
#define NPS5_G2H(ctx, g) nps5_g2h((ctx), (uint64_t)(g))
#define NPS5_H2G(ctx, h) nps5_h2g((ctx), (h))

#endif

// What a generated thunk passes for a StructArray argument whose guest array cannot be read: not a
// valid pointer, only a marker the callee compares against (it fails with EFAULT).
#define NPS5_BAD_GUEST_ARRAY ((void *)(uintptr_t)1)

// The n-th System V stack argument of the guest call in progress: [rsp] is the return address.
#define NPS5_GUEST_STACK_ARG(ctx, n) (*(const uint64_t *)((ctx)->mem_base + (ctx)->rsp + 8 + 8 * (uint64_t)(n)))

#endif
