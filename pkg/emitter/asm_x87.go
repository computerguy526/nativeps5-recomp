package emitter

import (
	"strings"

	"ps4-recomp/pkg/lifter"
)

// X87BridgeAsm is the guest -> host bridge of the x87=f80 lowering (pkg/lifter
// arm64_lifter_x87f80.go, runtime pkg/runtime/core/nps5_x87.c). Three entries share one save/restore:
//
//	_recomp_x87      x0 = the op word, x1 = the argument (a host pointer to the memory operand, or a
//	                 value); the result (a status/control word, or NZCV bits for FCOMI) is in x0.
//	_recomp_x87_a    as _recomp_x87, for the arithmetic and compare kinds: first tries recomp_x87_fast (nps5_x87.c,
//	                 preserve_most, integer-only: no host state is saved), the full helper only if that declines.
//	_recomp_x87_run  x0 = pointer to n (op word, argument) pairs of 16 bytes, x1 = n: the lifter
//	                 batches a run of consecutive x87 instructions into one call. The result in x0 is
//	                 that of the last one (NZCV bits when the run ends with an FCOMI).
//
// Contract: the lifter calls them with bl and keeps nothing live in x0-x7, x16, x17. Everything
// else survives, but only what the C code can clobber is saved:
//   - NZCV, and the guest GPRs in x8-x15 (caller-saved in AAPCS64; stored to ctx and reloaded).
//     x19-x26 (guest rax..rdi, rsp, rbp) and x27/x28 are callee-saved, the C code preserves them.
//   - the vector registers v0-v7 (guest xmm0-xmm7). v8-v15 (the other guest XMM registers) are
//     callee-saved by AAPCS64 only in their low 64 bits, and v16-v31 (YMM upper halves) are
//     caller-saved, but nps5_x87.c and the SoftFloat subset it calls only ever use v0-v7 and
//     scalar integer registers: TestX87RuntimeVectorRegs disassembles the objects and fails when
//     that stops being true, and TestX87BridgeKeepsVectors runs the lowered code over all 32.
//
// It is self-contained (no macros of trampolineAsm) so the FEX harness can assemble it on its own.
const X87BridgeAsm = `
// ---- x87=f80 bridge ----
.section __TEXT,__recomp_guest,regular,pure_instructions
.p2align 2
.globl _recomp_x87
_recomp_x87:
    stp  x29, x30, [sp, #-32]!
    mov  x29, sp
    mrs  x16, nzcv
    str  x16, [sp, #16]
    add  x16, x27, #0x70
    ldr  x16, [x28, x16]
    stp  x8,  x9,  [x16, #0x40]
    stp  x10, x11, [x16, #0x50]
    stp  x12, x13, [x16, #0x60]
    stp  x14, x15, [x16, #0x70]
    b    _nps5_xs_recomp_x87_host

.p2align 2
.globl _recomp_x87_a
_recomp_x87_a:
    stp  x29, x30, [sp, #-32]!
    mov  x29, sp
    mrs  x16, nzcv
    stp  x16, x8, [sp, #16]
    sub  sp, sp, #16
    stp  x0, x1, [sp]
    add  x16, x27, #0x70
    ldr  x16, [x28, x16]
    mov  x2, x1
    mov  x1, x0
    mov  x0, x16
    bl   _recomp_x87_fast
    cbz  x1, Lrecomp_x87_a_slow
    add  sp, sp, #16
    ldp  x16, x8, [sp, #16]
    msr  nzcv, x16
    ldp  x29, x30, [sp], #32
    ret
Lrecomp_x87_a_slow:
    ldp  x0, x1, [sp], #16
    ldr  x8, [sp, #24]
    add  x16, x27, #0x70
    ldr  x16, [x28, x16]
    stp  x8,  x9,  [x16, #0x40]
    stp  x10, x11, [x16, #0x50]
    stp  x12, x13, [x16, #0x60]
    stp  x14, x15, [x16, #0x70]
    b    _nps5_xs_recomp_x87_host

.p2align 2
.globl _recomp_x87_run
_recomp_x87_run:
    stp  x29, x30, [sp, #-32]!
    mov  x29, sp
    mrs  x16, nzcv
    stp  x16, x8, [sp, #16]
    sub  sp, sp, #16
    stp  x0, x1, [sp]
    add  x16, x27, #0x70
    ldr  x16, [x28, x16]
    mov  x2, x1
    mov  x1, x0
    mov  x0, x16
    bl   _recomp_x87_run_fast
    ldp  x2, x3, [sp], #16
    subs x3, x3, x1
    b.ne Lrecomp_x87_run_slow
    ldp  x16, x8, [sp, #16]
    msr  nzcv, x16
    ldp  x29, x30, [sp], #32
    ret
Lrecomp_x87_run_slow:
    ldr  x8, [sp, #24]
    add  x2, x2, x1, lsl #4
    mov  x0, x2
    mov  x1, x3
    add  x16, x27, #0x70
    ldr  x16, [x28, x16]
    stp  x8,  x9,  [x16, #0x40]
    stp  x10, x11, [x16, #0x50]
    stp  x12, x13, [x16, #0x60]
    stp  x14, x15, [x16, #0x70]
    b    _nps5_xs_recomp_x87_run_host

.text
.p2align 2
_nps5_xs_recomp_x87_host:
    sub  sp, sp, #128
    stp  q0,  q1,  [sp, #0]
    stp  q2,  q3,  [sp, #32]
    stp  q4,  q5,  [sp, #64]
    stp  q6,  q7,  [sp, #96]
    mov  x2, x1
    mov  x1, x0
    mov  x0, x16
    bl   _recomp_x87_impl
    b    _nps5_xs_recomp_x87_ret

.p2align 2
_nps5_xs_recomp_x87_run_host:
    sub  sp, sp, #128
    stp  q0,  q1,  [sp, #0]
    stp  q2,  q3,  [sp, #32]
    stp  q4,  q5,  [sp, #64]
    stp  q6,  q7,  [sp, #96]
    mov  x2, x1
    mov  x1, x0
    mov  x0, x16
    bl   _recomp_x87_run_impl
    b    _nps5_xs_recomp_x87_ret

.p2align 2
_nps5_xs_recomp_x87_ret:
    ldp  q0,  q1,  [sp, #0]
    ldp  q2,  q3,  [sp, #32]
    ldp  q4,  q5,  [sp, #64]
    ldp  q6,  q7,  [sp, #96]
    add  sp, sp, #128
    add  x16, x27, #0x70
    ldr  x16, [x28, x16]
    ldp  x8,  x9,  [x16, #0x40]
    ldp  x10, x11, [x16, #0x50]
    ldp  x12, x13, [x16, #0x60]
    ldp  x14, x15, [x16, #0x70]
    ldr  x16, [sp, #16]
    msr  nzcv, x16
    ldp  x29, x30, [sp], #32
    ret
`

// X87BridgeSource returns the x87 bridge for the selected call model. In the flat model x29 is the
// shadow return stack pointer while the host pc is in __recomp_guest (docs/m1/flatstack.md): the
// bridge saves and restores it like any callee-saved register but does not turn it into a frame
// pointer, as the other mid-instruction helpers of the flat trampoline.
func X87BridgeSource(model lifter.CallModel) string {
	if model != lifter.CallFlat {
		return X87BridgeAsm
	}
	if strings.Count(X87BridgeAsm, "    mov  x29, sp\n") != 3 {
		panic("emitter: X87BridgeAsm: expected three frame-pointer links")
	}
	return strings.ReplaceAll(X87BridgeAsm, "    mov  x29, sp\n", "")
}
