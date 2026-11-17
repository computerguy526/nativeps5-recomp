package lifter

import (
	"fmt"

	"ps4-recomp/pkg/disasm"

	"golang.org/x/arch/x86/x86asm"
)

// x87=f80 performance paths (docs/m2/x87-f80.md, "Performance"):
//
//  1. Inline fast paths for the instructions that need no SoftFloat: data movement between
//     registers and memory, the sign operations, the TOP/tag bookkeeping, the control and status
//     word moves, the exact integer -> extended conversion. They work on the x87 state of the
//     GuestContext (x16 = ctx; fpu_top, fpu_valid, fpu80[], fpu_sw, fpu_cw) exactly as the helper
//     in nps5_x87.c does. Whenever the helper would report an exception (stack overflow or
//     underflow) the fast path has changed nothing yet and branches to a call of the bridge with
//     the same op word, so the exceptional behaviour stays in one place (the C helper).
//     The sequences use x0-x7, x16, x17 only, no vector register and no instruction that sets
//     NZCV (the guest flags live there).
//  2. Batching: a run of two or more consecutive helper operations of one basic block becomes one
//     call of _recomp_x87_run (one save/restore of the host state) with the (op word, argument)
//     pairs built on the host stack.

// Offsets in GuestContext (recomp_context.h asserts them).
const (
	x87CtxTop   = 0x2e0 // int fpu_top
	x87CtxCw    = 0x2e4 // uint16_t fpu_cw
	x87CtxSw    = 0x2e6 // uint16_t fpu_sw (without TOP)
	x87CtxValid = 0xc50 // uint8_t fpu_valid
	x87SlotSE   = x87CtxFpu80 + 8
)

// x87MaxBatch bounds a batch: its pairs are addressed with the stp immediate (-512..504).
const x87MaxBatch = 16

// SetX87Opt switches the x87=f80 inline fast paths and the batching of helper runs. Both are on
// for a new lifter.
func (l *Arm64Lifter) SetX87Opt(inline, batch bool) {
	l.x87NoInline = !inline
	l.x87NoBatch = !batch
}

// SetBlock tells the lifter which basic block the following instructions belong to (batching looks
// ahead in it). A nil block, or never calling it, turns batching off.
func (l *Arm64Lifter) SetBlock(b *disasm.BasicBlock) {
	l.x87Blk = b
	l.x87RunFirst, l.x87RunLast = 0, 0
	l.ppSkipFrom, l.ppSkipUntil = 0, 0
	l.fuseSkip, l.cbzAt, l.cbzReg = 0, 0, ""
}

// x87Emit lowers one helper operation, whose memory pointer (if any) is already in x1.
func (l *Arm64Lifter) x87Emit(w x87Word) []string {
	if l.x87NoInline {
		return x87Call(w)
	}
	body, slow := l.x87Inline(w)
	if body == nil {
		return x87Call(w)
	}
	lines := append(l.emitLoadCtxPtr("x16"), body...)
	if slow {
		lines = append(lines, "    b    7f", "6:")
		lines = append(lines, x87Call(w)...)
		lines = append(lines, "7:")
	}
	return lines
}

func slotAddr(dst, phys string) string {
	return fmt.Sprintf("    add  %s, x16, %s, uxtw #4", dst, phys)
}

// x87C1Clear clears C1 of the status word (set_c1(c, 0)); scratch w5.
var x87C1Clear = []string{
	fmt.Sprintf("    ldrh w5, [x16, #%#x]", x87CtxSw),
	"    and  w5, w5, #0xfffffdff",
	fmt.Sprintf("    strh w5, [x16, #%#x]", x87CtxSw),
}

// x87PopTop pops the register stack (st_pop): w2 = top, w3 = fpu_valid, both in registers.
var x87PopTop = []string{
	"    mov  w5, #1",
	"    lsl  w5, w5, w2",
	"    bic  w3, w3, w5",
	fmt.Sprintf("    strb w3, [x16, #%#x]", x87CtxValid),
	"    add  w2, w2, #1",
	"    and  w2, w2, #7",
	fmt.Sprintf("    str  w2, [x16, #%#x]", x87CtxTop),
}

// x87LoadTopValid loads w2 = fpu_top and w3 = fpu_valid.
var x87LoadTopValid = []string{
	fmt.Sprintf("    ldr  w2, [x16, #%#x]", x87CtxTop),
	fmt.Sprintf("    ldrb w3, [x16, #%#x]", x87CtxValid),
}

// x87PushPrep computes w4 = the new top and branches to the slow path when that register is
// not empty (stack overflow).
var x87PushPrep = cat(x87LoadTopValid, []string{
	"    sub  w4, w2, #1",
	"    and  w4, w4, #7",
	"    lsr  w5, w3, w4",
	"    tbnz w5, #0, 6f"})

// x87PushCommit pushes the value x0 (significand) / w17 (sign + exponent) into register w4.
var x87PushCommit = cat([]string{
	fmt.Sprintf("    str  w4, [x16, #%#x]", x87CtxTop),
	"    mov  w5, #1",
	"    lsl  w5, w5, w4",
	"    orr  w3, w3, w5",
	fmt.Sprintf("    strb w3, [x16, #%#x]", x87CtxValid),
	slotAddr("x5", "w4"),
	fmt.Sprintf("    str  x0, [x5, #%#x]", x87CtxFpu80),
	fmt.Sprintf("    strh w17, [x5, #%#x]", x87SlotSE),
}, x87C1Clear)

func x87Ld(dstM, dstSE, slot string) []string {
	return []string{
		fmt.Sprintf("    ldr  %s, [%s, #%#x]", dstM, slot, x87CtxFpu80),
		fmt.Sprintf("    ldrh %s, [%s, #%#x]", dstSE, slot, x87SlotSE),
	}
}
func x87St(srcM, srcSE, slot string) []string {
	return []string{
		fmt.Sprintf("    str  %s, [%s, #%#x]", srcM, slot, x87CtxFpu80),
		fmt.Sprintf("    strh %s, [%s, #%#x]", srcSE, slot, x87SlotSE),
	}
}

func cat(parts ...[]string) []string {
	var out []string
	for _, p := range parts {
		out = append(out, p...)
	}
	return out
}

// x87Inline returns the fast path of an operation (nil when the operation is a helper call) and
// whether it can branch to the slow path (label 6).
func (l *Arm64Lifter) x87Inline(w x87Word) (body []string, slow bool) {
	checkSt0 := []string{"    lsr  w5, w3, w2", "    tbz  w5, #0, 6f"} // ST(0) not empty
	switch w.kind {
	case x87KFldSt:
		return cat(x87LoadTopValid, []string{
			fmt.Sprintf("    add  w6, w2, #%d", w.a),
			"    and  w6, w6, #7",
			"    lsr  w5, w3, w6",
			"    tbz  w5, #0, 6f", // ST(i) empty: underflow
			"    sub  w4, w2, #1",
			"    and  w4, w4, #7",
			"    lsr  w5, w3, w4",
			"    tbnz w5, #0, 6f", // destination not empty: overflow
			slotAddr("x7", "w6")}, x87Ld("x0", "w17", "x7"), x87PushCommit), true
	case x87KFldConst:
		if w.a != 0 && w.a != 6 {
			return nil, false // log2(10), pi, ...: rounded by RC, helper
		}
		mant, se := "    movz x0, #0x8000, lsl #48", "    mov  w17, #0x3fff"
		if w.a == 6 {
			mant, se = "    mov  x0, #0", "    mov  w17, #0"
		}
		return cat(x87PushPrep, []string{mant, se}, x87PushCommit), true
	case x87KFldM:
		switch w.sz {
		case 0: // float32: normal numbers only (zero, denormal, infinity and NaN take the helper)
			return cat(x87PushPrep, []string{
				"    ldr  w6, [x1]",
				"    ubfx w7, w6, #23, #8",
				"    cbz  w7, 6f",
				"    add  w5, w7, #1",
				"    tbnz w5, #8, 6f",
				"    and  x0, x6, #0x7fffff",
				"    lsl  x0, x0, #40",
				"    orr  x0, x0, #0x8000000000000000",
				"    mov  w17, #0x3f80", // exponent bias 0x3fff - 0x7f
				"    add  w17, w17, w7",
				"    lsr  w5, w6, #31",
				"    orr  w17, w17, w5, lsl #15"}, x87PushCommit), true
		case 1: // float64, normal numbers only
			return cat(x87PushPrep, []string{
				"    ldr  x6, [x1]",
				"    ubfx x7, x6, #52, #11",
				"    cbz  w7, 6f",
				"    add  w5, w7, #1",
				"    tbnz w5, #11, 6f",
				"    lsl  x0, x6, #11",
				"    orr  x0, x0, #0x8000000000000000",
				"    mov  w17, #0x3c00", // exponent bias 0x3fff - 0x3ff
				"    add  w17, w17, w7",
				"    lsr  x5, x6, #63",
				"    orr  w17, w17, w5, lsl #15"}, x87PushCommit), true
		}
		return cat(x87PushPrep, []string{
			"    ldr  x0, [x1]", "    ldrh w17, [x1, #8]"}, x87PushCommit), true
	case x87KFild:
		ld := [...]string{"ldrsh", "ldrsw", "ldr"}[w.sz]
		return cat(x87PushPrep, []string{
			fmt.Sprintf("    %s x7, [x1]", ld),
			"    asr  x6, x7, #63", // sign mask
			"    eor  x0, x7, x6",
			"    sub  x0, x0, x6", // |x|
			"    clz  x5, x0",
			"    lsl  x0, x0, x5", // normalised significand (0 stays 0)
			"    mov  w17, #0x403e",
			"    sub  w17, w17, w5",
			"    and  w6, w6, #0x8000",
			"    orr  w17, w17, w6",
			"    sub  x2, xzr, x7", // zero gives sign and exponent 0
			"    orr  x2, x2, x7",
			"    asr  x2, x2, #63",
			"    and  w17, w17, w2"}, x87PushCommit), true
	case x87KFist:
		if w.aux != 1 { // only FISTTP (truncation): FIST/FISTP round by RC in the helper
			return nil, false
		}
		n := [...]int{15, 31, 63}[w.sz]
		st := [...]string{"    strh w4, [x1]", "    str  w4, [x1]", "    str  x4, [x1]"}[w.sz]
		// Fast path: |x| < 2^n, or |x| < 1, a normal number (or zero). The helper takes every
		// other value (NaN, infinity, out of range, unnormal) and the empty register.
		b := cat(x87LoadTopValid, checkSt0, []string{slotAddr("x5", "w2")}, x87Ld("x0", "w6", "x5"), []string{
			"    and  w7, w6, #0x7fff",
			"    mov  w5, #0x3fff",
			"    sub  w5, w7, w5", // E = unbiased exponent
			"    tbnz w5, #31, 4f",
			fmt.Sprintf("    sub  w4, w5, #%d", n),
			"    tbz  w4, #31, 6f", // E >= n: out of range
			"    tbz  x0, #63, 6f", // unnormal
			"    mov  w4, #63",
			"    sub  w4, w4, w5",
			"    lsr  x4, x0, x4", // |trunc(x)|
			"    lsl  x7, x0, x5",
			"    lsl  x7, x7, #1", // the bits cut off: nonzero = inexact
			"    tbz  w6, #15, 5f",
			"    neg  x4, x4",
			"    b    5f",
			"4:", // |x| < 1: 0 and inexact unless x is zero
			"    lsr  x17, x0, #63",
			"    cbz  w7, 8f",
			"    cbz  x17, 6f", // unnormal
			"    b    9f",
			"8:",
			"    cbnz x17, 6f", // pseudo-denormal
			"9:",
			"    mov  x4, #0",
			"    mov  x7, x0",
			"5:",
			"    cbz  x7, 3f",
			fmt.Sprintf("    ldrh w5, [x16, #%#x]", x87CtxSw),
			"    orr  w5, w5, #0x20", // PE
			fmt.Sprintf("    strh w5, [x16, #%#x]", x87CtxSw),
			"3:", st}, x87C1Clear, x87PopTop)
		return b, true
	case x87KFstM:
		if w.sz != 2 {
			return nil, false
		}
		b := cat(x87LoadTopValid, checkSt0, []string{slotAddr("x5", "w2")}, x87Ld("x0", "w17", "x5"),
			[]string{"    str  x0, [x1]", "    strh w17, [x1, #8]"}, x87C1Clear)
		if w.pop {
			b = append(b, x87PopTop...)
		}
		return b, true
	case x87KFstSt:
		b := cat(x87LoadTopValid, checkSt0)
		if w.a != 0 { // ST(0) to ST(i): the destination becomes valid
			b = cat(b, []string{
				fmt.Sprintf("    add  w4, w2, #%d", w.a),
				"    and  w4, w4, #7",
				slotAddr("x5", "w2")}, x87Ld("x0", "w17", "x5"), []string{slotAddr("x5", "w4")},
				x87St("x0", "w17", "x5"), []string{
					"    mov  w5, #1", "    lsl  w5, w5, w4", "    orr  w3, w3, w5",
					fmt.Sprintf("    strb w3, [x16, #%#x]", x87CtxValid)})
		}
		b = append(b, x87C1Clear...)
		if w.pop {
			b = append(b, x87PopTop...)
		}
		return b, true
	case x87KXch:
		return cat(x87LoadTopValid, []string{
			fmt.Sprintf("    add  w4, w2, #%d", w.a),
			"    and  w4, w4, #7",
			"    lsr  w5, w3, w2",
			"    tbz  w5, #0, 6f",
			"    lsr  w5, w3, w4",
			"    tbz  w5, #0, 6f",
			slotAddr("x5", "w2"), slotAddr("x6", "w4")},
			x87Ld("x0", "w17", "x5"), x87Ld("x7", "w3", "x6"),
			x87St("x7", "w3", "x5"), x87St("x0", "w17", "x6"), x87C1Clear), true
	case x87KChs, x87KAbs:
		sign := "    eor  w6, w6, #0x8000"
		if w.kind == x87KAbs {
			sign = "    and  w6, w6, #0x7fff"
		}
		return cat(x87LoadTopValid, checkSt0, []string{
			slotAddr("x5", "w2"),
			fmt.Sprintf("    ldrh w6, [x5, #%#x]", x87SlotSE), sign,
			fmt.Sprintf("    strh w6, [x5, #%#x]", x87SlotSE)}, x87C1Clear), true
	case x87KFree:
		b := cat(x87LoadTopValid, []string{
			fmt.Sprintf("    add  w4, w2, #%d", w.a),
			"    and  w4, w4, #7",
			"    mov  w5, #1",
			"    lsl  w5, w5, w4",
			"    bic  w3, w3, w5"})
		if w.pop {
			return append(b, x87PopTop...), false
		}
		return append(b, fmt.Sprintf("    strb w3, [x16, #%#x]", x87CtxValid)), false
	case x87KIncstp, x87KDecstp:
		d := "    add  w2, w2, #1"
		if w.kind == x87KDecstp {
			d = "    sub  w2, w2, #1"
		}
		return cat([]string{fmt.Sprintf("    ldr  w2, [x16, #%#x]", x87CtxTop), d, "    and  w2, w2, #7",
			fmt.Sprintf("    str  w2, [x16, #%#x]", x87CtxTop)}, x87C1Clear), false
	}
	return nil, false
}

// x87LoadStsw leaves the status word with TOP in w0 (FNSTSW).
func (l *Arm64Lifter) x87LoadStsw() []string {
	if l.x87NoInline {
		return x87Call(x87Word{kind: x87KStsw})
	}
	return cat(l.emitLoadCtxPtr("x16"), []string{
		fmt.Sprintf("    ldrh w0, [x16, #%#x]", x87CtxSw),
		fmt.Sprintf("    ldr  w2, [x16, #%#x]", x87CtxTop),
		"    bfi  w0, w2, #11, #3"})
}

// x87LoadStcw leaves the control word in w0 (FNSTCW).
func (l *Arm64Lifter) x87LoadStcw() []string {
	if l.x87NoInline {
		return x87Call(x87Word{kind: x87KStcw})
	}
	return cat(l.emitLoadCtxPtr("x16"), []string{fmt.Sprintf("    ldrh w0, [x16, #%#x]", x87CtxCw)})
}

// x87StoreCw sets the control word from w1 (FLDCW).
func (l *Arm64Lifter) x87StoreCw() []string {
	if l.x87NoInline {
		return x87Call(x87Word{kind: x87KLdcw})
	}
	return cat(l.emitLoadCtxPtr("x16"), []string{fmt.Sprintf("    strh w1, [x16, #%#x]", x87CtxCw)})
}

// ---- batching ----

// x87Op is one recorded helper operation.
type x87Op struct {
	word   x87Word
	hasMem bool
	mem    x86asm.Mem
	nextPC uint64
}

type x87Recorder struct {
	ops  []x87Op
	comi bool // the instruction is an FCOMI/FUCOMI(P): the caller converts x0 to flags
}

// x87Describe lowers inst in recording mode: it returns the single helper operation the
// instruction is, or ok = false when it is anything else (FCOMI, FNSTSW, FCMOVcc, FXSAVE, an
// unsupported form, a non-x87 instruction).
func (l *Arm64Lifter) x87Describe(inst disasm.Instruction) (op x87Op, ok bool) {
	if !isX87Op(inst.Inst.Op) {
		return x87Op{}, false
	}
	rec := &x87Recorder{}
	l.x87Rec = rec
	out, isX87 := l.liftX87One(inst, inst.Address+uint64(inst.Inst.Len), nil)
	l.x87Rec = nil
	if !isX87 || len(out) != 0 || len(rec.ops) != 1 {
		return x87Op{}, false
	}
	if rec.comi { // FCOMI: only as the last operation of a batch, its NZCV result comes back in x0
		return rec.ops[0], true
	}
	switch rec.ops[0].word.kind {
	case x87KLdcw, x87KStcw, x87KStsw, x87KComi, x87KCmov, x87KFxsave, x87KFxrstor:
		return x87Op{}, false
	}
	return rec.ops[0], true
}

// x87IsInline reports whether the operation has an inline fast path (and so is not worth batching
// on its own).
func (l *Arm64Lifter) x87IsInline(w x87Word) bool {
	b, _ := l.x87Inline(w)
	return b != nil
}

// x87LiftRun emits a batch when inst starts a run with at least two helper operations.
func (l *Arm64Lifter) x87LiftRun(inst disasm.Instruction, lines []string) ([]string, bool) {
	if l.x87NoBatch || l.x87Blk == nil || l.x87Rec != nil {
		return lines, false
	}
	insts := l.x87Blk.Insts
	start := -1
	for i := range insts {
		if insts[i].Address == inst.Address {
			start = i
			break
		}
	}
	if start < 0 {
		return lines, false
	}
	var ops []x87Op
	var addrs []uint64
	endComi := false
	for i := start; i < len(insts) && len(ops) < x87MaxBatch; i++ {
		op, ok := l.x87Describe(insts[i])
		if !ok {
			break
		}
		ops = append(ops, op)
		addrs = append(addrs, insts[i].Address)
		if op.word.kind == x87KComi {
			endComi = true
			break
		}
	}
	// The span from the first to the last operation that needs the helper; the inline operations
	// after it are lowered one by one, and a run that starts with an inline operation is left to
	// the caller too (the next instruction plans again).
	first, last, n, nSlow := -1, -1, 0, 0
	for i, op := range ops {
		if !l.x87NoInline && l.x87IsInline(op.word) {
			continue
		}
		if first < 0 {
			first = i
		}
		last = i
		n++
		if !x87FastKind(op.word) {
			nSlow++
		}
	}
	// A run of operations that the lean entry executes without the full bridge (x87FastKind) costs less
	// as separate calls of _recomp_x87_a than as one batch with its stack frame; a batch pays when it
	// replaces at least two full bridge calls.
	if n < 2 || nSlow < 2 || first != 0 {
		return lines, false
	}
	if endComi && last == len(ops)-1 {
		// the run ends with the compare
	} else {
		endComi = false
		ops, addrs = ops[:last+1], addrs[:last+1]
	}
	frame := 16 * len(ops)
	lines = append(lines, fmt.Sprintf("    sub  sp, sp, #%d", frame))
	for i, op := range ops {
		if op.hasMem {
			lines = append(lines, l.x87MemArg(op.mem, op.nextPC)...)
		} else {
			lines = append(lines, "    mov  x1, xzr")
		}
		v := op.word.value()
		lines = append(lines, fmt.Sprintf("    movz w0, #0x%x", v&0xffff))
		if v>>16 != 0 {
			lines = append(lines, fmt.Sprintf("    movk w0, #0x%x, lsl #16", v>>16))
		}
		lines = append(lines, fmt.Sprintf("    stp  x0, x1, [sp, #%d]", 16*i))
	}
	lines = append(lines, "    mov  x0, sp", fmt.Sprintf("    mov  x1, #%d", len(ops)), "    bl   _recomp_x87_run",
		fmt.Sprintf("    add  sp, sp, #%d", frame))
	if endComi {
		lines = append(lines, "    msr  nzcv, x0")
		lines = append(lines, l.fpCompareFlags()...)
	}
	l.x87RunFirst, l.x87RunLast = addrs[0], addrs[len(addrs)-1]
	return lines, true
}

// x87FastKind: the operations that recomp_x87_fast (nps5_x87.c) executes without SoftFloat when their
// operands are zero or normal numbers: FADD/FSUB/FSUBR/FMUL and the compares (FDIV and the rest never).
func x87FastKind(w x87Word) bool {
	switch w.kind {
	case x87KArithSt, x87KArithM:
		return w.aux <= x87Mul
	case x87KComSt, x87KComM, x87KComi, x87KTst:
		return true
	}
	return false
}

// isX87Op reports whether op is an x87 mnemonic (the F* family of x86asm, FXSAVE/FXRSTOR aside:
// those are not batched).
func isX87Op(op x86asm.Op) bool {
	s := op.String()
	return len(s) > 1 && s[0] == 'F' && op != x86asm.FXSAVE && op != x86asm.FXRSTOR
}
