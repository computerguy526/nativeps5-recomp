// SPDX-License-Identifier: GPL-2.0-or-later
//
// NPS5_GPU=kyty: KytyPS5's GPU stack (kyty_gpu.dylib) in place of AnyPS5's libSceAgc / libSceAgcDriver / libSceVideoOut
// (docs/m3/kyty-seam.md). Off by default. Off costs nothing per call: the routable thunks call through a function
// pointer that is statically initialised with the AnyPS5 function (the same load-and-branch a dylib call is).

#ifndef NPS5_KYTY_H
#define NPS5_KYTY_H

#include <stdbool.h>
#include <stdint.h>

#include "recomp_context.h"

// One routable export (generated: nps5_anyps5_thunks.c, from pkg/thunkgen/kyty_nids.tsv).
typedef struct Nps5KytyRoute {
  const char *nid;     // "Yw0jKSqop+E"
  const char *name;    // AnyPS5 symbol the thunk calls by default, "sceAgcDcbDrawIndexAuto"
  const char *library; // "libSceAgc.prx"
  void **slot;         // the thunk's function pointer
  void *raw;           // adapter with the slot's prototype that calls the kyty entry with guest (untranslated) pointers
} Nps5KytyRoute;
extern const Nps5KytyRoute nps5_kyty_routes[];
extern const unsigned nps5_kyty_route_count;

// The kyty entry of route `index` (KytyGpuApi.nid_entry / dlsym). Never returns NULL: a NID the library does not
// implement stops the process with the NID and name (as AstroHybrid's trap slots do).
void *nps5_kyty_entry(unsigned index);
// The guest context of the calling thread (inside a routed thunk).
GuestContext *nps5_kyty_ctx(void);

// Reads NPS5_GPU. With "kyty": loads the library, hands it the host callbacks and guest memory, binds the routes and
// returns true; the caller then leaves AnyPS5's driver glue (allocation registry, write watch) uninstalled. Anything
// else: returns false and does nothing. `gpu_image` is the dlopen handle of libnps5_gpu.dylib (event queue, pad and
// exit helpers of the host side). A failure with NPS5_GPU=kyty set is fatal (no silent fallback to another renderer).
bool nps5_kyty_startup(GuestContext *ctx, void *gpu_image);
bool nps5_kyty_active(void);

#endif
