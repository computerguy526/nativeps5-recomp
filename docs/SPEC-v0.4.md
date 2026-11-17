# nativeps5: PS5 games as native Apple Silicon apps by static recompilation

As-built spec, v0.4, 2026-11-14. Status: M0 and M1 largely met, M2 partly met, M3 in progress. This is the current spec; `SPEC.md` (v0.3) is the design it revises.

Citations are to files in this repo (`docs/...`) and to out-of-tree notes by path (`STATUS.md`, `work/...`, `anyps5-arm64/docs/...`). Numbers are quoted from those documents; where a document gives a commit or machine, it is kept. **(unverified)** marks anything not confirmed by an as-built document or re-measured here.

---

## 1. What changed since v0.3

v0.3 was a design with nothing built. v0.4 records what was built and merged to `main` at 41f2376 (`STATUS.md`). The main reversals: the guest address mode is now **base** (host = guest + 2^40 bias), not identity, with identity kept as a per-title optimisation that the translator refuses when a title hardcodes addresses inside the GPU carveout. The call/return model keeps host `bl`/`ret` with a flat host stack and a per-thread shadow return stack; x28 stays the memory base and the shadow pointer sits in x29. x87 defaults to full 80-bit precision through SoftFloat instead of the inherited double model. New since v0.3: a tier-1 miss recompiler, a run-bundle diagnostics layer, a 240-case memory adversarial suite, an instruction oracle built from FEX unit tests, a loader that accepts PS5 SELF and dynamic tables, and AnyPS5's AGC driver on KosmicKrisp, which renders Calculator3D with frame 0 pixel-identical to the x86 reference. Homebrew Syobon Action and Greenhollow run natively. The retail target PPSA30552 has reached UE log output but no frame yet (`STATUS.md`).

---

## 2. As built, by spec section

### 2.1 Architecture (SPEC §2)

**Decision (as built).** The shape holds. A Go translator (`ps4-recomp`, `pkg/...`) lifts x86-64 to arm64 assembly in chunks; clang assembles the chunks in a bounded worker pool and ld links them into dylibs. A C runtime (`pkg/runtime/core`) hosts the code, loads guest images and runs HLE. HLE is arm64 code reached through generated thunks (`pkg/thunkgen`). Tier-1 miss recompilation is in the runtime. GPU is AnyPS5's AGC driver and RDNA recompiler on KosmicKrisp. Every translate and run writes a run bundle under `runs/` (`docs/m1/diag.md`).

**Evidence.** Syobon eboot asm: 119.7 MB before the streaming and lean changes, 49.1 MB lean, with 13.0 MB of objects (`docs/m2/emit-size.md`). The 2.5 M-instruction retail eboot emits 187.9 MB lean asm (same doc).

**Deviations from v0.3.** Base mode is the default (§2.7). No `.app` bundle, launcher or profile yet; output is a directory run through `tools/run_sandboxed.sh`.

**Open items.** `.app` packaging, launcher and profiles (M4). No x86-64 CI runner (SPEC §1 says none exists; no doc records one).

### 2.2 CPU and lifter (SPEC §3.1, §3.8, §3.10)

**Decision (as built).** Fork of PS4Recomp's lifter, extended. Guest memory operands go through one emitter (`pkg/lifter/arm64_lifter_mem.go`, `gm()`), with both base and identity lowerings (`-addr-mode base|identity|auto`). Unsupported instructions are translate-time errors; `-allow-unsupported` is bring-up only (`docs/m1/lifter.md`, STATUS). The call model is `-call-model flat|nested`, flat by default. PF and AF are lazy context bytes with liveness passes; DF is a context byte that every string op reads (`docs/m1/fex-mapping.md`, implementation status). SYSCALL lowers to `shim_raw_syscall` (`docs/m1/lifter.md`).

**Evidence.**
- Census: Syobon emitted stubs went 5,265 (0.92%) to 2, a PSHUFW (MMX) form; PS5 hello_cxx 101 to 0 (`docs/m1/lifter.md`). The analyzer's `unsupported` count (28,534) was a stale hand list and is not a census (`work/s2/REPORT.md`).
- FEX unit-test oracle, 2,291 tests, full rerun on main 2104899: 1,465 PASS, 230 FAIL, 596 SKIP, up from 1,351 / 290 / 650 (`docs/m1/fex-oracle.md`).
- Call/return subset (492 tests): 403 PASS, 41 SKIP, 48 FAIL, identical in both call models (`docs/m1/flatstack.md` §8).
- AVX2 gathers, SHA-NI and several SSE/MMX forms added (`docs/m1/lifter.md`, m2-gamelift).
- Emitted asm fell from 182 to 75 bytes per guest instruction (lean) (`docs/m2/emit-size.md`).
- x87 full-precision mode: 91 native cases in `pkg/lifter/arm64_x87f80_test.go`; `t_x87` passes unchanged in f80 mode, 36/36 (`docs/m2/x87-f80.md`).

**Deviations from v0.3.** L1 (call model) and L5 (translate-time unsupported errors) are done. L2 is done per mode, but base is the default, so x28 remains a memory base. L3 (miss path) is done via tier 1. L4 (TSO as structured emission) is not confirmed: the TSO rewriter still works on text forms (`docs/m1/lifter.md`) **(unverified)**. L6 is partly done (SELF magic, dynamic tags; `docs/m1/loader.md`). L7 is not done: the Bloodborne patch table still exists (`docs/m1/memory.md` open item 8). PF/AF/DF, which v0.3 §3.10 called required, are now modelled, except BCD, which stays a trap.

**Open items.** Vector scratch allocation is not explicit (`docs/m1/lifter.md`, Gaps). No instruction-level differential suite on x86 CI. Remaining FEX FAIL classes (`docs/m1/fex-oracle.md`, "Top failing mnemonics").

### 2.3 Code discovery (SPEC §3.2)

**Decision (as built).** Seeds are entry, exports, FDE starts, relocation and dynsym targets, LSDA landing pads and call-return sites. Jump tables are recovered when bounded; the rest become dispatch-mediated indirect jumps. Anything still missed is found at run time by tier 1 (§2.5).

**Evidence (`docs/m1/discovery.md` §2–4).** Syobon: 6,925 FDEs, 6,931 functions reached, 0 seed decode errors, 0 mid-instruction entries. hello_cxx: 2,253 FDEs, 3,857 functions. Jump-table recovery on Syobon is 584 of 1,638 indirect jumps (35.7%), but most unresolved sites are not switches. Classified unresolved: 752 GOT tail calls, 94 vtable or callback tails, 208 `jmp reg` with a parameter or call-result register. The S2 figure of 42% (`work/s2/REPORT.md`) predates the loader fix that changed reachability from 5,246 to 6,931 functions. The S2 report also found the testbed `libc.prx` hollow (0 FDEs).

**Deviations from v0.3.** Bundled `-fno-plt` GOT imports are now given synthesized stubs, which v0.3 did not spell out (`docs/m1/loader.md`).

**Open items.** Retail coverage is incomplete: 2.5 M of about 21.6 M instructions were found in PPSA30552 (`docs/m2/emit-size.md`). Functions with no FDE, symbol or relocation are found only at run time. Data-class GOT imports still resolve to `shim_unresolved_stub` (`docs/m1/loader.md`, Gaps).

### 2.4 Calls, returns and the host stack (SPEC §3.3)

**Decision (as built).** The guest stack is authoritative. No recompiled function keeps a host frame across guest code. CALL keeps `bl` and RET keeps `ret` (S2 variant B3), so the host return predictor stays paired. The callee's call entry pushes a shadow entry `{guest return address, guest RSP at entry, host continuation}`; the RET path verifies it. On mismatch, the continuation is looked up by address (continuation table, dispatch table or tier 1). The shadow pointer lives in x29. x27 is the FS base and x28 the memory base (`docs/m1/flatstack.md` §2). Fiber identity is structural: each GuestContext owns its shadow stack (§5 of the same doc). A code-generation change poisons entries through `recomp_shadow_invalidate`.

**Evidence.** Callbench on M4 Pro (`docs/m1/flatstack.md` §8, min/median, nested vs flat): fib(38) +1.4% / +2.1%; leaf -1.8% / +5.3%; indirect +8.6% / +4.6%; recursion 20,000 deep +3.3% / +7.2%; recursion 100,000 deep (past the shadow stack) +164%; shim call (`abs`) +22% / +19%; qsort comparator callbacks +6.8% / +3.3%. The shadow stack compacts at about 32,768 entries; only deeper recursion pays a slow return (about 25 ns). t_callmodel passes in flat mode (34/36 overall) and fails in nested mode (32/36).

**Deviations from v0.3.** v0.3 wanted no host `bl`; the as-built flat model keeps `bl`/`ret` as S2 recommended. The shadow pointer is in x29, not in x28 as v0.3 §3.3 proposed. The 5% budget is met for fib and leaf calls but not for shim calls or deep recursion.

**Open items.** The shadow stack is not refilled after compaction. `GuestContext sub = *ctx` in `ps4_font.c` shares a shadow stack with its original, which is stale after the callback. x29 breaks frame-pointer samplers such as Instruments (in-process walkers are handled).

### 2.5 Miss fallback (SPEC §3.4)

**Decision (as built).** Tier 1 is implemented. `recomp_dispatch_miss` (`pkg/runtime/core/nps5_tier1.c`) runs `ps4-recomp fragment`, which lifts a bounded fragment with the title's recorded lowering (call model, x87, addr mode, exclusions), assembles and links a dylib, dlopens it and registers its entries. Misses are logged, and `-seeds-from <bundle>` folds them into the next translate (`docs/m1/tier1.md`). Tier 2 (dynamic translator) is not built.

**Evidence (`docs/m1/tier1.md` Results).** A hidden naked-asm function reached through a rel32 table: first run compiles it (3 `compiled` records, about 0.5–0.8 s each); a 4-thread race yields 1 compile and 3 waits; the second run hits the cache with 0 compiles; a damaged cached dylib is detected by size and hash and rebuilt. Bad targets give `tier1_kind` `outside_guest_code` or `undecodable` in crash.txt. Caps are `NPS5_TIER1_TIMEOUT_MS`, `NPS5_TIER1_MAX_FRAGMENTS` and `NPS5_TIER1_CACHE_MB`; each produces a logged outcome.

**Deviations from v0.3.** Fragments are registered as function entries. A return site or landing pad that is not a function entry is served through its function's fragment (`docs/m1/tier1.md`, Gaps). v0.3 §3.10 asked for entry at a validated instruction boundary; that is not shown. Code is never unloaded (`recomp_code_generation` is a constant 0).

**Open items.** Tier 2 (R9). Invalidation of translations when guest executable pages change is not built; §3.10 requires it, or rejection of the transition. Run-time coverage of retail indirect transfers (§2.3) is unmeasured.

### 2.6 Memory ordering (SPEC §3.5)

**Decision (as built).** Not stated as one decision in the as-built docs. The TSO rewriter exists and the FEX harness runs with "TSO on" (`docs/m1/fex-oracle.md`), with a `-tso=false` switch. Which access classes are ordered and by which mapping is **(unverified)**.

**Evidence (`docs/m0/S1.md` §3, M4 Pro, clang 21).** Cost: LDAPR/STLR is 1.0x on pointer-chasing and 1.8–2.5x on streaming loads and stores. A `dmb ish` around each access costs 3.4x on loads and 30–87x on stores. Litmus: plain LDR/STR violated message passing in 2 of 40M runs (the TSO-forbidden outcome). STLR+LDAPR and dmb ish showed 0. S1 found no public interface to enable hardware TSO (`ACTLR.TSOEN`) (§4 of the same doc).

**Deviations from v0.3.** v0.3 wanted the conservative full-barrier baseline as the release default. The as-built default is not documented.

**Open items.** Confirm the default mapping and which access classes (vector, x87, string, stack) it covers. Run litmus tests on emitted code. Add the `tso=strict|relaxed` profile modes that §3.5 describes; none is in the as-built docs.

### 2.7 Guest memory model (SPEC §3.6, R-ADDR)

**Decision (as built).** Base mode is the default: `host = guest | 1<<40`, one 1 TiB reservation, with all 40 guest address bits backed by memory the runtime owns (`docs/m1/memory.md` §1–2). Guest layout follows the console: executable at 0x40_0000, unhinted mappings from 0x2_0000_0000, application area up to 0xFC_0000_0000. Identity mode (guest = host, reservation exactly 0x70_0000_0000–0xFC_0000_0000) is an optional per-title mode. `pkg/guestmem.ScanTitle` selects it with `auto`, and refuses it with `identity` when the title contains page-aligned constants in [0x1_8000_0000, 0x70_0000_0000) (`docs/m1/memory.md` §4). Pages are 16 KiB. The HLE pointer boundary is `NPS5_G2H` with a checked translation. The AnyPS5 driver reads the same bias through `AgcSetGuestBias` (`anyps5-arm64/docs/nps5-gpu-addr.md`).

**Evidence.**
- Why base: Calculator3D needs guest 0x2_0000_0000–0x3_0000_0000, which is the dyld shared cache on macOS (`docs/m0/S1.md` §1). 007 First Light hardcodes 0x30_0000_0000 (`docs/m1/memory.md` §1, from `limits.md`). `t_fixedmap` passes in base mode only.
- Cost of base: micro-loops are within noise. Stream 224 vs 220 ms, chase 3,642 vs 3,820 ms, fib(40) 545 vs 535 ms (base vs identity). Syobon emitted asm is 2.8–2.9% larger in base (`docs/m1/memory.md` §3, `docs/m1/lifter.md`).
- Native arena (S1): a 560 GiB fixed reservation at 0x70_0000_0000 succeeded in 30/30 launches with Metal before or after. The GPU carveout 0x10_0000_0000–0x70_0000_0000 (384 GiB) reproduces on this machine.
- Adversarial suite `tools/memtests.sh --strict`: 240 PASS, 0 FAIL, 0 KNOWN-FAIL. Reproduced twice in the recheck (`docs/m1/memtests.md`, `work/reviews/memory-recheck.md` §2).
- Review 1 found 1 blocker (a self-deadlock in `sceKernelMapDirectMemory` with MAP_FIXED), 5 major, 10 minor, 5 nit. All were fixed (`docs/m1/memory-fixes-1.md`, `memory-fixes-2.md`). The recheck verdict is MERGE-WITH-FOLLOWUPS, with 3 medium issues (`work/reviews/memory-recheck.md` §3).

**Deviations from v0.3.** The default is base, not identity (v0.3 §3.6 and §9 M1). The as-built reservation is 1 TiB at 2^40, not the 560 GiB range in v0.3; identity keeps the 560 GiB range. R-ADDR is handled by the base mode and the scan, not by a dual-mode rule (`docs/m1/memory.md` §4).

**Open items.** The 1 TiB base reservation has not been checked with Metal or KosmicKrisp initialised in the same process (`docs/m1/memory.md` §10.2). Map-time protections are bookkeeping only; host enforcement covers `mprotect` (m7 remainder). Recheck medium issues: `recomp_vm_find` returns an extent pointer after `vm_mutex` is released; `sceKernelMapDirectMemory` returns 0 when the alias fails; the loader's new hard errors are untested on a retail ELF. About 1,163 `mem_base +` sites in PS4Recomp's HLE were written for a bias (`docs/m1/memory.md` §1), and about 750 CPU-side shims read guest memory without a safe read (`docs/m3/gpu-followups.md` §2b).

### 2.8 Exceptions, longjmp, fibers (SPEC §3.7, §3.10)

**Decision (as built).** The C++ unwinder is guest code: libc++abi and libunwind are recompiled from the test ELFs. Landing pads come from LSDA and are registered in the continuation table. setjmp and longjmp restore guest state. HLE callbacks are exception barriers: an exception that reaches one is fatal (`docs/m1/setjmp-eh.md` §4, §8; `docs/m1/flatstack.md` §5). Fibers are GuestContexts that own a shadow stack and are switched with it (`docs/m1/flatstack.md` §5).

**Evidence.** `m1_main` (setjmp, C++ EH, threads) passes plt and noplt (`docs/m1/tests.md`). The source-level and ELF-level oracles match in 22 of 22 cells across 11 tests and 2 variants, `m1_main` included (`docs/m1/oracle.md`). The boundary harness runs 22 cases per call model, including fibers in both modes, exception handlers in shims and fibers, thread start, and crash and longjmp reports (`docs/m1/flatstack.md` §8). Signal handling: a crash handler with fault resolvers ahead of other handlers (`docs/m3/gpu-followups.md` §3).

**Deviations from v0.3.** The §3.10 reconstruction records (one per faulting host instruction) are not in the as-built docs; the crash report carries a GuestContext dump and a frame-pointer walk (`docs/m1/diag.md`).

**Open items.** Signal masks, `sigaltstack`, and x87/XMM state across longjmp are not modelled (`docs/m1/setjmp-eh.md` §8). Forced unwinding and `pthread_cancel` are untested. Exceptions across qsort and thread-start callbacks are fatal by design. The `ps4_font.c` GuestContext copy (§2.4) gives a wrong boundary for longjmp inside that callback.

### 2.9 TLS, syscalls, CPU identification (SPEC §3.8)

**Decision (as built).** FS base is pinned in x27 (`docs/m1/flatstack.md` §2; `docs/m1/diag.md` crash fields). Raw SYSCALL goes to `shim_raw_syscall` with the FreeBSD-style register convention (`docs/m1/lifter.md`). Titles reach the kernel through libkernel HLE: the payload SDK's own `mmap()` is a raw SYSCALL wrapper that the lifter rejects, so `t_fixedmap` uses `sceKernelMmap` (`docs/m1/memory.md` §10.7). Multi-module TLS and `__tls_get_addr` are exercised by `t_tls` (`docs/m1/discovery.md` §1).

**Evidence.** `t_tls` PASS in both variants at 4c9d416 (`docs/m1/tests.md`, Current status; `t_varargs` and `t_abi` also PASS there). `cpuid` and `xgetbv` masks, and the rdtsc-to-CNTVCT mapping, are not documented in the as-built set **(unverified)**. The fex-oracle skips `cpuid`, `rdtsc` and `xgetbv` as host-specific (`docs/m1/fex-oracle.md` §Classification).

**Deviations from v0.3.** None recorded.

**Open items.** Confirm `cpuid`/`xgetbv` advertise only implemented features, and that XSAVE masks agree with them. Confirm the rdtsc frequency.

### 2.10 AVX2 on NEON, x87 (SPEC §3.9, §3.8 x87)

**Decision (as built).** AVX2 uses the pair-of-NEON scheme kept from PS4Recomp, with lowerings for gathers, FMA, SHA-NI and the SSE/MMX forms the census needed. x87 defaults to full 80-bit (`-x87 f80`), with SoftFloat-3e helpers. `-x87 f64` keeps the double model per title (`docs/m2/x87-f80.md`, `work/x87/X87-DECISION.md`). Every x87 op is a helper call through a bridge that saves v0–v31 and NZCV.

**Evidence.** The UE eboot has 43 x87 instructions in 23.3 M; the non-hollow retail `libc.prx` has 5,314 x87 instructions in 167 functions (`work/x87/X87-DECISION.md` §1). With 64-bit intermediates, 17-digit output of a formatter was wrong for 74% of doubles; with 80-bit it is never wrong at 15 digits. Cost in f80: about 13 ns per x87 instruction; a dependent `acc = acc*c + d` costs 52 ns against 3.3 ns in f64 (`docs/m2/x87-f80.md`). The decision doc's estimate is 5–15 ns per SoftFloat op on M4.

**Deviations from v0.3.** v0.3 §3.8 kept PS4Recomp's x87 as low priority; it is now a full-precision default. v0.3 §3.9's work items (gathers, masked moves, cross-lane permutes) are lowered (§2.2), with masked-load fault behaviour not separately confirmed **(unverified)**.

**Open items.** FSIN, FCOS, FSINCOS, FPTAN, FPATAN, F2XM1, FYL2X and FYL2XP1 are translate-time unsupported. Seven undocumented encodings fail in FEX tests. Unmasked x87 exceptions are not delivered. MMX aliasing is partial. Per-function x87 mode is not implemented. Inlining FLD/FST is future work.

### 2.11 HLE (SPEC §4)

**Decision (as built).** AnyPS5 host libraries are built for arm64 and reached through thunks. `APS5_VABI` is empty off Windows, so exports use AAPCS64 (`anyps5-arm64/docs/nps5-video-pad.md`). Built and linked: libkernel, libc, libSceAgc, libSceAgcDriver, VideoOut, Pad, UserService and `libnps5_gpu.dylib` (`anyps5-arm64/docs/nps5-arm64.md`). Not every one of the 413 AnyPS5 directories is built. The runtime has its own libc HLE (312 shims, `docs/m1/libc.md`), since the testbed's `libc.prx` is hollow. Online and PSN calls are offline stubs, listed below. With `-gpu anyps5`, every `scePad*` import is rebound to AnyPS5's pad (`docs/m3/controller.md`).

**Evidence.**
- libc: 138 of 149 distinct import names bound by plain name and NID (`docs/m1/libc.md`); the libc table moved t_simd and t_abi to PASS.
- Online stubs: 125 hand-written plus 998 generated for libSceNpCppWebApi. Policy: init, terminate and set-option return 0; local state objects return inert tokens (`docs/m2/online_stubs.md`).
- Syobon: 201 distinct imports; 91 host shims, 86 linked PRX, 24 unresolved (`docs/m1/loader.md`).
- Calculator3D runs with SaveData `sceSaveDataMount3` and related calls from m2-hlesys (`docs/m2/anyps5-integration.md` finding 5).
- Controllers: DualSense and Switch Pro via SDL2 GameController and HIDAPI; a scripted virtual pad drives Calculator3D menu navigation (`docs/m3/controller.md`, `anyps5-arm64/docs/nps5-controllers.md`).

**Deviations from v0.3.** v0.3 §4.2 specified a full reverse-thunk and callback barrier for every export. The as-built bridge is the `GuestAddr` choke point in AnyPS5 plus per-signature thunks; `thunkgen` rejects unmarshalable types (`docs/m1/memory-fixes-1.md`, commit 53110ca). Guest-valued pointers (Packet::addr, Shader) are passed as `GuestAddr`, not translated (`docs/m1/memory.md` §11.3).

**Open items.** Data-class GOT imports (§2.3). About 750 CPU-side shims still read or write guest memory without a safe accessor (`docs/m3/gpu-followups.md` §2b, deferred as a title bug). `AgcGuestAllocation*` wiring is deferred (`docs/m1/memory.md` deferred list). The PS4 Gnm `.mm` modules do not build in this tree and were not run (`docs/m3/gpu-followups.md`).

### 2.12 GPU (SPEC §5)

**Decision (as built).** Phase A: AnyPS5's AGC driver and RDNA recompiler on KosmicKrisp, built from upstream Mesa `03ac14ac`, not shadPS4's fork (`docs/m0/S3.md`). Presentation uses an SDL2 Vulkan window, not Metal or MetalFX. Guest addresses are translated at each driver dereference through the `GuestAddr` choke point (`anyps5-arm64/docs/nps5-gpu-addr.md`). Shader caching uses AnyPS5's disk shader cache, VkPipelineCache and pipeline journal, with prebuilt Metal libraries when present (`anyps5-arm64/docs/nps5-pipeline-cache.md`). Phase B (direct Metal) is not started.

**Evidence.**
- S3: Mesa builds for arm64 in about 17 s and the hard-requirement probe shows 0 failures (SPEC §11, S3 result); the recompiler builds for arm64 with zero source changes (124 translation units) (`docs/m0/S3.md`).
- Calculator3D: frame 0 is pixel-identical (1920x1080, max channel difference 0) to the x86 Rosetta reference. About 60 FPS warm (`docs/m2/anyps5-integration.md`). Native replay of Calculator3D captures is pixel-exact at about 1.2–1.3 ms per frame (`STATUS.md`).
- Pipeline costs (M4 Pro, loaded machine): `vkCreateDevice` 275–290 ms warm. The big pair with a warm archive: 6.8 / 5.5 ms on warm launches. With a cold OS library cache: 2.3–2.5 s. With prebuilt metallib: 5.0–7.2 ms (`anyps5-arm64/docs/nps5-pipeline-cache.md`). The S3 pipeline create was 36–58 ms cold and 0.5–1.8 ms on a second launch.
- Cold first launch: the guest clock excludes host GPU setup time (`docs/m3/gpu-followups.md` §4).

**Deviations from v0.3.** Pipeline-cache persistence was unverified in v0.3 §5.2 item 4. It is now measured (Apple's Metal cache holds it, not KosmicKrisp; `docs/m0/S3.md`). The KK archive patch exists in one AnyPS5 build, but the installed ICD at `work/s3/install` lacks it, so the archive path was not exercised there (`docs/m3/gpu-followups.md` §4). The feature-disable list (v0.3 §5.3) is not verified against the as-built driver **(unverified)**. The GpuDevice seam is not confirmed in as-built docs **(unverified)**.

**Open items.** AGC Gears was not built (`docs/m2/anyps5-integration.md`). The Phase B gate (25% of frame time) has not been measured. Frame-replay GPU correctness is shown for Calculator3D and captures only. No MetalFX. Ray tracing is disabled (diagnostic `miss` mode only, `STATUS.md`).

### 2.13 Packaging and caches (SPEC §6, §6.1)

**Decision (as built).** A translated title is a directory: chunk objects `code_NNN.o`, `guest_image.bin` (magic `NPS5IMG\x01`, region and segment tables), `lifter_manifest.json`, `memory_manifest.json`, and the runtime and HLE dylibs. Translate writes chunks in a bounded pool and deletes each `.s` on success (`docs/m2/emit-size.md`, `docs/m1/memory.md` §5). Runtime caches go under `$PS4RECOMP_CACHE_DIR` and `~/Library/Application Support/nativeps5/<titleId>/gpu-cache`. Tier-1 fragments are cached by size and hash.

**Evidence.** Peak intermediate disk for the whole Unreal title is estimated at about 50 MB in flight, against 6.4–14.1 GB of `.s` before (`docs/m2/emit-size.md`, extrapolation).

**Deviations from v0.3.** No `content_id` or `build_id`, no `profile.toml`, no fingerprinted patches, no chain stamp, no `.app`, no launcher. Caches are keyed by device, driver build and OS build (`docs/m3/gpu-followups.md` §4), which is narrower than the v0.3 chain stamp.

**Open items.** M4 packaging (§9). Dependency manifest with source URL, commit and per-file licence (§6.1). Atomic artifact writes and hash verification on load are not documented.

### 2.14 Licensing (SPEC §7)

**Decision (as built).** The tree is GPL-2.0-only. No GPL-3 code is used; FEX is MIT and credited in NOTICE (`STATUS.md`). FEX's unit tests are used as an oracle and are not vendored (`docs/m1/fex-oracle.md`). AnyPS5 is GPL-2.0-only and sets the terms (SPEC §7).

**Evidence.** Security scan (`work/SECURITY-SCAN.md`): no malware, keylogger, credential access or persistence found in the scanned sources and binaries. Medium residuals: AnyPS5 downloads prebuilt FFmpeg static libraries at CMake configure time with no hash pin; an unknown-author clone (AnyPS5-OSX) is present.

**Deviations from v0.3.** None.

**Open items.** Verify the exact FEX or Madeira files before any use of GPL-3 components (SPEC §7 "unverified" row, still open). Pin or verify the FFmpeg hash before shipping. Write the dependency manifest.

### 2.15 Validation and diagnostics (SPEC §8, §8.1, §8.2)

**Decision (as built).** Run bundles per translate and run with `manifest.json`, `log.jsonl`, `census.json`, `imports.json`, `addrmap.txt` and `crash.txt` (`docs/m1/diag.md`). `ps4-recomp last` and `addr2guest` read them. Structured logs use `NPS5_LOG=subsys=level`. Test suites: Go unit tests, `tools/memtests.sh`, `tools/m1_run.sh`, `tools/tier1_test.sh`, `tools/mem_prove.sh`, `tools/fex_oracle.sh`, `tools/ref_rosetta.sh` (Rosetta ELF oracle), `tools/diag_acceptance.sh`.

**Evidence.**
- `m1_run.sh`: 32/34 at memory-fixes time (only t_x87 failing, now fixed in f80); 34/36 with flat calls; 38/38 on main per `STATUS.md` (no doc in this tree gives that run).
- Memtests: 240 PASS.
- FEX oracle: 1,465 / 2,291 PASS.
- Source-level and ELF-level oracles: 22 of 22 cells match, with 4 modulo documented Rosetta or printf deviations (`docs/m1/oracle.md`).
- Go: `LIFTER_NO_X86=1 GOFLAGS=-mod=vendor go test ./pkg/...` green (`docs/m1/memory-fixes-2.md`).
- The M1 acceptance (SPEC §8.2, four bundles from a broken ELF) has a script (`diag_acceptance.sh`); a result is not recorded in the docs **(unverified)**.

**Deviations from v0.3.** The instruction-level differential suite on x86 CI (SPEC §8 item 1) is not met; the reference is Rosetta on the development Mac, which v0.3 called a convenient second oracle. `nps5 tracediff` (§8.2) is not documented **(unverified)**.

**Open items.** x86-64 CI. Image-diff thresholds for GPU replay are written only in AnyPS5 (exact for integer captures). The HLE trace diff format (§8.1 item 3) is not confirmed.

### 2.16 Milestones and risks

Covered in §3 and §4 below.

---

## 3. Milestone status

| Milestone | v0.3 exit criteria | Met | Not met or unverified |
|---|---|---|---|
| **M0** Spikes and skeleton | Three spike reports with numbers; arena, call/return and GPU requirement decisions confirmed or revised | S1 and S2 reports with numbers; S3 report with some timings, the rest later; arena and call/return revised (§2.7, §2.4); GPU requirement probe with 0 failures | x86-64 CI runner (none, SPEC §1); S3 was partial at writing (full AnyPS5 build came later) |
| **M1** Homebrew ELF, console only | Trivial ELF output matches the Rosetta reference (threads, mutex, C++ EH, longjmp); zero unsupported instructions; instruction-level differential green on x86 CI; hidden function reached via tier 1 and logged | m1_main passes both variants; the 11-test oracle set matches in 22 of 22 cells; tier-1 hidden-function test passes with a miss-log record; run-bundle diagnostics exist; Syobon census 2 unsupported forms before PSHUFW was lowered | x86 CI differential not run; zero unsupported is not re-measured after m1-fex; diag acceptance result not recorded |
| **M2** Homebrew graphics | AGC Gears and Calculator3D render at 60 FPS; Syobon and PixelFireworks presentation checks; frame replay within the §8.1 threshold; GpuDevice seam; pad input | Calculator3D frame 0 pixel-identical, about 60 FPS warm; capture replay pixel-exact; pad input via SDL and scripted virtual pad; Syobon plays natively (`STATUS.md`) | AGC Gears not built; PixelFireworks not in docs; GpuDevice seam unverified; 60 FPS is warm-cache only |
| **M3** First commercial 2D title | Dreaming Sarah boots to menu, plays 30 min, audio and saves work, HLE trace matches reference, 60 FPS on M2, translate under 15 min | In progress. Greenhollow and Syobon play natively; SaveData and audio HLE merged (`STATUS.md`); retail PPSA30552 reached UE log output and is root-causing a NULL dereference in fn 0x56d0990 (`STATUS.md`). Target title changed from Dreaming Sarah to PPSA30552 | Dreaming Sarah not started (needs user's copy); no 30-minute run; no HLE-trace match; thunkgen coverage of every import unverified; full-title translate time not measured |
| **M4–M7** | Hardening, `.app`, second title; 3D title; performance; breadth | Partial pieces: fibers (structural), tier-1 miss loop, shader prewarm (metallib path) | Not started as milestones: `.app`, launcher, profiles, fingerprinted patches, Phase B gate |

---

## 4. Risks and queue

### 4.1 Current risks, ranked

Ranking is by this doc's reading of the evidence above. Likelihood and impact are qualitative.

| # | Risk | Likelihood / impact | Evidence and mitigation in place |
|---|---|---|---|
| 1 | Retail boot blockers: the PPSA30552 NULL dereference is unresolved; 752 GOT tails, 94 vtable tails and 208 `jmp reg` on Syobon are run-time only; data-class GOT imports unresolved | High / high | Tier 1 serves misses; crash bundles name the function; `-seeds-from` folds misses back (§2.3, §2.5) |
| 2 | Lifter soundness at title scale: 230 FEX FAIL and 596 SKIP; AVX2 long tail; vector scratch not explicit; PF/AF liveness unverified on real titles; no x86 CI | High / high | FEX oracle, Rosetta ELF oracle, memtests, census gate (§2.2, §2.15) |
| 3 | Memory model residuals: base reservation not checked with Metal/KK in one process; map-time protections unenforced; recheck mediums (`recomp_vm_find` use after unlock; silent alias failure); about 750 unchecked guest reads in HLE | Medium-high / high | Review 1 and recheck; 240 memtests; deferred list (§2.7) |
| 4 | GPU on KosmicKrisp: the archive patch is not in the installed ICD; cold OS cache stalls 2.3–2.5 s; Phase B gate not measured; GPU correctness shown only for Calculator3D and captures; no MetalFX | Medium-high / high | Prebuilt metallib path; guest-clock credit; `GpuDevice` seam **(unverified)** (§2.12) |
| 5 | Memory ordering: default mapping and covered access classes undocumented; S1 showed plain accesses violate message passing (2 in 40M) | Medium / high | Litmus tests in S1; TSO rewriter; per-mode flag (§2.6) |
| 6 | Write-watch and fault cost: about 3.1 µs per fault single-threaded, collapsing at 8 threads (p99 0.28 ms); one Mach call per writev | Medium / medium | Coarse protection changes; relaxed coherence planned (`docs/m0/S1.md` §2) |
| 7 | Call/return budget: shim calls +19–22%, 100,000-deep recursion +164% | Medium / low-medium | Shadow stack compaction; slow-path measured (§2.4) |
| 8 | Supply chain: unpinned FFmpeg download; unknown-author AnyPS5-OSX clone | Medium / high | Scan found no malware; user decision pending (`work/SECURITY-SCAN.md`) |
| 9 | Exceptions and signals: HLE-callback exceptions fatal; signal masks and XMM across longjmp unmodelled | Low-medium / medium | Boundary harness; fail-fast by design (§2.8) |
| 10 | Upstream churn (AnyPS5, PS4Recomp) | High / medium | Pinned commits; patch series behind seams (SPEC R7, unchanged) |
| 11 | Legal pressure on the ecosystem | Unknown / high | SPEC §12, unchanged |

### 4.2 Current queue

From `STATUS.md` (last updated 2026-11-14). Its "State of main" section says everything in queue items 1 and 2 is merged; the queue text is stale and is listed here as written, with the docs' status added.

1. Memory fix round 2, re-check, merge m1-memory and m1-lifterfu. Done per `docs/m1/memory-fixes-2.md` and `work/reviews/memory-recheck.md`.
2. Flat-stack call/return, tier-1 miss fallback, x87 f80. Each has an as-built doc with results (`docs/m1/flatstack.md`, `docs/m1/tier1.md`, `docs/m2/x87-f80.md`).
3. Merge nps5-agcguest, nps5-metallib and nps5-kkbuild into nps5-link and push. Status not in the docs.
4. Boot PPSA30552: multi-module load; wire AnyPS5 libs into the runtime; missing kernel and system HLE; full-eboot translation within disk limits; iterate on crash bundles to the first frame.
5. Deferred memory items: guest mprotect enforcement (m7), host readers (m8), decommit max-protection (n1), snapshot republish performance (n5).

Agents running at the time of `STATUS.md`: boot PPSA30552 (Fable, m3-boot), x87 bridge performance (Sonnet, m3-x87perf), GPU shader cache (Sonnet, nps5-shcache), Syobon profiling (Sonnet, m3-syoperf), homebrew audio verification (Sonnet, m3-verify, `work/verify/VERIFY.md`), this spec (Haiku, m3-specv4), and iteration speed (Sonnet, m3-iterspeed, `docs/m3/iteration-speed.md`, not in this tree).

---

## 5. How to run

All commands run from the repo root. Test ELFs and retail dumps are the user's own; run x86 binaries only through the sandbox or Rosetta reference paths.

**Build the translator**
```
GOFLAGS=-mod=vendor go build -o out/ps4-recomp ./cmd/ps4-recomp
```

**Unit and Go tests**
```
LIFTER_NO_X86=1 GOFLAGS=-mod=vendor go test ./pkg/...
```

**Build the M1 test ELFs and run the M1 harness**
```
tools/m1_build_tests.sh
tools/m1_run.sh [--variant plt|noplt|both] [--call-model nested|flat] [--timeout N] [--rebuild] [--no-elf-build] [test ...]
```

**Memory suite and memory proof**
```
tools/memtests.sh --strict
tools/mem_prove.sh
```

**Tier-1 test and diagnostics acceptance**
```
tools/tier1_test.sh
tools/diag_acceptance.sh
out/ps4-recomp last
```

**Translate a title (x86 to arm64 chunks)**
```
ps4-recomp -compile=false -gpu anyps5 -o out eboot.elf   # chunks only, then link
tools/m1_native_build.sh out out/native
# or, one step:
ps4-recomp -gpu anyps5 -o out eboot.elf
```
Useful flags: `-addr-mode base|identity|auto` (default base), `-call-model flat|nested` (default flat), `-x87 f80|f64` (default f80), `-tso=false`, `-allow-unsupported` (bring-up only), `-asm-only`, `-keep-asm`.

**Build AnyPS5's arm64 host libraries**
```
cmake -S . -B build-arm64 -G Ninja -DAPS5_HOST_ARM64=ON -DCMAKE_BUILD_TYPE=Release
ninja libSceAgc libSceAgcDriver libkernel libc nps5_gpu
```
Export `NPS5_ANYPS5_BUILD=<path>/build-arm64`. The arm64 KosmicKrisp ICD is set with `VK_DRIVER_FILES=<icd.json>`; `SDL_VULKAN_LIBRARY` defaults to Homebrew's libvulkan.

**Run a translated title (sandboxed)**
```
tools/run_sandboxed.sh 30 ./native                       # env -i, timeout, one process
(cd out && NPS5_EXTRA_ENV="VK_DRIVER_FILES=<icd.json> NPS5_ANYPS5_TRACE=1" tools/run_sandboxed.sh 30 ./native)
tools/m2_anyps5_run.sh <eboot.elf> [secs] [frames]       # build, run and dump frame_NNN.bmp
```
Logs: `NPS5_LOG=hle=debug,loader=trace`. Pipeline caches: `tools/nps5_kk_prewarm.sh <titleId>`.

**Oracles**
```
tools/fex_oracle.sh [filter-regexp]       # FEX unit-test oracle, arm64 side only
tools/ref_rosetta.sh --elf [--variant ...] [test ...]   # x86 ELFs under Rosetta
tools/ref_compare.sh [--variant v] [test ...]
```

---

## 6. Contradictions and unverified items

Contradictions between documents, with the resolution this spec uses:

1. **Address mode.** v0.3 §3.6 and `SPEC.md` say identity is the default. `docs/m1/memory.md`, `docs/m1/lifter.md` and `STATUS.md` say base. This spec uses base; identity is per-title.
2. **Pipeline cache.** `anyps5-arm64/docs/nps5-pipeline-cache.md` reports the patched KosmicKrisp archive path (6.8 / 5.5 ms warm). `docs/m3/gpu-followups.md` §4 says the installed ICD at `work/s3/install` lacks the 0001/0002 patches and the archive path was not exercised. Both can be true for different builds; this spec does not claim the archive path is verified on the installed ICD.
3. **Queue.** `STATUS.md`'s queue items 1 and 2 are listed as pending, and its "State of main" line says they are merged. The docs show them merged.
4. **Flat-stack and tier-1 status.** `STATUS.md` describes both as paused, and also merged to main. `docs/m1/flatstack.md` and `docs/m1/tier1.md` have results.
5. **M1 harness counts.** 32/34 (memory-fixes docs), 34/36 (flatstack), 36/36 (x87-f80), 38/38 (`STATUS.md`, main). These are different branches and commits. No doc in this tree records 38/38.
6. **Call/return cost.** `docs/m1/flatstack.md` §3 reports recursion +1.4 to +2.1% (within budget). §8 reports recursion 20,000 deep at +3.3 to +7.2%. The two use different benchmarks; both are kept above.
7. **Syobon reachability.** `docs/m0/S2.md` gives 5,246 reachable functions. `docs/m1/discovery.md` gives 6,931. The difference is the loader fix; the S2 figure is superseded.
8. **libc.prx.** `docs/m0/S2.md` calls the testbed `libc.prx` hollow. `work/x87/X87-DECISION.md` says a non-hollow retail `libc.prx` has 5,314 x87 instructions. These are different binaries.
9. **Commit trailer.** `STATUS.md` and the brief name Claude Opus 5.5 for commits. The attribution instruction in effect for this run names Claude Haiku 5.5. The commit uses the instruction in effect; the discrepancy is flagged for the user.
10. **Duplicate docs.** `docs/m1/memtests.md` and `docs/m1/memtests_findings.md` carry the same text. `docs/SPEC-v0.2.md` is referenced by SPEC.md but is not in the tree.

Unverified in this spec: the default TSO mapping (§2.6); `cpuid`/`xgetbv`/rdtsc behaviour (§2.9); the GpuDevice seam and the feature-disable list (§2.12); the M1 diag acceptance result (§2.15); the `nps5 tracediff` tool; the 38/38 harness run; and the claim that Syobon plays natively, which comes from `STATUS.md` alone.
