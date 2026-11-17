// SPDX-License-Identifier: GPL-2.0-or-later
//
// Event queues in `-gpu anyps5` mode. AnyPS5's VideoOut, AGC driver and Pad register their flip/vblank/
// completion events on queues they own (EqueuePin on the handle), so the queue the title creates with
// sceKernelCreateEqueue must be AnyPS5's, not the PS4Recomp one (whose small integer handles AnyPS5 rejects with
// VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE). These shims replace the runtime's equeue shims for the create/wait/user-event
// family and call the exports of AnyPS5's libkernel through dlsym (the runtime itself defines functions with the
// same plain names). The handle is 64-bit: (id << 32) | descriptor, written whole into the guest's SceKernelEqueue.
// GetEvent* accessors stay with the runtime: both sides use the kevent-compatible 32-byte event layout.
#include "nps5_anyps5_config.h"
#if NPS5_HAVE_ANYPS5_AGC
#include "nps5_thunk.h"
#include <dlfcn.h>
#include <stdio.h>

static int (*k_create)(int64_t *eq, const char *name);
static int (*k_delete)(int64_t eq);
static int (*k_wait)(int64_t eq, void *ev, int num, int *out, const uint32_t *timeout);
static int (*k_add_user)(int64_t eq, int id);
static int (*k_add_user_edge)(int64_t eq, int id);
static int (*k_trigger_user)(int64_t eq, int id, void *udata);
static int (*k_delete_user)(int64_t eq, int id);
static int (*k_add_ampr)(int64_t eq, int id, void *udata);
static int (*k_delete_ampr)(int64_t eq, int id);
static int (*k_trigger)(int64_t eq, uintptr_t ident, int16_t filter, void *data);
extern int (*nps5_ampr_ext_post)(int64_t eq, uint64_t ident, int64_t data);
extern int nps5_ampr_note_external_event(int64_t eq64, uint64_t ident, uint64_t udata);
extern void nps5_ampr_remove_external_event(int64_t eq64, uint64_t ident);

static int ampr_post(int64_t eq, uint64_t ident, int64_t data) {
  return k_trigger(eq, (uintptr_t)ident, -25 /* EVFILT_AMPR */, (void *)(intptr_t)data);
}

int nps5_anyps5_bind_kernel(void) {
  void *k = dlopen("@rpath/libkernel.prx", RTLD_NOW | RTLD_NOLOAD);
  if (!k) k = dlopen("@rpath/libkernel.prx", RTLD_NOW | RTLD_LOCAL);
  if (!k) {
    fprintf(stderr, "[anyps5] cannot open AnyPS5 libkernel: %s\n", dlerror());
    return 1;
  }
  int missing = 0;
#define BIND(fp, name) \
  if (!(*(void **)&(fp) = dlsym(k, name))) { fprintf(stderr, "[anyps5] missing export %s\n", name); missing++; }
  BIND(k_create, "sceKernelCreateEqueue")
  BIND(k_delete, "sceKernelDeleteEqueue")
  BIND(k_wait, "sceKernelWaitEqueue")
  BIND(k_add_user, "sceKernelAddUserEvent")
  BIND(k_add_user_edge, "sceKernelAddUserEventEdge")
  BIND(k_trigger_user, "sceKernelTriggerUserEvent")
  BIND(k_delete_user, "sceKernelDeleteUserEvent")
  BIND(k_add_ampr, "sceKernelAddAmprEvent")
  BIND(k_delete_ampr, "sceKernelDeleteAmprEvent")
  BIND(k_trigger, "EqueueTriggerEvent_nid_postfix")
#undef BIND
  if (k_trigger) nps5_ampr_ext_post = ampr_post;
  return missing;
}

void shim_aps5_sceKernelCreateEqueue(GuestContext *ctx) {
  int64_t eq = 0;
  const char *name = ctx->rsi ? (const char *)NPS5_G2H(ctx, ctx->rsi) : NULL;
  int rc = k_create(&eq, name);
  if (rc == 0 && ctx->rdi) *(int64_t *)NPS5_G2H(ctx, ctx->rdi) = eq;
  ctx->rax = (uint64_t)(int64_t)rc;
  SHIM_RETURN();
}

void shim_aps5_sceKernelDeleteEqueue(GuestContext *ctx) {
  ctx->rax = (uint64_t)(int64_t)k_delete((int64_t)ctx->rdi);
  SHIM_RETURN();
}

void shim_aps5_sceKernelWaitEqueue(GuestContext *ctx) {
  void *ev = ctx->rsi ? NPS5_G2H(ctx, ctx->rsi) : NULL;
  int *out = ctx->rcx ? (int *)NPS5_G2H(ctx, ctx->rcx) : NULL;
  const uint32_t *timeout = ctx->r8 ? (const uint32_t *)NPS5_G2H(ctx, ctx->r8) : NULL;
  ctx->rax = (uint64_t)(int64_t)k_wait((int64_t)ctx->rdi, ev, (int)ctx->rdx, out, timeout);
  SHIM_RETURN();
}

void shim_aps5_sceKernelAddUserEvent(GuestContext *ctx) {
  ctx->rax = (uint64_t)(int64_t)k_add_user((int64_t)ctx->rdi, (int)ctx->rsi);
  SHIM_RETURN();
}

void shim_aps5_sceKernelAddUserEventEdge(GuestContext *ctx) {
  ctx->rax = (uint64_t)(int64_t)k_add_user_edge((int64_t)ctx->rdi, (int)ctx->rsi);
  SHIM_RETURN();
}

void shim_aps5_sceKernelTriggerUserEvent(GuestContext *ctx) {
  // udata is an opaque guest value, copied back verbatim into the event.
  ctx->rax = (uint64_t)(int64_t)k_trigger_user((int64_t)ctx->rdi, (int)ctx->rsi, (void *)(uintptr_t)ctx->rdx);
  SHIM_RETURN();
}

void shim_aps5_sceKernelDeleteUserEvent(GuestContext *ctx) {
  ctx->rax = (uint64_t)(int64_t)k_delete_user((int64_t)ctx->rdi, (int)ctx->rsi);
  SHIM_RETURN();
}

void shim_aps5_sceKernelAddAmprEvent(GuestContext *ctx) {
  if (!k_add_ampr) { ctx->rax = 0x80020016u; SHIM_RETURN(); } // EINVAL: AnyPS5's libkernel lacks the export
  int rc = k_add_ampr((int64_t)ctx->rdi, (int)ctx->rsi, (void *)(uintptr_t)ctx->rdx);
  if (rc == 0 && nps5_ampr_note_external_event((int64_t)ctx->rdi, (uint32_t)ctx->rsi, ctx->rdx) != 0) {
    // the runtime cannot remember the event: undo it, and fail (a silent success would lose its completions)
    if (k_delete_ampr) k_delete_ampr((int64_t)ctx->rdi, (int)ctx->rsi);
    rc = (int)0x8002000C; // ENOMEM
  }
  ctx->rax = (uint64_t)(int64_t)rc;
  SHIM_RETURN();
}

void shim_aps5_sceKernelDeleteAmprEvent(GuestContext *ctx) {
  if (!k_delete_ampr) { ctx->rax = 0x80020016u; SHIM_RETURN(); }
  nps5_ampr_remove_external_event((int64_t)ctx->rdi, (uint32_t)ctx->rsi);
  ctx->rax = (uint64_t)(int64_t)k_delete_ampr((int64_t)ctx->rdi, (int)ctx->rsi);
  SHIM_RETURN();
}

#endif // NPS5_HAVE_ANYPS5_AGC
