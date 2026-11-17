# M2 online / system stub block

Branch `m2-hlestubs`. Goal: a PS5 title that imports the PSN/online/telemetry/system-service surface runs fully offline, as on a console that was never signed in and has no network.

Code: `pkg/runtime/modules/libSceNpOnlineStubs/` (hand-written, 125 stubs), `pkg/runtime/modules/libSceNpCppWebApi/` (generated, 998), binding `pkg/emitter/online_stubs.go` + `online_stubs_cppwebapi_gen.go`, generator `tools/gen_online_stubs.py`, fixture `pkg/emitter/testdata/online_stub_imports.txt` (library, NID, name: Sony API identifiers only), tests `pkg/emitter/online_stubs_test.go` and `tests/online/online_unit.c` (`tools/online_unit.sh`).

## Policy
* initialise / terminate / destroy / delete / abort / set-option: succeed (0), no-ops.
* objects that are only local state (UDS events and property objects, Http2 templates and requests, Trophy2 contexts and handles, WebApi2 library context, push filters, GameUpdate request): succeed and return an inert token (monotonic, never a pointer, never dereferenced).
* anything that needs an account or a server: the Sony signed-out / not-signed-in / network code; out parameters that are non-NULL are set to 0 (NULL stays NULL); callbacks are accepted and never invoked.
* trophy unlocks arrive as UDS events and are dropped locally (PostEvent returns 0); Trophy2 enumerates 0 trophies.
* every stub logs once at info (`offline stub <NID name> -> 0x...`, subsystem `hle`).
* pointers are translated only with `NPS5_G2H` (`on_put32/on_put64/on_zero` in `nps5_online.h`).

## Error values (all as returned in eax)
| value | meaning | used by | source |
|---|---|---|---|
| 0x80550006 | NP SIGNED_OUT | CheckPremium, CheckNpReachability, NpAuth V3, EntitlementAccess Request*, GameUpdateCheck | shadPS4 np_error.h; AnyPS5 libSceNpManager |
| 0x80431063 / 0x80431065 | HTTP NETWORK / BEFORE_SEND | Http2 send/read/wait / status, headers, length | shadPS4 http_error.h; AnyPS5 libSceHttp2 |
| 0x80553407 / 0x80553406 / 0x80553405 | WEBAPI2 NOT_SIGNED_IN / REQUEST_NOT_FOUND / USER_CONTEXT_NOT_FOUND | NpWebApi2 | shadPS4 np_error.h |
| 0x80552907 | NP WEBAPI NOT_SIGNED_IN | every NpCppWebApi call that would talk to the server | shadPS4 np_error.h |
| 0x817D0005 | ENTITLEMENT_ACCESS NOT_FOUND | Entitlement Poll/Delete | shadPS4 np_entitlement_access_error.h |
| 0x80553806 / 0x80553807 | GAME_INTENT INTENT/VALUE_NOT_FOUND | GameIntent | AnyPS5 libSceNpGameIntent |
| 0x80B2000E | PLAYGO NOT_SUPPORT_PLAYGO | optional chunk calls (matches the existing PlayGo module) | shadPS4 playgo_types.h |
| 0x80B80006 | COMMON_DIALOG INVALID_STATE | dialog Open before Initialize, WebBrowser Open | existing libSceInvitationDialog |
| 0x80A10002 | SYSTEM_SERVICE unavailable | LaunchPlayerDialog | AnyPS5 libSceSystemService |
| 0x80960005 | USER_SERVICE INVALID_ARGUMENT | GetAgeLevel(NULL) | shadPS4 userservice_error.h |
| 0x81940002 | ACM BAD_CONTEXT | all of libSceAcm | AnyPS5 libSceAcm |

AnyPS5 and shadPS4 disagree on EntitlementAccess and WebApi2 values; shadPS4's tables (derived from SDK headers) were used.

## libSceNpCppWebApi
The title bundles `libSceNpCppWebApi.prx`; the loader skips it on purpose (`IsSystemStubModule`), so the 998 exports the eboot imports had no code. They are bound by name (the import name is the Itanium-mangled symbol, so plain-name lookup and the NID derived from it both work) or, for the 41 whose name is unknown, by NID. C++ signatures are not recoverable from mangling, so the generator picks the return from the demangled (c++filt) function name: constructors/destructors return `this` (an `IntrusivePtr<T>` constructor also zeroes its single pointer), getters/setters/operators/`size`/`begin`/`end`/`*IsSet`/`terminate` return 0, everything else (`create`, `initialize`, `start`, `finish`, `*Api` calls) returns 0x80552907. Non-IntrusivePtr constructors do not initialise the object (size unknown); a title that gets that far has already ignored the failed `initialize`.

## Out of scope here
SaveData, AudioOut2, Ampr/Apr, Ajm, VideoOut gaps, kernel gaps, Pad extras, Agc/AgcDriver (m2-hlesys and later). Acm is stubbed as unavailable, not emulated.
