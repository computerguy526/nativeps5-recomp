package lifter

import (
	"fmt"
	"regexp"
	"strconv"
	"strings"

	"ps4-recomp/pkg/disasm"

	"golang.org/x/arch/x86/x86asm"
)

// x87=f80: every x87 instruction is one call of the runtime's bridge _recomp_x87
// (pkg/emitter/asm_x87.go) into recomp_x87_impl (pkg/runtime/core/nps5_x87.c), which keeps the
// registers as 80-bit values and does the arithmetic with Berkeley SoftFloat. The instruction is
// passed as an op word (x0) and an argument (x1: a host pointer to the memory operand, or a
// value); the result (status/control word, NZCV for FCOMI) comes back in x0. The bridge keeps all
// guest registers, the vector registers and NZCV, so the lowering needs no spills; x0-x7, x16 and
// x17 are clobbered as everywhere else in the lifter. docs/m2/x87-f80.md describes the model.
//
// The op word layout and the kind numbers mirror pkg/runtime/core/nps5_x87.h (the Go test
// TestX87KindsMatchHeader parses the header).

// Kinds (X87_K_*).
const (
	x87KFldM     = 1
	x87KFldSt    = 2
	x87KFldConst = 3
	x87KFild     = 4
	x87KFstM     = 5
	x87KFstSt    = 6
	x87KFist     = 7
	x87KArithSt  = 8
	x87KArithM   = 9
	x87KChs      = 10
	x87KAbs      = 11
	x87KSqrt     = 12
	x87KRndint   = 13
	x87KScale    = 14
	x87KXtract   = 15
	x87KPrem     = 16
	x87KComSt    = 17
	x87KComM     = 18
	x87KComi     = 19
	x87KTst      = 20
	x87KXam      = 21
	x87KXch      = 22
	x87KLdcw     = 23
	x87KStcw     = 24
	x87KStsw     = 25
	x87KNinit    = 26
	x87KNclex    = 27
	x87KCmov     = 28
	x87KFree     = 29
	x87KDecstp   = 30
	x87KIncstp   = 31
	x87KStenv    = 32
	x87KLdenv    = 33
	x87KSave     = 34
	x87KRstor    = 35
	x87KFxsave   = 36
	x87KFxrstor  = 37
	x87KBld      = 38
	x87KBstp     = 39
)

// Arithmetic operations (X87_ARITH_*).
const (
	x87Add = iota
	x87Sub
	x87SubR
	x87Mul
	x87Div
	x87DivR
)

// x87Word is the op word: kind | a<<8 | b<<11 | pop<<14 | pop2<<15 | sz<<16 | aux<<18.
type x87Word struct {
	kind, a, b, sz, aux int
	pop, pop2           bool
}

func (w x87Word) value() uint32 {
	v := uint32(w.kind) | uint32(w.a)<<8 | uint32(w.b)<<11 | uint32(w.sz)<<16 | uint32(w.aux)<<18
	if w.pop {
		v |= 1 << 14
	}
	if w.pop2 {
		v |= 1 << 15
	}
	return v
}

// x87Call emits the call of the bridge for an op word; the argument, if any, is already in x1.
func x87Call(w x87Word) []string {
	v := w.value()
	lines := []string{fmt.Sprintf("    movz w0, #0x%x", v&0xffff)}
	if v>>16 != 0 {
		lines = append(lines, fmt.Sprintf("    movk w0, #0x%x, lsl #16", v>>16))
	}
	entry := "_recomp_x87"
	switch w.kind {
	case x87KArithSt, x87KArithM, x87KComSt, x87KComM, x87KComi, x87KTst:
		entry = "_recomp_x87_a" // tries the integer fast path first (nps5_x87.c, recomp_x87_fast)
	}
	return append(lines, "    bl   "+entry)
}

// SetX87Mode selects the x87 model: "f80" (80-bit registers, the default of the translator) or
// "f64" (the stack of doubles). The default of a lifter made by NewArm64Lifter is f64, which
// keeps every existing lifter test as it was.
func (l *Arm64Lifter) SetX87Mode(mode string) error {
	switch mode {
	case "f80":
		l.x87F80 = true
	case "f64":
		l.x87F80 = false
	default:
		return fmt.Errorf("unknown x87 mode %q (want f80 or f64)", mode)
	}
	return nil
}

// X87Mode returns "f80" or "f64".
func (l *Arm64Lifter) X87Mode() string {
	if l.x87F80 {
		return "f80"
	}
	return "f64"
}

func x87IsSt(a x86asm.Arg) (int, bool) {
	if r, ok := a.(x86asm.Reg); ok && r >= x86asm.F0 && r <= x86asm.F7 {
		return int(r - x86asm.F0), true
	}
	return 0, false
}

// memArg computes the host pointer of a memory operand into x1.
func (l *Arm64Lifter) x87MemArg(mem x86asm.Mem, nextPC uint64) []string {
	lines := computeEffAddr(mem, nextPC, "x16")
	return append(lines, l.gAdd("x1", "x16"))
}

// liftX87F80 lowers the x87 instructions (and the x87 part of FXSAVE/FXRSTOR) for x87=f80. The
// second result is false for an instruction that is not x87. Instructions it knows but cannot
// lower (the transcendentals) become unsupported stubs.
func (l *Arm64Lifter) liftX87F80(inst disasm.Instruction, nextPC uint64, lines []string) ([]string, bool) {
	if inst.Address > l.x87RunFirst && inst.Address <= l.x87RunLast {
		return lines, true // emitted with the batch that started at x87RunFirst
	}
	if out, ok := l.x87LiftRun(inst, lines); ok {
		return out, true
	}
	return l.liftX87One(inst, nextPC, lines)
}

// liftX87One lowers one x87 instruction.
func (l *Arm64Lifter) liftX87One(inst disasm.Instruction, nextPC uint64, lines []string) ([]string, bool) {
	op := inst.Inst.Op
	args := inst.Inst.Args
	msz := defMemSz(inst)
	mem, hasMem := args[0].(x86asm.Mem)

	// call emits one helper operation: inline when the fast path covers it (arm64_lifter_x87inline.go),
	// else the bridge call. callMem first computes the memory operand's host pointer into x1.
	// While a batch is being planned (l.x87Rec) the operations are recorded instead of emitted.
	call := func(w x87Word) {
		if l.x87Rec != nil {
			l.x87Rec.ops = append(l.x87Rec.ops, x87Op{word: w})
			return
		}
		lines = append(lines, l.x87Emit(w)...)
	}
	callMem := func(w x87Word) {
		if l.x87Rec != nil {
			l.x87Rec.ops = append(l.x87Rec.ops, x87Op{word: w, hasMem: true, mem: mem, nextPC: nextPC})
			return
		}
		lines = append(lines, l.x87MemArg(mem, nextPC)...)
		lines = append(lines, l.x87Emit(w)...)
	}
	// sizes of the float and integer memory forms
	fsz := func() int {
		if isX87M80(inst) {
			return 2
		}
		if msz == 8 {
			return 1
		}
		return 0
	}
	isz := func() int { // FILD/FIST: 0 = int16, 1 = int32, 2 = int64
		switch msz {
		case 2:
			return 0
		case 4:
			return 1
		}
		return 2
	}

	switch op {
	case x86asm.FLDZ:
		call(x87Word{kind: x87KFldConst, a: 6})
	case x86asm.FLD1:
		call(x87Word{kind: x87KFldConst, a: 0})
	case x86asm.FLDL2T:
		call(x87Word{kind: x87KFldConst, a: 1})
	case x86asm.FLDL2E:
		call(x87Word{kind: x87KFldConst, a: 2})
	case x86asm.FLDPI:
		call(x87Word{kind: x87KFldConst, a: 3})
	case x86asm.FLDLG2:
		call(x87Word{kind: x87KFldConst, a: 4})
	case x86asm.FLDLN2:
		call(x87Word{kind: x87KFldConst, a: 5})

	case x86asm.FLD:
		if i, ok := x87IsSt(args[0]); ok {
			call(x87Word{kind: x87KFldSt, a: i})
		} else if hasMem {
			callMem(x87Word{kind: x87KFldM, sz: fsz()})
		} else {
			return l.x87Unsupported(inst, lines), true
		}
	case x86asm.FILD:
		if !hasMem {
			return l.x87Unsupported(inst, lines), true
		}
		callMem(x87Word{kind: x87KFild, sz: isz()})
	case x86asm.FST, x86asm.FSTP:
		pop := op == x86asm.FSTP
		if i, ok := x87IsSt(args[0]); ok {
			call(x87Word{kind: x87KFstSt, a: i, pop: pop})
		} else if hasMem {
			callMem(x87Word{kind: x87KFstM, sz: fsz(), pop: pop})
		} else {
			return l.x87Unsupported(inst, lines), true
		}
	case x86asm.FIST, x86asm.FISTP, x86asm.FISTTP:
		if !hasMem {
			return l.x87Unsupported(inst, lines), true
		}
		w := x87Word{kind: x87KFist, sz: isz(), pop: op != x86asm.FIST}
		if op == x86asm.FISTTP {
			w.aux = 1
		}
		callMem(w)

	case x86asm.FXCH:
		i := 1
		if r, ok := x87IsSt(args[0]); ok {
			i = r
		}
		call(x87Word{kind: x87KXch, a: i})
	case x86asm.FCHS:
		call(x87Word{kind: x87KChs})
	case x86asm.FABS:
		call(x87Word{kind: x87KAbs})
	case x86asm.FSQRT:
		call(x87Word{kind: x87KSqrt})
	case x86asm.FRNDINT:
		call(x87Word{kind: x87KRndint})
	case x86asm.FSCALE:
		call(x87Word{kind: x87KScale})
	case x86asm.FXTRACT:
		call(x87Word{kind: x87KXtract})
	case x86asm.FPREM:
		call(x87Word{kind: x87KPrem})
	case x86asm.FPREM1:
		call(x87Word{kind: x87KPrem, aux: 1})

	case x86asm.FADD, x86asm.FADDP, x86asm.FSUB, x86asm.FSUBP, x86asm.FSUBR, x86asm.FSUBRP,
		x86asm.FMUL, x86asm.FMULP, x86asm.FDIV, x86asm.FDIVP, x86asm.FDIVR, x86asm.FDIVRP:
		aop, pop := x87Add, false
		switch op {
		case x86asm.FADDP:
			pop = true
		case x86asm.FSUB:
			aop = x87Sub
		case x86asm.FSUBP:
			aop, pop = x87Sub, true
		case x86asm.FSUBR:
			aop = x87SubR
		case x86asm.FSUBRP:
			aop, pop = x87SubR, true
		case x86asm.FMUL:
			aop = x87Mul
		case x86asm.FMULP:
			aop, pop = x87Mul, true
		case x86asm.FDIV:
			aop = x87Div
		case x86asm.FDIVP:
			aop, pop = x87Div, true
		case x86asm.FDIVR:
			aop = x87DivR
		case x86asm.FDIVRP:
			aop, pop = x87DivR, true
		}
		dst, src := 1, 0 // no operands: ST(1) op= ST(0)
		if args[0] != nil {
			dst = 0
			if pop {
				dst = 1
			}
			if d, ok := x87IsSt(args[0]); ok {
				dst = d
				if len(args) > 1 && args[1] != nil {
					if s, ok := x87IsSt(args[1]); ok {
						src = s
					}
				}
			} else if hasMem {
				sz := 0
				if msz == 8 {
					sz = 1
				}
				callMem(x87Word{kind: x87KArithM, aux: aop, sz: sz})
				return lines, true
			}
		}
		call(x87Word{kind: x87KArithSt, aux: aop, a: dst, b: src, pop: pop})

	case x86asm.FIADD, x86asm.FISUB, x86asm.FISUBR, x86asm.FIMUL, x86asm.FIDIV, x86asm.FIDIVR:
		if !hasMem {
			return l.x87Unsupported(inst, lines), true
		}
		aop := map[x86asm.Op]int{x86asm.FIADD: x87Add, x86asm.FISUB: x87Sub, x86asm.FISUBR: x87SubR,
			x86asm.FIMUL: x87Mul, x86asm.FIDIV: x87Div, x86asm.FIDIVR: x87DivR}[op]
		sz := 3
		if msz == 2 {
			sz = 2
		}
		callMem(x87Word{kind: x87KArithM, aux: aop, sz: sz})

	case x86asm.FCOM, x86asm.FCOMP, x86asm.FCOMPP, x86asm.FUCOM, x86asm.FUCOMP, x86asm.FUCOMPP,
		x86asm.FICOM, x86asm.FICOMP:
		w := x87Word{kind: x87KComSt, a: 1}
		switch op {
		case x86asm.FCOMP, x86asm.FICOMP:
			w.pop = true
		case x86asm.FCOMPP:
			w.pop, w.pop2 = true, true
		case x86asm.FUCOM:
			w.aux = 1
		case x86asm.FUCOMP:
			w.aux, w.pop = 1, true
		case x86asm.FUCOMPP:
			w.aux, w.pop, w.pop2 = 1, true, true
		}
		if i, ok := x87IsSt(args[0]); ok {
			w.a = i
			call(w)
		} else if hasMem {
			w.kind, w.a = x87KComM, 0
			if op == x86asm.FICOM || op == x86asm.FICOMP {
				w.sz = 3
				if msz == 2 {
					w.sz = 2
				}
			} else if msz == 8 {
				w.sz = 1
			}
			callMem(w)
		} else {
			call(w) // FCOM with no operand: ST(1)
		}
	case x86asm.FTST:
		call(x87Word{kind: x87KTst})
	case x86asm.FXAM:
		call(x87Word{kind: x87KXam})

	case x86asm.FCOMI, x86asm.FCOMIP, x86asm.FUCOMI, x86asm.FUCOMIP:
		i := 1
		if len(args) > 1 && args[1] != nil {
			if r, ok := x87IsSt(args[1]); ok {
				i = r
			}
		}
		w := x87Word{kind: x87KComi, a: i, pop: op == x86asm.FCOMIP || op == x86asm.FUCOMIP}
		if op == x86asm.FUCOMI || op == x86asm.FUCOMIP {
			w.aux = 1
		}
		call(w)
		if l.x87Rec != nil {
			l.x87Rec.comi = true
			return lines, true
		}
		// x0 = NZCV as after an arm64 fcmp; convert exactly as the f64 lowering does.
		lines = append(lines, "    msr  nzcv, x0")
		lines = append(lines, l.fpCompareFlags()...)

	case x86asm.FCMOVB, x86asm.FCMOVE, x86asm.FCMOVBE, x86asm.FCMOVU,
		x86asm.FCMOVNB, x86asm.FCMOVNE, x86asm.FCMOVNBE, x86asm.FCMOVNU:
		i := 1
		if len(args) > 1 {
			if r, ok := x87IsSt(args[1]); ok {
				i = r
			}
		}
		cc := map[x86asm.Op]int{
			x86asm.FCMOVB: 2, x86asm.FCMOVNB: 3, x86asm.FCMOVE: 4, x86asm.FCMOVNE: 5,
			x86asm.FCMOVBE: 6, x86asm.FCMOVNBE: 7, x86asm.FCMOVU: 10, x86asm.FCMOVNU: 11,
		}[op]
		pre, cond := l.condFor(cc)
		lines = append(lines, pre...)
		var post []string
		if isParityCond(cc) && !inst.ParityInV {
			var p []string
			p, post = l.parityToV()
			lines = append(lines, p...)
		}
		// The call clobbers x6, where parityToV keeps the flags: restore them on both paths
		// before it, the bridge itself keeps NZCV.
		lines = append(lines, fmt.Sprintf("    b.%s 8f", invertArm64Cond(cond)))
		lines = append(lines, post...)
		call(x87Word{kind: x87KCmov, a: i})
		lines = append(lines, "    b    9f", "8:")
		lines = append(lines, post...)
		lines = append(lines, "9:")

	case x86asm.FNSTSW:
		lines = append(lines, l.x87LoadStsw()...)
		if r, ok := args[0].(x86asm.Reg); ok && r == x86asm.AX {
			lines = append(lines, "    bfi x19, x0, #0, #16") // AX is x19
		} else if hasMem {
			lines = append(lines, "    mov  w17, w0")
			lines = append(lines, computeEffAddr(mem, nextPC, "x5")...)
			lines = append(lines, "    strh w17, "+l.gm("x5"))
		} else {
			return l.x87Unsupported(inst, lines), true
		}
	case x86asm.FNSTCW:
		if !hasMem {
			return l.x87Unsupported(inst, lines), true
		}
		lines = append(lines, l.x87LoadStcw()...)
		lines = append(lines, "    mov  w17, w0")
		lines = append(lines, computeEffAddr(mem, nextPC, "x5")...)
		lines = append(lines, "    strh w17, "+l.gm("x5"))
	case x86asm.FLDCW:
		if !hasMem {
			return l.x87Unsupported(inst, lines), true
		}
		lines = append(lines, computeEffAddr(mem, nextPC, "x16")...)
		lines = append(lines, "    ldrh w1, "+l.gm("x16"))
		lines = append(lines, l.x87StoreCw()...)
	case x86asm.FNINIT:
		call(x87Word{kind: x87KNinit})
	case x86asm.FNCLEX:
		call(x87Word{kind: x87KNclex})
	case x86asm.FFREE:
		i, _ := x87IsSt(args[0])
		call(x87Word{kind: x87KFree, a: i})
	case x86asm.FFREEP:
		i, _ := x87IsSt(args[0])
		call(x87Word{kind: x87KFree, a: i, pop: true})
	case x86asm.FDECSTP:
		call(x87Word{kind: x87KDecstp})
	case x86asm.FINCSTP:
		call(x87Word{kind: x87KIncstp})

	case x86asm.FNSTENV, x86asm.FLDENV, x86asm.FNSAVE, x86asm.FRSTOR, x86asm.FBLD, x86asm.FBSTP:
		if !hasMem {
			return l.x87Unsupported(inst, lines), true
		}
		kind := map[x86asm.Op]int{x86asm.FNSTENV: x87KStenv, x86asm.FLDENV: x87KLdenv, x86asm.FNSAVE: x87KSave,
			x86asm.FRSTOR: x87KRstor, x86asm.FBLD: x87KBld, x86asm.FBSTP: x87KBstp}[op]
		w := x87Word{kind: kind}
		if inst.Inst.DataSize == 16 && kind <= x87KRstor && kind >= x87KStenv {
			w.sz = 1 // 16-bit operand size: the 14-byte environment
		}
		callMem(w)

	case x86asm.FXSAVE, x86asm.FXRSTOR:
		// The SSE half (MXCSR, XMM) is the existing lowering; the x87 half (FCW, FSW, FTW and
		// ST0-7) is done by the helper, over the same image.
		var err error
		if op == x86asm.FXSAVE {
			lines, err = l.liftFxsave(inst, nextPC, lines)
		} else {
			lines, err = l.liftFxrstor(inst, nextPC, lines)
		}
		if err != nil || !hasMem {
			return lines, true
		}
		// liftFxrstor leaves x16 intact, but loads MXCSR into FPCR afterwards through w0 only;
		// recompute the pointer instead of relying on either.
		kind := x87KFxsave
		if op == x86asm.FXRSTOR {
			kind = x87KFxrstor
		}
		callMem(x87Word{kind: kind})

	case x86asm.FSIN, x86asm.FCOS, x86asm.FSINCOS, x86asm.FPTAN, x86asm.FPATAN, x86asm.F2XM1, x86asm.FYL2X, x86asm.FYL2XP1:
		// No call site in the binaries examined (docs/m2/x87-f80.md): the hardware's argument
		// reduction (66-bit pi) and polynomial results are not reproducible with a libm call.
		return l.x87Unsupported(inst, lines), true

	default:
		return lines, false
	}
	return lines, true
}

func (l *Arm64Lifter) x87Unsupported(inst disasm.Instruction, lines []string) []string {
	if l.x87Rec != nil { // planning a batch: not an operation, and no unsupported site is recorded
		return append(lines, "    // unsupported")
	}
	return append(lines, l.emitUnsupported(inst, fmt.Sprintf("x87=f80: %v not lowered", inst.Inst.Op))...)
}

var mmxOffsetRe = regexp.MustCompile(`\[x5, #(\d+)\]`)

// x87AliasMmx rewrites the MMX register accesses of one lowered instruction (lines[start:]) to the
// significand of the x87 register of the same number, GuestContext.fpu80[n].mant: MMn aliases
// physical register n. Only the significand is shared; the exponent field, TOP and the tag word
// are not set to the MMX state (0xFFFF, 0, valid) by an MMX write.
func x87AliasMmx(lines []string, start int) []string {
	for i := start; i < len(lines); i++ {
		if !strings.Contains(lines[i], "[x5, #") {
			continue
		}
		lines[i] = mmxOffsetRe.ReplaceAllStringFunc(lines[i], func(m string) string {
			off, _ := strconv.Atoi(mmxOffsetRe.FindStringSubmatch(m)[1])
			if idx := (off - mmxOffset(0)) / 8; off >= mmxOffset(0) && idx < 8 && (off-mmxOffset(0))%8 == 0 {
				return fmt.Sprintf("[x5, #%d]", x87CtxFpu80+16*idx)
			}
			return m
		})
	}
	return lines
}

// x87CtxFpu80 is the offset of GuestContext.fpu80 (asserted in recomp_context.h).
const x87CtxFpu80 = 0xbd0
