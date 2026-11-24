// SPDX-License-Identifier: GPL-2.0-or-later
// Host unit test for the offline online/system stubs (pkg/runtime/modules/libSceNpOnlineStubs and
// libSceNpCppWebApi). Builds natively with tools/online_unit.sh: a fake GuestContext over a malloc'ed
// arena, shims called directly. No guest code runs.
#include "nps5_online.h"
#include <stdio.h>
#include <stdlib.h>

uint8_t g_nps5_level[NPS5_S_COUNT];
static int g_logs;
void nps5_log(int s, int l, const char *fmt, ...) { (void)s; (void)l; (void)fmt; g_logs++; }

#define ARENA (16ULL << 20)
static GuestContext *C;
#define H(a) ((uint8_t *)C->mem_base + (a))
#define CALL(fn) do { uint64_t rsp0 = C->rsp; fn(C); if (C->rsp != rsp0 + 8) { printf("FAIL %s:%d rsp\n", __FILE__, __LINE__); fails++; } C->rsp = rsp0; } while (0)
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define RET32 ((int32_t)C->rax)
static int fails;

#define DECL(n) void shim_##n(GuestContext *)
DECL(sceHttp2CreateTemplate); DECL(sceHttp2SendRequestAsync); DECL(sceHttp2GetStatusCode); DECL(sceHttp2GetAllResponseHeaders);
DECL(sceHttp2GetResponseContentLength); DECL(sceHttp2GetMemoryPoolStats); DECL(sceHttp2DeleteRequest);
DECL(sceNpWebApi2Initialize); DECL(sceNpWebApi2CreateUserContext); DECL(sceNpWebApi2CreateRequest);
DECL(sceNpWebApi2GetHttpResponseHeaderValueLength);
DECL(sceNpUniversalDataSystemCreateContext); DECL(sceNpUniversalDataSystemCreateEvent); DECL(sceNpUniversalDataSystemPostEvent);
DECL(sceNpUniversalDataSystemEventPropertyObjectSetObject); DECL(sceNpUniversalDataSystemEventPropertyArraySetObject);
DECL(sceNpUniversalDataSystemEventPropertyObjectSetInt32);
DECL(sceNpTrophy2CreateContext); DECL(sceNpTrophy2GetGameInfo); DECL(sceNpTrophy2GetTrophyInfoArray); DECL(sceNpTrophy2RegisterUnlockCallback);
DECL(sceNpEntitlementAccessRequestUnifiedEntitlementInfoList); DECL(sceNpEntitlementAccessPollUnifiedEntitlementInfoList);
DECL(sceGameUpdateCreateRequest); DECL(sceGameUpdateCheck);
DECL(sceNpCheckPremium); DECL(sceNpNotifyPremiumFeature); DECL(sceNpAuthGetIdTokenV3);
DECL(sceNpGameIntentReceiveIntent); DECL(sceNpGameIntentGetPropertyValueString);
DECL(scePlayGoGetInstallChunkId); DECL(scePlayGoGetOptionalChunk);
DECL(scePlayGoDialogInitialize); DECL(scePlayGoDialogOpen); DECL(scePlayGoDialogUpdateStatus); DECL(scePlayGoDialogGetResult);
DECL(scePlayGoDialogTerminate);
DECL(sceSystemServiceInitializePlayerDialogParam); DECL(sceSystemServiceLaunchPlayerDialog);
DECL(sceUserServiceGetAgeLevel);
DECL(sceAcmContextCreate);
DECL(sceAcmBatchWait);
DECL(sceShareInitialize);
// generated libSceNpCppWebApi samples
DECL(_ZN3sce2Np9CppWebApi6Common12IntrusivePtrINS1_14SessionManager2V132RequestPlayerSessionMemberPlayerEEC1Ev);

static void clear(void) { C->rdi = C->rsi = C->rdx = C->rcx = C->r8 = C->r9 = 0; }

int main(void) {
  C = calloc(1, sizeof *C);
  C->mem_base = calloc(1, ARENA);
  C->mem_size = ARENA;
  C->rsp = 0x100000;
  const uint64_t P = 0x2000, Q = 0x3000, R = 0x4000;  // scratch guest addresses

  // ---- Http2: send needs a network, status before send, NULL out-params untouched and no crash
  clear(); CALL(shim_sceHttp2CreateTemplate); CHECK(RET32 > 0);
  int32_t t1 = RET32; CALL(shim_sceHttp2CreateTemplate); CHECK(RET32 > 0 && RET32 != t1);
  clear(); CALL(shim_sceHttp2SendRequestAsync); CHECK((uint32_t)RET32 == 0x80431063u);
  memset(H(P), 0xAB, 16); C->rdi = 5; C->rsi = P; CALL(shim_sceHttp2GetStatusCode);
  CHECK((uint32_t)RET32 == 0x80431065u && *(int32_t *)H(P) == 0 && H(P)[4] == 0xAB);
  clear(); CALL(shim_sceHttp2GetStatusCode); CHECK((uint32_t)RET32 == 0x80431065u);  // NULL stays NULL
  memset(H(P), 0xAB, 32); C->rsi = P; C->rdx = Q; memset(H(Q), 0xCD, 8); CALL(shim_sceHttp2GetAllResponseHeaders);
  CHECK(*(uint64_t *)H(P) == 0 && *(uint64_t *)H(Q) == 0);
  memset(H(P), 0xAB, 16); memset(H(Q), 0xCD, 16); C->rsi = P; C->rdx = Q; CALL(shim_sceHttp2GetResponseContentLength);
  CHECK(*(int32_t *)H(P) == 0 && *(uint64_t *)H(Q) == 0 && H(Q)[8] == 0xCD);
  memset(H(P), 0xEE, 64); C->rsi = P; CALL(shim_sceHttp2GetMemoryPoolStats);
  CHECK(*(uint64_t *)H(P + 24) == 0 && H(P + 32)[0] == 0xEE && RET32 == 0);
  clear(); CALL(shim_sceHttp2DeleteRequest); CHECK(RET32 == 0);

  // ---- NpWebApi2: library context ok, no user context, requests fail signed out
  clear(); CALL(shim_sceNpWebApi2Initialize); CHECK(RET32 > 0);
  CALL(shim_sceNpWebApi2CreateUserContext); CHECK((uint32_t)RET32 == 0x80553407u);
  memset(H(P), 0x77, 16); C->r9 = P; CALL(shim_sceNpWebApi2CreateRequest);
  CHECK((uint32_t)RET32 == 0x80553407u && *(int64_t *)H(P) == 0 && H(P)[8] == 0x77);
  clear(); CALL(shim_sceNpWebApi2CreateRequest); CHECK((uint32_t)RET32 == 0x80553407u);
  memset(H(P), 0x77, 16); C->rdx = P; CALL(shim_sceNpWebApi2GetHttpResponseHeaderValueLength);
  CHECK((uint32_t)RET32 == 0x80553406u && *(uint64_t *)H(P) == 0);

  // ---- UDS: all succeeds locally with distinct inert tokens
  clear(); C->rdi = P; CALL(shim_sceNpUniversalDataSystemCreateContext);
  CHECK(RET32 == 0 && *(uint32_t *)H(P) != 0);
  clear(); CALL(shim_sceNpUniversalDataSystemCreateContext); CHECK(RET32 == 0);  // NULL out: no crash
  clear(); C->rdx = P; C->rcx = Q; CALL(shim_sceNpUniversalDataSystemCreateEvent);
  CHECK(RET32 == 0 && *(uint64_t *)H(P) != 0 && *(uint64_t *)H(Q) != 0 && *(uint64_t *)H(P) != *(uint64_t *)H(Q));
  clear(); C->rcx = P; CALL(shim_sceNpUniversalDataSystemEventPropertyObjectSetObject); CHECK(RET32 == 0 && *(uint64_t *)H(P) != 0);
  clear(); C->rdx = P; CALL(shim_sceNpUniversalDataSystemEventPropertyArraySetObject); CHECK(RET32 == 0 && *(uint64_t *)H(P) != 0);
  clear(); CALL(shim_sceNpUniversalDataSystemEventPropertyObjectSetInt32); CHECK(RET32 == 0);
  CALL(shim_sceNpUniversalDataSystemPostEvent); CHECK(RET32 == 0);

  // ---- Trophy2
  clear(); C->rdi = P; CALL(shim_sceNpTrophy2CreateContext); CHECK(RET32 == 0 && *(int32_t *)H(P) > 0);
  memset(H(P), 0x55, 200); memset(H(Q), 0x55, 40); C->rdx = P; C->rcx = Q; CALL(shim_sceNpTrophy2GetGameInfo);
  CHECK(RET32 == 0 && *(uint64_t *)H(P + 144) == 0 && H(P + 152)[0] == 0x55 && *(uint64_t *)H(Q + 16) == 0 && H(Q + 24)[0] == 0x55);
  memset(H(P), 0x55, 8); clear(); C->r9 = P; CALL(shim_sceNpTrophy2GetTrophyInfoArray);
  CHECK(RET32 == 0 && *(uint32_t *)H(P) == 0 && H(P)[4] == 0x55);
  clear(); CALL(shim_sceNpTrophy2RegisterUnlockCallback); CHECK(RET32 == 0);

  // ---- entitlement / game update / np checks
  memset(H(P), 0x11, 16); clear(); C->r9 = P; CALL(shim_sceNpEntitlementAccessRequestUnifiedEntitlementInfoList);
  CHECK((uint32_t)RET32 == 0x80550006u && *(int64_t *)H(P) == 0 && H(P)[8] == 0x11);
  CALL(shim_sceNpEntitlementAccessPollUnifiedEntitlementInfoList); CHECK((uint32_t)RET32 == 0x817D0005u);
  clear(); CALL(shim_sceGameUpdateCreateRequest); CHECK((uint32_t)RET32 >= 0x20000000u);
  CALL(shim_sceGameUpdateCheck); CHECK((uint32_t)RET32 == 0x80550006u);
  CALL(shim_sceNpCheckPremium); CHECK((uint32_t)RET32 == 0x80550006u);
  CALL(shim_sceNpNotifyPremiumFeature); CHECK(RET32 == 0);
  CALL(shim_sceNpAuthGetIdTokenV3); CHECK((uint32_t)RET32 == 0x80550006u);
  CALL(shim_sceNpGameIntentReceiveIntent); CHECK((uint32_t)RET32 == 0x80553806u);
  memset(H(P), 0x99, 8); clear(); C->rdx = P; C->rcx = 8; CALL(shim_sceNpGameIntentGetPropertyValueString);
  CHECK((uint32_t)RET32 == 0x80553807u && H(P)[0] == 0 && H(P)[1] == 0x99);

  // ---- PlayGo optional chunks (not a PlayGo install) and the dialogs
  memset(H(P), 0x33, 8); clear(); C->rcx = P; CALL(shim_scePlayGoGetInstallChunkId);
  CHECK((uint32_t)RET32 == 0x80B2000Eu && *(uint32_t *)H(P) == 0);
  clear(); CALL(shim_scePlayGoGetOptionalChunk); CHECK((uint32_t)RET32 == 0x80B2000Eu);
  clear(); CALL(shim_scePlayGoDialogUpdateStatus); CHECK(RET32 == 0);
  CALL(shim_scePlayGoDialogOpen); CHECK((uint32_t)RET32 == 0x80B80006u);  // not initialised
  CALL(shim_scePlayGoDialogInitialize); CHECK(RET32 == 0);
  CALL(shim_scePlayGoDialogUpdateStatus); CHECK(RET32 == 1);
  CALL(shim_scePlayGoDialogOpen); CHECK(RET32 == 0);
  CALL(shim_scePlayGoDialogUpdateStatus); CHECK(RET32 == 3);
  memset(H(P), 0x44, 0x30); C->rdi = P; CALL(shim_scePlayGoDialogGetResult);
  CHECK(RET32 == 0 && *(uint32_t *)H(P + 4) == 1 && H(P + 0x28)[0] == 0x44 && H(P)[0] == 0);
  clear(); CALL(shim_scePlayGoDialogTerminate); CALL(shim_scePlayGoDialogUpdateStatus); CHECK(RET32 == 0);

  // ---- system service / user service / acm / share
  memset(H(P), 0x66, 0x50); clear(); C->rdi = P; CALL(shim_sceSystemServiceInitializePlayerDialogParam);
  CHECK(*(uint64_t *)H(P) == 0x40 && *(int32_t *)H(P + 12) == -1 && H(P + 0x3F)[0] == 0 && H(P + 0x40)[0] == 0x66);
  clear(); CALL(shim_sceSystemServiceInitializePlayerDialogParam);  // NULL: no crash
  CALL(shim_sceSystemServiceLaunchPlayerDialog); CHECK((uint32_t)RET32 == 0x80A10002u);
  clear(); C->rsi = P; CALL(shim_sceUserServiceGetAgeLevel); CHECK(RET32 == 0 && *(uint32_t *)H(P) == 0xFFFFFFFFu);
  clear(); CALL(shim_sceUserServiceGetAgeLevel); CHECK((uint32_t)RET32 == 0x80960005u);
  clear(); CALL(shim_sceAcmContextCreate); CHECK((uint32_t)RET32 == 0x81940002u);
  setenv("NPS5_ACM_CONTEXT_OK", "1", 1); // opt-in: a context token, batches accepted
  memset(H(P), 0, 4);
  clear();
  C->rdi = P;
  CALL(shim_sceAcmContextCreate);
  CHECK(RET32 == 0 && *(uint32_t *)H(P) != 0);
  clear();
  CALL(shim_sceAcmBatchWait);
  CHECK(RET32 == 0);
  unsetenv("NPS5_ACM_CONTEXT_OK");
  clear();
  CALL(shim_sceAcmBatchWait);
  CHECK((uint32_t)RET32 == 0x81940002u);
  clear(); CALL(shim_sceShareInitialize); CHECK(RET32 == 0);

  // ---- generated NpCppWebApi: IntrusivePtr<T>::IntrusivePtr() zeroes the pointer and returns this
  memset(H(R), 0x5A, 16); clear(); C->rdi = R;
  CALL(shim__ZN3sce2Np9CppWebApi6Common12IntrusivePtrINS1_14SessionManager2V132RequestPlayerSessionMemberPlayerEEC1Ev);
  CHECK(C->rax == R && *(uint64_t *)H(R) == 0 && H(R)[8] == 0x5A);

  CHECK(g_logs > 0);
  if (!fails) printf("online_unit: all passed\n");
  return fails != 0;
}
