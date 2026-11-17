// SPDX-License-Identifier: GPL-2.0-or-later
//
// Offline-console stubs for the online / system-service block (Http2, NpWebApi2, UDS, Trophy2,
// NpEntitlementAccess, GameUpdate, NpManager/NpAuth checks, GameIntent, Share, PlayGo optional
// chunks, dialogs, Acm) and for libSceNpCppWebApi (generated, modules/libSceNpCppWebApi).
// See docs/m2/online_stubs.md.
//
// Behaviour model: a console that has never been signed in to PSN and has no network.
//   * initialise / terminate / destroy / delete / abort / set-option calls succeed (no-ops);
//   * create calls that only build local objects (UDS events, Http2 templates, trophy contexts)
//     succeed and hand back an inert token that is never dereferenced;
//   * everything that needs an account or a server returns the Sony "signed out" / "not signed
//     in" / "network" error code, with out parameters set to a defined value (0) when the
//     out-pointer is non-NULL;
//   * callbacks are accepted and never invoked;
//   * every stub logs once, at info level, with its NID name (subsystem hle).
//
// Error values: shadPS4 src/core/libraries/np/np_error.h, network/http_error.h,
// np/np_entitlement_access_error.h, playgo/playgo_types.h (GPL-2.0-or-later); AnyPS5
// core/libs/prx/* (GPL-2.0-only) for Acm, GameUpdate, GameIntent, SystemService, UserService
// (behaviour model and error values; the code here is independent).
#ifndef NPS5_ONLINE_H
#define NPS5_ONLINE_H

#include "recomp_runtime.h"
#include "nps5_thunk.h"
#include "nps5_diag.h"
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- error codes (all as int32; the values are the Sony SDK's)
enum {
  ON_NP_ERROR_SIGNED_OUT = (int32_t)0x80550006u,          // shadPS4 np_error.h ORBIS_NP_ERROR_SIGNED_OUT
  ON_HTTP_ERROR_NETWORK = (int32_t)0x80431063u,           // http_error.h ORBIS_HTTP_ERROR_NETWORK
  ON_HTTP_ERROR_BEFORE_SEND = (int32_t)0x80431065u,       // http_error.h ORBIS_HTTP_ERROR_BEFORE_SEND
  ON_WEBAPI2_ERROR_USER_CONTEXT_NOT_FOUND = (int32_t)0x80553405u,
  ON_WEBAPI2_ERROR_REQUEST_NOT_FOUND = (int32_t)0x80553406u,
  ON_WEBAPI2_ERROR_NOT_SIGNED_IN = (int32_t)0x80553407u,  // np_error.h ORBIS_NP_WEBAPI2_ERROR_*
  ON_WEBAPI_ERROR_NOT_SIGNED_IN = (int32_t)0x80552907u,   // np_error.h ORBIS_NP_WEBAPI_ERROR_NOT_SIGNED_IN
  ON_ENTITLEMENT_ERROR_NOT_FOUND = (int32_t)0x817D0005u,  // np_entitlement_access_error.h
  ON_GAMEINTENT_ERROR_INTENT_NOT_FOUND = (int32_t)0x80553806u,  // AnyPS5 libSceNpGameIntent
  ON_GAMEINTENT_ERROR_VALUE_NOT_FOUND = (int32_t)0x80553807u,
  ON_PLAYGO_ERROR_NOT_SUPPORT_PLAYGO = (int32_t)0x80B2000Eu,    // playgo_types.h
  ON_ACM_ERROR_BAD_CONTEXT = (int32_t)0x81940002u,        // AnyPS5 libSceAcm
  ON_SYSTEMSERVICE_ERROR_UNAVAILABLE = (int32_t)0x80A10002u,    // AnyPS5 libSceSystemService
  ON_USERSERVICE_ERROR_INVALID_ARGUMENT = (int32_t)0x80960005u, // userservice_error.h
  ON_DIALOG_ERROR_INVALID_STATE = (int32_t)0x80B80006u,   // as modules/libSceInvitationDialog
};

// Common dialog status values returned by *UpdateStatus.
enum { ON_DIALOG_NONE = 0, ON_DIALOG_INITIALIZED = 1, ON_DIALOG_RUNNING = 2, ON_DIALOG_FINISHED = 3 };

// ---- logging: once per stub, with the NID name. Expands inside the shim body.
#define ON_LOG(name, rc)                                                                       \
  do {                                                                                         \
    static int on_once_;                                                                       \
    if (!on_once_) {                                                                           \
      on_once_ = 1;                                                                            \
      NPS5_LOG(NPS5_S_HLE, NPS5_L_INFO, "offline stub %s -> 0x%08x (never signed in, no network)", \
               (name), (unsigned)(uint32_t)(int32_t)(rc));                                     \
    }                                                                                          \
  } while (0)

// Return an int32 (sign-extended into rax, as the lifted callers read eax).
#define ON_RET(name, rc)                     \
  do {                                       \
    ON_LOG(name, rc);                        \
    ctx->rax = (uint64_t)(int64_t)(int32_t)(rc); \
    SHIM_RETURN();                           \
  } while (0)

// Return a 64-bit value (this pointers, 64-bit ids).
#define ON_RET64(name, v)                    \
  do {                                       \
    ON_LOG(name, 0);                         \
    ctx->rax = (uint64_t)(v);                \
    SHIM_RETURN();                           \
  } while (0)

// void shim_<fn>(ctx) returning a constant.
#define ON_FIXED(fn, rc) \
  void shim_##fn(GuestContext *ctx) { ON_RET(#fn, (rc)); }

// ---- guest memory out parameters (NULL stays NULL; only translated through NPS5_G2H)
static inline void on_put32(GuestContext *ctx, uint64_t g, uint32_t v) {
  if (g) memcpy(NPS5_G2H(ctx, g), &v, sizeof v);
}
static inline void on_put64(GuestContext *ctx, uint64_t g, uint64_t v) {
  if (g) memcpy(NPS5_G2H(ctx, g), &v, sizeof v);
}
static inline void on_zero(GuestContext *ctx, uint64_t g, size_t n) {
  if (g) memset(NPS5_G2H(ctx, g), 0, n);
}

// Inert token: a small positive id that is never a valid pointer or object. Monotonic, per process.
uint32_t on_token(void);

#ifdef __cplusplus
}
#endif
#endif
