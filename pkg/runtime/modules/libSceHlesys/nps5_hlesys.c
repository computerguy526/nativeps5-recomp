// SPDX-License-Identifier: GPL-2.0-or-later
//
// Small system-library entry points a PS5 title imports at boot (m2-hlesys), each either a real
// (if trivial) implementation or a stub that logs once and returns a documented error. Values
// and error codes follow AnyPS5 (core/libs/prx/{libkernel,libScePad}; GPL-2.0-only) where it has
// evidence for them. (UserService / SystemService / PlayGo / Acm entry points of the title are in
// m2-hlestubs' libSceNpOnlineStubs.)

#include "nps5_diag.h"
#include "nps5_thunk.h"
#include "recomp_runtime.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RET32(ctx, v) do { (ctx)->rax = (uint64_t)(int64_t)(int32_t)(v); SHIM_RETURN(); } while (0)

static void hs_once(const char *name, const char *what) {
  static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
  static char seen[48][64];
  static int n;
  pthread_mutex_lock(&lk);
  for (int i = 0; i < n; i++) {
    if (!strcmp(seen[i], name)) { pthread_mutex_unlock(&lk); return; }
  }
  if (n < 48) { snprintf(seen[n], sizeof(seen[n]), "%s", name); n++; }
  pthread_mutex_unlock(&lk);
  NPS5_LOG(NPS5_S_HLE, NPS5_L_WARN, "%s: %s", name, what);
}

// ---- libkernel -------------------------------------------------------------------------------

// int sceKernelSetPrtAperture(int index, void *addr, size_t len): three apertures; recorded only
// (the AGC path does not map PRT yet).
static struct { uint64_t addr, len; } g_prt[3];
void shim_sceKernelSetPrtAperture(GuestContext *ctx) {
  int idx = (int)ctx->rdi;
  if (idx < 0 || idx >= 3) RET32(ctx, (int32_t)0x80020016);
  g_prt[idx].addr = ctx->rsi;
  g_prt[idx].len = ctx->rdx;
  hs_once("sceKernelSetPrtAperture", "PRT aperture recorded, not mapped");
  RET32(ctx, 0);
}

// int sceKernelIsTrinityMode(void): the host presents a base PS5, not the Pro ("Trinity") mode.
void shim_sceKernelIsTrinityMode(GuestContext *ctx) { RET32(ctx, 0); }

// Coredumps are never generated on the host (same class as libSceNoopStubs' coredump hooks).
void shim_sceCoredumpWriteUserString(GuestContext *ctx) {
  hs_once("sceCoredumpWriteUserString", "no coredump is generated on the host; ignored");
  RET32(ctx, 0);
}
void shim_sceCoredumpSetUserDataType(GuestContext *ctx) {
  hs_once("sceCoredumpSetUserDataType", "no coredump is generated on the host; ignored");
  RET32(ctx, 0);
}

// int pthread_attr_setsolosched_np(pthread_attr_t *attr, int value): the console accepts 0 and
// 16 and answers ENOTSUP (45) for anything else; the value does not change host scheduling.
void shim_pthread_attr_setsolosched_np(GuestContext *ctx) {
  if (!ctx->rdi) RET32(ctx, 22);
  int v = (int)ctx->rsi;
  if (v != 0 && v != 16) RET32(ctx, 45);
  if (v) hs_once("pthread_attr_setsolosched_np", "solo scheduling is not modelled; ignored");
  RET32(ctx, 0);
}

// ---- libScePad (adaptive triggers / vibration mode) --------------------------------------------
// Host fallback for a build without a pad backend (`-gpu off`): there is no controller, so there is nothing to drive.
// With `-gpu anyps5` the four imports below are rebound to AnyPS5's libScePad (shim_aps5_<name>, emitter.EnableAnyPS5Shims),
// which sends the vibration mode, the trigger effects and the state report to the SDL controller (DualSense through HIDAPI,
// rumble-only triggers on Xbox, nothing on a Switch Pro); docs/m3/controller.md.

#define PAD_ERR_INVALID_ARG ((int32_t)0x80920001)

void shim_scePadSetVibrationMode(GuestContext *ctx) {
  int mode = (int)ctx->rsi;
  if (mode <= 0 || mode > 5) RET32(ctx, PAD_ERR_INVALID_ARG);
  hs_once("scePadSetVibrationMode", "mode recorded only; host rumble follows scePadSetVibration");
  RET32(ctx, 0);
}

void shim_scePadGetTriggerEffectState(GuestContext *ctx) {
  void *info = NPS5_G2H(ctx, ctx->rsi);
  if (!info) RET32(ctx, PAD_ERR_INVALID_ARG);
  memset(info, 0, 8); // PadTriggerEffectStateInformation: int32_t state[2], both triggers report "no effect active"
  hs_once("scePadGetTriggerEffectState", "adaptive triggers are not emulated; reports no effect");
  RET32(ctx, 0);
}

void shim_scePadSetTriggerEffect(GuestContext *ctx) {
  if (!ctx->rsi) RET32(ctx, PAD_ERR_INVALID_ARG);
  hs_once("scePadSetTriggerEffect", "adaptive triggers are not emulated; effect ignored");
  RET32(ctx, 0);
}

void shim_scePadIsRemoteController(GuestContext *ctx) {
  uint8_t *remote = (uint8_t *)NPS5_G2H(ctx, ctx->rsi);
  if (!remote) RET32(ctx, PAD_ERR_INVALID_ARG);
  *remote = 0;
  RET32(ctx, 0);
}
