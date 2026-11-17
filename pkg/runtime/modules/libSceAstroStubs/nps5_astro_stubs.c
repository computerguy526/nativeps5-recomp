// SPDX-License-Identifier: GPL-2.0-or-later
//
// Astro Bot (PPSA21564) HLE: system-library calls the eboot imports that no other module
// provides (branch astro-hle2). Policy: a console that is signed out, offline, has no
// microphone, no capture/share service and no spatial-audio geometry engine. Initialise /
// destroy / register calls succeed; calls that need a service return the Sony error a base
// console without that service returns, with out parameters written to a defined value, so
// the title continues. Signatures are System V (guest registers) as in the SDK / AnyPS5.
// Not implemented (stubbed): libSceJson C++ object API (inert: see "JSON" below),
// AudioPropagation geometry (valid handles, no audio paths, silent renders),
// VoiceChat, PngEnc encode, Share capture, ContentExport, Http2 send/read, AudioIn HQ open,
// PlayerInvitationDialog, Font glyph generation.
#include "recomp_runtime.h"
#include "nps5_thunk.h"
#include "nps5_diag.h"
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#define RET32(ctx, v) do { (ctx)->rax = (uint64_t)(int64_t)(int32_t)(v); SHIM_RETURN(); } while (0)
#define RET64(ctx, v) do { (ctx)->rax = (uint64_t)(v); SHIM_RETURN(); } while (0)
#define P(ctx, g) ((g) ? (uint8_t *)NPS5_G2H((ctx), (g)) : (uint8_t *)NULL)

static void astro_once(const char *name, int32_t rc) {
  static char seen[256][72];
  static int n;
  static volatile int lk;
  while (__atomic_exchange_n(&lk, 1, __ATOMIC_ACQUIRE)) {}
  for (int i = 0; i < n; i++) if (!strcmp(seen[i], name)) { __atomic_store_n(&lk, 0, __ATOMIC_RELEASE); return; }
  if (n < 256) { snprintf(seen[n], sizeof(seen[n]), "%s", name); n++; }
  __atomic_store_n(&lk, 0, __ATOMIC_RELEASE);
  NPS5_LOG(NPS5_S_HLE, NPS5_L_INFO, "astro stub %s -> 0x%08x", name, (unsigned)rc);
}
#define LOGRC(name, rc) astro_once(name, (int32_t)(rc))
#define FIXED(name, rc) void shim_##name(GuestContext *ctx) { LOGRC(#name, rc); RET32(ctx, rc); }

#define E_SIGNED_OUT 0x80550006
#define E_HTTP_NETWORK 0x80431063
#define E_SHARE_NOT_SUPPORTED 0x81960007
#define E_PNG_FATAL 0x80690120
#define E_CE_NOT_INIT 0x809D3004
#define E_DIALOG_NOT_INIT 0x80B80003
#define E_AUDIOIN_INVALID_PARAM 0x80260106

static uint64_t g_tok = 0x7A5000;
static uint64_t tok(void) { return __atomic_add_fetch(&g_tok, 0x40, __ATOMIC_RELAXED); }
static void put64(GuestContext *ctx, uint64_t g, uint64_t v) { uint8_t *p = P(ctx, g); if (p) memcpy(p, &v, 8); }
static void put32(GuestContext *ctx, uint64_t g, uint32_t v) { uint8_t *p = P(ctx, g); if (p) memcpy(p, &v, 4); }

// ---------------------------------------------------------------- libkernel odds and ends
// int sceKernelNanosleep(const struct timespec *req, struct timespec *rem)
void shim_sceKernelNanosleep(GuestContext *ctx) {
  const int64_t *ts = (const int64_t *)P(ctx, ctx->rdi);
  if (!ts) RET32(ctx, (int32_t)0x80020016);
  struct timespec t = {(time_t)ts[0], (long)ts[1]};
  if (t.tv_sec < 0 || t.tv_nsec < 0 || t.tv_nsec >= 1000000000L) RET32(ctx, (int32_t)0x80020016);
  nanosleep(&t, NULL);
  int64_t *rem = (int64_t *)P(ctx, ctx->rsi);
  if (rem) { rem[0] = 0; rem[1] = 0; }
  RET32(ctx, 0);
}

void shim_scePthreadRwlockTryrdlock(GuestContext *ctx);
void shim_scePthreadRwlockTrywrlock(GuestContext *ctx);
static void timed_rw(GuestContext *ctx, void (*trylock)(GuestContext *)) {
  uint64_t rsp0 = ctx->rsp, limit_us = ctx->rsi, waited = 0;
  for (;;) {
    ctx->rsp = rsp0;
    trylock(ctx);
    ctx->rsp = rsp0;
    if ((int32_t)ctx->rax == 0) break;
    if (waited >= limit_us) { ctx->rax = (uint64_t)(int64_t)(int32_t)0x8002003C; break; } // ETIMEDOUT
    usleep(100);
    waited += 100;
  }
  SHIM_RETURN();
}
void shim_scePthreadRwlockTimedrdlock(GuestContext *ctx) { timed_rw(ctx, shim_scePthreadRwlockTryrdlock); }
void shim_scePthreadRwlockTimedwrlock(GuestContext *ctx) { timed_rw(ctx, shim_scePthreadRwlockTrywrlock); }

// libc.prx crash-report hooks: nothing to report on the host.
FIXED(sceLibcInternalBacktraceForGame, 0)
FIXED(sceLibcInternalHeapErrorReportForGame, 0)

// ---------------------------------------------------------------- small system services
// int sceSystemServiceGetNoticeScreenSkipFlag(bool *value)
void shim_sceSystemServiceGetNoticeScreenSkipFlag(GuestContext *ctx) {
  uint8_t *p = P(ctx, ctx->rdi);
  if (!p) RET32(ctx, (int32_t)0x80A10003);
  *p = 0;
  RET32(ctx, 0);
}
// int sceNpEntitlementAccessGetAddcontEntitlementInfoList(u32 label, list*, u32 num, u32 *hit): no DLC
void shim_sceNpEntitlementAccessGetAddcontEntitlementInfoList(GuestContext *ctx) {
  if (!ctx->rcx) RET32(ctx, (int32_t)0x817D0002);
  put32(ctx, ctx->rcx, 0);
  LOGRC("sceNpEntitlementAccessGetAddcontEntitlementInfoList", 0);
  RET32(ctx, 0);
}
FIXED(sceNpUniversalDataSystemTerminate, 0)
FIXED(sceNpWebApi2PushEventDeleteHandle, 0)
// int sceAudioOut2GetSpeakerInfo(AudioOut2SpeakerInfo *info, u32 flags): a television, front L/R at +-30 deg.
void shim_sceAudioOut2GetSpeakerInfo(GuestContext *ctx) {
  uint8_t *p = P(ctx, ctx->rdi);
  if (!p) RET32(ctx, (int32_t)0x8026800C);
  if ((uint32_t)ctx->rsi > 1) RET32(ctx, (int32_t)0x80268001);
  memset(p, 0, 16 + 16 * 4);
  uint32_t avail = 3;
  memcpy(p + 4, &avail, 4);
  for (int i = 0; i < 16; i++) { int16_t a[2] = {-360, -360}; memcpy(p + 16 + i * 4, a, 4); }
  int16_t l[2] = {30, 0}, r[2] = {-30, 0};
  memcpy(p + 16, l, 4); memcpy(p + 20, r, 4);
  RET32(ctx, 0);
}

// ---------------------------------------------------------------- libSceAudioIn (no microphone)
// int sceAudioInHqOpen(userId, type, index, len, freq, param): no capture device.
FIXED(sceAudioInHqOpen, E_AUDIOIN_INVALID_PARAM)

// ---------------------------------------------------------------- libSceConvertKeycode
// int sceConvertKeycodeGetImeKeyboardType(userId, u32 *type): ANSI/US keyboard.
void shim_sceConvertKeycodeGetImeKeyboardType(GuestContext *ctx) {
  if (!ctx->rsi) RET32(ctx, (int32_t)0x80BA0003);
  put32(ctx, ctx->rsi, 1);
  RET32(ctx, 0);
}
// int sceConvertKeycodeGetVirtualKeycode(u16 keycode, u32 keyboardType, u16 *virtualKeycode):
// identity mapping (HID usage in, virtual keycode out); the title only reads keyboard text input
// when a keyboard is attached, which the host path does not provide.
void shim_sceConvertKeycodeGetVirtualKeycode(GuestContext *ctx) {
  uint8_t *p = P(ctx, ctx->rdx);
  if (!p) RET32(ctx, (int32_t)0x80BA0003);
  uint16_t v = (uint16_t)ctx->rdi;
  memcpy(p, &v, 2);
  RET32(ctx, 0);
}

// ---------------------------------------------------------------- libScePlayerInvitationDialog
FIXED(scePlayerInvitationDialogClose, 0)
FIXED(scePlayerInvitationDialogGetResult, E_DIALOG_NOT_INIT)

// ---------------------------------------------------------------- libSceHttp2 (sync send/read need a network)
FIXED(sceHttp2SendRequest, E_HTTP_NETWORK)
FIXED(sceHttp2ReadData, E_HTTP_NETWORK)

// ---------------------------------------------------------------- libSceShare (no capture service)
// int sceShareCaptureScreenshot[Extended](param, i32 *requestId)
static void share_capture(GuestContext *ctx, const char *n) {
  if (ctx->rsi) put32(ctx, ctx->rsi, 0xFFFFFFFFu);
  LOGRC(n, E_SHARE_NOT_SUPPORTED);
  RET32(ctx, E_SHARE_NOT_SUPPORTED);
}
void shim_sceShareCaptureScreenshot(GuestContext *ctx) { share_capture(ctx, "sceShareCaptureScreenshot"); }
void shim_sceShareCaptureScreenshotExtended(GuestContext *ctx) { share_capture(ctx, "sceShareCaptureScreenshotExtended"); }
FIXED(sceShareRegisterContentEventCallback, 0)   // accepted, never invoked
FIXED(sceShareUnregisterContentEventCallback, 0)

// ---------------------------------------------------------------- libSceContentExport (no save-to-capture-gallery)
FIXED(sceContentExportStart, E_CE_NOT_INIT)
FIXED(sceContentExportFromData, E_CE_NOT_INIT)
FIXED(sceContentExportFinish, 0)

// ---------------------------------------------------------------- libScePngEnc
// Create param: {u32 size, u32 attribute, u32 maxImageWidth, u32 maxFilterNumber}
static uint32_t png_mem(GuestContext *ctx, uint64_t param) {
  const uint32_t *p = (const uint32_t *)P(ctx, param);
  if (!p) return 0;
  return 285808u + ((4u * p[2] + 28u) & ~31u) * (p[3] + 2u);
}
void shim_scePngEncQueryMemorySize(GuestContext *ctx) {
  if (!ctx->rdi) RET32(ctx, (int32_t)0x80690101);
  RET32(ctx, (int32_t)png_mem(ctx, ctx->rdi));
}
// (param, memory, memorySize, void **handle): the handle is the memory block itself
void shim_scePngEncCreate(GuestContext *ctx) {
  if (!ctx->rdi || !ctx->rsi || !ctx->rcx) RET32(ctx, (int32_t)0x80690101);
  if ((uint32_t)ctx->rdx < png_mem(ctx, ctx->rdi)) RET32(ctx, (int32_t)0x80690102);
  put64(ctx, ctx->rcx, ctx->rsi);
  RET32(ctx, 0);
}
FIXED(scePngEncDelete, 0)
// (handle, encodeParam, outputInfo{u32 outputSize,..}): encoding is not provided
void shim_scePngEncEncode(GuestContext *ctx) {
  uint8_t *o = P(ctx, ctx->rdx);
  if (o) memset(o, 0, 8);
  LOGRC("scePngEncEncode", E_PNG_FATAL);
  RET32(ctx, E_PNG_FATAL);
}

// ---------------------------------------------------------------- libSceFont glyph API (not implemented)
// The system font path (open, layout, render into a surface) lives in libSceFont; the per-glyph
// generate/delete API is not provided. Reported as "invalid parameter" (0x80460002) so a caller
// that probes falls back to the surface renderer.
#define E_FONT_PARAM 0x80460002
FIXED(sceFontGenerateCharGlyph, E_FONT_PARAM)
FIXED(sceFontGetRenderCharGlyphMetrics, E_FONT_PARAM)
FIXED(sceFontGetVerticalLayout, E_FONT_PARAM)
FIXED(sceFontGlyphDefineAttribute, E_FONT_PARAM)
FIXED(sceFontDeleteGlyph, 0)

// ---------------------------------------------------------------- libSceVoiceChat (signed out, no mic)
FIXED(sceVoiceChatInitialize, 0)
FIXED(sceVoiceChatTerminate, 0)
FIXED(sceVoiceChatRegisterHandlers, 0)
FIXED(sceVoiceChatRegisterMicEventHandler, 0)
FIXED(sceVoiceChatProcessEvent, 0)
FIXED(sceVoiceChatDeleteRequest, 0)
FIXED(sceVoiceChatCreateRequest, E_SIGNED_OUT)
FIXED(sceVoiceChatRequestRegisterSession, E_SIGNED_OUT)
FIXED(sceVoiceChatRequestUnregisterSession, E_SIGNED_OUT)
FIXED(sceVoiceChatRequestCreateGameSessionVoiceChatChannel, E_SIGNED_OUT)
FIXED(sceVoiceChatRequestDeleteGameSessionVoiceChatChannel, E_SIGNED_OUT)
FIXED(sceVoiceChatRequestJoinGameSessionVoiceChatChannel, E_SIGNED_OUT)
FIXED(sceVoiceChatRequestLeaveGameSessionVoiceChatChannel, E_SIGNED_OUT)
FIXED(sceVoiceChatRequestCreatePlayerSessionVoiceChatChannel, E_SIGNED_OUT)
FIXED(sceVoiceChatRequestDeletePlayerSessionVoiceChatChannel, E_SIGNED_OUT)
FIXED(sceVoiceChatRequestJoinPlayerSessionVoiceChatChannel, E_SIGNED_OUT)
FIXED(sceVoiceChatRequestLeavePlayerSessionVoiceChatChannel, E_SIGNED_OUT)

// ---------------------------------------------------------------- libSceAudioPropagation
// Valid, inert: handles are opaque tokens (the system handle is its memory block), attribute and
// geometry calls are accepted, no audio paths are ever produced and renders are silent
// (channelCount 0), as AnyPS5's implementation does.
// Mem struct: {desc 0x10, void *cpuMem, u64 cpuSize, void *gpuMem, u64 gpuSize}.
// Options: {desc 0x10, maxSources u32, maxMaterials, p24, reflectionOrder, frameSamples, speed f32, maxPathLen f32, flags}.
void shim_sceAudioPropagationSystemQueryMemory(GuestContext *ctx) {
  const uint32_t *o = (const uint32_t *)P(ctx, ctx->rdi);
  uint8_t *m = P(ctx, ctx->rsi);
  if (!o || !m) RET32(ctx, (int32_t)0x8A700003);
  uint32_t sources = o[4], mats = o[5], order = o[7], frame = o[8], flags = o[11];
  float speed, maxlen;
  memcpy(&speed, &o[9], 4); memcpy(&maxlen, &o[10], 4);
  if (sources == 0 || sources > 64 || mats == 0 || mats > 64 || order == 0 || order > 2 || speed <= 0) RET32(ctx, (int32_t)0x8A700001);
  uint64_t paths = (uint64_t)sources * (order == 1 ? 6 : 36);
  uint64_t delay = (uint64_t)((maxlen / speed) * 48000.0f) + 1;
  uint64_t bytes = (uint64_t)sources * 1024 + paths * (32 * (order + 2) + 8928);
  bytes += frame == 256 ? 2048 : 4ull * frame;
  bytes += 4 * delay * sources;
  bytes += bytes & 15;
  bytes += 160 * paths + 12352 + 4208ull * mats;
  if (frame == 256) bytes += (uint64_t)sources * ((flags & 2) ? 36896 : 16416);
  if (flags & 4) bytes += 2000656;
  if (flags & 8) bytes += 288 * paths;
  if (flags & 16) bytes += 144 * paths;
  uint64_t sz = bytes + 5368 - ((bytes & 15) ^ 8);
  put64(ctx, ctx->rsi + 0x10, 0); put64(ctx, ctx->rsi + 0x18, sz);
  put64(ctx, ctx->rsi + 0x20, 0); put64(ctx, ctx->rsi + 0x28, 0);
  RET32(ctx, 0);
}
void shim_sceAudioPropagationSystemCreate(GuestContext *ctx) {
  const uint8_t *m = P(ctx, ctx->rsi);
  if (!P(ctx, ctx->rdi) || !m || !ctx->rdx) RET32(ctx, (int32_t)0x8A700003);
  uint64_t mem; memcpy(&mem, m + 0x10, 8);
  put64(ctx, ctx->rdx, mem ? mem : tok());
  RET32(ctx, 0);
}
FIXED(sceAudioPropagationSystemDestroy, 0)
FIXED(sceAudioPropagationSystemSetAttributes, 0)
FIXED(sceAudioPropagationSystemUnregisterMaterial, 0)
FIXED(sceAudioPropagationSystemSetRays, 0)
FIXED(sceAudioPropagationRoomDestroy, 0)
FIXED(sceAudioPropagationPortalDestroy, 0)
FIXED(sceAudioPropagationPortalSetAttributes, 0)
FIXED(sceAudioPropagationSourceDestroy, 0)
FIXED(sceAudioPropagationSourceSetAttributes, 0)
FIXED(sceAudioPropagationSourceSetAudioPaths, 0)
FIXED(sceAudioPropagationSourceCalculateAudioPaths, 0)
// out-handle creators: (system, [params/desc,] u64 *out)
static void ap_create(GuestContext *ctx, uint64_t out) {
  if (!out) RET32(ctx, (int32_t)0x8A700003);
  put64(ctx, out, tok());
  RET32(ctx, 0);
}
void shim_sceAudioPropagationRoomCreate(GuestContext *ctx) { ap_create(ctx, ctx->rsi); }
void shim_sceAudioPropagationSourceCreate(GuestContext *ctx) { ap_create(ctx, ctx->rsi); }
void shim_sceAudioPropagationPortalCreate(GuestContext *ctx) { ap_create(ctx, ctx->rdx); }
void shim_sceAudioPropagationSystemRegisterMaterial(GuestContext *ctx) { ap_create(ctx, ctx->rdx); }
// (source, u32 *count): 36 path slots, as the real library
void shim_sceAudioPropagationSourceGetAudioPathCount(GuestContext *ctx) {
  if (!ctx->rsi) RET32(ctx, (int32_t)0x8A700003);
  put32(ctx, ctx->rsi, 36);
  RET32(ctx, 0);
}
// (source, u32 index, u64 *path)
void shim_sceAudioPropagationSourceGetAudioPath(GuestContext *ctx) {
  if (!ctx->rdx) RET32(ctx, (int32_t)0x8A700003);
  if ((uint32_t)ctx->rsi >= 36) RET32(ctx, (int32_t)0x8A700001);
  put64(ctx, ctx->rdx, ctx->rdi + 0x100 + 0x40 * (uint64_t)(uint32_t)ctx->rsi);
  RET32(ctx, 0);
}
// (handle, rays*, u32 *count): no rays
static void ap_get_rays(GuestContext *ctx) {
  if (!ctx->rsi || !ctx->rdx) RET32(ctx, (int32_t)0x8A700003);
  put32(ctx, ctx->rdx, 0);
  RET32(ctx, 0);
}
void shim_sceAudioPropagationSystemGetRays(GuestContext *ctx) { ap_get_rays(ctx); }
void shim_sceAudioPropagationSourceGetRays(GuestContext *ctx) { ap_get_rays(ctx); }
// (system, RenderInfo *infos (0x30 each), u32 count): silent, channelCount = 0
void shim_sceAudioPropagationSourceRender(GuestContext *ctx) {
  uint32_t n = (uint32_t)ctx->rdx;
  if (!ctx->rsi || n == 0) RET32(ctx, (int32_t)0x8A700003);
  for (uint32_t i = 0; i < n; i++) put32(ctx, ctx->rsi + 0x30ull * i + 0x2c, 0);
  RET32(ctx, 0);
}


// libSceJson (sce::Json object API): implemented in modules/libSceJson2/ps4_json2.c.

// ---------------------------------------------------------------- NIDs with no name in the DB (astroboot)
// Bound by NID in pkg/emitter/shims.go (shimAliases).
#include <pthread.h>
#include <errno.h>
#include <time.h>
// 00oCq0RwSAY sce::Json::Initializer::setGlobalNullAccessCallBack(cb, ctx): accepted, never called.
void shim_nps5_json_set_null_access_cb(GuestContext *ctx) { RET32(ctx, 0); }
// X+4jdIS75P0 sceAudioInIsPadEmbeddedMic: no.
void shim_nps5_audioin_is_pad_embedded_mic(GuestContext *ctx) { RET32(ctx, 0); }
// cfwBSQyr5Ys sceKernelDebugWriteCppExceptionInfo: nothing to record.
void shim_nps5_debug_write_cpp_exception_info(GuestContext *ctx) { RET32(ctx, 0); }
// Hc4CaR6JBL0 sceKernelSyncOnAddressWait(u32 *addr, u32 expected, const u32 *timeout_us) and
// q2y-wDIVWZA sceKernelSyncOnAddressWake(void *addr, int count): a futex. One process-wide
// condition variable: wakes are broadcast, waiters re-check their word (spurious wake = return 0,
// which the futex contract allows).
static pthread_mutex_t g_soa_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_soa_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_soa_gen;
void shim_nps5_sync_on_address_wait(GuestContext *ctx) {
  const volatile uint32_t *addr = (const volatile uint32_t *)P(ctx, ctx->rdi);
  const uint32_t expected = (uint32_t)ctx->rsi;
  const uint32_t *tmo = ctx->rdx ? (const uint32_t *)P(ctx, ctx->rdx) : NULL;
  if (!addr) RET32(ctx, 0x8002000e); // EFAULT
  int rc = 0;
  pthread_mutex_lock(&g_soa_mu);
  if (*addr != expected) {
    rc = (int)0x80020023; // EAGAIN
  } else {
    const uint64_t gen = g_soa_gen;
    struct timespec ts;
    if (tmo) {
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += *tmo / 1000000u;
      ts.tv_nsec += (long)(*tmo % 1000000u) * 1000;
      if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    }
    while (g_soa_gen == gen && *addr == expected) {
      if (!tmo) {
        pthread_cond_wait(&g_soa_cv, &g_soa_mu);
      } else if (pthread_cond_timedwait(&g_soa_cv, &g_soa_mu, &ts) == ETIMEDOUT) {
        rc = (int)0x8002003c; // ETIMEDOUT
        break;
      }
    }
  }
  pthread_mutex_unlock(&g_soa_mu);
  RET32(ctx, rc);
}
void shim_nps5_sync_on_address_wake(GuestContext *ctx) {
  pthread_mutex_lock(&g_soa_mu);
  g_soa_gen++;
  pthread_cond_broadcast(&g_soa_cv);
  pthread_mutex_unlock(&g_soa_mu);
  RET32(ctx, 0);
}
