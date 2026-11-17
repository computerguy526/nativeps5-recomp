# Cold-section outlining, min/max litmus, cache identity (branch m3-coldpath)

Three tasks from `research/armsx2.md` (ideas only; no ARMSX2 code, GPL-3.0): ideas 3 (cold arena), 10 (NaN-eating
min/max) and 14 (cache identity discipline).

## 1. Cold-path outlining

### What is cold, measured

Static census of the lowering (`cmd/coldstat`: every reachable function lifted once with outlining off and once on,
host instructions counted; nothing assembled or run). Per category, from the emitted text of Syobon
(`code_*.s`, 2.23 M instructions) and the UE title:

| inline construct | where | Syobon sites | share |
|---|---|---|---|
| TSO 16-byte-crossing check + fenced plain access (`tbz #4`, `dmb`, plain `ldr`/`str`, `b done`) | `orderedAccess`, `foldedAccess` | 67,448 | dominant: 2.4% `tbz`, 3.0% `dmb` of all instructions |
| TSO unaligned-base path of the folded form (`add; eor; tbz; fenced access; ordered access; b`) | `foldedAccess` | 30,020 of the above | 7-9 instructions each |
| call-site IC miss (`bl _recomp_indirect_call_ic; b done`) | `arm64_lifter_callic.go` | 5,741 | 2 instructions; not moved (below) |
| shadow-stack mismatch (`b.ne Lf_retslow`) | every `ret` | 6,931 | already out of line: one shared `b _recomp_ret_slow` at the function tail |
| indirect-jump miss `b _recomp_indirect_jump(_ic)` | jmp sites | 2,699 | 1 instruction, the miss target itself |
| fault stubs (`brk #0` for UD2/unsupported, `#DE`) | | 7,395 | 1 instruction each; a `brk` costs nothing in the icache of the hot path |
| HLE error paths | C runtime (`pkg/runtime/core`), not in the translated text | | not part of the 434 MB |

So the only class with real volume is the TSO slow path. The call-IC miss would save nothing: `bl ic; b done` (2)
becomes `b cold` (1) and the hit path still needs the `b.eq`s, so the hot path stays the same length and the cold path
grows. The shadow-stack and jump-miss paths are one branch already.

### The change

`-cold-outline` (default **on**; `-cold-outline=false` restores the old text bit for bit). The lifter keeps the rare body of
a TSO access in a side buffer (`Arm64Lifter.TakeCold`) and leaves, in the hot stream, a skip over one `b`:

```
ordered access, general form                    folded form (base + displacement)
  add  x7, x16, #3                               add  x7, x28, x23
  eor  x7, x7, x16                               and  x16, x7, #3
  tbz  x7, #4, Lo5      (taken: common case)     cbz  x16, Lal5        (taken: common case)
  b    Lc5               <- new                  b    Lc5               <- new
Lo5: add x7, x28, x16                           Lal5: ldapur w9, [x7, #20]
  ldapr w9, [x7]                                Ld5:
Ld5:
                                                cold:  Lc5: add x16,x7,#20; add x7,x16,#3; eor; tbz x7,#4,Lo5;
cold:  Lc5: ldr w9,[x16]; dmb ishld; b Ld5             ldr w9,[x16]; dmb ishld; b Ld5; Lo5: ldapr w9,[x16]; b Ld5
```

The hot test branch is the same forward-taken branch as before; the cold body is reached through a `b` (reach +-128 MB) because
`tbz` (+-32 KB) and `cbz` (+-1 MB) cannot span a chunk. The emitter writes the cold bodies after the last function of the chunk,
in the **same** `__TEXT,__recomp_guest` section, so `recomp_pc_in_guest_code` (signal handler, sampler, tier-1 range) holds
for them unchanged. They are outside every function's pc-map record: a fault inside a cold body is attributed to the
chunk's last function by nothing (the diagnostics print the host pc, no guest pc). Acceptable: a fault there is an
access that was already faulting.

The asm sizer does not count cold lines (they are written to a separate buffer), so the `.if` pc-map size check of the
hot function stays exact. Labels are unique per chunk (`labelCounter` restarts per chunk).

Also in this change: `fexasm` (the FEX oracle) drains the cold buffer after each function (`-cold-outline`, default true, same
as the translator) and `Probe` discards it.

### Numbers

Static, host instructions of the lowering (peephole + TSO on), `cmd/coldstat`:

| title | off | hot (on) | cold | hot change | `b cold` added |
|---|---|---|---|---|---|
| Syobon (657,714 x86 insts) | 2,016,839 | 1,699,304 | 384,983 | **-15.7%** | 67,448 |
| UE title PPSA30552 (21.19 M x86 insts, 258,974 functions) | 76,249,679 | 61,328,955 | 18,292,427 | **-19.6%** | 3,371,703 |

On the UE title 3.37 M access sites move 5.43 instructions each out of the hot stream; the hot share of the translated text is
77%. The total grows by 3.37 M instructions (the `b cold`, +4.4%), so the image gets slightly *larger* and the hot code
denser; the gain is in the instruction cache and iTLB (ARMSX2 measured +5.4% icache-miss density for inline cold code
on a small core), not in size.

Dynamic: see section 4.

### Flag plumbing

`ConfigKey` (lifter) has `cold=`, `translate.stamp` has `cold=`, the tier-1 manifest has `cold_outline` and the fragment
translator lifts fragments the same way as the title. `tier1Version` (the fragment cache key) now also carries
`call_ic`, `fp_cmp_fuse`, `pushpop_fuse`, `jmp_ic` and the simd-liveness mode: it carried none of them before, so a
shared `-cache-dir` could mix fragments lifted with different switches.

## 2. NaN / +-0 litmus for MINPS/MAXPS/MINSS/MAXSS (and PD/SD, VEX)

x86: `min(a,b) = a < b ? a : b`, `max(a,b) = a > b ? a : b`: the SECOND operand (src2, bit for bit) comes back when either
input is a NaN (quiet or signalling) and when both are zero. ARM `FMIN/FMAX` return the (quieted) NaN and order -0 < +0;
`FMINNM/FMAXNM` return the number when one input is a quiet NaN.

Result: **the lowering was already correct** (`x86MinMax`: `fcmgt` mask + `bsl`; the scalar forms in `liftAvxMinMax`).
No fix needed. `pkg/lifter/arm64_minmax_test.go` pins it: 22 boundary values (+-0, +-1, +-2, 0.5, +-inf, qNaN, -qNaN, sNaN,
-sNaN, NaN with payload, denormals, smallest normal, +-max, pi) for f32 and f64; every ordered pair appears in every lane
(lane l of test (i,j) holds values (i+l, j+2l): a bijection of the pairs); 40 forms (min/max x ps/ss/pd/sd x legacy,
VEX.128, VEX.256 x register and memory source); the expected bits come from the rule above computed in Go, the lifted code
runs on the host (nothing x86 is executed). SSE scalar forms keep the destination's upper lanes, VEX scalar forms take them
from src1 and zero bits 255:128; checked. Mutation check: replacing `fcmgt` with `fcmge` in the max path fails 12 of the 40 forms (every packed max).
The FEX tests of the same instructions (`FEX_bugs/MAXPS_NaN`, `TwoByte/0F_5D`, `0F_5F`, `REP/F3_5D`, `F3_5F`, `VEX/v{min,max}{ps,ss}`)
run in `tools/fex_oracle.sh`.

## 3. Cache identity discipline

Every cached artifact is keyed by the lowering id (hash of the lifter/emitter/disasm sources). That misses what the sources
do not contain: the runtime struct layout the text bakes in (changes in `pkg/runtime/core` headers do not change the
lowering id and the translation stamp deliberately ignores runtime sources), the contract with the runtime that no header
asserts, and translator environment switches.

* `emitter.LayoutHash()`: sha256 over the `RECOMP_CTX_OFFSET` asserts, `RECOMP_TCB_CTX_SLOT`, every `_Static_assert` of
  `recomp_context.h` and `nps5_shadow.h` (RecompBoundary, 208 bytes), the Go-side offsets (`ctxShadowSP`,
  `ctxHostBoundary`, `ctxLeanActive`) and `ABIVersion`. It reads the headers embedded in the binary; whitespace and
  comments do not count.
* `emitter.ABIVersion` (const, now 1): bump it when a contract the asserts do not show changes (shadow entry layout and
  stride, IC record, dispatch table shape, pc-map record, shim/lean bridge convention).
* Where it is checked: the chunk cache key (`AsmEmitter.configKey`: `layout=`, `abi=`, and `lean=`: `NPS5_LEAN=0` at translate
  time emits no lean stubs and was not in the key), the translation stamp (`layout=`, `abi=`, `cold=`, `NPS5_LEAN`), the
  tier-1 manifest (`layout_hash`, `code_abi`, `cold_outline`) and the fragment cache version. A title whose manifest has
  another layout hash or ABI, or none (written before this change), is **refused** by the fragment translator
  (`checkTier1Layout`), not warned about; a stale chunk is simply not served (key mismatch).
* Tests: `TestLayoutHashFollowsOffsets` (changing any offset, the TCB slot or the boundary size changes the hash; comments do
  not), `TestChunkCacheRefusedOnLayoutMismatch` (a layout change, and `NPS5_LEAN=0`, invalidate every cached chunk through the
  real `emitChunks` path), `TestTier1LayoutIdentity`, `TestStampCarriesLayout`, `TestConfigKeyCarriesLayoutABIAndLean`.

Not done here: per-entry checksums on the shader/pipeline caches (idea 14's second half; that is the shader pipeline's).

## 4. Status and handoff (end of session 1)

| task | state |
|---|---|
| 1 cold outlining | Done and default on (`-cold-outline`), off is byte-identical to origin/main (Syobon asm shasum equal), static numbers in section 1. Not done: a quiet-machine dynamic A/B. |
| 2 MINPS/MAXPS litmus | Done: lowering was already correct, exhaustive test in `pkg/lifter/arm64_minmax_test.go` (mutation-checked). No fix needed. |
| 3 layout hash / ABI | Done (section 3), tests pass. Not done: per-entry checksums on shader caches (other owner). |

Gates so far (load 10-30 on the machine):
* `go test ./pkg/...`: all 15 packages ok (regress gotest). `go test ./...` fails only on stray build dirs under `tests/ps5/build/liftbench` (deleted; do not leave them) and once on `TestGuestVM` timing out at 120 s under load (passes in regress).
* FEX oracle (`FEX_J=3 GOFLAGS="-mod=vendor -buildvcs=false" tools/fex_oracle.sh`, cold on): **finished, 0 regressions**, 2 newly passing; results.json committed.
* `REGRESS_DIR=work/regress-coldpath tools/regress.sh --quick -j 3`: **PASS** (gotest, m1_flat 23/23, calc3d).
* `--full`: was started in background (output in the old session scratchpad `cp/regress_full.txt`); result NOT seen. Rerun if needed: `REGRESS_DIR=$NPS5_ROOT/work/regress-coldpath gtimeout 9000 tools/regress.sh --full -j 3`.

Measurements: static, Syobon hot lowering -15.7%, UE -19.6% (3.37 M sites, 5.43 cold insts each; coldstat on the UE eboot, nothing run).
`tools/liftbench.sh` (LIFTBENCH_CFGS="nocold cold", 5 runs, load ~10): all cases equal within noise (e.g. fp_clamp 1.52 -> 1.34, tail_virt_poly2 2.10 -> 2.20 ns); not TSO-heavy.
Syobon guest-thread CPU/frame (NPS5_PERF=1, 40 s, interleaved, machine load 10-30): off 27.08, 28.32, 33.10 ms; on 27.70, 28.42 ms. Not distinguishable; frame time 33.04 ms p50 both (the 30 fps cap). Greenhollow not measured.

Pending / next:
1. Greenhollow + quiet-machine Syobon A/B: script idea = translate with `-gpu anyps5 -cold-outline=false|true`, `tools/m1_native_build.sh`, run via `tools/run_sandboxed.sh 40` with `NPS5_EXTRA_ENV="VK_DRIVER_FILES=<KK icd> NPS5_AUDIO_NULL=1 NPS5_PERF=1"`, read `perf.txt`; interleave >=5 rounds (runs wait up to 5 min while a PPSA30552 title runs).
2. `--full` regress result.
3. Rebase on origin/main when integrate2 pushes; `NPS5_LEAN` lean key may conflict in `pkg/emitter/chunkpar.go` (take both).
4. Optional: pc-map record for the cold area so faults in cold bodies get a guest pc.
