// SPDX-License-Identifier: GPL-2.0-or-later
// Offline-console stubs, hand-written part. Policy and sources: nps5_online.h, docs/m2/online_stubs.md.
// Binding: pkg/emitter/online_stubs.go (names), fixture pkg/emitter/testdata/online_stub_imports.txt.
#include "nps5_online.h"

static volatile uint32_t g_on_token = 0x100;  // never 0, never small enough to look like an error
uint32_t on_token(void) { return __atomic_add_fetch(&g_on_token, 1, __ATOMIC_RELAXED); }

// =============================================================== libSceHttp2
// A template/request is a local object (token); sending needs a network: ORBIS_HTTP_ERROR_NETWORK.
// Async completion never happens (no kqueue/epoll event is posted, no callback fires).
#define HTTP2_TOKEN(fn)                         \
  void shim_##fn(GuestContext *ctx) {           \
    uint32_t t = on_token();                    \
    ON_LOG(#fn, t);                             \
    ctx->rax = t;                               \
    SHIM_RETURN();                              \
  }
HTTP2_TOKEN(sceHttp2CreateTemplate)
HTTP2_TOKEN(sceHttp2CreateRequestWithURL)
ON_FIXED(sceHttp2DeleteTemplate, 0)
ON_FIXED(sceHttp2DeleteRequest, 0)
ON_FIXED(sceHttp2AbortRequest, 0)
ON_FIXED(sceHttp2SetResolveTimeOut, 0)
ON_FIXED(sceHttp2SetConnectTimeOut, 0)
ON_FIXED(sceHttp2SetRecvTimeOut, 0)
ON_FIXED(sceHttp2SetSendTimeOut, 0)
ON_FIXED(sceHttp2SetRedirectCallback, 0)  // accepted, never called
ON_FIXED(sceHttp2AddRequestHeader, 0)
ON_FIXED(sceHttp2SendRequestAsync, ON_HTTP_ERROR_NETWORK)
ON_FIXED(sceHttp2ReadDataAsync, ON_HTTP_ERROR_NETWORK)
ON_FIXED(sceHttp2WaitAsync, ON_HTTP_ERROR_NETWORK)

// (req, int *statusCode)
void shim_sceHttp2GetStatusCode(GuestContext *ctx) {
  on_put32(ctx, ctx->rsi, 0);
  ON_RET("sceHttp2GetStatusCode", ON_HTTP_ERROR_BEFORE_SEND);
}
// (req, char **headers, size_t *size)
void shim_sceHttp2GetAllResponseHeaders(GuestContext *ctx) {
  on_put64(ctx, ctx->rsi, 0);
  on_put64(ctx, ctx->rdx, 0);
  ON_RET("sceHttp2GetAllResponseHeaders", ON_HTTP_ERROR_BEFORE_SEND);
}
// (req, int *result, uint64_t *contentLength)
void shim_sceHttp2GetResponseContentLength(GuestContext *ctx) {
  on_put32(ctx, ctx->rsi, 0);
  on_put64(ctx, ctx->rdx, 0);
  ON_RET("sceHttp2GetResponseContentLength", ON_HTTP_ERROR_BEFORE_SEND);
}
// (libCtx, Http2MemoryPoolStats *stats) { size_t pool, max_inuse, current_inuse; int32 reserved }  32 bytes
void shim_sceHttp2GetMemoryPoolStats(GuestContext *ctx) {
  on_zero(ctx, ctx->rsi, 32);
  ON_RET("sceHttp2GetMemoryPoolStats", 0);
}

// =============================================================== libSceNpWebApi2
// No user context can exist (no signed-in user), so everything hanging off one is "not found".
// Library context and push-event handle/filter objects are local registries and succeed.
#define TOKEN_RET(fn)                           \
  void shim_##fn(GuestContext *ctx) {           \
    uint32_t t = on_token();                    \
    ON_LOG(#fn, t);                             \
    ctx->rax = t;                               \
    SHIM_RETURN();                              \
  }
TOKEN_RET(sceNpWebApi2Initialize)               // returns the library context id (> 0)
ON_FIXED(sceNpWebApi2Terminate, 0)
TOKEN_RET(sceNpWebApi2PushEventCreateHandle)    // (libCtx) -> handle id
TOKEN_RET(sceNpWebApi2PushEventCreateFilter)    // (libCtx, handle, serviceName, label, filter, num) -> filter id
ON_FIXED(sceNpWebApi2PushEventDeleteFilter, 0)
ON_FIXED(sceNpWebApi2CheckTimeout, 0)           // void in the SDK; rax ignored
ON_FIXED(sceNpWebApi2CreateUserContext, ON_WEBAPI2_ERROR_NOT_SIGNED_IN)
ON_FIXED(sceNpWebApi2DeleteUserContext, ON_WEBAPI2_ERROR_USER_CONTEXT_NOT_FOUND)
ON_FIXED(sceNpWebApi2PushEventRegisterCallback, ON_WEBAPI2_ERROR_USER_CONTEXT_NOT_FOUND)
ON_FIXED(sceNpWebApi2PushEventUnregisterCallback, ON_WEBAPI2_ERROR_USER_CONTEXT_NOT_FOUND)
ON_FIXED(sceNpWebApi2PushEventCreatePushContext, ON_WEBAPI2_ERROR_USER_CONTEXT_NOT_FOUND)
ON_FIXED(sceNpWebApi2PushEventDeletePushContext, ON_WEBAPI2_ERROR_USER_CONTEXT_NOT_FOUND)
ON_FIXED(sceNpWebApi2PushEventRegisterPushContextCallback, ON_WEBAPI2_ERROR_USER_CONTEXT_NOT_FOUND)
ON_FIXED(sceNpWebApi2PushEventUnregisterPushContextCallback, ON_WEBAPI2_ERROR_USER_CONTEXT_NOT_FOUND)
ON_FIXED(sceNpWebApi2PushEventStartPushContextCallback, ON_WEBAPI2_ERROR_USER_CONTEXT_NOT_FOUND)
ON_FIXED(sceNpWebApi2SetRequestTimeout, ON_WEBAPI2_ERROR_REQUEST_NOT_FOUND)
ON_FIXED(sceNpWebApi2SendRequest, ON_WEBAPI2_ERROR_REQUEST_NOT_FOUND)
ON_FIXED(sceNpWebApi2ReadData, ON_WEBAPI2_ERROR_REQUEST_NOT_FOUND)
ON_FIXED(sceNpWebApi2AddHttpRequestHeader, ON_WEBAPI2_ERROR_REQUEST_NOT_FOUND)
ON_FIXED(sceNpWebApi2DeleteRequest, ON_WEBAPI2_ERROR_REQUEST_NOT_FOUND)
ON_FIXED(sceNpWebApi2GetHttpResponseHeaderValue, ON_WEBAPI2_ERROR_REQUEST_NOT_FOUND)

// (userCtx, apiGroup, path, method, contentParam, int64_t *requestId)
void shim_sceNpWebApi2CreateRequest(GuestContext *ctx) {
  on_put64(ctx, ctx->r9, 0);
  ON_RET("sceNpWebApi2CreateRequest", ON_WEBAPI2_ERROR_NOT_SIGNED_IN);
}
// (requestId, fieldName, size_t *valueLength)
void shim_sceNpWebApi2GetHttpResponseHeaderValueLength(GuestContext *ctx) {
  on_put64(ctx, ctx->rdx, 0);
  ON_RET("sceNpWebApi2GetHttpResponseHeaderValueLength", ON_WEBAPI2_ERROR_REQUEST_NOT_FOUND);
}

// =============================================================== libSceNpUniversalDataSystem
// UDS is a local event recorder (trophies and activity telemetry are derived from it): contexts,
// handles, events and property objects are inert tokens, PostEvent drops the event.
ON_FIXED(sceNpUniversalDataSystemInitialize, 0)
ON_FIXED(sceNpUniversalDataSystemDestroyContext, 0)
ON_FIXED(sceNpUniversalDataSystemRegisterContext, 0)
ON_FIXED(sceNpUniversalDataSystemDestroyHandle, 0)
ON_FIXED(sceNpUniversalDataSystemPostEvent, 0)
ON_FIXED(sceNpUniversalDataSystemDestroyEvent, 0)
ON_FIXED(sceNpUniversalDataSystemDestroyEventPropertyObject, 0)
ON_FIXED(sceNpUniversalDataSystemDestroyEventPropertyArray, 0)
// (int *context, userId, serviceLabel, options)
void shim_sceNpUniversalDataSystemCreateContext(GuestContext *ctx) {
  on_put32(ctx, ctx->rdi, on_token());
  ON_RET("sceNpUniversalDataSystemCreateContext", 0);
}
// (int *handle)
void shim_sceNpUniversalDataSystemCreateHandle(GuestContext *ctx) {
  on_put32(ctx, ctx->rdi, on_token());
  ON_RET("sceNpUniversalDataSystemCreateHandle", 0);
}
// (name, const Object *prop, Event **newEvent, Object **propPtr)
void shim_sceNpUniversalDataSystemCreateEvent(GuestContext *ctx) {
  on_put64(ctx, ctx->rdx, on_token());
  on_put64(ctx, ctx->rcx, on_token());
  ON_RET("sceNpUniversalDataSystemCreateEvent", 0);
}
// (Object **newObject) / (Array **newArray)
void shim_sceNpUniversalDataSystemCreateEventPropertyObject(GuestContext *ctx) {
  on_put64(ctx, ctx->rdi, on_token());
  ON_RET("sceNpUniversalDataSystemCreateEventPropertyObject", 0);
}
void shim_sceNpUniversalDataSystemCreateEventPropertyArray(GuestContext *ctx) {
  on_put64(ctx, ctx->rdi, on_token());
  ON_RET("sceNpUniversalDataSystemCreateEventPropertyArray", 0);
}
// Object setters (object, key, value): scalars are dropped.
ON_FIXED(sceNpUniversalDataSystemEventPropertyObjectSetInt32, 0)
ON_FIXED(sceNpUniversalDataSystemEventPropertyObjectSetInt64, 0)
ON_FIXED(sceNpUniversalDataSystemEventPropertyObjectSetUInt32, 0)
ON_FIXED(sceNpUniversalDataSystemEventPropertyObjectSetUInt64, 0)
ON_FIXED(sceNpUniversalDataSystemEventPropertyObjectSetFloat32, 0)
ON_FIXED(sceNpUniversalDataSystemEventPropertyObjectSetFloat64, 0)
ON_FIXED(sceNpUniversalDataSystemEventPropertyObjectSetBool, 0)
ON_FIXED(sceNpUniversalDataSystemEventPropertyObjectSetString, 0)
// (object, key, value, nested **valuePtr)
void shim_sceNpUniversalDataSystemEventPropertyObjectSetObject(GuestContext *ctx) {
  on_put64(ctx, ctx->rcx, on_token());
  ON_RET("sceNpUniversalDataSystemEventPropertyObjectSetObject", 0);
}
void shim_sceNpUniversalDataSystemEventPropertyObjectSetArray(GuestContext *ctx) {
  on_put64(ctx, ctx->rcx, on_token());
  ON_RET("sceNpUniversalDataSystemEventPropertyObjectSetArray", 0);
}
// Array setters (array, value)
ON_FIXED(sceNpUniversalDataSystemEventPropertyArraySetInt32, 0)
ON_FIXED(sceNpUniversalDataSystemEventPropertyArraySetFloat32, 0)
ON_FIXED(sceNpUniversalDataSystemEventPropertyArraySetBool, 0)
ON_FIXED(sceNpUniversalDataSystemEventPropertyArraySetString, 0)
// (array, value, nested **valuePtr)
void shim_sceNpUniversalDataSystemEventPropertyArraySetObject(GuestContext *ctx) {
  on_put64(ctx, ctx->rdx, on_token());
  ON_RET("sceNpUniversalDataSystemEventPropertyArraySetObject", 0);
}
void shim_sceNpUniversalDataSystemEventPropertyArraySetArray(GuestContext *ctx) {
  on_put64(ctx, ctx->rdx, on_token());
  ON_RET("sceNpUniversalDataSystemEventPropertyArraySetArray", 0);
}

// =============================================================== libSceNpTrophy2
// Trophies are local: contexts and handles succeed, the title has no trophy set to enumerate here
// (0 trophies), and the unlock callback is accepted but never fires. Unlocks themselves arrive
// through UDS (above) and are dropped.
void shim_sceNpTrophy2CreateContext(GuestContext *ctx) {  // (int *context, user, serviceLabel, options)
  on_put32(ctx, ctx->rdi, on_token());
  ON_RET("sceNpTrophy2CreateContext", 0);
}
void shim_sceNpTrophy2CreateHandle(GuestContext *ctx) {  // (int *handle)
  on_put32(ctx, ctx->rdi, on_token());
  ON_RET("sceNpTrophy2CreateHandle", 0);
}
ON_FIXED(sceNpTrophy2RegisterContext, 0)
ON_FIXED(sceNpTrophy2DestroyContext, 0)
ON_FIXED(sceNpTrophy2DestroyHandle, 0)
ON_FIXED(sceNpTrophy2RegisterUnlockCallback, 0)
ON_FIXED(sceNpTrophy2UnregisterUnlockCallback, 0)
// (context, handle, GameDetails *details (152 B), GameData *data (24 B))
void shim_sceNpTrophy2GetGameInfo(GuestContext *ctx) {
  on_zero(ctx, ctx->rdx, 152);
  on_zero(ctx, ctx->rcx, 24);
  ON_RET("sceNpTrophy2GetGameInfo", 0);
}
// (context, handle, offset, limit, details[], data[], uint32_t *count)
void shim_sceNpTrophy2GetTrophyInfoArray(GuestContext *ctx) {
  on_put32(ctx, ctx->r9, 0);
  ON_RET("sceNpTrophy2GetTrophyInfoArray", 0);
}

// =============================================================== libSceNpEntitlementAccess
ON_FIXED(sceNpEntitlementAccessInitialize, 0)
ON_FIXED(sceNpEntitlementAccessDeleteRequest, ON_ENTITLEMENT_ERROR_NOT_FOUND)
ON_FIXED(sceNpEntitlementAccessPollUnifiedEntitlementInfoList, ON_ENTITLEMENT_ERROR_NOT_FOUND)
ON_FIXED(sceNpEntitlementAccessPollServiceEntitlementInfoList, ON_ENTITLEMENT_ERROR_NOT_FOUND)
// (userId, serviceLabel, labels, labelCount, param, int64_t *requestId)
void shim_sceNpEntitlementAccessRequestUnifiedEntitlementInfoList(GuestContext *ctx) {
  on_put64(ctx, ctx->r9, 0);
  ON_RET("sceNpEntitlementAccessRequestUnifiedEntitlementInfoList", ON_NP_ERROR_SIGNED_OUT);
}
void shim_sceNpEntitlementAccessRequestServiceEntitlementInfoList(GuestContext *ctx) {
  on_put64(ctx, ctx->r9, 0);
  ON_RET("sceNpEntitlementAccessRequestServiceEntitlementInfoList", ON_NP_ERROR_SIGNED_OUT);
}

// =============================================================== libSceGameUpdate
ON_FIXED(sceGameUpdateInitialize, 0)
ON_FIXED(sceGameUpdateTerminate, 0)
ON_FIXED(sceGameUpdateDeleteRequest, 0)
void shim_sceGameUpdateCreateRequest(GuestContext *ctx) {  // () -> request id
  uint32_t id = 0x20000000u + (on_token() & 0xFFFFFu);
  ON_LOG("sceGameUpdateCreateRequest", id);
  ctx->rax = id;
  SHIM_RETURN();
}
// No update server reachable. The SDK has no network code of its own; the NP signed-out code is what
// a title already handles from the NP stack.
ON_FIXED(sceGameUpdateCheck, ON_NP_ERROR_SIGNED_OUT)

// =============================================================== libSceNpManager (3 missing)
ON_FIXED(sceNpCheckPremium, ON_NP_ERROR_SIGNED_OUT)        // (reqId, param, result): not PS Plus
ON_FIXED(sceNpCheckNpReachability, ON_NP_ERROR_SIGNED_OUT)
ON_FIXED(sceNpNotifyPremiumFeature, 0)

// =============================================================== libSceNpAuth
ON_FIXED(sceNpAuthGetAuthorizationCodeV3, ON_NP_ERROR_SIGNED_OUT)
ON_FIXED(sceNpAuthGetIdTokenV3, ON_NP_ERROR_SIGNED_OUT)

// =============================================================== libSceNpGameIntent
ON_FIXED(sceNpGameIntentReceiveIntent, ON_GAMEINTENT_ERROR_INTENT_NOT_FOUND)  // nothing launched the title
// (intentData, key, char *valueBuf, size_t bufSize)
void shim_sceNpGameIntentGetPropertyValueString(GuestContext *ctx) {
  if (ctx->rdx && ctx->rcx) memset(NPS5_G2H(ctx, ctx->rdx), 0, 1);
  ON_RET("sceNpGameIntentGetPropertyValueString", ON_GAMEINTENT_ERROR_VALUE_NOT_FOUND);
}

// =============================================================== libSceShare
ON_FIXED(sceShareInitialize, 0)
ON_FIXED(sceShareTerminate, 0)
ON_FIXED(sceShareFeaturePermit, 0)
ON_FIXED(sceShareFeatureProhibit, 0)

// =============================================================== libScePlayGo (optional chunks)
// The existing PlayGo module reports NOT_SUPPORT_PLAYGO for an installed (non-PlayGo) title; so do these.
ON_FIXED(scePlayGoGetSupportedOptionalChunk, ON_PLAYGO_ERROR_NOT_SUPPORT_PLAYGO)
ON_FIXED(scePlayGoGetOptionalChunk, ON_PLAYGO_ERROR_NOT_SUPPORT_PLAYGO)
ON_FIXED(scePlayGoPrefetchOptionalChunk, ON_PLAYGO_ERROR_NOT_SUPPORT_PLAYGO)
// (handle, uint16_t *chunkIds, numberOfEntries, uint32_t *outEntries)
void shim_scePlayGoGetInstallChunkId(GuestContext *ctx) {
  on_put32(ctx, ctx->rcx, 0);
  ON_RET("scePlayGoGetInstallChunkId", ON_PLAYGO_ERROR_NOT_SUPPORT_PLAYGO);
}

// =============================================================== dialogs (PlayGo, PlayerInvitation, WebBrowser)
// No dialog is ever shown: Open finishes at once with USER_CANCELED. State is per dialog.
static volatile int32_t g_playgo_dialog = ON_DIALOG_NONE, g_invite_dialog = ON_DIALOG_NONE;

void shim_scePlayGoDialogInitialize(GuestContext *ctx) {
  g_playgo_dialog = ON_DIALOG_INITIALIZED;
  ON_RET("scePlayGoDialogInitialize", 0);
}
void shim_scePlayGoDialogTerminate(GuestContext *ctx) {
  g_playgo_dialog = ON_DIALOG_NONE;
  ON_RET("scePlayGoDialogTerminate", 0);
}
void shim_scePlayGoDialogOpen(GuestContext *ctx) {
  if (g_playgo_dialog != ON_DIALOG_INITIALIZED && g_playgo_dialog != ON_DIALOG_FINISHED)
    ON_RET("scePlayGoDialogOpen", ON_DIALOG_ERROR_INVALID_STATE);
  g_playgo_dialog = ON_DIALOG_FINISHED;
  ON_RET("scePlayGoDialogOpen", 0);
}
void shim_scePlayGoDialogUpdateStatus(GuestContext *ctx) {
  ON_RET("scePlayGoDialogUpdateStatus", g_playgo_dialog);
}
// (PlayGoDialogResult *result): 0x28 bytes, CommonDialog result at +4 (shadPS4 playgo_dialog.h); 1 = USER_CANCELED
void shim_scePlayGoDialogGetResult(GuestContext *ctx) {
  if (ctx->rdi) {
    on_zero(ctx, ctx->rdi, 0x28);
    on_put32(ctx, ctx->rdi + 4, 1);
  }
  ON_RET("scePlayGoDialogGetResult", 0);
}

void shim_scePlayerInvitationDialogInitialize(GuestContext *ctx) {
  g_invite_dialog = ON_DIALOG_INITIALIZED;
  ON_RET("scePlayerInvitationDialogInitialize", 0);
}
void shim_scePlayerInvitationDialogTerminate(GuestContext *ctx) {
  g_invite_dialog = ON_DIALOG_NONE;
  ON_RET("scePlayerInvitationDialogTerminate", 0);
}
void shim_scePlayerInvitationDialogOpen(GuestContext *ctx) {
  if (g_invite_dialog != ON_DIALOG_INITIALIZED && g_invite_dialog != ON_DIALOG_FINISHED)
    ON_RET("scePlayerInvitationDialogOpen", ON_DIALOG_ERROR_INVALID_STATE);
  g_invite_dialog = ON_DIALOG_FINISHED;
  ON_RET("scePlayerInvitationDialogOpen", 0);
}
void shim_scePlayerInvitationDialogUpdateStatus(GuestContext *ctx) {
  ON_RET("scePlayerInvitationDialogUpdateStatus", g_invite_dialog);
}

ON_FIXED(sceWebBrowserDialogResetCookie, 0)
ON_FIXED(sceWebBrowserDialogOpenForPredeterminedContent, ON_DIALOG_ERROR_INVALID_STATE)  // no browser, no network

// =============================================================== libSceSystemService / libSceUserService
// void sceSystemServiceInitializePlayerDialogParam(SystemServicePlayerDialogParam *p): 0x40 bytes
// { u64 size; i32 mode; i32 userId; u64 targetAccountId; u8 reserved[40] } (AnyPS5 SceTypes.hpp)
void shim_sceSystemServiceInitializePlayerDialogParam(GuestContext *ctx) {
  if (ctx->rdi) {
    on_zero(ctx, ctx->rdi, 0x40);
    on_put64(ctx, ctx->rdi, 0x40);
    on_put32(ctx, ctx->rdi + 12, 0xFFFFFFFFu);
  }
  ON_LOG("sceSystemServiceInitializePlayerDialogParam", 0);
  SHIM_RETURN();
}
ON_FIXED(sceSystemServiceLaunchPlayerDialog, ON_SYSTEMSERVICE_ERROR_UNAVAILABLE)

// (userId, uint32_t *ageLevel): 0xFFFFFFFF = unrestricted (AnyPS5 libSceUserService)
void shim_sceUserServiceGetAgeLevel(GuestContext *ctx) {
  if (!ctx->rsi) ON_RET("sceUserServiceGetAgeLevel", ON_USERSERVICE_ERROR_INVALID_ARGUMENT);
  on_put32(ctx, ctx->rsi, 0xFFFFFFFFu);
  ON_RET("sceUserServiceGetAgeLevel", 0);
}

// =============================================================== libSceAcm
// The audio convolution/FFT service is a console DSP; there is no host implementation. Reporting a
// bad context makes a title fall back to its non-ACM path instead of consuming undefined output.
ON_FIXED(sceAcmContextCreate, ON_ACM_ERROR_BAD_CONTEXT)
ON_FIXED(sceAcmContextDestroy, ON_ACM_ERROR_BAD_CONTEXT)
ON_FIXED(sceAcmBatchWait, ON_ACM_ERROR_BAD_CONTEXT)
ON_FIXED(sceAcmBatchStartBuffers, ON_ACM_ERROR_BAD_CONTEXT)
ON_FIXED(sceAcm_ConvReverb_SharedInput, ON_ACM_ERROR_BAD_CONTEXT)
ON_FIXED(sceAcm_FFT, ON_ACM_ERROR_BAD_CONTEXT)
