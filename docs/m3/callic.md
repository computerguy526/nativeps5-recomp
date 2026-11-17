# Call-site inline caches (m3-callic)

Scope: indirect and virtual calls in the flat call model (docs/m1/flatstack.md; its rules are binding
and unchanged). Flags: translate time `-call-ic` (default on; `-call-ic=false` restores the old
`bl _recomp_indirect_call`; ignored in the nested model; recorded in the tier-1 manifest so
fragments are lifted the same way); run time `NPS5_CALL_IC=0` (disables filling, the sites then
always take the dispatch path). Code: `pkg/lifter/arm64_lifter_callic.go`,
`pkg/emitter/asm_trampoline_flat.go` (`_recomp_indirect_call_ic`, `_g_recomp_ic`),
`pkg/runtime/core/nps5_shadow.c` (flush), `recomp_runtime.c` (replacement hook).

## 1. Measured cost before

Instruction counts are read from the emitted assembly (`tests/ps5/build/bench/flat/out/code_000.s`).

| construct | host instructions |
|---|---|
| direct `call` + `ret` | call site 5 (`sub x26`, `movz`, `movk`, `str`, `bl`) + entry 2 (`stp`, `str x30`) + ret 8 (`ldr x7`, `ldp`, `cmp`, `ccmp`, `add x26`, `b.ne`, `ldur x30`, `ret`) = 15 |
| indirect call (target already in x0) | site 5 + `bl _recomp_indirect_call` stub: 3 dependent table loads (11 instructions with the index extraction and `cbz`s) + `br` + entry 2 = about 19 before the callee runs; a `call [rax+N]` adds the vtable loads and the TSO alignment check around the pointer load (about 16-20) = the ~35 of the UE study |

`tools/callbench.sh` (M4 Pro, load 11-20, min of 5 runs, ns per call; new rows `vcall_*` are
`o->vt->f(o, a)` through objects the compiler cannot see through):

| case | before | after |
|---|---|---|
| fib38 (direct) | 1.31 | 1.30 |
| leaf (direct) | 1.02 | 1.02 |
| mix (direct) | 1.29 | 1.28 |
| indirect (one site, 4 targets round robin: the worst case) | 1.31 | 1.39 |
| vcall_mono (one site, one target) | 2.11 | 1.91 |
| vcall_poly2 (one site, 2 targets alternating) | 2.12 | 1.95 |
| deep20k / deep100k (recursion, direct) | 6.79 / 16.35 | 6.82 / 16.62 |
| shim_abs / qsort_cb | 8.08 / 29.08 | 8.01 / 28.93 |

The micro-benchmark under-states the gain: the three table loads of the old path are independent of
each other's results only through the address, hot in cache and perfectly predicted, so the
out-of-order core hides most of them. Instruction count for a hit: 11 (stub) + `bl` + `br` ->
6 (`adrp`, `add`, `ldp`, `cmp`, `b.eq`, `blr`); no data-dependent chain of three loads, and no
`bl` to a shared stub that the return-address predictor must pair.

## 2. The inline cache

### Site record

Every indirect call site owns 48 bytes, 16-byte aligned, in `__DATA,__nps5_ic` (`no_dead_strip`,
defined inline by the lowering with `.pushsection`, so a site is in the same object as its code and
needs no table in the emitter):

```
+0   slot0 {guest target, host call entry}
+16  slot1 {guest target, host call entry}
+32  miss counter
```

An empty slot holds `{~0, _recomp_indirect_call}`: a (wild) call to the address `~0` "hits" an empty
slot and takes the plain dispatch path with its structured miss report instead of jumping to nothing. Inline sequence (`x0` = target,
`x7` = return address already stored on the guest stack):

```
adrp x1, site ; add x1, x1, site@PAGEOFF
ldp x2, x3, [x1]     ; cmp x2, x0 ; b.eq hit
ldp x2, x3, [x1,#16] ; cmp x2, x0 ; b.eq hit
bl _recomp_indirect_call_ic ; b done
hit:  blr x3
done:                              <- ReturnSiteLabel (continuation table), as for any CALL
```

`blr x3` calls the *call entry* the dispatch table holds, so the callee pushes its shadow entry
exactly as before; x30 is the host continuation, the `ret` predictor stays paired. The guest return
address on the guest stack, x7, x26 and the shadow entry are produced by the unchanged CALL lowering
and callee entry: the cache replaces only "find the host entry of x0". Guest-visible return
addresses, setjmp/longjmp and unwinding are therefore untouched (the shadow stack still caches a pure
function of the guest address). The miss path's `bl` returns to the `b done`, a second host
continuation of the same guest return address; both are valid (the continuation of an address is
not unique, only its meaning is).

### Miss path (`_recomp_indirect_call_ic`)

1. Megamorphic bypass: counter >= 64 -> plain three-level lookup and `br`, no stores to the site.
2. Read the flush epoch (`ldar`) **before** the lookup, do the lookup. A target that is not in the
   dispatch table (not translated yet, tier-1 fragment not served yet, HLE not registered) goes to the
   existing slow path (`recomp_flat_continue` -> tier 1) and **is never cached**. A target that becomes
   translated later is cached by the first call after the dispatch table has it.
3. Choose the slot: slot0 if empty, else slot1 if empty, else (both taken) bump the counter and
   replace slot1 on every 16th miss only (a site that changes to a new stable target takes it
   within 16 calls; a site cycling through many targets leaves the record alone after 64 misses).
4. Publish `{target, entry}` with **one** `stp`, `dmb ish`, re-read the epoch; if it changed, a flush
   ran concurrently: store the empty pair back.
5. `br` to the entry.

HLE/shim targets are ordinary dispatch table entries (shim stubs have the two entries of any
function), so they cache like functions.

### Invalidation

A cached entry is stale only if the dispatch table entry for its target changes or the code it points
into is unloaded. Both already have a hook:

* `recomp_register_fn` overwriting a non-zero, different entry (function replacement, HLE override
  such as the m3-mallochle facility) calls `recomp_ic_flush_all()`.
* `recomp_shadow_invalidate(lo, hi)` (the tier-1 contract: called before code is unloaded) calls
  `recomp_ic_flush_range` for the host range.

Flush: bump `g_recomp_ic[0]` (epoch), full fence, then walk `__DATA,__nps5_ic` of every loaded image
(`_dyld_image_count` / `getsectiondata`, so tier-1 fragment dylibs are covered) and `stp` the empty pair into
each slot whose host entry is in range. Race with a concurrent fill (Dekker pairing): the filler
does store, `dmb ish`, load epoch; the flusher does bump epoch, `dmb ish`, clear. Either the filler's
store precedes the clearing scan (cleared) or the filler sees the new epoch (clears itself). A thread
that loaded a slot just before the flush may still make one call to the old entry: the same window
the dispatch table has for a thread that read the entry just before a replacement, and the unload
contract already requires callers of `recomp_shadow_invalidate` to keep code mapped until threads
have left it.

First registrations and tier-1 `table_put` (never replaces) need no flush.

### Thread safety

* Reader: one `ldp` of a 16-byte aligned pair. Writer: one `stp` of the same pair. With FEAT_LSE2
  (every Apple M-series core; the runtime checks `hw.optional.arm.FEAT_LSE2` at startup and disables
  filling if it is absent) a 16-byte aligned `ldp`/`stp` is single-copy atomic, so a reader sees an
  old pair or a new pair, never half of each. No seqlock, no lock, no version word.
* Any pair that is ever stored is self-consistent: the entry is a function of the target (the
  dispatch table maps a target to one call entry; replacements flush). Two threads filling the same
  slot with different targets just race to a consistent last writer.
* The two slots are independent; the counter is written racily on purpose (it only samples).
* A reader that loads the entry of freshly published code executes it without an extra barrier, as it
  does when it reads the dispatch table (code publication orders its own instruction-cache
  maintenance; `table_put` uses release stores and tier 1 runs `sys_icache_invalidate`).
* `NPS5_IC_FLUSH_STRESS=<usec>` flushes all caches from a host thread at that period; the tests
  run with it.

## 3. Direct call and ret: what the flat-call rules allow

The invariants of flatstack.md fix: the guest return address on the guest stack (games, longjmp,
unwinding read it), the shadow entry `{ra, rsp, cont}` (`recomp_shadow_trim`, the boundary rules,
the setjmp anchor and the crash chain all use `rsp`), `bl`/`ret` pairing, x7. What was examined:

* One-instruction push (`stp x7, x30`, drop `rsp`) or one-compare return (`ra` only): the
  continuation is a function of `ra`, so a stale entry would still jump correctly, but without `rsp`
  the shadow stack cannot be trimmed after longjmp/exceptions/fiber switches (slow paths would
  take over permanently) and the diagnostics lose their liveness test. Rejected.
* Packing `{ra, rsp}` into one compared word: not injective in 64 bits, a collision would continue at
  the wrong host address. Rejected.
* `adr x7, Lret` for the return address: x7 must be the *guest* address (it is stored on the guest
  stack); the host address is not a function of it. Rejected. `movz`+`movk` stays.
* Leaf functions skipping the `ldur x30` reload (x30 untouched in the body): saves one instruction
  of 15 and needs a whole-function scan of every lowering (helper calls, `adr x30`, TSO slow
  paths all write x30). Not done; value too small for the risk.
* `sub x26` + `str [x28, x26]` cannot fuse: base+register addressing has no write-back.

Result: the direct sequence is unchanged (15 instructions); the measured direct rows above are
within noise. The cost model that matters for UE is the indirect path, which this change addresses.
Further direct-call gains need a different shadow layout (e.g. dropping `cont` by deriving it from
`ra`), which is a flat-stack redesign, not a trim.

## 4. Tests

* `tests/ps5/src/t_callic.c` (in `tests/ps5/tests.list`, expected output from the host build,
  identical with caches on, off and with `NPS5_SHADOW_ENTRIES=16`): one site whose target changes
  (phases of 1, 2 and 8 targets), round-robin with 2/3/8 targets, random targets, a vtable-shaped
  slot rewritten between bursts, recursion through a pointer 20000 and 100000 deep (past the 32768
  entry shadow stack) and mutual recursion through two pointers, 400 longjmps out of recursions that
  went through the cached site followed by more calls, 4 worker threads calling one site with
  thread-private targets while a fifth thread flips a shared pointer (result must be one of the
  legal targets). Run also with `NPS5_IC_FLUSH_STRESS=20`.
* `tests/ps5/tier1/t_tier1_hidden.c`: one site calls a translated target, then a tier-1-served hidden
  function before and after publication, then both in alternation.
* `tests/shadow_test`: flush by range, full flush, epoch, `recomp_shadow_invalidate` clearing caches.
* `TestCallICLowering` (exact lowering, off/nested), `TestFlatTrampolineShape`.

## 5. Known limits

* A site with more than two live targets is slower than before by the inline checks (about +6 % in
  the 4-way round-robin worst case after the megamorphic bypass).
* Indirect `jmp` (tail calls through a vtable) is not cached.
* The inline sequence is about 8 instructions larger per site (the record is 48 bytes of data).
* Fragments of a title translated with `-call-ic=false` stay uncached (manifest field `call_ic`).
