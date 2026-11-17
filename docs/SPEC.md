# nativeps5: PS5 games as native Apple Silicon apps by static recompilation (v0.3, superseded: current spec is docs/SPEC-v0.4.md)

Engineering spec, v0.3, 2026-11-14. Status: revised design; feasibility gates remain open; nothing built.

v0.3 changes (small, from testbed evidence): §3.6 arena lower bound now backed by a native-arm64 measurement, plus a new hardcoded-address risk (R-ADDR) and a dual addressing mode; milestone test inputs bound to the open-source homebrew in `testbed/` (§9); S1 trimmed (§11); dev-machine baseline recorded (§1). Everything else is v0.2 unchanged.

Supersedes v0.1 for implementation planning. The original is preserved in `SPEC.md`; v0.2 is `SPEC-v0.2.md`; this revision is `SPEC-v0.3.md`. Review findings and their resolutions are recorded in Appendix B. MUST requirements are release gates; proposed techniques and estimates are not validated results.

Inherited evidence consists of the six research notes in `research/` and read-only spot checks of `repos/`. Citations look like `[anyps5 §3]`, `[aot §1]`, `[gpu §1]`, `[nx §3]`, `[ports §5]`, `[prior §1.3]`. Anything I could not confirm in source or in those notes is tagged **(unverified)**. Nothing in `repos/` was built or run for this spec. v0.2 reviews the design and the existing research; inherited “verified” labels describe the v0.1 inspection, not independent runtime verification. Primary references added by this review are in Appendix C. A source-level implementation is not proof of semantic correctness or title compatibility.

---

## 0. Decisions at a glance

| Area | Decision |
|---|---|
| Reference architecture | bloodborne-mac's hybrid: AOT first, runtime fallback for what AOT missed, local translation on the user's Mac `[aot §1, §5]`. Design reference only; its `runtime/` and `launcher/` submodules are empty in our clone. |
| CPU lifter | Fork PS4Recomp's Go lifter (direct x86-64 to ARM64 assembly, pinned registers, NZCV flags) `[aot §1]`. No LLVM or C emission. |
| Call/return model | Replace PS4Recomp's host-nested `bl`/`ret` with a guest-stack-authoritative model and a flat host stack. This removes one obstacle to nonlocal control flow; unwind support, complete state reconstruction and callback barriers are separate deliverables. |
| Miss fallback | Tier 1: recompile-on-miss with the same lifter, in process lifetime, cached and logged. Tier 2 (genuinely dynamic guest code): a runtime translator, bbcpu if it passes review, otherwise FEXCore; deferred until a title needs it. |
| Memory ordering | Code-generated ordering with a conservative baseline; PS4Recomp’s mapping is a candidate requiring validation (§3.5). Do not depend on Apple's hardware TSO bit. |
| Guest memory | Identity mapping (guest VA equals host VA) inside one reserved arena. No base register. PS5 images are relocatable, so we choose the addresses. |
| HLE | AnyPS5's ~413 library directories rebuilt for arm64 as ordinary AAPCS64 code, reached through generated SysV-to-Apple-arm64 thunks keyed by the full import identity. |
| GPU | Phase A: AnyPS5 AGC driver and RDNA shader recompiler on KosmicKrisp. Phase B (gated on measurement): direct Metal 4 backend behind a `GpuDevice` seam. |
| Ray tracing | Unsupported; require a working non-RT title path. Diagnostic-only `APS5_RAYTRACING=miss` `[anyps5 §3]`. |
| License | The product is GPL-2.0-only. |
| Distribution | Tools only. The user translates their own decrypted dump locally. No keys, no decryption, no Sony code, no game code. |

---

## 1. Goals, non-goals, constraints

**Goal.** Given a user's own, already decrypted PS5 application dump (eboot ELF, bundled `sce_module/*.prx`, `app0` data), produce a `.app` that runs as a native arm64 process on Apple Silicon with no Rosetta 2 involvement in the shipped path.

**Why now.** Every existing PS5-on-Mac route (AnyPS5 macOS output, KytyPS5-mac, sharpemu-mac) executes the x86 code under Rosetta `[prior §0]`. General Rosetta support ends after macOS 27 `[prior §4]`. A native arm64 output has no such expiry.

**Non-goals.**
- Decryption, SELF unwrapping with keys, dumping, or anything touching access controls.
- iOS/iPadOS in v1 (the fallback tiers assume `dlopen` of locally built code).
- Intel Macs, macOS older than 26 (KosmicKrisp requires Metal 4 `[gpu §1]`).
- Online services, trophies sync, PSN. Offline stubs only.
- Ray tracing, at any milestone in this spec.

**Platform floor.** macOS 26, Apple Silicon M1 or later, Xcode command line tools installed (clang is used as the assembler).

**Dev-machine baseline (v0.3).** Apple M4 Pro, 24 GB, macOS 26.6.2 (25G83), SDK 27.2, Apple clang 21, Go 1.25.4, glslang 16.6. Not yet installed: cmake, ninja, meson, vulkaninfo (needed for S3). No x86-64 CI runner exists; until one does, the M1 differential reference is the x86-64 build under Rosetta on this Mac.

---

## 2. Architecture

The shape is bloodborne-mac's: an offline recompiler produces a native library, a runtime hosts it with reimplemented system libraries, and a fallback covers what AOT did not reach `[aot §1]`. The parts are sourced differently: PS4Recomp for the lifter, AnyPS5 for PS5 system libraries and GPU, nxnative for the build and packaging discipline.

```
 USER-SUPPLIED (never shipped by us)             OUR TOOLS (GPL-2.0-only)
 decrypted eboot ELF + sce_module/*.prx + app0

        |
        v
 +--------------+   fingerprint = SHA-256(eboot + each prx); profile lookup
 | 1 INGEST     |   ELF/PRX parse, NID import/export tables, TLS template,
 |  (ps5ingest) |   relocations, .eh_frame + LSDA, per-build patches (fail closed)
 +------+-------+
        v
 +--------------+   seeds: entry, exports, FDE starts, reloc targets into text,
 | 2 ANALYSIS   |   LSDA landing pads, call-return sites, miss-log entries
 |  (analyzer)  |   CFG, jump-table recovery, carry liveness, syscall scan
 +------+-------+
        v
 +--------------+   x86-64 (Zen 2: AVX2/FMA/BMI1-2/F16C) -> ARM64 asm text
 | 3 CPU AOT    |   pinned regs, NZCV flags, TSO codegen, flat host stack
 |  (lifter,    |   chunked .s -> clang -c (parallel) -> ld -> libgame_NN.dylib
 |   emitter)   |   + dispatch table image + guestPC<->hostPC map
 +------+-------+
        |                         +---------------------------------------+
        v                         | 4 RUNTIME FALLBACK                    |
 +--------------------------+     | tier 1: miss -> lifter on that function|
 | RUNTIME (arm64 process)  |<--->|   -> dylib -> dlopen -> register, log |
 |  loader, arena, threads, |     | tier 2: runtime translator for dynamic|
 |  dispatch, shadow return |     |   guest code (bbcpu or FEXCore), later|
 |  stack, signal handling  |     +---------------------------------------+
 +------+-------------------+
        | generated SysV -> AAPCS64 thunks, bound by NID
        v
 +--------------------------+     +---------------------------------------+
 | 5 HLE                    |     | 6 GPU                                 |
 |  AnyPS5 libs, arm64 build|---->| libSceAgc + AgcDriver (PM4, state)    |
 |  libkernel, VideoOut,    |     | RDNA2 -> IR -> SPIR-V recompiler      |
 |  AudioOut, Pad, SaveData |     | Phase A: Vulkan 1.4 on KosmicKrisp    |
 |  (bundled guest libc.prx |     | Phase B: direct Metal 4 (GpuDevice)   |
 |   is recompiled, not HLE)|     | present: MetalFX scale -> CAMetalLayer|
 +--------------------------+     +---------------------------------------+
        v
 +--------------+   Game.app: launcher + runtime + libgame_*.dylib + HLE dylibs
 | 7 PACKAGING  |   + KosmicKrisp ICD + caches; ad-hoc signed; game data by
 |              |   APFS clone; caches keyed by inputs and toolchain
 +--------------+
```

Two processes exist at translate time (the Go recompiler, and clang/ld as subprocesses) and one at run time (plus the recompiler as a subprocess on a tier 1 miss).

---

## 3. CPU

### 3.1 Lifter: fork PS4Recomp, do not write a new one

**Decision: build on PS4Recomp's `pkg/disasm`, `pkg/analyzer`, `pkg/lifter`, `pkg/emitter`.**

Reasons:

1. It is the only tested x86-64 to arm64 lowering available to us: about 15.2k lines of non-test lifter code (verified by line count), 204 test functions, an AMD64-vs-ARM64 differential harness, SSE through AVX2, BMI, x87, LOCK-prefixed atomics, a TSO mode, `rdtsc`, `cpuid` `[aot §1, §2]`. PS5 adds no ISA beyond what it already targets (Zen 2, no AVX-512) `[aot §5]`.
2. Direct lowering provides a useful pinned-register baseline, but full guest state is not automatically canonical. Flags, temporary spills and multi-instruction expansions require explicit reconstruction metadata (§3.10). AOT, miss fragments and tier 2 MUST share a versioned state ABI. Arbitrary fragment entry MUST NOT inherit predecessor-specific flag-liveness assumptions.
3. Build time and size. bloodborne-mac's C-via-LLVM path takes about 14 minutes and yields a 296 to 710 MB library for one PS4 game of 10.7 M instructions `[aot §1]`. PS5 executables are expected to be several times larger **(unverified, spike S2 measures)**. Assembling text with `clang -c` in parallel scales far better than optimising C.
4. License fit. PS4Recomp source files carry `GPL-2.0-or-later` SPDX headers (90 files in `pkg/` and `cmd/`, verified) and can be combined into a GPL-2.0-only work. Linking LLVM libraries (Apache-2.0 with exception) into a GPL-2.0-only tool is a question we avoid by only invoking clang as a separate program.

Costs accepted: no IR, so optimisation is limited to peepholes and liveness; regex post-passes over emitted assembly (the TSO pass works this way) are fragile; a single squashed commit means maturity is judged from content, not history `[aot headline]`. If spike S2 shows the lifter is unsound on PS5 code in ways local fixes cannot address, the fallback plan is a thin structured-instruction layer between lowering and printing (same Go code, typed ARM64 instructions instead of strings), not a move to LLVM.

**Required changes to the fork, in priority order:**

| # | Change | Why |
|---|---|---|
| L1 | New call/return model (§3.3) | Verified in `pkg/lifter/arm64_lifter.go:1198-1238`: `CALL` pushes the guest return address then does a host `bl`; `RET` pops the guest return address into x16, discards it, restores x30 from the host stack and does a host `ret`. The host stack therefore mirrors the guest call chain, and anything that rewrites the guest stack (exceptions, `longjmp`, fibers, return-address tricks) desynchronises it. This is the root cause of "no unwinding" `[aot §1]`. |
| L2 | Identity addressing (§3.6) | Drop `[x28, xN]`; fold displacements into ARM addressing modes. Interim step: run unmodified with x28 = 0. |
| L3 | Miss path (§3.4) | `recomp_dispatch_external` currently fails `[aot §1]`. |
| L4 | TSO as structured emission, not regex | Needed before L2's addressing changes can be trusted. |
| L5 | Unsupported instruction is a translate-time error by default | Today it emits a runtime trap stub (257 call sites) `[aot §1]`. Keep the trap behind `--allow-unsupported` for bring-up only. |
| L6 | PS5 `cpuid` leaves, PS5 ELF/PRX parsing, Prospero NID tables | PS4-specific today `[aot §5]`. |
| L7 | Remove per-game Go patches (`bloodborne_patch.go`) in favour of fingerprinted profile patches (§8) | |

One audit item: PS4Recomp maps XMM0-15 to v0-v15 and YMM upper halves to v16-v31 `[aot §1]`, which leaves no free vector register. How the SIMD lifter obtains temporaries (spill, or borrowing an upper-half register) was not checked **(unverified)**; S2 must read `arm64_lifter_simd.go` for this before AVX2-heavy code is trusted.

### 3.2 Code discovery

PS5 code is a better static target than the general case:

- Where present, `.eh_frame` FDEs provide candidate function ranges; coverage is measured per module and is not assumed complete. PS4Recomp already uses FDE bounds `[aot §1]`. This gives function starts without symbols.
- The images are relocatable (AnyPS5 places "virtual address 0" at a base of its choosing, verified in `AnyPS5/docs/user/USAGE.md:141`), but relocatability does not imply every computed code pointer has a relocation. PC-relative address construction and relative jump tables require separate discovery. Every executable relocation target is a seed, not proof of coverage.
- LSDA call-site tables name every landing pad. Every landing pad is a seed (new work; needed by §3.7).
- Every instruction following a `CALL` is a return site and gets a dispatch entry (needed by §3.3).
- Retail PS5 code pages are believed to be execute-only and unwritable by the title, which would rule out self-patching of the kind bloodborne-mac reports on PS4 `[aot §1]`. **(unverified; not in the research set)**

Seed order: ELF entry and init arrays, exports, FDE starts, relocation targets, landing pads, return sites, recovered jump-table targets, entries from the miss log, entries from the profile. Jump tables use PS4Recomp's recovery for Clang PIC rel32/rel8/rel16/abs64 shapes `[aot §1]`. Where recovery fails, the site becomes a general indirect jump through the dispatch table instead of an assertion, and is recorded in the build report. Superset disassembly (Elevator, MirrorBall) is rejected: roughly 50x code growth `[prior §1.2]`.

Bundled guest modules (`sce_module/libc.prx`, `libSceFios2.prx` and others in the dump) are recompiled exactly like the eboot. AnyPS5 already gives bundled modules precedence over its HLE replacements `[anyps5 §2]`; we keep that rule.

### 3.3 Calls, returns and the host stack

**Decision: the guest stack is the only call stack. The host stack of a guest thread stays flat while guest code runs.**

- `CALL target`: push the guest return address on the guest stack (as today), push (guest return address, guest RSP, fiber identity, code generation, host continuation address) on a per-thread shadow return stack, then `b` to the target. No host `bl`, no host frame.
- `RET`: pop the guest return address from the guest stack. If the address, stack identity and code generation match the top shadow entry, pop that and `br` to its host continuation. Otherwise take the slow path: look the guest address up in the dispatch table, and clear the shadow cache. Never match by return PC alone: recursion and nonlocal jumps can reuse a PC. The cache is bounded and may always be discarded without changing guest semantics. `RET imm16` also applies its guest stack adjustment.
- Indirect `CALL`/`JMP`: dispatch table lookup as today, with a per-site inline cache added later as an optimisation.

Consequences:

- `longjmp`, C++ unwinding and fiber switches use explicit guest-state restoration. Host boundary restrictions in §3.7 still apply.
- A thread's host stack is used only by the runtime and by HLE functions, so it can be small and fixed.
- Cost: about six extra instructions and two memory touches per call/return pair on the fast path **(estimate, unmeasured)**. S2 measures it against the current `bl`/`ret` model on a call-heavy benchmark. If measured overhead exceeds 5 percent, optimise dispatch and the shadow cache first. Host-nested calls require a separate proof covering faults, signals and nonlocal exits; being a leaf is insufficient.

Register use: with identity addressing, x28 no longer holds a memory base and becomes the per-thread runtime context pointer (shadow stack pointer, dispatch table root, saved host SP, FS base copy). x18 stays untouched (Apple reserves it `[nx §1]`).

**As built (M1, docs/m1/flatstack.md; `-call-model flat`, the default, with `nested` kept for A/B):**

- The call keeps `bl` and the return keeps `ret` (S2 variant B3), so host return prediction stays paired; the shadow entry `{guest return address, guest RSP at entry, host continuation}` is pushed by the callee's *call entry* and only verifies. Tail calls and the dispatcher use the *jump entry* 8 bytes later.
- The shadow stack pointer is pinned in x29, not reached through x28: base addressing stayed first-class, so x28 is still the memory base. x29 is the only callee-saved register no lowering used.
- RET compares return address and RSP. Fiber identity is structural (one shadow stack per thread or fiber context, swapped with the context) and a code generation change poisons the affected entries (`recomp_shadow_invalidate`) instead of being compared on every return.
- "Never match by return PC alone" stands, but for cache alignment, not for correctness: with no host frame the continuation of a guest address is a pure function of the address, so any miss (mismatch, overflow, discarded cache, unwinding by guest code) is continued by address through the continuation table, the dispatch table or tier 1 (`recomp_dispatch_api.h`).
- Measured (call-saturated, M4 Pro): recursion +1.4 to +2.1 percent, leaf and mixed calls within +/-5 percent, indirect calls about +5 percent, HLE shim calls +1.5 ns. Within the 5 percent budget on the recursion benchmark.

### 3.4 Miss fallback

A miss is an indirect branch, return or landing-pad target that has no dispatch entry.

**Tier 1, decided: recompile on miss with the same lifter.**

1. The dispatch slow path saves guest state and calls the runtime.
2. The runtime checks that the target lies in an executable segment of a loaded, fingerprinted module. If not, go to tier 2 or fail.
3. It runs the recompiler as a subprocess in single-function mode on that address, assembles and links a small dylib, `dlopen`s it, registers the new entries, and resumes. Expected cost is a few hundred milliseconds, once **(estimate)**.
4. The address is appended to `misses.tsv` under the build fingerprint. The next full rebuild (launcher offers it when the log is non-empty, or runs it in the background) folds those seeds into the main library. This is Polynima's additive loop `[prior §1.2]`.

Why this and not an interpreter or FEXCore as the default: there is exactly one implementation of x86 semantics to validate. An interpreter would need its own AVX2/x87/flags coverage and its own differential testing. Mid-function entry requires a validated instruction boundary, full entry-state contract and independent flag liveness; it is not safe solely because registers are pinned. Developer builds set `MISS=fatal` so discovery bugs are fixed at the source instead of being papered over.

No JIT entitlement is needed: the app is ad-hoc signed without hardened runtime, as nxnative's Mac app is `[nx §5]`, and loads locally built dylibs.

**Tier 2, deferred: a runtime translator for code that does not exist at translate time** (guest JIT through libkernel's JIT shared-memory calls, which AnyPS5 implements in `libkernel/DirectMemory/Jit.cpp` `[anyps5 §1]`; script engines; anything self-modifying). This is bloodborne-mac's bbcpu role.

*How to get or replace bbcpu.* bbcpu ("decoder, interpreter, translator and the recompiled-code runtime") lives at `github.com/bloodborne-native/bbcpu`, vendored as `runtime/bbcpu/` inside `bloodborne-runtime`, which is the unchecked-out `runtime` submodule (verified in `bloodborne-mac/docs/ARCHITECTURE.md` and `.gitmodules`). Plan:

1. Clone bbcpu and bloodborne-recomp into a quarantine directory, read-only, and review: license per file (the family is stated GPL-2.0-or-later, with a GPL-3.0-or-later online module that must not be taken `[aot §5]`), CPU state layout, and instruction coverage.
2. Expect a coverage gap: bbcpu targets a PS4 title, and the PS4's Jaguar CPU has no AVX2, FMA or BMI2, so those paths may be missing **(unverified until read)**.
3. Adopt bbcpu as tier 2 only if it is GPL-2.0-compatible, covers or can cheaply be extended to the Zen 2 set, and can run from our pinned-register context. Otherwise use FEXCore (MIT per `[aot §5]`, but `[anyps5 §5]` lists "FEX/Madeira" as GPL-3.0+; verify the exact component before linking). FEXCore brings a JIT, so it needs `MAP_JIT`; that is acceptable on macOS.
4. Until a target title demonstrably needs tier 2, a tier 2 condition is a clean, logged failure naming the address and module.

The seam for either is the one driveclub-recomp already defines in `guest_cpu.h` (enter guest, call host function, hook, signal registers) `[ports §1, §5]`. Our runtime exposes the same four operations so the tier 2 engine is swappable.

### 3.5 Memory ordering and the TSO question

**Decision: generate ordering in code. Treat Apple's hardware TSO bit as unavailable. The inherited mapping below is a candidate, not the release default until validated.**

Whether a third-party arm64 process can enable `ACTLR.TSOEN` for its threads is unresolved; no public API was found `[prior §6]`, `[ports §5]`. A design that needs it would also be exposed to the same policy risk as Rosetta itself. Spike S1 probes for it anyway; if a supported interface exists it becomes an optional fast mode, never a requirement.

Codegen mapping, from PS4Recomp `[aot §1]`:

- Non-stack scalar loads use `LDAPR`, stores use `STLR`. The complete mapping, including cumulative ordering and mixed access classes, requires architecture review and model-based validation; passing hardware tests alone is insufficient.
- `LOCK`-prefixed instructions use `CASAL`, `LDADDAL`, `SWPAL` or an `LDAXR`/`STLXR` loop. `MFENCE` becomes `dmb ish`.
- RSP- and RBP-relative accesses stay plain.

Known weakness: vector, x87 and string-instruction accesses are not ordered `[aot §1]`. A `memcpy` built from `rep movsb` or AVX moves that publishes data to another thread is therefore weaker than on a PS5. Policy:

| Mode | Behaviour | Use |
|---|---|---|
| `tso=default` | Conservative baseline: full barriers around ordinary guest memory accesses, including vector, x87, strings and stack accesses; validated atomic lowering separately | Correctness bring-up and release until an optimised mapping passes the gate |
| `tso=strict` | Alias of the conservative baseline; no stack exemptions | Diagnosis and differential comparison |
| `tso=relaxed` | Explicit experimental profile opt-in; exact changed accesses recorded | Cannot qualify a title as correctness-validated |

The baseline may be stronger and slower than x86. Optimisations require a written mapping for every access class, formal litmus-model checks and tests on supported hardware. RSP/RBP addressing does not prove thread privacy. Load-to-store order must be covered; a store-only barrier before vector stores is not by itself a complete solution. Include fences, locked operations, non-temporal accesses, page crossings and fault restart semantics.

A global lock around only unaligned atomics does not coordinate with ordinary accesses or inline atomic paths. Until a coherent implementation exists, unsupported unaligned/page-crossing atomic forms fail with a precise diagnostic. Define required atomicity per instruction and alignment; do not assert that every x86 256-bit access is single-copy atomic.

### 3.6 Guest memory model

**Decision: identity mapping. A guest pointer is a host pointer.**

PS4Recomp reserves 512 GB and addresses memory as base plus guest address in x28 `[aot §3]`. We do not keep that, for one overriding reason: AnyPS5's HLE libraries and AGC driver treat guest pointers as raw host pointers everywhere, including pointers nested inside guest structures (`ResolveMemory`) `[anyps5 §1, §5]`. A base offset would require translating pointers at every dereference in several hundred thousand lines of code we do not own and want to keep rebasing onto upstream. Identity mapping avoids pointer rebasing, but still requires layout conversion, pointer validation and ABI marshalling (§4). It also saves an instruction on most memory operands (`ldr x, [reg, #disp]` instead of `add` then `ldr [x28, x16]`) and frees a register.

This is possible because PS5 images are relocatable and AnyPS5 already runs them at non-console addresses on every host `[anyps5 §2]`.

**Arena.** One reservation, made before anything else in the process, at a fixed host range:

- Proposed range: `0x70_0000_0000` to `0xFC_0000_0000` (560 GiB of address space, demand-paged). The lower bound avoids the commpage near `0xF_C000_0000` and the GPU carve-out `0x10_0000_0000` to `0x6F_FFFF_FFFF` that shadPS4 documents on Apple hardware `[gpu §3]`. The upper bound keeps every guest pointer under 40 bits, which PS4-lineage code relies on for pointer packing `[ports §3]` and which matches AnyPS5's arena end `[anyps5 §2]`.
- Those shadPS4 figures were observed for an x86-64 process under Rosetta. **v0.3:** `testbed/ps5-macos-native/docs/limits.md` measured the same "GPU Carveout" `0x10_0000_0000`–`0x70_0000_0000` (384 GiB) in a **native arm64** process on macOS 26 (M1 Pro): fixed 2 MiB probes at `0x70_0000_0000`, `0x80_0000_0000`, `0xFB_0000_0000` map; probes inside the carveout return `EACCES`; it cannot be deallocated or overwritten. So the proposed lower bound is supported by third-party evidence. Whether a single 560 GiB `PROT_NONE` reservation is granted, on M4-class hardware and with Metal/KK initialised, remains for S1 to confirm. **S1 result (M4 Pro, `work/s1/REPORT.md`):** carveout bounds confirmed; the full `0x70_0000_0000`–`0xFC_0000_0000` reservation via `mach_vm_allocate(VM_FLAGS_FIXED)` after a free-check succeeded 30/30 launches with and without Metal initialised. `-segaddr` is rejected (dyld ASLR slides it; `-no_pie` ignored on arm64). `0x2_0000_0000`–`0xF_C000_0000` is **not** a usable second arena: shared cache, two per-launch-randomised 12 GiB carveout windows and a fixed 1 GiB carveout at `0xF_C000_0000`; only ~21 GiB (`0x3_0000_0000`–`0x5_5400_0000`) and ~8.5 GiB (`0xD_DA00_0000`–`0xF_C000_0000`) were free every launch. The commpage is at `0x1_8000_0000`, not near `0xF_C000_0000`. Same-process Vulkan/KosmicKrisp initialisation still to be checked.
- **R-ADDR (new, v0.3): hardcoded guest addresses inside the carveout.** The PS5 application area is `0x2_0000_0000`–`0xFC_0000_0000`, so 384 GiB of it can never be identity-mapped on macOS. Titles that request fixed mappings there and hardcode the address break: `limits.md` reports 007 First Light (PPSA11386) mapping `0x30_0000_0000` from a bundled-libc initializer and storing to it directly, with that base appearing 1,418 times in the executable. Identity mapping cannot serve such a title. Mitigation: the lifter fork keeps PS4Recomp's base-plus-offset addressing as a **second, per-title mode** from M1 (a translate-time flag, not deleted code); identity stays the default because AnyPS5's HLE assumes raw pointers. Base mode requires pointer translation at the HLE boundary and is out of scope until a target title needs it; the loader detects fixed-map requests into the carveout and fails with a clear "title needs base addressing" error rather than crashing. Reservation is done either with a zero-fill Mach-O segment at a fixed address (shadPS4's `-segaddr` technique `[gpu §3]`) or a collision-safe reservation that fails if occupied. Destructive `MAP_FIXED` over an unowned address range is prohibited. Fixed mapping is allowed only inside a reservation already owned by the runtime.
- Image bases are assigned at translate time inside the arena and recorded in the manifest, so RIP-relative operands lower to constants. Guest stacks, TLS blocks, direct memory and flexible memory are carved from the arena by the AnyPS5 libkernel allocator.
- Fallback if S1 fails: keep PS4Recomp's base-plus-offset mode, and pay for it with a pointer-translation layer in HLE. This would roughly double the HLE porting estimate, so S1 comes first.

**Page size.** PS5 pages are 16 KiB (`PS5_PAGE_SIZE = 0x4000` `[anyps5 §1]`), the same as arm64 macOS. No sub-page emulation is needed, unlike nxnative's 4K-on-16K slicing `[nx §1]` or the PS4 case `[ports §5]`.

**Faults and write-watch.** AnyPS5's macOS write-watch uses `mprotect` plus SIGSEGV/SIGBUS `[anyps5 §2]`. It carries over, with two notes: recompiled code lives in its own section (`__TEXT,__recomp_guest` in PS4Recomp `[aot §1]`) so the handler can classify the faulting PC, and host syscalls writing into watched pages can return EFAULT `[anyps5 §2]`. Fault cost on macOS is a performance risk (§10, R5).

**Original image bytes.** ELF segments are mapped at their guest addresses with text mapped read-only as data (jump tables and constants live there). It is never executed.

### 3.7 Exceptions, `longjmp`, fibers

§3.3 is necessary but insufficient. These are implementation obligations, not established support.

- **C++ exceptions.** The unwinder is guest code: it lives in the title's bundled `libc.prx` (recompiled) A host-compiled AnyPS5 unwinder cannot simply walk guest frames; using it requires an explicit guest DWARF register/context adapter. It reads guest `.eh_frame`, walks the guest stack, and finally loads registers and jumps to a landing pad. This must be demonstrated with guest personality routines, LSDA parsing, cross-module unwinds, destructors, rethrows and cleanup paths. Our obligations: guest return addresses on the guest stack are real (they are), landing pads have dispatch entries (§3.2), the module-enumeration calls the unwinder uses report guest `.eh_frame` locations (HLE), and the "install context" jump goes through dispatch.
- **Callee-saved registers across HLE frames.** An exception thrown by guest code called back from HLE (a comparator, a thread entry) cannot unwind through the host frame. Rule: HLE-to-guest callbacks are exception barriers; an exception reaching one terminates with a diagnostic. Revisit only if a title needs it.
- **`setjmp`/`longjmp`.** Guest versions are candidates for reuse; validate signal masks, full saved state and nonlocal exits. A jump crossing an active host callback boundary fails explicitly until boundary unwinding is implemented. PS4Recomp's fatal shims for `__cxa_throw`, `longjmp`, `siglongjmp` `[aot §1]` are deleted.
- **Fibers.** libSceFiber switches guest stacks and callee-saved registers `[ports §3]`. With a flat host stack this is a guest register swap plus a shadow-stack swap (one shadow stack per fiber, or simply clear it and take slow-path returns after a switch).
- **Signals and guest exception handlers.** Use §3.10 reconstruction records. A fault in HLE may be a guest-pointer error or write-watch event; PC location alone cannot classify it. Signal handlers must not allocate, compile, load dylibs or take ordinary runtime locks; defer work through a tested recovery trampoline.

### 3.8 TLS, syscalls, CPU identification

- **TLS / `%fs`.** FS base is pinned in x27 as in PS4Recomp, with a FreeBSD variant II TCB in guest memory `[aot §1]`, `[ports §3]`. `fs:`-prefixed operands lower to x27-relative accesses, so none of AnyPS5's macOS `%fs` patching `[anyps5 §2]` is needed. `__tls_get_addr` binds to the HLE implementation with multi-module TLS from the ingest manifest.
- **Syscalls.** Titles call libkernel exports, and libkernel is HLE, so raw `syscall` instructions should be rare in translated code `[ports §3]`. Any that exist are scanned at analysis time (AnyPS5's `SyscallScanner` refuses unknown ones `[anyps5 §1]`; same policy) and lowered to a call into a FreeBSD-numbered table.
- **`cpuid`, `rdtsc`, `xgetbv`.** `cpuid` and `xgetbv` advertise only implemented, tested instruction and extended-state features; XSAVE/XRSTOR support must agree with these masks; `rdtsc` derives from CNTVCT scaled to a fixed guest frequency `[aot §1]`.
- **Floating-point environment.** MXCSR and x87 state are retained explicitly; FPCR/FPSR are not assumed to reproduce independent DAZ/FTZ, exception flags or x87 extended precision. Define helpers or reject unsupported modes. Thunks save and restore both host and guest environments on normal and exceptional exits.
- **x87.** Keep PS4Recomp's implementation; low priority, used mainly for `long double` `[prior §1.3]`.

### 3.9 AVX2 on NEON

Keep PS4Recomp's scheme: a YMM register is two 128-bit NEON registers; VEX-encoded 128-bit operations zero the upper half `[aot §1]`. Work items, in the order S2's census will most likely demand: gathers (`vpgatherdd` family), masked moves (`vmaskmov`, `vpmaskmov`; must not fault on masked-out lanes, a known Rosetta bug class `[gpu §3]`), cross-lane permutes (`vpermq`, `vpermd`, `vperm2i128`), `pshufb`/`vpshufb` edge semantics, FMA exactness (maps to `fmla`/`fmls`, which are fused), F16C conversions, BMI2 `pdep`/`pext` (no NEON equivalent; table or loop helper), NaN propagation and `min`/`max` operand-order semantics, `cvt*` out-of-range results. FEX and Box64 are the semantic references `[prior §1.1]`.

---

### 3.10 Architectural state and fault recovery (required before M1)

Define a versioned `GuestContext` containing GPRs, RIP, all observable RFLAGS (including PF, AF and DF), XMM/YMM, MXCSR, x87 registers/control/status/tag state, TLS bases, signal mask and thread/fiber identity. The existing research reports incomplete PF/AF support; these cannot be hidden by a per-mnemonic census. Every helper, thunk, dispatch entry and translator tier uses this contract.

Each host instruction that can fault needs a reconstruction record: guest PC, source-state locations, committed side effects and restart/continuation action. A host-PC-to-guest-PC map alone is insufficient when one guest instruction expands into multiple host operations. String and gather instructions require their defined partial-progress behavior; retrying must not duplicate stores or lose progress. Unknown reconstruction cases fail explicitly.

Tier 1 compiles a bounded fragment starting at a validated guest instruction boundary, not an assumed whole function. Deduplicate simultaneous misses by module generation and address; cap time, memory, subprocess count and cache size. Compile outside runtime locks, validate and locally sign output, then publish dispatch entries atomically. Waiters receive the same success or structured error. Do not unload code while any thread or shadow continuation references it.

Guest executable-page writes, module unload/reload and executable-permission transitions must invalidate affected translations and inline/shadow caches before reuse. Until coherent invalidation or tier 2 exists, reject such transitions explicitly. Original guest code remains non-executable on the host. No compiler work occurs in signal context.

Loader acceptance includes bounded ELF/PRX parsing, relocation overflow checks, supported relocation lists, constructor/destructor order, module dependencies, dynamic TLS allocation and library-qualified import resolution. Unsupported relocation or TLS forms fail before guest execution. Native arm64 output is not a sandbox: guest memory requests and HLE pointers still require arena bounds and permission checks.

## 4. HLE

### 4.1 Reuse AnyPS5's libraries, built for arm64

**Decision: compile AnyPS5 `core/libs/prx` (413 directories, verified count) as native arm64 dylibs using the platform calling convention, and reach them through generated thunks.**

A point in our favour, verified in `core/libs/prx/libc/include/general/VabiMacros.hpp`: `APS5_VABI` expands to `__attribute__((sysv_abi))` only under `_WIN32` and to nothing elsewhere. On arm64 macOS the libraries therefore already compile as AAPCS64 functions with no source change to the ABI annotation. What remains is host-CPU-specific code (`HostCpu.cpp`, x86 FP-environment handling, inline assembly, the custom Mach-O unwinder) `[anyps5 §5 item 3]`, which is ported or stubbed per library as milestones require.

We do not adopt PS4Recomp's `GuestContext*` shim convention `[aot §1]`: it would mean rewriting every HLE function signature.

Priority order of libraries: libkernel, libc replacement pieces not covered by a bundled `libc.prx`, libSceVideoOut, libSceAgc, libSceAgcDriver, libScePad, libSceAudioOut, libSceUserService, libSceSystemService, libSceSaveData, then by demand. AnyPS5's rule that unimplemented exports throw instead of returning success `[anyps5 §2]` is kept.

### 4.2 The thunk

Guest code uses the title’s verified x86-64 ABI; host code uses Apple’s arm64 ABI, including its deviations from generic AAPCS64 (Appendix C). Import declarations alone do not prove the guest ABI: each signature needs provenance and layout checks.

The initial bridge prioritises correctness:

1. Save the complete guest context before marshalling: all GPRs, vector halves, guest flags, FP/x87 state and guest SP. x8-x15 and most vector state are host-call-clobbered; host preservation of v8-v15 covers only their low 64 bits. Never make preservation conditional on a title profile.
2. Enter an aligned host frame and marshal from the saved context using independent integer and FP allocation counters. The seventh integer argument can use host x6; it is not automatically a host stack argument. Handle stack alignment, aggregate classification, hidden result pointers, packed layouts and narrow-value extension explicitly.
3. Call HLE with the host FP environment; capture typed results before restoring guest state. Apply only documented guest ABI clobbers and results, then return through guest dispatch.
4. Reverse callbacks use typed wrappers and a per-thread stack of boundary records. Support nested callbacks and callbacks from registered foreign threads. Block nonlocal escapes across active host frames. A single saved host SP is insufficient for reentrancy.

| Class | Handling |
|---|---|
| Scalars and stack arguments | Generate from both ABI classifiers, including mixed integer/FP register exhaustion |
| Aggregates and results | Classify each field and ABI class; marshal small/mixed aggregates, homogeneous FP aggregates and indirect returns; reject unsupported layouts |
| Variadics | Explicit adapters for the guest `va_list` and Apple stack-based variadic convention |
| Pointer-bearing structures | Verify sizes, offsets, packing and callback fields; identity addresses do not imply matching layouts |
| Guest callbacks | Typed reverse thunk, registered thread context, reentrant boundary record and checked sentinel return |

`thunkgen` fails the build on any export it cannot classify.

### 4.3 NID binding

Ingest records each import as `NID#library#module`, with version constraints where provided. At translate time each import slot is resolved to one of:

1. An export of another recompiled guest module in the same title (direct branch target).
2. An HLE thunk symbol `hle_<module>_<library>_<version>_<nid>`, resolved by `dlsym` at load.
3. A trap stub that logs the NID and the best-known name and aborts.

Names come from AnyPS5's NID tooling (`tools/nid_names.py`, `nid_patcher`) `[anyps5 §1, §5]`. Our loader does the binding itself; AnyPS5's Mach-O bind-opcode route `[anyps5 §1]` is not used because the guest image is no longer a Mach-O.

---

## 5. GPU

### 5.1 Decision

**Phase A: AnyPS5's AGC stack on KosmicKrisp. Phase B: a direct Metal 4 backend, started only when a named measurement says the Vulkan layer is the bottleneck.**

The honest weighing:

| | KosmicKrisp via Vulkan | Direct Metal 4 |
|---|---|---|
| What exists for PS5 today | AnyPS5's driver (about 120k lines) and RDNA shader recompiler (about 86k lines) target Vulkan and already special-case `VK_DRIVER_ID_MESA_KOSMICKRISP` `[anyps5 §3]` (driver-ID use verified in `Float32FlushControl.hpp`) | Nothing. AnyPS5-OSX's `MetalDevice.mm` presents and runs limited compute; `Draw()` throws `[anyps5 §4]` |
| Feature coverage | Vulkan 1.4 conformant; BDA, descriptor indexing, dynamic rendering, shaderInt64, 8/16-bit ints, GS and tessellation by compute emulation `[gpu §1]` | Everything Metal has, including mesh shaders and hardware RT on M3+, but we write all of it |
| Performance evidence | None published for KK | bloodborne-mac: direct Metal 48-49 FPS vs MoltenVK 30.6 FPS at 1080p on M3 Pro `[aot §4]`. nxnative and PS4Recomp also chose direct Metal `[nx §3]`, `[aot §4]` |
| Cost to first frame | Port the driver's host assumptions and work through KK gaps | Rewrite the Vulkan device layer (5k+ lines in `VulkanDevice.cpp` alone, plus pipeline, descriptor and resource code) against Metal, and own GS/tessellation emulation |
| Upstream tracking | Rebase onto AnyPS5 main, which moves by hundreds of commits per week `[anyps5 §6]` | A permanent fork of the driver's lower half |

Three independent Apple Silicon projects found direct Metal faster or preferable. That evidence is real, but it is against MoltenVK and on PS4 renderers; KosmicKrisp is a different driver with a different shader path, and nobody has measured it against direct Metal. Meanwhile the only PS5 command processor and shader recompiler in existence sits on Vulkan. Starting with direct Metal would put a renderer rewrite in front of the first pixel, while the project's real unknown is CPU translation. So KosmicKrisp carries us to a running 3D title, and the user's proposal is adopted for that phase.

Direct Metal is the expected long-term renderer for hot paths, not a hedge. To keep that door open from day one:

- Port AnyPS5-OSX's `GpuDevice` interface (exists only in the fork at `libSceAgcDriver/Execution/include/GpuDevice.hpp`; verified absent upstream) onto upstream's driver as our first GPU patch. All Vulkan calls stay behind it.
- Metal presentation is the target; use the validated Vulkan present path until interop ownership and synchronisation pass M2 (§5.4), so MetalFX and `CAMetalLayer` code is shared by both phases.

**Phase B gate.** Start the Metal backend when, on the M5 reference title, GPU-side profiling attributes more than 25 percent of frame time to KosmicKrisp-specific overhead (compute-emulated geometry, MSL compile stalls not fixed by caching, sparse-binding queue operations), or when a required feature is absent from KK and cannot be lowered. Missing required features can trigger this gate at S3/M2; do not wait for M5. Separate CPU pipeline compilation stalls from GPU execution time, and require a prototype to demonstrate recoverable overhead before committing to a backend rewrite. Phase B is built incrementally behind `GpuDevice`: compute passes first (AnyPS5-OSX already has SPIRV-Cross compute `[anyps5 §4]`), then the main raster path, with KK remaining the fallback renderer as MoltenVK is for bloodborne-mac.

**KosmicKrisp build.** Start from upstream Mesa. shadPS4's fork adds sparse buffer binding only `[gpu §2]`; take that commit only if AnyPS5's driver needs sparse buffers on our path (it lists sparse as optional `[anyps5 §3]`). Bundle `libvulkan_kosmickrisp.dylib` and its ICD JSON in the app as shadPS4 does `[ports §6]`, built natively for arm64.

**Known hard-requirement check.** AnyPS5 throws at device creation without: float-controls signed-zero/inf/NaN preserve, bufferDeviceAddress with shaderInt64, 8-bit storage, vertex and fragment stores and atomics, anisotropy, BC textures, independent blend `[anyps5 §3]`. KK's feature table lists BDA, shaderInt64, shaderInt8, both stores-and-atomics features, and BC where the device reports it `[gpu §1]`. Float-controls preserve and `storageBuffer8BitAccess` were not individually confirmed in the notes **(unverified)**; madeira's standalone `vk_requirements.c` probe answers this in S3 `[anyps5 §4]`.

### 5.2 Shader pipeline

`RDNA2 binary -> AnyPS5 decoder -> IR -> SPIR-V -> KosmicKrisp (NIR -> MSL text -> Metal 4 compiler)` `[anyps5 §3]`, `[gpu §1]`.

shadPS4's recompiler is GCN-only and is not used `[gpu §4]`.

The last hop compiles MSL source at run time, so stutter is the default outcome. Mitigation follows nxnative `[nx §3]`, adapted to the fact that PS5 shaders are not statically enumerable from the executable:

1. **Runtime translation with AnyPS5's disk cache** keyed by its specialization keys `[anyps5 §3]`.
2. **Chain stamp.** A hash over recompiler commit, SPIR-V options, KosmicKrisp build, macOS build and GPU family. Caches from another chain are refused, never reused.
3. **Recorded prewarm list.** Every (shader keys, pipeline state) pair seen in play is appended to a per-fingerprint list. On the next launch they are compiled before the first frame behind a "Preparing shaders" screen.
4. **Pipeline cache persistence.** KosmicKrisp initialises a Mesa pipeline cache (`kk_physical_device_init_pipeline_cache`, verified present). Whether compiled Metal pipelines survive across launches through it is **unverified**; S3 measures second-launch compile time. If they do not, this alone may trigger Phase B for the pipeline-creation path, where Metal 4 archives are available directly `[nx §3]`.
5. **Offline harvest** (later): scan `app0` for shader containers and translate ahead of time, per title, when a profile describes where they live.

Sharing caches between users is not offered: translated shaders derive from game data.

### 5.3 Feature-disable list

The table records inherited candidate workarounds, not blanket correctness approval. Exact emulation or a verified title-native alternative is required for normal operation. Approximations (ray misses, fp64-to-fp32, ignored interlock/depth behavior, dropped culling or texture mips) require explicit diagnostic opt-in and mark results degraded. Required features without an exact path fail closed. A log message does not make a semantic change safe. “Loud” means logged once per site with the title fingerprint.

| Feature | Host status | Action | Fallback detail |
|---|---|---|---|
| Ray tracing (`image_bvh_intersect_ray`, BVH builds) | KK exposes no ray query or acceleration structures `[gpu §1]` | Disabled | `APS5_RAYTRACING=miss`: every ray reports no hit `[anyps5 §3]`. RT shadows and reflections disappear; where a title has a non-RT mode, the profile selects it. |
| Mesh / amplification shaders, NGG primitive shaders | KK exposes no mesh shaders `[gpu §1]` | Lowered | Vertex-only NGG programs become plain vertex shaders; NGG culling is dropped. True mesh shading goes through AnyPS5's non-mesh fallback (mesh shader is optional there `[anyps5 §3]`). Titles that depend on it are Phase B candidates. |
| Geometry shaders, tessellation | Emulated by KK through compute and indirect draws; slow `[gpu §1, §5]` | Allowed, minimised | Rect lists and barycentrics use non-GS paths where AnyPS5 or the madeira patches provide them `[anyps5 §4]`. |
| Sparse images / partially resident textures | No sparse images in KK, including the fork `[gpu §1, §2]` | Disabled | Fully committed textures; top mips dropped under memory pressure. |
| `shaderFloat64` | Absent `[gpu §1]` | Lowered | fp64 to fp32 pass in the recompiler (AnyPS5 treats fp64 as optional `[anyps5 §3]`; shadPS4's `lower_fp64_to_fp32` is the model `[gpu §4]`). Loud. |
| 64-bit atomics (buffer, shared, image) | Not advertised `[gpu §1]` | Lowered | Two independent 32-bit CAS operations are not a 64-bit atomic. Require proven coordinated emulation for all accesses or reject the shader. |
| Atomics through buffer device addresses | Metal has no atomic on a device pointer; AnyPS5 already degrades to load/store `[anyps5 §3]` | Degraded | Non-atomic lowering is forbidden in normal mode. Reject required operations until an exact backend implementation exists. |
| Transform feedback | Not usable `[gpu §1]` | Disabled | Throw; compute emulation only if a title needs it. |
| Depth bounds test | Only on supporting GPUs or with `kk_fake_depth_bounds` `[gpu §1]` | Emulated or ignored | Enable the driconf option; AnyPS5's partial emulation otherwise `[anyps5 §3]`. |
| Fragment barycentrics | Not in KK's table `[gpu §1]` | Replaced | Independently implemented fixed-function substitutions informed by documented behavior `[anyps5 §4]`. |
| Variable rate shading | Not supported in this path | Ignored | Shade at full rate. |
| Wave64 shaders, subgroup ops outside compute and fragment | Subgroup size fixed at 32; stages compute and fragment only `[gpu §1]` | Lowered | PS5 graphics shaders are mostly wave32 `[gpu §4]`. Wave64 compute gets ballot emulation. Loud for vertex-stage lane ops. |
| Graphics-stage hardware float flush | Crashes Metal's compiler service on KK `[anyps5 §3]` | Emulated | Keep AnyPS5's emulated flush in graphics stages. |
| List-topology primitive restart | KK unrolls; slow `[gpu §1]` | Disabled | Index rewrite on the CPU. |
| Raster order groups / pixel interlock | Unavailable through KK `[anyps5 §3]` | Disabled | Loud. |
| Samplers above 1024 | Allocation fails beyond that on some devices `[gpu §3]` | Capped | Sampler deduplication cache. |
| 8x MSAA | Usually fine on Mac GPUs `[anyps5 §3]` | Allowed | Madeira's 2x4 emulation if a device lacks it. |

### 5.4 Performance knobs

Exposed in the launcher and in the profile:

- **Output mode.** VideoOut reports 1080p (default) or 1440p/2160p to the title. This is the dependable resolution control because the title chooses its own render targets from it.
- **Present upscaling.** MetalFX spatial scaler from the title's output to the display. The final Vulkan image is handed to Metal through KK's Metal interop (`EXT_external_memory_metal` is advertised, verified; the exact export path for a swapchain-less present is **unverified**). Fallback: AnyPS5's FSR path `[anyps5 §2]`.
- **MetalFX temporal upscaling and frame interpolation.** Both need depth and motion vectors `[prior §4]`. Per-title only, when a profile identifies the title's own TAA or upscaler inputs (the PS4 ports derive them from camera matrices `[ports §4]`; nxnative maps DLSS inputs `[nx §3]`). Off by default, not before M6.
- **Frame-rate cap** 30/60/120 and present interval.
- **Ray tracing** strict rejection by default; `miss` only for visibly degraded diagnostic runs.
- **Coherence strictness.** AnyPS5 favours exactness: per-draw snapshots and write-watch `[anyps5 §6]`. A relaxed level per title trades that for speed.
- **TSO mode** per title and per function (§3.5).
- **Shader prewarm** on or off; pipeline compile concurrency.

---

## 6. Packaging and per-title handling

Borrowed from nxnative `[nx §1, §4, §5]`.

- **Fingerprint.** SHA-256 over the eboot and each bundled module. Every cache, miss log, patch and profile is keyed by it.
- **Profiles.** `games/<title-id>/profile.toml`: memory sizing, TSO overrides, GPU knobs, extra seeds, known tier 2 regions. Patches are byte patches with the original bytes asserted, bound to a fingerprint, failing closed. AnyPS5's TOML patch format and the community XML format are both importable `[anyps5 §1]`, `[ports §1]`. Policy: fix the host generally before adding a title patch.
- **Build stages**, each cached by input fingerprint: ingest, analysis, lift, assemble, link, thunk generation, package. Code is linked as several dylibs of bounded size so branches stay in range and incremental rebuilds touch few files.
- **Bundle.** `Game.app/Contents/MacOS/{launcher, runtime}`, `Frameworks/` (HLE dylibs, KosmicKrisp ICD, `libgame_NN.dylib`), `Resources/` (manifest, profile, caches). Game data is APFS-cloned or referenced in place. Ad-hoc signed locally; not notarized and not distributable, by design.
- **Launcher.** Spawns the runtime as a child (nxnative's pattern for file-access prompts), shows translate progress, shader preparation, miss-log rebuild prompts, and settings.

---

### 6.1 Reproducible artifacts and local data

Separate `content_id` from `build_id`. `content_id` hashes a canonical, sorted, length-delimited list of module paths, sizes and SHA-256 hashes; reject duplicate normalised paths. `build_id` additionally includes schema versions, pinned source revisions, toolchain versions, target ABI, arena layout, effective profile and patch hashes, lifter options and thunk signatures. Stage-specific cache keys include all upstream dependencies. GPU caches also include shader content, specialization/pipeline state, GPU identity and OS/driver chain stamps.

Resolve the module precedence and address plan before lifting. Store immutable manifests beside outputs. Write artifacts atomically, verify their hashes on load and invalidate on schema mismatch. Keep runtime caches and miss logs in per-user Application Support/cache locations, outside the signed app bundle. Background rebuilds produce a new generation and activate on restart; they do not replace loaded libraries.

If APFS cloning is unavailable, offer an explicit copy or reference-in-place choice with disk-space estimates. Detect changed source data before reuse. Saves live separately from app/cache generations; use atomic replacement, retain a previous valid save and validate persistence across restart. Guest file paths must remain inside declared app/save roots; reject traversal and symlink escapes. Bug reports use an allowlist and exclude raw memory, shader data, guest strings and user paths by default.

Releases require a dependency manifest with source URL, exact commit, per-file license exceptions, local patches and build configuration. The GPL target is conditional on this inventory. The Appendix A reuse plan does not authorise copying license-incompatible Madeira code; apply the same review to probes, snippets, build tools and transitive dependencies.

## 7. Licensing

**The combined runtime and tools are GPL-2.0-only**, because AnyPS5 is "GPL version 2 only" `[anyps5 §5]` and we link its libraries.

| Component | License | Combine? |
|---|---|---|
| AnyPS5 (HLE, AGC driver, shader recompiler, ingest pieces) | GPL-2.0-only | Yes. Sets the terms. |
| PS4Recomp (lifter, analyzer, emitter, runtime pieces) | LICENSE is GPL v2 text; source SPDX headers say GPL-2.0-or-later (verified, 90 files) `[aot §5]` | Yes. Take only files with a clear header; keep headers. |
| bbcpu / bloodborne-runtime | Stated GPL-2.0-or-later; online module is GPL-3.0-or-later `[aot §5]` | Core yes after per-file review. Never the GPL-3 module. |
| shadPS4 and the PS4 ports | GPL-2.0-or-later `[ports §6]` | Yes (patterns, fp64 lowering). |
| nxnative | MIT `[nx §6]` | Yes, with attribution. |
| Mesa / KosmicKrisp | MIT `[ports §6]` | Yes; shipped as a separate dylib. |
| FEXCore | MIT per `[aot §5]`; "FEX/Madeira GPL-3.0+" per `[anyps5 §5]` | **Unverified.** Check the exact files before any use. |
| Madeira, OptiScaler, any GPL-3-only code | GPL-3.0(+) | No. Incompatible with GPL-2.0-only. |
| SPIRV-Cross | Apache-2.0 or MIT `[anyps5 §5]` | Yes, under MIT. |
| Apache-2.0-only libraries | Incompatible with GPL-2.0-only under the FSF guidance (Appendix C) `[anyps5 §5]` | Do not link into the runtime. Use as separate programs (clang, ld) or find a dual-licensed alternative. |
| FFmpeg, FidelityFX-FSR | Varies by build `[anyps5 §5]` | Review before enabling. |
| MetalFX, Metal, system frameworks | Apple system libraries | Usable under the GPL system-library exception. |
| DLSS, XeSS | Proprietary `[ports §6]` | No. |

Consequences: all our own code is published under GPL-2.0-only (new files may be dual-licensed GPL-2.0-or-later where they contain no AnyPS5-derived code, to ease upstreaming to PS4Recomp). The generated app is never distributed, so the GPL's distribution terms are not triggered by output; the reason it is never distributed is the game's copyright, not ours. Not legal advice.

---

## 8. Validation strategy

1. **Instruction-level differential tests.** Extend PS4Recomp's AMD64-vs-ARM64 harness `[aot §2]` with randomised operands and flag/MXCSR state for every lifted form. Ground truth is real x86-64 hardware in CI. Rosetta on the development Mac is a convenient second oracle but has known mistranslations (`VCMPSS` predicates, reciprocal precision, `VMASKMOV` faults `[gpu §3]`), so it never arbitrates alone.
2. **Census gate.** For each title, a per-mnemonic count of lifted, unsupported and never-tested forms. A build with untested forms on a hot path is flagged.
3. **HLE-boundary trace comparison.** Reference: the same dump relinked by AnyPS5 for macOS and run under Rosetta (with `--to-rosetta` `[anyps5 §1]`), for as long as Rosetta exists. Both sides log, per guest thread, the sequence of (NID, integer arguments, return value). Time, randomness and thread scheduling inputs are pinned in a deterministic mode. The first divergence per thread localises a lifter or thunk bug to the code between two HLE calls.
4. **Function-level record and check**, as bloodborne-mac does "against calls recorded from the original" `[aot §1]`: capture inputs and outputs of selected pure functions on the reference, replay through the recompiled function.
5. **Concurrency litmus tests** for the TSO mapping (store buffering, message passing, load buffering shapes), run in all three TSO modes `[prior §1.3]`.
6. **GPU frame replay.** AnyPS5's `agc_frame_replay` and capture minimiser `[anyps5 §3]`: frames captured on the reference are replayed natively on arm64 against KosmicKrisp, with image diff against the reference output. This runs with no CPU recompiler in the loop, so GPU bring-up proceeds in parallel with CPU work.
7. **Regression.** Per-title image-diff and boot-to-checkpoint runs on each commit that touches lifter, thunks or driver, in the manner of nxnative's `regress.py` `[nx §4]`.

When Rosetta is gone, items 3 and 4 move to an x86-64 Linux box running AnyPS5's Linux output.

---

### 8.1 Concrete acceptance protocol

Before measuring, record hardware model/RAM, OS build, toolchain, all dependency commits, input/build IDs, effective settings, scene/checkpoint and display resolution. Separate cold caches, warm caches, translation, load and gameplay timings. Report peak RSS, disk usage, frame-time median/p95/p99 and compile-stall counts, not only average FPS. Use three repeat runs of a fixed ten-minute scene after a documented warm-up; retain the existing 30-minute stability requirement. FPS targets remain aspirational until measured.

GPU replay thresholds must be fixed before evaluating output: exact comparison for integer fixtures; for floating-point scenes, record the chosen error metric, tolerance and approved reference image per fixture. Missing geometry, synchronization errors and lost atomic updates are failures regardless of aggregate image score. No universal tolerance is claimed here; unset thresholds block M2 acceptance.

M1 requires mixed integer/FP and aggregate ABI fixtures, full vector/flag preservation, nested callbacks, stack-escaped memory litmus cases, PF/AF/DF behavior, fault restart, simultaneous tier-1 misses and cross-module exception cleanup tests. Include malformed ELF/PRX and relocation rejection tests. M3 adds save/reload, audio continuity and clean shutdown; M4 adds cache invalidation, out-of-space and interrupted-build recovery. Match instruction forms, widths, addressing modes and operand edge cases rather than just mnemonic counts.

Reference traces include typed FP/aggregate results and selected memory effects as well as integer arguments. Normalise pointers to module/arena offsets and nondeterministic inputs. Scheduling control is a separate implementation task; compare partial orders where total deterministic replay is unavailable. Agreement with AnyPS5 is a regression oracle, not proof that shared HLE/GPU behavior matches hardware. Any future Linux reference route must itself be qualified before replacing the macOS reference.

### 8.2 Diagnostics and observability (v0.3; first M1 deliverable)

Goal: every failed run answers "where in the guest, doing what, and why" without a debugger. Built before the loader/lifter work so that work iterates fast. Off-by-default paths cost nothing beyond a branch on a global flag; a `-trace` build may add inline instrumentation.

**Run bundle.** Every translate and every run writes `runs/<timestamp>-<title>-<phase>/` containing `manifest.json` (tool + dependency commits, input file hashes, settings/env, host model and OS build), `log.jsonl`, and any crash/miss/census artefacts. A `latest` symlink points at the newest bundle.

**Structured log.** JSONL with `t`, `level`, `subsys` (`loader`, `lifter`, `dispatch`, `tier1`, `hle`, `kernel`, `mem`, `gpu`, `audio`, `pad`), `tid`, `guest_pc` where known, and a message plus fields. Levels per subsystem via `NPS5_LOG=hle=debug,loader=trace`. Rate-limit repeated lines with a coalesced count (AnyPS5's logger already does this).

**Translate-time reports.**
- `census.json`: emitted unsupported stubs with guest address, mnemonic and operand form (counted from emitted markers, never from `analyze`); functions found vs FDE count; jump-table sites recovered/unrecovered with addresses; indirect call count.
- `imports.json`: every import with library, NID, resolved name, and bound/stub/missing.
- `addrmap.bin` + text form: guest PC ↔ host PC ↔ function, plus §3.10 reconstruction records. Used by the crash handler and by `nps5 addr2guest <host_pc>`.

**Runtime diagnostics.**
- Crash handler (SIGSEGV/SIGBUS/SIGILL/SIGTRAP from `brk`, uncaught C++ exceptions, `abort`): writes `crash.txt` with signal, faulting host PC mapped to guest RIP/module+offset/function, full `GuestContext` (GPRs, flags incl. carry polarity, XMM/YMM, MXCSR, FS base), faulting address classified (arena/stack/image/unmapped/carveout), last N guest branch targets from an optional per-thread ring buffer, the HLE call in progress, and a host backtrace. Never compiles or allocates in signal context; writes with pre-opened fds.
- Unsupported-instruction and unresolved-import hits: log guest PC and NID once, count every hit, print a table at exit.
- Tier-1 miss log: address, module generation, outcome, compile time.
- HLE call trace: per-NID filter (`NPS5_TRACE_HLE=sceKernel*,sceAgc*`), args and return in typed form; also the differential HLE-boundary trace format of §8.1 with pointer normalisation, plus `nps5 tracediff a.jsonl b.jsonl`.
- Memory: arena map dump on demand (`SIGUSR1` or `NPS5_DUMP_MAP=1` at exit), guest mmap/munmap log, write-watch fault counters.
- GPU: toggles for Vulkan validation layers and KosmicKrisp debug env, per-frame CPU/GPU time and pipeline-compile stalls into `log.jsonl`, frame dump (`NPS5_DUMP_FRAMES=n`) as PNG, AnyPS5 frame capture (needs the macOS guest-memory enumeration patch from S3b).
- Exit summary: wall time, guest instructions/sec estimate (sampled), dispatch hit/miss rates, HLE calls by NID top 20, faults, frames and frame-time percentiles.

**Tooling.** `nps5 doctor` checks environment (KK ICD present and selected, arena reservable, Rosetta available for references, toolchain versions). `nps5 last` prints the newest bundle's summary and crash. Agents and humans iterate from bundles, not terminal scrollback.

**Acceptance (M1).** A deliberately broken test ELF (bad import, unsupported instruction, null-deref, uncaught exception) produces four bundles whose crash/exit reports name the guest function and address correctly.

## 9. Milestones

Durations are optimistic planning estimates for one to two engineers, not commitments. Re-estimate after M0; M1 exception support, ABI and fault semantics are independent gates. Failed feasibility gates stop dependent title milestones. No build or performance claims have been demonstrated by this review.

| | Scope | Exit criteria |
|---|---|---|
| **M0** Spikes and skeleton (3 wk) | S1, S2, S3 (§11). Repo layout, quarantined build of forks at pinned commits, CI with an x86-64 runner. | Three spike reports with numbers. Arena decision, call/return decision and GPU hard-requirement list confirmed or revised in this spec. |
| **M1** Homebrew ELF, console only (4 wk) | Diagnostics first (§8.2: run bundles, structured log, census/imports/addrmap, crash handler, HLE trace, `nps5 doctor`); then PS5 ELF ingest, lifter fork with identity addressing (base mode kept behind a flag, §3.6 R-ADDR) and flat-stack calls, loader, TLS, minimal libkernel thunks, tier 1 miss path. Inputs: `testbed/sdk/samples/` (hello_world, hello_stdio, hello_cxx, hello_so, hello_sprx, hello_dlfcn, relocatable_linking) built with the payload SDK; the test ELF in the exit criteria is a new sample written for it. Payload ELFs are not app packages, so SELF/app ingest is also smoke-tested on `testbed/bin/syobon/.../eboot.bin` (parse and census only). | A trivial homebrew ELF (prints through libkernel, spawns two threads, uses a mutex, throws and catches a C++ exception, calls `longjmp`) produces output identical to the Rosetta reference. Zero unsupported instructions. Instruction-level differential suite green on x86 CI. A deliberately hidden function is reached through tier 1 and appears in the miss log. |
| **M2** Homebrew graphics (4 wk) | arm64 builds of libSceVideoOut, libSceAgc, libSceAgcDriver on KosmicKrisp; `GpuDevice` seam; validated presentation; pad input. | AGC Gears (`testbed/ps5-agc-gears`) and Calculator3D (`testbed/bin/calculator3d`) render correctly at 60 FPS; Syobon Action (SDL 2D) and PixelFireworks are the 2D presentation checks. A reference frame capture replays on arm64 with image diff meeting the predeclared threshold in §8.1. |
| **M3** First commercial 2D title (8 wk) | Dreaming Sarah (AnyPS5's verified title `[prior §2.1]`; requires the user's own copy). Open-source rehearsal before it: Greenhollow (`testbed/bin/greenhollow`, 2.5D, has native Mac build for visual reference); bundled `libc.prx` recompiled; audio, save data; `thunkgen` covering every import the title uses. | Boots to menu, starts a game, 30 minutes of play without a crash, audio and saves working, HLE-boundary trace matches the reference to the title screen, stable 60 FPS on an M2. Full translate under 15 minutes. |
| **M4** Hardening and packaging (6 wk) | Fibers, signal paths, miss-log rebuild loop, profiles and fingerprinted patches, launcher, `.app` output, shader prewarm, second 2D title of a Dead Cells type. | Second title playable with no title-specific lifter code. One-command build from dump to `.app`. Second launch reaches gameplay with no shader compile stall over 50 ms. bbcpu review complete, with a written tier 2 decision. |
| **M5** First 3D title (10 wk) | A small 3D title chosen for no RT dependency and no JIT. AVX2 long tail, TSO overrides, coherence tuning, RT `miss`. | In-game, 30 FPS at 1080p output on an M3 Pro, 30 minutes without a crash. A profile of frame time split into guest CPU, HLE, driver CPU, KK, GPU. |
| **M6** Performance (8 wk) | MetalFX spatial at present, call/return and dispatch inline caches, TSO relaxations with evidence, Phase B gate evaluation; first Metal backend slice if the gate trips. | M5 title at 60 FPS on M3 Pro, or a documented reason with the bottleneck named. Phase B go or no-go recorded with data. |
| **M7** Breadth (ongoing) | Five titles across at least three engines; tier 2 if a title needs it; MetalFX temporal on one title. | Five titles playable from unmodified tool builds; regression suite covers all five. |

---

## 10. Risks, ranked

| # | Risk | Likelihood / impact | Mitigation |
|---|---|---|---|
| R1 | Lifter soundness at PS5 scale: untested instruction forms, AVX2 edge semantics, SIMD temporaries, flag corner cases. PS4Recomp's best result is "playable (first mission)" on one title `[aot §2]`. | High / high | Differential testing against real x86 before any title work; census gate; HLE-boundary traces to localise. Structured-emission layer if text passes prove unmaintainable. |
| R2 | HLE port is larger than "recompile for arm64": hidden x86 assumptions, variadics, callbacks, AnyPS5's own macOS path being young and largely unrun `[anyps5 §6]`. | High / high | Port by demand per milestone; `thunkgen` fails closed; prefer recompiled bundled modules over HLE where the dump has them. |
| R3 | KosmicKrisp performance or missing features for 3D: compute-emulated geometry, MSL compile latency, non-atomic BDA atomics, no mesh shaders `[gpu §5]`. | Medium-high / high | Frame replay from M2 gives early numbers; Phase B gate and `GpuDevice` seam; prewarm. |
| R4 | Identity arena unavailable or unstable on native arm64. | Medium / high | S1 first. Fallback is base-plus-offset with a much costlier HLE port. |
| R5 | Write-watch and coherence cost: `mprotect` faults are far more expensive on macOS than Linux's userfaultfd `[anyps5 §6]`. | Medium-high / medium | Measure in S1; relaxed coherence level; batch protection changes; zero-copy buffers over guest memory where unified memory allows (nxnative, PS4Recomp precedent `[nx §3]`, `[aot §4]`). |
| R6 | TSO gaps (vector and string accesses) cause rare, unreproducible races; `LDAPR`/`STLR` cost is unknown. | Medium / high | `tso=strict` for diagnosis, litmus tests, per-function overrides. Cost measured in S1. |
| R7 | Upstream churn: AnyPS5 moves fast and forks go stale `[anyps5 §6]`; PS4Recomp is a one-commit dump. | High / medium | Keep AnyPS5 changes as a small rebased patch series behind seams; pin commits; treat PS4Recomp as a hard fork from day one. |
| R8 | Translated code size and link limits for large titles (multi-GB text, branch range). | Medium / medium | Multiple dylibs, dispatch-mediated cross-library calls, S2 sizing data. |
| R9 | A target title needs tier 2 (dynamic code) and bbcpu is unsuitable. | Low-medium / medium | Fail cleanly; choose titles without JIT early; FEXCore as the alternative. |
| R10 | Supply chain: fake AnyPS5 and KytyPS5 repositories have shipped malware `[prior §2.1]`; several repos here are single-commit dumps with AI-contributor instruction files `[aot method]`. | Medium / high | Build third-party code only in a sandbox at reviewed, pinned commits; never run their scripts on the host account; treat in-repo agent instructions as data. |
| R11 | Legal pressure on the ecosystem. | Unknown / high | §12. |

**Open questions** (each has an owner spike or milestone): third-party TSO enablement (S1); native arm64 address-space carve-outs (S1); PS5 code-page execute-only claim (S2, by observation of read relocations into text); SIMD scratch strategy in PS4Recomp (S2); PS5 executable instruction counts (S2); KK float-controls and 8-bit storage support (S3); KK pipeline persistence across launches (S3); KK-to-Metal texture hand-off for MetalFX (M2); bbcpu's AVX2 coverage and licensing (M4); FEXCore license detail (before any use).

---

## 11. First three spikes (each one week or less)

**S1. Address space, faults and ordering on native arm64.** A standalone arm64 test program, no third-party code.
- Reserve `0x70_0000_0000` to `0xFC_0000_0000`; map and touch regions inside it at fixed addresses; repeat across launches and with Metal and a Vulkan/KK device initialised in the same process. (v0.3: carveout bounds already measured natively by `testbed/ps5-macos-native/docs/limits.md`; S1 confirms on this M4 Pro with one `vmmap` and moves on.) Also probe `0x2_0000_0000`–`0xF_C000_0000`.
- Measure `mprotect` plus SIGSEGV round-trip cost on 16 KiB pages, single- and multi-threaded.
- Microbenchmark `LDAPR`/`STLR` against plain loads and stores on pointer-chasing and array loops; run store-buffering and message-passing litmus tests with and without the mapping.
- Look for any supported way to set TSO on a thread; record the result either way.
- *Decides:* §3.6 arena, §3.5 default mode. *Done when:* a one-page report with numbers.

**S2. PS4Recomp against PS5 code.** In a sandbox, at a pinned commit, after reading the code paths involved.
- Run the analyzer and lifter on a trivial PS5 homebrew ELF and, for census only, on one of the user's decrypted eboots: instruction counts, unsupported forms ranked by frequency, function count, jump-table recovery rate, emitted size, assemble time.
- Run the homebrew ELF end to end with x28 = 0 against a hand-made arena.
- Prototype the flat-stack call/return model on a call-heavy benchmark and measure against `bl`/`ret`.
- Read `arm64_lifter_simd.go` for the vector-temporary strategy.
- *Decides:* §3.1 (fork confirmed, or the structured-emission layer brought forward), §3.3 mitigation. *Done when:* census tables and the two benchmark numbers exist.

**S2 result (v0.3, `work/s2/REPORT.md`).** Fork confirmed (PS4Recomp `65ac2fbb`). A PS5-SDK ELF (`sceKernelWrite` + fib) lifted and ran natively; lifter stubs 0–0.06% on non-AVX2 code, 0.92% on Syobon (~40 AVX2/FMA forms: VPBROADCAST*, VPERM*, variable shifts, VFMADD*). `analyze` overstates unsupported 5x; count emitted stubs. Emission is fast (Syobon 572k insts: 2.5 s emit, 5.4 s assemble; ~3.5x expansion). Jump-table recovery only 29–42% on PS5 binaries (95% on PS4 hello) and 76% of FDE functions reachable — code discovery needs FDE/relocation seeding. Required pre-L2 work: (1) one `memOperand()` emitter replacing ~290 literal `[x28,…]` sites; (2) typed vector-scratch allocation replacing the regex `fixUpperHalves` (has a control-leaves restore hole); (3) PS5 loader fixes — SELF magic `5414F5EE`, standard dynamic tags + SCE `0x6100004x` tags (currently 0 relocs/imports found), GOT-style `-fno-plt` imports; (4) unsupported instruction = translate-time error. **Correction to §3.6:** no "interim x28=0" exists — macOS arm64 kills binaries with `__PAGEZERO` < 4 GiB and the guest layout starts at 0, so rebasing image, allocator, stack, TLS and emitted constants to the arena is one atomic change. Flat stack: B1/B3 (shadow pointer in a register) cost +1.8–2.7% on fib, up to +32–42% on call-saturated mixes; shadow pointer in memory is +140–260% — register-resident is mandatory. PF/AF not modelled (PF aliased to V after FP compares; 110 PF consumers in Syobon), DF=1 fails loudly; §3.10 flag work is real. Bundled libc.prx in Syobon is hollow (99.98% zero text), so HLE libc is mandatory.

**S3. AnyPS5 GPU stack on KosmicKrisp, natively.** In a sandbox, at pinned commits.
- Build upstream KosmicKrisp for arm64; run madeira's `vk_requirements.c` probe against it and list every missing hard requirement.
- Build AnyPS5's shader recompiler and `agc_frame_replay` for arm64; replay one captured frame from the reference path; record correctness, frame time, shader compile time on first and second launch.
- *Decides:* §5.1 Phase A viability, §5.2 item 4. *Done when:* a gap list and four timings exist.

**S3 result (v0.3, `work/s3/REPORT.md`; partial).** Upstream Mesa `03ac14ac` KosmicKrisp builds for arm64 in ~17 s and reports Vulkan 1.4.368 on the M4 Pro. Hard-requirement probe: 0 failures; independentBlend, float-controls signed-zero/inf/NaN preserve and storageBuffer8BitAccess are present (closes those unverified items). Missing optional: shaderFloat64, sparse binding, int64 atomics, fragment interlock, barycentrics, mesh shaders, shader_clock, depthBounds, graphics_pipeline_library, fillModeNonSolid. **New risk:** maxComputeSharedMemorySize 32 KiB vs AnyPS5's compute target up to 48 KiB (shaders needing more must fail clearly or be split). **Pipeline cache:** KK persists nothing itself (cache blob is MSL text); second launch is fast (≈1 ms vs 22–78 ms cold) only because of Apple's OS Metal cache, which we don't control. §5.2 therefore needs our own persistent layer (Metal binary archives, nxnative-style) before M4's stall criterion. AnyPS5's 124-file shader recompiler compiles for arm64 unchanged and `agc_shader_replay` runs natively (valid SPIR-V in 0.2–27 ms, toy shaders only). Full AnyPS5 arm64 build is blocked (CMake rejects non-x86_64 on macOS, empty submodules, frame replay needs x86 guest shims); no captured frame exists in the repo, so frame correctness/time are **not measured**. Arena coexistence: the fixed reservation stays intact with a KK device and 3 GiB+ of buffers mapped, 5/5 launches. Next: capture `.req` frames from an x86 AnyPS5 run under Rosetta, replay with the arm64 tool.

---

## 12. Legal and ethics guardrails

These are requirements, enforced in code and review where possible. Not legal advice.

1. **No circumvention.** The tools accept only already decrypted ELF/PRX input. Encrypted or signed containers are rejected with an explanation. No key handling, no key files, no SELF decryption, no firmware parsing for keys. Dumping guidance is out of scope for the project and its documentation `[prior §5]`.
2. **No Sony code.** No firmware, no system libraries, no SDK headers. HLE comes from clean-room projects. System modules found inside a dump are processed locally like the rest of the dump and never uploaded or bundled by us.
3. **No game code or assets leave the machine.** Translated libraries, shader caches, pipeline caches, frame captures and traces derive from the game and stay local. There is no cache-sharing feature. Bug-report bundles contain fingerprints, addresses, NIDs and logs; captures are attached only by explicit user action, with a warning.
4. **Tools only.** Releases contain the recompiler, runtime, HLE and profiles. Publishers have acted against recompilation releases that included translated code `[prior §5]`. The output app is ad-hoc signed for the local machine.
5. **Profiles and patches are metadata.** Offsets, replacement bytes and asserted original bytes only, never copied code regions beyond what a fail-closed check needs.
6. **Ownership statement.** First run requires the user to confirm the dump is from a copy they own. The project does not link to sources of dumps.
7. **Naming.** No Sony or game trademarks in the project name, binaries or generated app metadata beyond the title the user's own dump reports `[prior §5]`.
8. **Supply-chain care** as in R10, because a tool that asks users to process valuable local files is an attractive disguise for malware.

---

## Appendix A. What comes from where

| Need | Source | Form |
|---|---|---|
| x86-64 decode, CFG, jump tables, lifter, emitter, dispatch | PS4Recomp `pkg/{disasm,analyzer,lifter,emitter}` | Hard fork |
| Hybrid AOT plus fallback architecture, function-level checking, sizing data | bloodborne-mac docs | Design reference |
| Tier 2 translator | bbcpu (to be fetched and reviewed) or FEXCore | Candidate |
| Guest-CPU seam | driveclub-recomp `src/guest_cpu.h` | Interface shape |
| PS5 ELF/PRX ingest, NID tables, syscall scan, patch format | AnyPS5 `core/relinker`, `core/libs/nid`, `tools/` | Reuse |
| PS5 system libraries | AnyPS5 `core/libs/prx` | arm64 rebuild plus patch series |
| AGC command processing, RDNA2 shader recompiler, frame replay | AnyPS5 `libSceAgc`, `libSceAgcDriver`, `core/shader/recompiler` | arm64 rebuild plus `GpuDevice` seam |
| `GpuDevice` interface, MetalFX presenter, SPIRV-Cross compute | AnyPS5-OSX | Port onto upstream |
| Apple GPU workaround observations, requirement probe | madeira-anyps5 | Reference only until per-file license clearance; independently implement incompatible patches |
| Vulkan on Metal | Mesa KosmicKrisp upstream (shadPS4 sparse-buffer commit if needed) | Bundled ICD |
| Build stages, fingerprints, chain stamps, prewarm, profiles, packaging, regression | nxnative | Patterns, some MIT code |
| fp64 and wave64 lowering ideas | shadPS4 shader recompiler passes | Reference |


## Appendix B. v0.2 review findings and disposition

| Priority | v0.1 issue | v0.2 resolution |
|---|---|---|
| P0 | HLE thunk preserves too little state and misclassifies stack/aggregate arguments | Complete-context bridge, Apple ABI classifiers and reentrant callback records (§4.2) |
| P0 | Pinned registers and a PC map treated as complete architectural state | Explicit flags/FP state, fault reconstruction and fragment-entry contract (§3.10) |
| P0 | Incomplete memory ordering accepted as default; unaligned lock fallback unjustified | Conservative baseline and proof/testing gate; unsupported atomic forms fail (§3.5) |
| P0 | Non-atomic GPU operations accepted with a log warning | Exact emulation or rejection; approximate output explicitly degraded (§5.3) |
| P1 | Flat stack treated as sufficient for exceptions/nonlocal control flow | Guest unwind adapter requirements and callback barriers (§3.7) |
| P1 | Address-only shadow returns can reuse stale continuations | Stack/fiber/generation validation and safe cache clearing (§3.3) |
| P1 | Fixed-address reservation could replace existing mappings | Non-destructive reservation, owned-range mapping only (§3.6) |
| P1 | Miss compilation lacks concurrency, invalidation and resource limits | Bounded fragments, atomic publication and generation lifecycle (§3.10) |
| P1 | Module hashes alone under-key caches; writes inside signed bundle | Separate content/build IDs, immutable generations and external caches (§6.1) |
| P1 | Madeira copying conflicts with the GPL-2.0-only policy | Per-file clearance and independent implementation of incompatible work (§6.1, Appendix A) |
| P1 | GPU failure gate waits too long; performance criteria vague | Early feature gate, reproducible measurement and predeclared image tolerances (§5.1, §8.1) |
| P2 | Discovery completeness and platform behavior overstated | Treat metadata as seeds; retain explicit feasibility gates and inherited evidence labels |

The design remains ambitious. S1–S3 must resolve memory layout, lifter correctness and renderer viability before commercial-title schedules can be treated as credible. This review does not certify upstream repositories, license provenance, platform compatibility or performance.

## Appendix C. Primary references added in v0.2

- [Apple: Writing ARM64 code for Apple platforms](https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms): host ABI differences used to correct §4.2. Guest ABI remains independently specified and tested.
- [Arm: Memory Systems, Ordering, and Barriers](https://documentation-service.arm.com/static/62a304f231ea212bb662321d): ordering/barrier reference for the required §3.5 mapping review; not a proof of this project's proposed implementation.
- [GNU: License compatibility and relicensing](https://www.gnu.org/licenses/license-compatibility.html): Apache-2.0/GPLv2 incompatibility guidance underlying the §7 correction. Exact exceptions and dependencies still need per-file review.
