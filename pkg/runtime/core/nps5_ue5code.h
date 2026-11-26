// SPDX-License-Identifier: GPL-2.0-or-later
// UE5 shader library code source (see nps5_ue5code.c).
#ifndef NPS5_UE5CODE_H
#define NPS5_UE5CODE_H

#include "../modules/libSceAnyPS5/kytyGpuApi.h"
#include "recomp_context.h"
#include <stdint.h>

// A guest context owned by host code (own guest stack of stack_bytes, the process's memory and TLS template), for guest
// functions called from a host thread (UE5 shader library decoding). Free with nps5_hostctx_destroy.
GuestContext *nps5_hostctx_create(GuestContext *process, uint64_t stack_bytes);
void nps5_hostctx_destroy(GuestContext *ctx);

// The guest process the host-owned contexts and guest scratch belong to (set once at start-up; the first call wins).
void nps5_ue5_set_process(GuestContext *process);

// ABI 1.8 extract_ue5_shader_groups on the title's own decoder (guest vaddr q->decoder, abi 1 = oodlelz14): see kytyGpuApi.h.
// Callable from any host thread. -1 when the process is not registered, abi unknown or there is no IoStore.
int nps5_ue5_extract_groups(const KytyGpuUe5Query *q, KytyGpuUe5Result *out);

// UE5 code-source spike (WP0): NPS5_UE5_SELFTEST=<decoder hex vaddr>:<compressed file>:<raw bytes>:<out file> decodes one group
// with the guest's OodleLZ_Decompress (abi oodlelz14) on a host-owned context before the guest entry, writes the decoded bytes,
// logs "[ue5code] selftest ..." and exits (0 = decoded size matched). Off when unset.
void nps5_ue5_selftest_maybe(GuestContext *process);

#endif
