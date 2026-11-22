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

// int sceKernelGetOperationMode(int *mode, int *submode) (NID NH6xARDOVv8, SDK 11 titles: PPSA07631): base PS5
// (mode 2; 3 would be the Pro "trinity" mode, which sceKernelIsTrinityMode already answers 0 for), no sub-mode.
void shim_sceKernelGetOperationMode(GuestContext *ctx) {
  int32_t *mode = ctx->rdi ? (int32_t *)NPS5_G2H(ctx, ctx->rdi) : NULL;
  int32_t *submode = ctx->rsi ? (int32_t *)NPS5_G2H(ctx, ctx->rsi) : NULL;
  if (mode) *mode = 2;
  if (submode) *submode = 0;
  hs_once("sceKernelGetOperationMode", "base PS5 mode 2, sub-mode 0");
  RET32(ctx, 0);
}

// int _sceLibcInternalThreadAtexit(...) (NID qBS714-Jr3g): libc.prx's __cxa_thread_atexit hands the thread-exit
// destructor to the system. Accepted and not recorded: thread_local destructors do not run at guest thread exit.
void shim__sceLibcInternalThreadAtexit(GuestContext *ctx) {
  hs_once("_sceLibcInternalThreadAtexit", "thread-exit destructors accepted, not run");
  RET32(ctx, 0);
}

// ---- libScePsml Mfsr2 (PSSR 2 upscaler, PS5 Pro only; PPSA07631) ----------------------------------------
// Behaviour as upstream KytyPS5 (GPL-2.0, src/libs/libPsml.cpp, namespace Mfsr2) on a base console: Initialize
// answers NOT_AVAILABLE (0x8a8a0012) outside Pro ("trinity") mode, every other entry NOT_INITIALIZED (0x8a8a0001),
// so the title keeps its own upscaler. Bound by NID (no names in our NID table).
void shim_psml_mfsr2_initialize(GuestContext *ctx) {
  hs_once("scePsmlMfsr2Initialize", "base PS5: PSSR not available (0x8a8a0012)");
  RET32(ctx, (int32_t)0x8a8a0012u);
}
void shim_psml_mfsr2_not_initialized(GuestContext *ctx) { RET32(ctx, (int32_t)0x8a8a0001u); }

// ---- NpEntitlementAccess / NpManager account queries (PPSA07631) ----------------------------------------
// Behaviour of upstream KytyPS5 (GPL-2.0: libNpEntitlementAccess.cpp, network.cpp) for an offline, signed-out
// user: the game is the full SKU, no game trial, account age/language not available (SIGNED_OUT 0x80550006).
void shim_sceNpEntitlementAccessGetSkuFlag(GuestContext *ctx) {
  uint32_t *flag = ctx->rdi ? (uint32_t *)NPS5_G2H(ctx, ctx->rdi) : NULL;
  if (!flag) RET32(ctx, (int32_t)0x817D0002u);
  *flag = 3; // SKU_FLAG_FULL
  hs_once("sceNpEntitlementAccessGetSkuFlag", "full SKU");
  RET32(ctx, 0);
}
void shim_sceNpEntitlementAccessGetGameTrialsFlag(GuestContext *ctx) {
  uint32_t *flag = ctx->rdi ? (uint32_t *)NPS5_G2H(ctx, ctx->rdi) : NULL;
  if (!flag) RET32(ctx, (int32_t)0x817D0002u);
  *flag = 0; // GAME_TRIALS_FLAG_OFF
  RET32(ctx, 0);
}
void shim_sceNpGetAccountAge(GuestContext *ctx) {
  uint8_t *age = ctx->rdx ? (uint8_t *)NPS5_G2H(ctx, ctx->rdx) : NULL;
  if ((int32_t)ctx->rdi <= 0 || !age) RET32(ctx, (int32_t)0x80550003u);
  *age = 0;
  RET32(ctx, (int32_t)0x80550006u);
}
void shim_sceNpGetAccountLanguage2(GuestContext *ctx) {
  uint8_t *code = ctx->rdx ? (uint8_t *)NPS5_G2H(ctx, ctx->rdx) : NULL;
  if ((int32_t)ctx->rdi <= 0 || !code) RET32(ctx, (int32_t)0x80550003u);
  RET32(ctx, (int32_t)0x80550006u);
}

// ---- SDK 11 audio / entitlement imports with no shim yet (PPSA07631) ------------------------------------------
// Behaviour of upstream KytyPS5 (GPL-2.0, libAudio2.cpp / libAudio.cpp / libNpEntitlementAccess.cpp), ported, not
// copied: mastering is a no-op that succeeds, 3D latency is accepted for outputs 0..2, the propagation path setter
// and the decoder reset succeed, no entitlement key exists.
void shim_sceAudioOut2MasteringInit(GuestContext *ctx) { hs_once("sceAudioOut2MasteringInit", "no-op"); RET32(ctx, 0); }
void shim_sceAudioOut2MasteringSetParam(GuestContext *ctx) { RET32(ctx, 0); }
void shim_sceAudioOut2MasteringTerm(GuestContext *ctx) { RET32(ctx, 0); }
void shim_sceAudioOut2Set3DLatency(GuestContext *ctx) {
  if ((uint32_t)ctx->rsi > 2) RET32(ctx, (int32_t)0x80268001u); // INVALID_PARAM
  RET32(ctx, 0);
}
void shim_sceAudioPropagationSourceSetAudioPath(GuestContext *ctx) { RET32(ctx, 0); }
void shim_sceAudiodecClearContext(GuestContext *ctx) {
  if ((int32_t)ctx->rdi <= 0) RET32(ctx, (int32_t)0x807F000Fu); // INVALID_HANDLE
  RET32(ctx, 0);
}
void shim_sceNpEntitlementAccessGetEntitlementKey(GuestContext *ctx) {
  if (!ctx->rdi || !ctx->rdx) RET32(ctx, (int32_t)0x817D0002u); // PARAMETER
  RET32(ctx, (int32_t)0x817D0007u);                         // NO_ENTITLEMENT
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
