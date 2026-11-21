# Function-level HLE and the native UE allocator (branch m3-mallochle)

Scope: a translate-time facility that runs chosen guest functions of a title as host code, and its first user, a native
replacement for Unreal Engine's `FMallocBinned3`. Default off. Study that motivates it: `work/ue-arm-study/REPORT.md` (section 6, item 4:
4-8 % of dynamic instructions, estimated, not measured here; the Orbitals pipeline does the same with a host vtable).

Code: `pkg/emitter/funcrepl.go` (facility), `pkg/emitter/funcrepl_ue5.go` (profiles), `pkg/cli/funcrepl.go` (flags),
`pkg/runtime/modules/libSceUeMalloc/` (allocator core + shims), tests `pkg/emitter/funcrepl_test.go`, `pkg/cli/funcrepl_test.go`,
`tests/uemalloc/`, `tools/uemalloc_unit.sh`, `tools/uemalloc_test.sh`. Evidence for the one title: `work/ppsa30552/MALLOC.md`.

## 1. The facility

```
-hle-malloc ue5-binned3          built-in per-title profiles (pkg/emitter/funcrepl_ue5.go)
-hle-replace table.json          any table: {"name", "funcs":[{"name","addr","size","sha256","shim"}], "vtable_checks", "params"}
-hle-replace-unverified         accept a table entry that has no sha256 and no vtable check (off: such an entry is refused)
```

A **ReplacementSet** is a list of guest functions (link-time address, size, SHA-256 of its bytes) bound to host shims, a list of
vtable-slot checks and a few parameters for the host side. `AsmEmitter.ApplyReplacements` does, in this order and all-or-nothing:
verify every digest against the loaded image (address + module bias), verify that each listed vtable slot holds the function it is
supposed to, require each function to be a discovered entry (`-allow-unsupported` etc. unchanged) and not already bound, then
`shimMap[guest] = shim`. A set that does not match is an **error** that names the first mismatch (a different build of the title
cannot silently get the wrong replacement; a title the user asked to replace and could not is not translated).

Why this is enough for "direct and via vtable":

* The emitter already turns every address in `shimMap` into a *shim stub* (`writeShimStub`): the function's own symbol
  `_fn_0x<addr>`, with the normal call entry (shadow-stack push) and a body that calls the host function through
  `_recomp_call_shim` and returns like a guest `RET`. The function is therefore still *registered in the dispatch table*, so
  `bl _fn_...` (direct), a tail `jmp`, `call [reg+disp]` through a vtable and any function pointer reach it. Nothing is patched at
  call sites, and the guest never sees a different address.
* Functions only a vtable reaches are not discovery seeds in every build, so `ReplacementSeeds` adds the entries of the matching sets
  to the seed list (`pkg/cli/cli.go`, 5 lines) - that is what puts the stub in the dispatch table.
* The shim ABI is the ordinary HLE one: `void shim(GuestContext *ctx)` reads System V arguments from `ctx`, writes `rax`, ends with
  `SHIM_RETURN()`. Guest pointers stay guest addresses (base mode: `host = guest + bias`, `NPS5_G2H`).
* Parameters that exist only at translate time (the biased guest address of a vtable) go through the generated
  `funcrepl_params.c` (`nps5_repl_params`, `nps5_funcrepl.h`); the runtime has an empty weak default.
* Tier-1 fragments: `lifter_manifest.json` `tier1` carries `hle_malloc`/`hle_replace` and the fragment emitter replays them, so a miss
  that lands in replaced code gets the same stub. `lifter_manifest.json` also records `func_replace` (set, bias, each replaced guest
  address, name and shim).

Why replace functions below the object instead of installing a host vtable: the engine already has the vtable in its image and calls
through it; binding the *slot functions* (and the constructor, which stores the vtable pointer) gives the same result as an object with
a host vtable in guest memory, needs no synthetic guest addresses and no second dispatch registration, and keeps the other slots
(stubs, base-class defaults that call back through the vtable, name getter) running as translated code. If a future title needs a
host-built vtable the facility can bind a constructor shim that allocates one: the shim already receives `this`.

What it does not do (yet): the replaced function's body is still discovered and emitted nowhere (the stub replaces it) but the code
it calls stays in the build; no dead-code pruning. There is no typed "thunk descriptor" fast path: every shim goes through
`_recomp_call_shim` (all 16 GPRs and xmm0-7 saved/restored, about 100 host instructions per call). A lean thunk (move 3-4 argument
registers, `bl` the C function, copy `rax`, guest `RET`; guest r12-r15 live in caller-saved x12-x15, so four registers must be
saved) is the next step if the call overhead shows in a profile; it must keep a pc in host C code resolvable by the signal paths
that assume `ctx` holds the registers there.

## 2. FMallocBinned3 in the title (PPSA30552, UE 5.6)

Summary of `MALLOC.md` (link VA; guest = +0x400000): constructor 0x9c2b0 (called once from `GCreateMalloc` at 0xb399 on the static
0x2420-byte object at 0x6c7dc88; `GMalloc` = 0x6c800a8), vtable 0x5dddad0 with Malloc 0xa11c0 (slot 6), Realloc 0xa12a0 (8), Free
0xa14e0 (10), QuantizeSize 0xa15b0 (13), GetAllocationSize 0xa1690 (14), Trim 0x9f300 (15), TLS hooks 0xa16e0..0xa17c0 (16-19),
D1 0x9d8a0 (0). The FMalloc defaults TryMalloc (7), TryRealloc (9), MallocZeroed/TryMallocZeroed (11, 12) are translated guest
code that call slots 6/8 through the vtable, so they reach the native allocator unchanged.

Closed class: the replaced functions are referenced only by that vtable and one call; the globals they use (pthread key 0x6c58268, VM
block 0x6c9c030, tables 0x6c9bf90/0x6c9c040) are read only from inside the same code; `GMalloc` readers (49 getter callers, 10
thunks) only call slots 6 and 10 and never read fields. The bit-tree helpers in the same file take their own static object.

## 3. The native allocator (`nps5_uemalloc_core.c`)

Guest-visible results: every returned pointer is a guest address inside memory taken from the runtime VM manager
(`recomp_vm_map_ex`, user zone, kind MAPPED, names `ue5_malloc`), so it is valid for the guest, for HLE pointer translation and for
the AGC registry exactly like any other mapping. Metadata (span table, page map, large-block table, thread caches) lives in host
memory; blocks have no header.

* **Small (<= 0x20000 bytes, the limit FMallocBinned3 uses):** 48 size classes (16 .. 128 in steps of 16, then four per doubling), 64 KiB-aligned spans cut from
  256 MiB slabs (address space only; pages are touched when blocks are first handed out), per-thread bins in front of per-class central
  lists (batch 2-64 blocks), a never-used-block bump pointer per span so untouched memory stays unfaulted, empty spans go back to a
  per-size pool when the class has another span.
* **Large:** one mapping per block rounded to the 16 KiB guest page (alignments above 64 KiB use the mapping's alignment), a hash table for
  ownership, and a cache of up to 48 freed blocks <= 16 MiB / 96 MiB total reused for a request that fits within 12.5 %.
* **Semantics** (checked against the disassembly of the original):

| call | native behaviour |
|---|---|
| `Malloc(n, a)` | `a` 0 or < 16 -> 16; not a power of two -> next power of two; result aligned to `a`; `n == 0` returns a unique 16-byte block (the original's table maps 0 to the smallest class); out of memory is fatal (message, `abort`), as `OutOfMemory` is in the original; `TryMalloc` shares the path as in this build (its slot is the FMalloc default that calls Malloc) |
| `Realloc(p, n, a)` | `p == NULL` = Malloc; `n == 0` = Free + NULL; a block that stays in its size class (small) or still fits within a quarter (large) and is aligned is returned as is; otherwise allocate, copy `min(old usable, n)`, free; OOM is fatal (non-Try path) and leaves `p` valid for the library function |
| `Free(p)` | NULL no-op; a pointer this allocator does not own is logged once and ignored (the original would assert); any thread may free any block |
| `QuantizeSize(n, a)` | the size `Malloc(n, a)` really gives; fixed point; equals `GetAllocationSize` of that block (tested) |
| `GetAllocationSize(p, &out)` | writes the usable size, returns true for a live block of ours, false otherwise |
| `Trim`, TLS cache hooks, `~FMallocBinned3` | no-ops: the thread caches are internal and flushed at thread exit (`pthread_key` destructor) |
| constructor | stores the vtable pointer, starts the allocator; the object's other 0x2418 bytes stay zero |

Thread safety: per-thread caches, `os_unfair_lock` per class, one lock for spans/slabs, one for large blocks; lock order class -> span/slab;
tested with TSAN and ASAN builds. Not async-signal-safe: a guest exception handler that allocates while the faulting thread was inside
the allocator would deadlock or corrupt (the original has the same limitation; not checked whether the title installs such a handler).

Env (runtime): `NPS5_UEMALLOC_STATS=1` prints a counter line at exit, `NPS5_UEMALLOC_POISON=1` fills freed small blocks with 0xDD.

## 4. Tests

* `tools/uemalloc_unit.sh [iters]` (host, mock arena): alignments {0..1 MiB}, size-class geometry (monotone, fixed point, waste bound),
  realloc across every class and into/out of the large range, large-block cache reuse and unmap, provider OOM (small, large, realloc keeps the
  block, capped provider then reuse), foreign-pointer free refused, 1/4/8-thread stress of random alloc/free/realloc with 15 % cross-thread frees and
  per-block pattern verification (poison on and off). `SAN=thread` / `SAN=address` run the same.
* `tools/uemalloc_test.sh` (end to end): a payload-SDK C++ program with a fake allocator object (vtable, ctor, Malloc/Realloc/Free/Quantize/
  GetAllocationSize/Trim; the original counts its calls) used through a global pointer; translated without and with a `-hle-replace` table, built, run
  with `tools/run_sandboxed.sh`, both import variants: control `replaced=no`, replaced `replaced=yes`, both `result=PASS`.
* Go: `pkg/emitter/funcrepl_test.go` (binding, digest/vtable/discovery/duplicate/conflict refusals leave the emitter untouched, file format,
  params file, profile consistency, every named shim exists in the runtime source), `pkg/cli/funcrepl_test.go`.

## 5. Using it on the title / adding a title

```
ps4-recomp -gpu anyps5 -exclude-module kitt_webkit-ps5-shipping -allow-unsupported -hle-malloc ue5-binned3 -app-dir <dump> -o out <dump>
```

The translator log shows `Function replacements ue5-binned3/PPSA30552: 12 guest functions run as host shims`; at run time the log has
`ue5-malloc: native allocator active (replaces FMallocBinned3)` and `[ps4-vm] vm alloc (ue5_malloc)` lines (slabs of 0x10000000 at 0x2_0000_0000 upwards).
A mismatch prints which function differs and stops. Another UE5 title: find `MallocBinned3.cpp`, its `lea` xrefs, the vtable (relocated slots, two
zero words before it), run the checks of `MALLOC.md`, add a `ReplacementSet` (digests from the repo disassembler's function extents).

## 6. Risks / open points

* Not measured on the title (the boot agent owns runs): the gain is the 4-8 % estimate minus the shim bridge cost (section 1).
* Every large (> 128 KiB) block that misses the cache is one VM mapping and one `[ps4-vm] vm alloc` debug line on stderr, like any guest mmap.
* Memory is not returned to the OS for small spans (they are reused); UE's own trim hooks are no-ops here.
* `GetDescriptiveName`-style slots stay guest code, so `GMalloc->GetDescriptiveName()` still says "MallocBinned3".
* UE LLM/memory-tag tracking and stats that would read the allocator's counters see nothing: the stats-related slots (20-23) are stubs or print a constant string in this build, and the counters lived in the replaced object.
* Only the exact binary of the profile is supported; the digests are the contract.

## 7. Wrapping instead of replacing (`"wrap": true`)

A table entry with `"wrap": true` binds the shim exactly like a replacement (the stub `_fn_0x<addr>` and its jump entry
`_fnj_0x<addr>` call the shim, so direct calls, tail jumps and vtable calls all reach it), but the guest body is
translated as well, under `_fn_0x<addr>_orig` / `_fnj_0x<addr>_orig` (`WrapSymbol`; its pc-map record names that symbol,
the addrmap keeps the stub as the function). `funcrepl_params.c` lists the kept bodies in `nps5_repl_wraps`
(`{shim, guest, body}`, weak empty default in `nps5_uemalloc.c`); `nps5_repl_wrap_body(shim, &guest)` returns the body,
which the shim enters with `recomp_call_entry` after pushing a null return slot (as `nps5_libc_call_guest` does). The
shim may run the original any number of times, change arguments or skip it. Restriction: the body must not call its own
entry (that reaches the shim again) and a shim that re-calls it must restore the argument registers itself.

First user: `[physsub]` (`pkg/runtime/modules/libSceAnyPS5/nps5_physsub.c`, switches in `nps5_physsub_logic.h`):
physics steps split into steps <= 1/hz s when a title runs at real-time speed below its fixed frame rate. Tests:
`tools/physsub_unit.sh` (logic), `tools/physsub_test.sh` (synthetic program: direct call, function pointer and tail call
reach the shim; counts and dt sums with the switch off, on, and on with substepped cloth), `pkg/emitter/funcwrap_test.go`.

ASTRO BOT (PPSA21564 01.007) ships the table `profiles/funcrepl/PPSA21564-physsub.json` (passed by `profiles/PPSA21564.json`),
on by default since v1.2.1: Havok world step <= 1/60 s (at most 4 per frame), ApCloth step clamped to 1/60 s, the
JungleBridge / SuspentionBridge spring steps <= 1/600 s (at most 8 per call). Every entry is sha256-checked: another build of
the game is refused with a named error, as the patch profile is. Off switches (run time, no retranslate):
`NPS5_PHYS_SUBSTEP=0` (world + cloth), `NPS5_PHYS_SUBSTEP_WORLD=0`, `NPS5_PHYS_SUBSTEP_CLOTH=off`, `NPS5_PHYS_SPRING=0`;
`NPS5_PHYS_SUBSTEP_LOG=1` / `NPS5_PHYS_SPRING_LOG=1` print `[physsub]` lines.
