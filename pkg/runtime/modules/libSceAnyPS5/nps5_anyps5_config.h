// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NPS5_ANYPS5_CONFIG_H
#define NPS5_ANYPS5_CONFIG_H

// 1 when the title was translated with `-gpu anyps5`: the runtime is linked against AnyPS5's arm64
// libraries (docs/m2/anyps5-integration.md). 0 compiles every AnyPS5 file to nothing.
#ifndef NPS5_HAVE_ANYPS5_AGC
#define NPS5_HAVE_ANYPS5_AGC 0
#endif

#include "recomp_context.h"

#if NPS5_HAVE_ANYPS5_AGC
// Called first by every AnyPS5 thunk (NPS5_ANYPS5_TRACE=2 logs the entry). The driver's allocation registry
// is kept by the VM hooks (nps5_anyps5.c), not here.
void nps5_anyps5_enter(GuestContext *ctx, const char *name);
// Last statement of every thunk (NPS5_ANYPS5_TRACE=1 logs the call and its result).
void nps5_anyps5_leave(GuestContext *ctx, const char *name);
// Startup (recomp_init_runtime, after the arena exists): sets the host environment the libraries need
// and keeps the guest address range of the AGC driver's own memory out of the guest allocator.
void nps5_anyps5_startup(GuestContext *ctx);
// Generated (nps5_anyps5_thunks.c): fills the function pointers of the VideoOut/Pad/UserService exports from the
// libnps5_gpu image; returns the number of missing exports.
int nps5_anyps5_bind_exports(void *handle);
int nps5_anyps5_bind_kernel(void); // equeue family from AnyPS5 libkernel (nps5_anyps5_equeue.c)
#endif

#endif
