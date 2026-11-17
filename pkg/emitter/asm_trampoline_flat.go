package emitter

import (
	"fmt"
	"strings"

	"ps4-recomp/pkg/lifter"
)

// GuestContext offsets of the flat call model fields (RECOMP_CTX_OFFSET asserts in
// recomp_context.h; context_offsets_test.go checks them against the header).
const (
	ctxShadowSP     = 0xc58
	ctxHostBoundary = 0xc60
	ctxLeanActive   = 0xc70 // checked lean calls (nps5_lean.h)
)

// TrampolineSource returns trampoline_arm64.s for the selected call model. The flat trampoline
// shares the macros and the mid-instruction helpers with the nested one (trampolineAsm) and
// replaces the three places where the models differ: host <-> guest transitions, the indirect
// branch entry points with their slow paths, and the non-local resume.
func TrampolineSource(model lifter.CallModel) string {
	if model != lifter.CallFlat {
		return trampolineAsm
	}
	const (
		aEnter   = "// void recomp_enter_guest(GuestContext *ctx, uint64_t target_fn)\n"
		aHelpers = "// ---- Helpers called in the middle of a guest instruction"
		aSlow    = "// Slow path of _recomp_indirect_call/_recomp_indirect_jump"
	)
	i, j, k := strings.Index(trampolineAsm, aEnter), strings.Index(trampolineAsm, aHelpers), strings.Index(trampolineAsm, aSlow)
	if i < 0 || j < i || k < j {
		panic("emitter: trampolineAsm anchors not found; update TrampolineSource")
	}
	helpers := trampolineAsm[j:k]
	// x29 is the shadow stack pointer for as long as the host pc is in guest code: the helper
	// entries store their frame record but must not link it.
	n := strings.Count(helpers, "    mov  x29, sp\n")
	if n != 3 {
		panic(fmt.Sprintf("emitter: expected 3 helper entries that set x29, found %d", n))
	}
	helpers = strings.ReplaceAll(helpers, "    mov  x29, sp\n", "")
	r := strings.NewReplacer("{SSP}", fmt.Sprintf("#0x%x", ctxShadowSP), "{HB}", fmt.Sprintf("#0x%x", ctxHostBoundary),
		"{LEAN}", fmt.Sprintf("#0x%x", ctxLeanActive))
	return trampolineAsm[:i] + r.Replace(trampolineFlatEnter) + helpers + r.Replace(trampolineFlatDispatch)
}

// Flat call model: recomp_enter_guest and the shim bridge. {SSP} = GuestContext.shadow_sp,
// {HB} = GuestContext.host_boundary.
const trampolineFlatEnter = `// ==== Flat call model (docs/m1/flatstack.md) ==========================================
// The contract above holds with these changes:
//   - the host stack pointer does not move while guest code runs: it stays at the frame of the
//     innermost recomp_enter_guest (ctx->host_boundary), below which only the scratch area of a
//     single lowering and the frames of the shim bridge and of C code live;
//   - x29 is the shadow return stack pointer of the running context for as long as the host pc
//     is in __recomp_guest (32-byte entries {guest return address, guest rsp at entry, host
//     continuation, aux}, growing down, guard pages on both sides); everywhere else
//     ctx->shadow_sp holds it;
//   - a function symbol has a call entry (pushes the shadow entry from x7 = guest return
//     address, x26 and x30) and a jump entry 8 bytes later; x30 is scratch in guest code;
//   - no guest function has a host frame, so non-local transfers only restore guest registers
//     and trim the shadow stack (_recomp_resume_guest), and a guest RET that does not match the
//     shadow top continues at its guest return address through _recomp_ret_slow.
.section __DATA,__data
.globl _g_recomp_call_model
.p2align 3
_g_recomp_call_model:
    .quad 1                      // 1 = flat (the runtime's weak default is 0 = nested)
// Call-site inline caches (docs/m3/callic.md): {flush epoch, enabled}. The runtime clears
// the enabled word (NPS5_CALL_IC=0, or no 16-byte single-copy atomicity) and bumps the epoch on a flush.
.globl _g_recomp_ic
.p2align 3
_g_recomp_ic:
    .quad 0, 1, _recomp_indirect_call   // +16: what an empty slot's entry is (flush writes it back)
HOST_SECTION

// void recomp_enter_guest(GuestContext *ctx, uint64_t target_fn)
// x0 = ctx, x1 = target_fn (the call entry, as registered in the dispatch table)
.globl _recomp_enter_guest
// Frame (RecompBoundary in nps5_shadow.h): [sp] x29, x30; [sp+16] x19-x28; [sp+96] d8-d15;
// [sp+160] ctx, host FPCR; [sp+176] previous ctx->host_boundary, guest rsp at entry;
// [sp+192] guest return address at entry, ctx->shadow_sp at entry.
_recomp_enter_guest:
    stp  x29, x30, [sp, #-208]!
    mov  x29, sp
    stp  x19, x20, [sp, #16]
    stp  x21, x22, [sp, #32]
    stp  x23, x24, [sp, #48]
    stp  x25, x26, [sp, #64]
    stp  x27, x28, [sp, #80]
    stp  d8,  d9,  [sp, #96]
    stp  d10, d11, [sp, #112]
    stp  d12, d13, [sp, #128]
    stp  d14, d15, [sp, #144]
    str  x0,       [sp, #160]

    // The context's shadow stack is mapped on its first entry.
    ldr  x2, [x0, {SSP}]         // shadow_sp
    cbnz x2, 3f
    str  x1, [sp, #168]
    bl   _recomp_shadow_attach
    ldr  x0, [sp, #160]
    ldr  x1, [sp, #168]
3:
    mrs  x2, fpcr
    str  x2, [sp, #168]
    ldr  w3, [x0, #0x2e8]        // mxcsr
    and  x4, x2, #0xfffffffffe3fffff
    ubfx w5, w3, #14, #1
    bfi  x4, x5, #22, #1
    ubfx w5, w3, #13, #1
    bfi  x4, x5, #23, #1
    ubfx w5, w3, #15, #1
    bfi  x4, x5, #24, #1
    cmp  x4, x2
    b.eq 1f
    msr  fpcr, x4
1:
    LOAD_GPRS x0
    LOAD_XMM0_7 x0
    ldr  x27, [x0, #0x88]        // fs_base
    ldr  x28, [x0, #0x330]       // mem_base

    // Boundary record and shadow entry {return address, rsp, Lenter_guest_ret, host sp}: the
    // guest function's RET matches the entry like any other and lands on Lenter_guest_ret.
    // ctx->shadow_sp is written after the entry, so a signal handler that enters the guest
    // from here pushes below it.
    ldr  x2, [x0, {HB}]          // host_boundary
    ldr  x17, [x0, {SSP}]        // shadow_sp
    ldr  x16, [x28, x26]         // guest return address
    stp  x2, x26, [sp, #176]
    stp  x16, x17, [sp, #192]
    RETURN_TO Lenter_guest_ret
    mov  x3, sp
    stp  x16, x26, [x17, #-32]!
    stp  x30, x3, [x17, #16]
    str  x17, [x0, {SSP}]        // shadow_sp
    str  x3, [x0, {HB}]          // host_boundary
    mov  x29, x17
    add  x1, x1, #8              // jump entry
    br   x1

GUEST_SECTION
.globl _recomp_enter_guest_ret
_recomp_enter_guest_ret:
Lenter_guest_ret:
    ldr  x0, [sp, #160]
    SAVE_GPRS x0
    SAVE_XMM0_7 x0
    str  x29, [x0, {SSP}]        // shadow_sp
    b    _nps5_xs_enter_guest_tail

HOST_SECTION
_nps5_xs_enter_guest_tail:
    ldr  x2, [sp, #176]
    str  x2, [x0, {HB}]          // host_boundary
    ldr  x2, [sp, #168]          // host FPCR
    mrs  x3, fpcr
    cmp  x2, x3
    b.eq 2f
    msr  fpcr, x2
2:
    ldp  x19, x20, [sp, #16]
    ldp  x21, x22, [sp, #32]
    ldp  x23, x24, [sp, #48]
    ldp  x25, x26, [sp, #64]
    ldp  x27, x28, [sp, #80]
    ldp  d8,  d9,  [sp, #96]
    ldp  d10, d11, [sp, #112]
    ldp  d12, d13, [sp, #128]
    ldp  d14, d15, [sp, #144]
    ldp  x29, x30, [sp], #208
    ret

// _recomp_call_shim: calls a host C shim from guest code. x0 = shim, x1 = guest address of the
// shimmed function (stored to ctx->rip). Reached from the jump entry of a shim stub: when the
// guest called the stub, its call entry has pushed the shadow entry; when the guest jumped to
// it (a PLT stub's tail jump), the entry of the original call is on top. Either way the shim
// returns like a guest function: it pops the guest return address (SHIM_RETURN) and the bridge
// then does what a guest RET does.
// Frame: [sp] x29, x30; [sp+16] shim, ctx; [sp+32] guest FPCR. The frame record links to the
// recomp_enter_guest frame (ctx->host_boundary), so host backtraces from a shim stay on the
// host stack; its return address is the continuation of the innermost guest call.
GUEST_SECTION
.globl _recomp_call_shim
_recomp_call_shim:
    add  x2, x27, #0x70
    ldr  x2, [x28, x2]           // ctx: the TCB slot
    str  x1, [x2, #0x80]         // rip
    SAVE_GPRS x2
    SAVE_XMM0_7 x2
    str  x29, [x2, {SSP}]        // shadow_sp
    b    _nps5_xs_call_shim_host

HOST_SECTION
_nps5_xs_call_shim_host:
    ldr  x30, [x29, #16]
    ldr  x29, [x2, {HB}]         // host_boundary
    stp  x29, x30, [sp, #-48]!
    mov  x29, sp
    stp  x0, x2, [sp, #16]
    mrs  x3, fpcr
    str  x3, [sp, #32]
    tst  x3, #0xc00000           // RMode
    b.eq 1f
    and  x4, x3, #0xffffffffff3fffff
    msr  fpcr, x4
1:
    mov  x0, x2
    ldr  x3, [sp, #16]
    adrp x4, _g_nps5_hle_hook@PAGE
    ldr  x4, [x4, _g_nps5_hle_hook@PAGEOFF]
    cbz  x4, 3f
    mov  x1, x3
    blr  x4
    ldr  x0, [sp, #24]
    ldr  x3, [sp, #16]
3:
    blr  x3
    // Performance trace (nps5_perf.c, NPS5_PERF=1): time spent in the shim. Null when off.
    adrp x4, _g_nps5_hle_exit_hook@PAGE
    ldr  x4, [x4, _g_nps5_hle_exit_hook@PAGEOFF]
    cbz  x4, 4f
    ldp  x1, x0, [sp, #16]
    blr  x4
4:

    // The ctx this call stored into (a context switch inside the shim may have changed the TCB
    // slot); its shadow stack pointer is whatever the shim left in ctx->shadow_sp.
    ldr  x1, [sp, #24]
    ldr  x2, [sp, #32]
    mrs  x3, fpcr
    cmp  x2, x3
    b.eq 2f
    msr  fpcr, x2
2:
    LOAD_GPRS x1
    LOAD_XMM0_7 x1
    ldr  x27, [x1, #0x88]        // fs_base
    ldr  x28, [x1, #0x330]       // mem_base
    add  sp, sp, #48
    ldr  x29, [x1, {SSP}]        // shadow_sp
    b    _nps5_xs_shim_return

GUEST_SECTION
_nps5_xs_shim_return:
    sub  x17, x26, #8
    ldr  x7, [x28, x17]          // the guest return address the shim returned through
    ldp  x0, x1, [x29], #32
    cmp  x7, x0
    ccmp x17, x1, #0, eq
    b.ne Lret_slow
    ldur x30, [x29, #-16]
    ret


// _recomp_call_lean: the lean bridge (docs/m3/leanshim.md). Reached from a lean shim stub
// (writeLeanStub) after the stub has stored the 16 guest GPRs, ctx->rip and the shadow stack
// pointer to the context, and set up:
//   x0-x5 the System V integer argument registers, x6 = ctx, x16 = ctx, x17 = the lean function,
//   w7 = g_nps5_lean_mode (1 lean, 2 with hooks, 3 checked), x29 = the shadow stack pointer.
// Like the full bridge it replaces x29 by the frame record of the innermost recomp_enter_guest
// frame, whose return address is the host continuation of the innermost guest call. xmm0-7 are not
// stored or reloaded (System V: they die at a call and the function takes and returns none), the
// FPCR is left as the guest set it. The callee preserves x19-x28 (rax, rbx, rcx, rdx, rsi, rdi,
// rbp, rsp, FS base, memory base) by the AAPCS; x8-x15 (r8-r15) are reloaded from the context.
// Frame: [sp] x29, x30; [sp+16] lean function, ctx.
HOST_SECTION
.globl _recomp_call_lean
_recomp_call_lean:
    ldr  x30, [x29, #16]
    ldr  x29, [x16, {HB}]        // host_boundary
    stp  x29, x30, [sp, #-32]!
    mov  x29, sp
    stp  x17, x16, [sp, #16]
    cmp  w7, #1
    b.ne Llean_slow
    blr  x17
_nps5_xs_lean_done:
    mov  x19, x0                 // rax
    ldr  x16, [sp, #24]
    ldp  x8,  x9,  [x16, #0x40]
    ldp  x10, x11, [x16, #0x50]
    ldp  x12, x13, [x16, #0x60]
    ldp  x14, x15, [x16, #0x70]
    ldr  x29, [x16, {SSP}]       // shadow_sp
    add  sp, sp, #32
    add  x26, x26, #8            // the function returns like a guest function: pop the return address
    b    _nps5_xs_shim_return

// Hooks (mode 2) and the register-preservation checker (mode 3). Frame (384 bytes): [0] the six
// arguments, [48] mode, [56] the result, [64] callee-saved registers before the call (x19-x28,
// d8-d15), [208] the same after it, [352] ctx->shadow_sp and ctx->host_boundary before it,
// [368] the lean function, [376] ctx. After the call everything is addressed from sp: the callee
// may have broken any other register, and the checker must still be able to say which.
Llean_slow:
    sub  sp, sp, #384
    stp  x0, x1, [sp]
    stp  x2, x3, [sp, #16]
    stp  x4, x5, [sp, #32]
    str  x7, [sp, #48]
    stp  x17, x16, [sp, #368]
    mov  x0, x16
    mov  x1, x17
    adrp x2, _g_nps5_hle_hook@PAGE
    ldr  x2, [x2, _g_nps5_hle_hook@PAGEOFF]
    cbz  x2, 1f
    blr  x2
1:
    ldr  x7, [sp, #48]
    cmp  w7, #3
    b.ne 3f
    ldp  x17, x16, [sp, #368]
    str  x17, [x16, {LEAN}]      // ctx->lean_active: recomp_call_entry refuses to enter guest code
    ldr  x0, [x16, {SSP}]
    ldr  x1, [x16, {HB}]
    stp  x0, x1, [sp, #352]
    stp  x19, x20, [sp, #64]
    stp  x21, x22, [sp, #80]
    stp  x23, x24, [sp, #96]
    stp  x25, x26, [sp, #112]
    stp  x27, x28, [sp, #128]
    stp  d8,  d9,  [sp, #144]
    stp  d10, d11, [sp, #160]
    stp  d12, d13, [sp, #176]
    stp  d14, d15, [sp, #192]
3:
    ldp  x17, x6, [sp, #368]
    ldp  x0, x1, [sp]
    ldp  x2, x3, [sp, #16]
    ldp  x4, x5, [sp, #32]
    blr  x17
    str  x0, [sp, #56]
    ldr  x7, [sp, #48]
    cmp  w7, #3
    b.ne 4f
    stp  x19, x20, [sp, #208]
    stp  x21, x22, [sp, #224]
    stp  x23, x24, [sp, #240]
    stp  x25, x26, [sp, #256]
    stp  x27, x28, [sp, #272]
    stp  d8,  d9,  [sp, #288]
    stp  d10, d11, [sp, #304]
    stp  d12, d13, [sp, #320]
    stp  d14, d15, [sp, #336]
    ldp  x1, x0, [sp, #368]      // lean function, ctx
    add  x2, sp, #64
    add  x3, sp, #208
    add  x4, sp, #384            // the sp of the lean frame
    mov  x5, x29                 // x29 as the callee left it (it must be this frame's record)
    add  x6, sp, #352
    bl   _nps5_lean_check_regs
4:
    // Performance trace (nps5_perf.c, NPS5_PERF=1): time spent in the function. Null when off.
    adrp x2, _g_nps5_hle_exit_hook@PAGE
    ldr  x2, [x2, _g_nps5_hle_exit_hook@PAGEOFF]
    cbz  x2, 5f
    ldp  x1, x0, [sp, #368]
    blr  x2
5:
    ldr  x0, [sp, #56]
    add  sp, sp, #384
    b    _nps5_xs_lean_done
`

// Flat call model: indirect branch entry points, the slow paths and the non-local resume.
const trampolineFlatDispatch = `// Three-level dispatch table lookup of the guest RIP in x0: the registered (call) entry in x1,
// or a branch to 99 when the target is unknown. Keeps x0, x7 (the guest return address of an
// indirect call) and x30.
.macro DISPATCH_LOOKUP_FLAT
    adrp x1, _g_dispatch_l1@PAGE
    add  x1, x1, _g_dispatch_l1@PAGEOFF
    ubfx x2, x0, #32, #16
    ldr  x1, [x1, x2, lsl #3]
    cbz  x1, 99f
    ubfx x2, x0, #16, #16
    ldr  x1, [x1, x2, lsl #3]
    cbz  x1, 99f
    and  x2, x0, #0xffff
    ldr  x1, [x1, x2, lsl #3]
    cbz  x1, 99f
.endm

GUEST_SECTION

// _recomp_indirect_call: x0 = guest target RIP, x7 = guest return address (already pushed on
// the guest stack), x30 = host continuation (the call site used bl).
.globl _recomp_indirect_call
_recomp_indirect_call:
    DISPATCH_LOOKUP_FLAT
    br   x1                      // call entry: pushes the shadow entry
99:
    stp  x7, x26, [x29, #-32]!   // what the call entry would have pushed
    str  x30, [x29, #16]
    mov  x1, #2                  // RECOMP_MISS_CALL
    b    _recomp_dispatch_continue

// _recomp_indirect_call_ic: the miss path of an inline-cached indirect call site. x0 = guest
// target RIP, x1 = the site record (48 bytes, 16-aligned: slot0 {guest, entry}, slot1, miss
// counter), x7 = guest return address (already pushed on the guest stack), x30 = host
// continuation. Looks the target up in the dispatch table, publishes {target, call entry} into a
// slot with one 16-byte STP (single-copy atomic: FEAT_LSE2, checked by the runtime), and enters
// the call entry. A target that is not translated yet is never cached (the slow path below).
// Flush protocol (nps5_shadow.c, recomp_ic_flush_*): the epoch is read before the lookup; after
// the store a full barrier and a re-read; a changed epoch means a flush ran concurrently, so the
// slot just written is cleared again.
.globl _recomp_indirect_call_ic
_recomp_indirect_call_ic:
    ldr  x3, [x1, #32]           // megamorphic site (64 misses with both slots taken): plain lookup,
    cmp  x3, #64                 // no stores to the shared site record
    b.lo 1f
    adrp x2, _g_dispatch_l1@PAGE
    add  x2, x2, _g_dispatch_l1@PAGEOFF
    ubfx x3, x0, #32, #16
    ldr  x2, [x2, x3, lsl #3]
    cbz  x2, 99f
    ubfx x3, x0, #16, #16
    ldr  x2, [x2, x3, lsl #3]
    cbz  x2, 99f
    and  x3, x0, #0xffff
    ldr  x2, [x2, x3, lsl #3]
    cbz  x2, 99f
    br   x2
1:
    adrp x16, _g_recomp_ic@PAGE
    add  x16, x16, _g_recomp_ic@PAGEOFF
    ldar x5, [x16]               // epoch
    adrp x2, _g_dispatch_l1@PAGE
    add  x2, x2, _g_dispatch_l1@PAGEOFF
    ubfx x3, x0, #32, #16
    ldr  x2, [x2, x3, lsl #3]
    cbz  x2, 99f
    ubfx x3, x0, #16, #16
    ldr  x2, [x2, x3, lsl #3]
    cbz  x2, 99f
    and  x3, x0, #0xffff
    ldr  x2, [x2, x3, lsl #3]
    cbz  x2, 99f
    ldr  x6, [x16, #8]           // enabled
    cbz  x6, 2f
    mov  x6, x1                  // slot0 when empty
    ldr  x3, [x1]
    cmn  x3, #1
    b.eq 5f
    add  x6, x1, #16             // slot1 when empty
    ldr  x3, [x6]
    cmn  x3, #1
    b.eq 5f
    ldr  x3, [x1, #32]           // both taken: replace slot1 on every 16th miss
    add  x3, x3, #1
    str  x3, [x1, #32]
    tst  x3, #15
    b.ne 2f
5:  stp  x0, x2, [x6]
    dmb  ish
    ldr  x3, [x16]
    cmp  x3, x5
    b.eq 2f
    ldr  x3, [x16, #16]          // flushed meanwhile: un-publish
    movn x4, #0
    stp  x4, x3, [x6]
2:  br   x2                      // call entry: pushes the shadow entry
99:
    stp  x7, x26, [x29, #-32]!   // what the call entry would have pushed
    str  x30, [x29, #16]
    mov  x1, #2                  // RECOMP_MISS_CALL
    b    _recomp_dispatch_continue

// _recomp_indirect_jump_ic: the miss path of an inline-cached jmp reg / jmp [mem] site (docs/m3/liftmisc.md
// item 3). Same record, same protocol as _recomp_indirect_call_ic; the slot holds {guest target, JUMP
// entry} (the call entry + FlatJumpEntryOffset), so a hit is br x3 and a flush, which clears the slots
// whose host entry lies in the unloaded range, treats both kinds alike. x0 = guest target RIP,
// x1 = the site record. An empty slot {~0, _recomp_indirect_call} is only reached by a jump to ~0, which
// is fatal anyway (it is reported as a call miss).
.globl _recomp_indirect_jump_ic
_recomp_indirect_jump_ic:
    ldr  x3, [x1, #32]
    cmp  x3, #64
    b.lo 1f
    adrp x2, _g_dispatch_l1@PAGE
    add  x2, x2, _g_dispatch_l1@PAGEOFF
    ubfx x3, x0, #32, #16
    ldr  x2, [x2, x3, lsl #3]
    cbz  x2, 99f
    ubfx x3, x0, #16, #16
    ldr  x2, [x2, x3, lsl #3]
    cbz  x2, 99f
    and  x3, x0, #0xffff
    ldr  x2, [x2, x3, lsl #3]
    cbz  x2, 99f
    add  x2, x2, #8
    br   x2
1:
    adrp x16, _g_recomp_ic@PAGE
    add  x16, x16, _g_recomp_ic@PAGEOFF
    ldar x5, [x16]               // epoch
    adrp x2, _g_dispatch_l1@PAGE
    add  x2, x2, _g_dispatch_l1@PAGEOFF
    ubfx x3, x0, #32, #16
    ldr  x2, [x2, x3, lsl #3]
    cbz  x2, 99f
    ubfx x3, x0, #16, #16
    ldr  x2, [x2, x3, lsl #3]
    cbz  x2, 99f
    and  x3, x0, #0xffff
    ldr  x2, [x2, x3, lsl #3]
    cbz  x2, 99f
    add  x2, x2, #8              // jump entry
    ldr  x6, [x16, #8]           // enabled
    cbz  x6, 2f
    mov  x6, x1                  // slot0 when empty
    ldr  x3, [x1]
    cmn  x3, #1
    b.eq 5f
    add  x6, x1, #16             // slot1 when empty
    ldr  x3, [x6]
    cmn  x3, #1
    b.eq 5f
    ldr  x3, [x1, #32]           // both taken: replace slot1 on every 16th miss
    add  x3, x3, #1
    str  x3, [x1, #32]
    tst  x3, #15
    b.ne 2f
5:  stp  x0, x2, [x6]
    dmb  ish
    ldr  x3, [x16]
    cmp  x3, x5
    b.eq 2f
    ldr  x3, [x16, #16]          // flushed meanwhile: un-publish
    movn x4, #0
    stp  x4, x3, [x6]
2:  br   x2                      // jump entry
99:
    mov  x1, #3                  // RECOMP_MISS_JUMP
    b    _recomp_dispatch_continue

// _recomp_indirect_jump: x0 = guest target RIP.
.globl _recomp_indirect_jump
_recomp_indirect_jump:
    DISPATCH_LOOKUP_FLAT
    add  x1, x1, #8              // jump entry
    br   x1
99:
    mov  x1, #3                  // RECOMP_MISS_JUMP
    b    _recomp_dispatch_continue

// _recomp_ret_slow: a guest RET whose {return address, rsp} did not match the shadow top.
// x7 = the guest return address (popped, x26 adjusted); the RET has popped the shadow entry.
.globl _recomp_ret_slow
_recomp_ret_slow:
Lret_slow:
    sub  x29, x29, #32           // un-pop: recomp_flat_continue decides what the entry is worth
    mov  x0, x7
    mov  x1, #1                  // RECOMP_MISS_RET
    // falls through

// _recomp_dispatch_continue: continue the guest at guest RIP x0 (x1 = RecompMissReason) when the
// fast paths cannot: stores the guest registers, asks recomp_flat_continue (nps5_shadow.c) for
// the host continuation - the shadow stack, the continuation table, the dispatch table, then
// recomp_dispatch_miss (recomp_dispatch_api.h) - and branches there. All vector registers
// survive; NZCV does not (flags are dead at every way in). Never returns to its caller.
.globl _recomp_dispatch_continue
_recomp_dispatch_continue:
    add  x2, x27, #0x70
    ldr  x2, [x28, x2]           // ctx: the TCB slot
    str  x0, [x2, #0x80]         // rip
    SAVE_GPRS x2
    str  x29, [x2, {SSP}]        // shadow_sp
    b    _nps5_xs_continue_host

HOST_SECTION
_nps5_xs_continue_host:
    ldr  x29, [x2, {HB}]         // host_boundary
    stp  x29, x30, [sp, #-32]!
    mov  x29, sp
    mrs  x3, fpcr
    stp  x2, x3, [sp, #16]
    tst  x3, #0xc00000           // RMode: C code runs with round to nearest
    b.eq 1f
    and  x4, x3, #0xffffffffff3fffff
    msr  fpcr, x4
1:
    SAVE_Q0_31
    mov  x3, x1
    mov  x1, x0
    mov  x0, x2
    mov  x2, x3
    bl   _recomp_flat_continue   // (ctx, guest rip, reason) -> host address; fatal inside if none
    mov  x16, x0
    LOAD_Q0_31
    ldp  x1, x2, [sp, #16]
    mrs  x3, fpcr
    cmp  x2, x3
    b.eq 2f
    msr  fpcr, x2
2:
    add  sp, sp, #32
    LOAD_GPRS x1
    ldr  x27, [x1, #0x88]        // fs_base
    ldr  x28, [x1, #0x330]       // mem_base
    ldr  x29, [x1, {SSP}]        // shadow_sp
    br   x16

// Non-local control transfer (setjmp/longjmp, C++ landing pads; nps5_eh.c):
//   void _recomp_resume_guest(GuestContext *ctx, uintptr_t unused, uintptr_t host_pc, uint64_t fpcr)
// The caller (a shim) has stored the target guest registers and the trimmed shadow stack
// pointer in ctx. Loads them, sets the guest FPCR, drops every host frame above the context's
// recomp_enter_guest boundary and branches to host_pc (block entry state). Never returns.
.globl _recomp_resume_guest
_recomp_resume_guest:
    mrs  x4, fpcr
    cmp  x3, x4
    b.eq 1f
    msr  fpcr, x3
1:
    LOAD_GPRS x0
    ldr  x27, [x0, #0x88]        // fs_base
    ldr  x28, [x0, #0x330]       // mem_base
    ldr  x16, [x0, {HB}]         // host_boundary
    mov  sp, x16
    ldr  x29, [x0, {SSP}]        // shadow_sp
    br   x2
`
