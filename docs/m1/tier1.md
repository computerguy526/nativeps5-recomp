# M1 tier-1 miss fallback (SPEC 3.4, 3.10, 8.2)

Branch `m1-tier1` (on `m1-memory`). A guest transfer to an address with no dispatch entry used to end in
`FATAL: Unresolved indirect jump/call to 0x...`. Now the runtime translates a bounded fragment starting at that
address with the same lifter, loads it and continues. What static discovery still misses (docs/m1/discovery.md,
section 4: tail calls through loaded pointers, vtable thunks, pointer-to-member calls, asm stubs without FDEs) is
served at run time; the next translation folds it in with `-seeds-from`.

## Pieces

| piece | where |
|---|---|
| slow path `recomp_dispatch_miss` (implements `recomp_dispatch_api.h`) | `pkg/runtime/core/nps5_tier1.c` |
| fragment translator `ps4-recomp fragment` | `pkg/cli/fragment.go`, `pkg/emitter/fragment.go` |
| translate-time records (`tier1` manifest section, `aot_funcs.bin`, `nps5_tier1_info.c`), `-seeds-from` | `pkg/cli/tier1.go` |
| diagnostics: structured crash fields, fragment pc maps | `pkg/runtime/core/nps5_diag.[ch]` |
| test | `tools/tier1_test.sh`, `tests/ps5/tier1/` |

`recomp_dispatch_api.h` is copied byte for byte from `m1-flatstack` (acb450e) so both branches add the same file;
this branch defines `recomp_dispatch_miss` strongly (the flat branch's weak default returning 0 loses to it) and
`recomp_code_generation` weakly (constant 0: tier 1 never unloads code). On this branch `recomp_dispatch` (C) calls
`recomp_dispatch_miss(ctx, target, RECOMP_MISS_JUMP)` and enters the result; the asm slow path
(`DISPATCH_EXTERNAL`) reaches it through `recomp_dispatch_external`. On the flat branch the call/return model calls it
directly with RET/CALL/JUMP/RESUME and enters the returned address as the contract says.

## Runtime flow (`recomp_dispatch_miss(ctx, rip, reason)`)

1. Already published by another thread? Return it (`raced`).
2. `NPS5_MISS=fatal` (developer builds, SPEC 3.4): fail with kind `miss_fatal`. No tier-1 configuration in the build:
   `disabled`.
3. Validate: `rip` in the 48-bit space and inside an executable segment of a registered guest module
   (`recomp_module_text_contains`); else `outside_guest_code` (data jump, null/wild pointer, guest-generated code).
4. Dedupe on (generation, rip): the first thread becomes the compiler, others block on a condition variable and
   get the same result or the same structured error (`waited` / `failed`).
5. No lock is held from here: disk cache lookup, else a compile slot (at most `NPS5_TIER1_MAX_CONCURRENT`), then
   `posix_spawn` of `<translator> fragment --manifest <lifter_manifest.json> --entry 0x<rip> --out <work>`
   (own process group, empty signal mask, stdout/stderr into `<work>/log.txt`, killed after the time cap).
   The result is `<work>/result.txt` (`key=value`: `ok`, `dylib`, `funcs`, `insts`, or `kind`, `message`).
6. The dylib is stored in the cache (dylib rename, then `.meta` with ABI, size, FNV-1a hash), the cache is trimmed.
7. `dlopen(RTLD_NOW|RTLD_LOCAL)` (a missing symbol or a signature problem fails here, `load_failed`), `dlsym`
   `nps5_frag_desc`, check magic/ABI/entry count, every entry inside executable guest code, `rip` among them.
8. The dylib's pc map is registered with diagnostics (host range + table), so crash.txt maps host PCs in fragments
   to guest RIP/function and `recomp_pc_in_guest_code` is true for them (EH frame walking works through them).
9. Publish: one release store per dispatch slot, only into empty slots (an AOT or earlier entry is never replaced),
   the requested rip last, under a publish mutex (readers are lock-free). Return the entry; the caller continues
   at `rip`. Nothing is ever unloaded, so no shadow continuation can reference freed code.

Caps (env, defaults): `NPS5_TIER1_TIMEOUT_MS` 120000 (whole translator run; also the clang/link cap inside it),
`NPS5_TIER1_MAX_CONCURRENT` 2, `NPS5_TIER1_MAX_FRAGMENTS` 1024 per run, `NPS5_TIER1_MAX_DYLIB_MB` 64,
`NPS5_TIER1_CACHE_MB` 512 (oldest dylibs removed after each store), fragment size: `--max-insts` 20000 and
`--max-funcs` 16 (the root function alone may be up to 4x the instruction cap).
`NPS5_TIER1_CACHE` relocates the cache (default `<translate out dir>/tier1_cache`).

## Fragment translator (`ps4-recomp fragment --manifest M --entry X[,Y] [--out D] [--max-insts N] [--max-funcs N] [--timeout-ms N]`)

The manifest is `lifter_manifest.json`; a full translation adds a `tier1` section: input ELF path/sha256/size/mtime,
app dir, resolved `addr_mode`, `tso`, `allow_unsupported`, `all_symbols`, `call_model`, the translator path and its
sha256, `aot_funcs.bin` (every guest entry the AOT build translated), out dir. The fragment mode

* refuses to run when the translator binary is not the one that built the title (`translator_mismatch`) or the input
  ELF changed (`module_changed`), because a different lifter or layout would emit code for a different contract;
* re-runs the load sequence of a translation (load ELF, memory plan with the recorded resolved mode, companion
  modules, TLS layout, relocations) so guest addresses are identical (identity mode places the image at its bias);
* checks `X` is executable guest code (`outside_code`), decodes the function at `X` (`undecodable`), then adds the
  not-yet-translated functions it directly reaches (BFS over the decoder's discovered call/tail targets, minus
  `aot_funcs.bin`) until the caps; anything else stays a dispatch call, and if it too is missing it misses again;
* lifts with a lifter whose known-function set is exactly the fragment, so every other call/jump lowers to
  `_recomp_indirect_call/jump` (dispatch), never to a symbol the dylib lacks; same TSO/unsupported/address-mode flags;
  an unsupported instruction fails the fragment (`unsupported`) unless the title was built with `-allow-unsupported`;
* instruments the assembly with the pc map (as `diag.ScanAsm` does for the full build), writes the descriptor
  (`frag_desc.c`: entries, pc map and landing-pad table accessors), assembles with clang (`-target arm64-apple-darwin
  -mcpu=apple-m1`, identity flag when needed), links `-dynamiclib -undefined dynamic_lookup -Wl,-bind_at_load` (the
  dispatch trampolines and helpers bind to the executable) and `codesign --force --sign -`.

The fragment pipeline duplicates the first steps of `Execute` instead of refactoring them, to stay out of the way of
the emitter/CLI rewrite (m2-emitstream); `TestFragment*`/`tools/tier1_test.sh` fail if the two diverge. Fragment
emission calls `AsmEmitter.EmitFragment` (new file) which wraps `emitSingleAsmChunk`; when the streaming emitter
lands, `EmitFragment` is the one function to move onto it.

## Miss log, seeds, failures

`tier1_misses.jsonl` in the run bundle, one line per miss:
`{t, rip, module, offset, reason (ret|call|jump|resume), outcome, kind, compile_ms, fragment_bytes, entries, insts,
generation, tid, detail}`. `outcome`: `compiled`, `cache_hit`, `waited`, `raced`, `failed` (then `kind` and `detail`).
Also logged on subsystem `tier1` (`NPS5_LOG=tier1=debug`).

`ps4-recomp -seeds-from <path> ...`: `<path>` is a `tier1_misses.jsonl`, a run bundle, or a runs directory (all bundles
below it); comma separated for several. Misses that were served (`compiled`, `cache_hit`, `waited`) become CFG entry
points (those outside executable code are reported and ignored). Addresses are final guest addresses, so a seed file
is valid for builds with the same addr mode and input (a different layout puts the seeds outside code or on a
non-boundary; the translator warns for the former).

Failures never continue silently. The runtime records `nps5_diag_set_tier1_failure` and returns 0; the caller aborts,
and `crash.txt` has `reason: tier-1 miss fallback failed ...`, `tier1_kind`, `tier1_guest_rip`, `tier1_module`,
`tier1_module_offset`, `tier1_miss_reason`, `tier1_detail`, plus the usual guest registers/stack. Kinds:
`outside_guest_code`, `undecodable`, `unsupported`, `cap_exceeded`, `timeout`, `compile_failed`, `translator_mismatch`,
`module_changed`, `bad_manifest`, `translator_spawn_failed|crashed|failed`, `load_failed`, `bad_descriptor`,
`publish_failed`, `cache_write_failed`, `miss_fatal`, `disabled`. A failed compile keeps its work dir
(`log.txt`, the `.s`, `result.txt`) and the path is in `tier1_detail`.

## Results

`tools/tier1_test.sh` (payload SDK `prospero-clang -O1`, both address modes, runs only through
`tools/run_sandboxed.sh`): `t_tier1_hidden.c` has three functions in a custom section as naked asm (no FDE, no FUNC
symbol, no relocation, no call), reached through the address of a rel32 offset table plus a volatile-read offset;
`hidden_fn` also calls an ordinary AOT function (a fragment calling back into AOT through dispatch), and four
threads race to call `hidden_thr` first.

| check | result |
|---|---|
| run 1 output equals expected, 3 `compiled` records (about 0.5 to 0.8 s each: Go load + lift + clang + link + codesign) | pass (base, identity) |
| 4-thread race on one address | 1 `compiled` + 3 `waited` |
| run 2 | 3 `cache_hit`, 0 compiles |
| damaged cached dylib (appended bytes) | detected by size/hash, recompiled, never loaded |
| `-seeds-from <run 1 bundle>` retranslation | 3 seeds, run 3 has no miss file at all, same output |
| data address / non-decoding bytes / null pointer | crash.txt with `tier1_kind` `outside_guest_code` / `undecodable` / `outside_guest_code`, miss log has the failure |
| `NPS5_MISS=fatal`, `NPS5_TIER1_TIMEOUT_MS=1`, `NPS5_TIER1_MAX_FRAGMENTS=1`, `NPS5_TIER1_CACHE_MB=0` | `miss_fatal`, `timeout`, `cap_exceeded` (after 1 compile), at most 1 cached dylib |

## Flat call model (integration)

The fragment is lifted in the call model recorded by the translator (`tier1Info.CallModel`, the lifter's model: flat by default);
the fragment mode refuses only a real mismatch (an unknown model, another translator binary, a changed ELF). `x87`, `gpu` and
`exclude_modules` are recorded too, so the fragment reproduces the title's lowering and module set. `Tier1ABI`/`NPS5_FRAG_ABI` is 2.

* `recomp_dispatch_miss` returns the **jump entry** in the flat model (`+RECOMP_FLAT_JUMP_ENTRY_OFFSET`); the C `recomp_dispatch` subtracts
  it again because `recomp_enter_guest` takes the symbol. The guest return address at a call entry is in x7 (the header in this tree is the
  final one; tier 1 no longer carries a copy).
* The requested address is also published with `recomp_cont_register` (flat only: a nested function body cannot be entered by address). The
  fragment's `__nps5_pads` table (landing pads; in the flat model every block and return site) is registered through the same continuation
  table, which `nps5_eh.c` and the RET/RESUME slow paths consult, so C++ exceptions and longjmp reach into tier-1 code. An AOT entry for the same
  address wins.
* Before a fragment is unloaded (`dlclose` after a rejected descriptor) `recomp_shadow_invalidate` is called for its guest-code range. Nothing is
  unloaded after publishing.
* `EmitFragment` uses the lean emission of `asm_chunk.go` (the same code path as a full translation) with the compact pc map; the descriptor's
  pc-map accessor returns bytes and `nps5_diag_register_fragment` indexes the records like the main image's.

## Gaps

* Fragment entries are registered as function entries (`recomp_register_fn` semantics); a non-function continuation (return site, landing pad) lands in
  the fragment of its function and is served as an entry of it (flat: also through `recomp_cont_register`).
* The decode check is the translator's: a mid-instruction address that happens to decode is translated. The runtime
  does not check against the AOT pc map.
* No test throws a C++ exception into a fragment (the hidden test functions have no unwind info by design); the pad registration is exercised only by the load path.
* No invalidation: guest writes to executable pages, module unload and permission changes are not tier 1's yet
  (SPEC 3.10 rejects those transitions explicitly until tier 2); code is never unloaded.
* The fragment pipeline is process-per-miss (a few hundred ms to about a second including loading the ELF); a long
  lived translator service or a cached loaded image would cut it. A title with a 300 MB ELF pays its load each time.
* A cached dylib is validated by size and hash, not by a signature check; the cache is trusted like the build output.
* Misses taken while another compile holds all slots wait; there is no per-miss priority and no background
  pre-compile of likely successors.
