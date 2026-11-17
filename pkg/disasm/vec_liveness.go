package disasm

import (
	"strings"

	"golang.org/x/arch/x86/x86asm"
)

// Vector lane liveness (docs/m3/simdregs.md).
//
// The guest vector registers are pinned in host NEON registers (xmm n low 128 bits in v<n>, the
// YMM upper half in v<16+n>). This backward dataflow pass records, for every instruction, which
// parts of the 16 guest registers cannot be read after it before being rewritten. The lifter
// uses it to skip work whose result nobody can observe: the zeroing of the upper half that every
// VEX-encoded write implies, the merge of the untouched lanes that a scalar instruction implies,
// and the save/restore of an upper half around a lowering that borrows its host register.
//
// A register is tracked as four lanes:
//
//	L0  bits   0..31    (the SS scalar)
//	L1  bits  32..63    (with L0: the SD scalar)
//	H   bits  64..127
//	U   bits 128..255   (the YMM upper half, a separate host register)
//
// The pass is conservative by construction: an instruction it has no exact rule for reads every
// lane of every vector register it names and kills nothing, and every way out of the function
// that is not a call or a return keeps everything live.

// VecLiveMode selects what the pass assumes at calls and returns.
type VecLiveMode int

const (
	// VecLiveABI (the default): System V at calls and returns. A call reads xmm0-7 in full
	// (the vector argument registers, YMM for __m256 arguments) and leaves every vector
	// register undefined; a return reads ymm0 and xmm1.
	VecLiveABI VecLiveMode = iota
	// VecLiveStrict: no calling convention is assumed. Calls and returns read everything; only
	// a rewrite inside the function makes a lane dead.
	VecLiveStrict
	// VecLiveOff: the pass does not run; the lifter keeps every lane as it did before.
	VecLiveOff
)

// ParseVecLiveMode parses the -simd-liveness value.
func ParseVecLiveMode(s string) (VecLiveMode, bool) {
	switch strings.ToLower(s) {
	case "", "abi", "on":
		return VecLiveABI, true
	case "strict":
		return VecLiveStrict, true
	case "off":
		return VecLiveOff, true
	}
	return VecLiveABI, false
}

func (m VecLiveMode) String() string {
	switch m {
	case VecLiveStrict:
		return "strict"
	case VecLiveOff:
		return "off"
	}
	return "abi"
}

// Lane masks of one register (shift by 4*n for register n).
const (
	VecL0  = 1 << 0
	VecL1  = 1 << 1
	VecH   = 1 << 2
	VecU   = 1 << 3
	VecSS  = VecL0
	VecSD  = VecL0 | VecL1
	VecLow = VecL0 | VecL1 | VecH
	VecAll = VecLow | VecU

	vecEverything = ^uint64(0)
	vecAllUpper   = 0x8888888888888888
)

// VecLanes returns mask m of register n as a lane set.
func VecLanes(n int, m uint64) uint64 { return m << (4 * uint(n)) }

// vecArgs is what a System V call reads: xmm0-7 in full.
const vecArgs = 0x00000000ffffffff

// vecRets is what a System V return leaves for the caller: ymm0 and xmm1.
const vecRets = VecAll | VecLow<<4

// vecReg returns the register number of an XMM/YMM operand.
func vecReg(a x86asm.Arg) (n int, ymm, ok bool) {
	r, isReg := a.(x86asm.Reg)
	if !isReg {
		return 0, false, false
	}
	if r >= x86asm.X0 && r <= x86asm.X15 {
		return int(r - x86asm.X0), false, true
	}
	if r >= x86asm.Y0 && r <= x86asm.Y15 {
		return int(r - x86asm.Y0), true, true
	}
	return 0, false, false
}

func widthMask(ymm bool) uint64 {
	if ymm {
		return VecAll
	}
	return VecLow
}

// vecScalar describes an instruction that writes the low scalar of its destination.
//
// VEX three-operand form (dst, src1, src2): dst = {res: f(src1.s1, src2.s2), other low lanes:
// src1, U: 0}. Legacy two-operand form (dst, src): dst.res = f(dst.s1, src.s2), every other lane
// of dst kept.
type vecScalar struct{ res, s1, s2 uint64 }

var vexScalarOps = map[x86asm.Op]vecScalar{
	x86asm.VADDSS: {VecSS, VecSS, VecSS}, x86asm.VSUBSS: {VecSS, VecSS, VecSS},
	x86asm.VMULSS: {VecSS, VecSS, VecSS}, x86asm.VDIVSS: {VecSS, VecSS, VecSS},
	x86asm.VMINSS: {VecSS, VecSS, VecSS}, x86asm.VMAXSS: {VecSS, VecSS, VecSS},
	x86asm.VADDSD: {VecSD, VecSD, VecSD}, x86asm.VSUBSD: {VecSD, VecSD, VecSD},
	x86asm.VMULSD: {VecSD, VecSD, VecSD}, x86asm.VDIVSD: {VecSD, VecSD, VecSD},
	x86asm.VMINSD: {VecSD, VecSD, VecSD}, x86asm.VMAXSD: {VecSD, VecSD, VecSD},
	x86asm.VSQRTSS: {VecSS, 0, VecSS}, x86asm.VSQRTSD: {VecSD, 0, VecSD},
	x86asm.VRSQRTSS: {VecSS, 0, VecSS}, x86asm.VRCPSS: {VecSS, 0, VecSS},
	x86asm.VROUNDSS: {VecSS, 0, VecSS}, x86asm.VROUNDSD: {VecSD, 0, VecSD},
	x86asm.VCVTSS2SD: {VecSD, 0, VecSS}, x86asm.VCVTSD2SS: {VecSS, 0, VecSD},
	x86asm.VCVTSI2SS: {VecSS, 0, 0}, x86asm.VCVTSI2SD: {VecSD, 0, 0},
}

var sseScalarOps = map[x86asm.Op]vecScalar{
	x86asm.ADDSS: {VecSS, VecSS, VecSS}, x86asm.SUBSS: {VecSS, VecSS, VecSS},
	x86asm.MULSS: {VecSS, VecSS, VecSS}, x86asm.DIVSS: {VecSS, VecSS, VecSS},
	x86asm.MINSS: {VecSS, VecSS, VecSS}, x86asm.MAXSS: {VecSS, VecSS, VecSS},
	x86asm.ADDSD: {VecSD, VecSD, VecSD}, x86asm.SUBSD: {VecSD, VecSD, VecSD},
	x86asm.MULSD: {VecSD, VecSD, VecSD}, x86asm.DIVSD: {VecSD, VecSD, VecSD},
	x86asm.MINSD: {VecSD, VecSD, VecSD}, x86asm.MAXSD: {VecSD, VecSD, VecSD},
	x86asm.SQRTSS: {VecSS, 0, VecSS}, x86asm.SQRTSD: {VecSD, 0, VecSD},
	x86asm.RSQRTSS: {VecSS, 0, VecSS}, x86asm.RCPSS: {VecSS, 0, VecSS},
	x86asm.ROUNDSS: {VecSS, 0, VecSS}, x86asm.ROUNDSD: {VecSD, 0, VecSD},
	x86asm.CVTSS2SD: {VecSD, 0, VecSS}, x86asm.CVTSD2SS: {VecSS, 0, VecSD},
	x86asm.CVTSI2SS: {VecSS, 0, 0}, x86asm.CVTSI2SD: {VecSD, 0, 0},
}

// vecScalarReads: instructions whose vector operands are all sources read as a scalar.
var vecScalarReads = map[x86asm.Op]uint64{
	x86asm.UCOMISS: VecSS, x86asm.COMISS: VecSS, x86asm.VUCOMISS: VecSS, x86asm.VCOMISS: VecSS,
	x86asm.UCOMISD: VecSD, x86asm.COMISD: VecSD, x86asm.VUCOMISD: VecSD, x86asm.VCOMISD: VecSD,
	x86asm.CVTSS2SI: VecSS, x86asm.CVTTSS2SI: VecSS, x86asm.VCVTSS2SI: VecSS, x86asm.VCVTTSS2SI: VecSS,
	x86asm.CVTSD2SI: VecSD, x86asm.CVTTSD2SI: VecSD, x86asm.VCVTSD2SI: VecSD, x86asm.VCVTTSD2SI: VecSD,
}

// Bitwise operations: lane i of the result depends on lane i of the sources only.
var vecBitwiseOps = map[x86asm.Op]bool{
	x86asm.ANDPS: true, x86asm.ANDPD: true, x86asm.ANDNPS: true, x86asm.ANDNPD: true,
	x86asm.ORPS: true, x86asm.ORPD: true, x86asm.XORPS: true, x86asm.XORPD: true,
	x86asm.PAND: true, x86asm.PANDN: true, x86asm.POR: true, x86asm.PXOR: true,
	x86asm.VANDPS: true, x86asm.VANDPD: true, x86asm.VANDNPS: true, x86asm.VANDNPD: true,
	x86asm.VORPS: true, x86asm.VORPD: true, x86asm.VXORPS: true, x86asm.VXORPD: true,
	x86asm.VPAND: true, x86asm.VPANDN: true, x86asm.VPOR: true, x86asm.VPXOR: true,
}

func vecIsXor(op x86asm.Op) bool {
	switch op {
	case x86asm.XORPS, x86asm.XORPD, x86asm.PXOR, x86asm.VXORPS, x86asm.VXORPD, x86asm.VPXOR:
		return true
	}
	return false
}

// Full-register moves (register or memory source, register or memory destination).
var vecMoveOps = map[x86asm.Op]bool{
	x86asm.MOVAPS: true, x86asm.MOVUPS: true, x86asm.MOVAPD: true, x86asm.MOVUPD: true,
	x86asm.MOVDQA: true, x86asm.MOVDQU: true,
	x86asm.VMOVAPS: true, x86asm.VMOVUPS: true, x86asm.VMOVAPD: true, x86asm.VMOVUPD: true,
	x86asm.VMOVDQA: true, x86asm.VMOVDQU: true,
}

// vexLowReads: VEX instructions (dst, src) whose register source is an XMM register read only in
// the given low lanes: broadcasts of the low element, conversions that widen the low elements.
var vexLowReads = map[x86asm.Op]uint64{
	x86asm.VBROADCASTSS: VecSS, x86asm.VBROADCASTSD: VecSD,
	x86asm.VPBROADCASTB: VecSS, x86asm.VPBROADCASTW: VecSS, x86asm.VPBROADCASTD: VecSS, x86asm.VPBROADCASTQ: VecSD,
}

// vexLowPairOps: (dst, src1, src2) with XMM operands, reading bits 0..63 of both sources.
var vexLowPairOps = map[x86asm.Op]bool{
	x86asm.VUNPCKLPS: true, x86asm.VUNPCKLPD: true, x86asm.VMOVLHPS: true,
	x86asm.VPUNPCKLBW: true, x86asm.VPUNPCKLWD: true, x86asm.VPUNPCKLDQ: true, x86asm.VPUNPCKLQDQ: true,
}

// VecVexWritesArg0 reports a VEX-encoded instruction (by mnemonic) whose first operand, when it
// is a vector register, is written in full: bits 128 and up are zeroed by an XMM form. The
// exceptions read their first operand (compares, tests, string compares, MASKMOVDQU) or merge
// into it (FMA, gathers).
func VecVexWritesArg0(op x86asm.Op) bool {
	name := op.String()
	if !strings.HasPrefix(name, "V") {
		return false
	}
	switch op {
	case x86asm.VCOMISS, x86asm.VUCOMISS, x86asm.VCOMISD, x86asm.VUCOMISD,
		x86asm.VPTEST, x86asm.VTESTPS, x86asm.VTESTPD,
		x86asm.VPCMPISTRI, x86asm.VPCMPISTRM, x86asm.VPCMPESTRI, x86asm.VPCMPESTRM,
		x86asm.VMASKMOVDQU, x86asm.VZEROUPPER, x86asm.VZEROALL:
		return false
	}
	if strings.HasPrefix(name, "VF") && (strings.Contains(name, "MADD") || strings.Contains(name, "MSUB")) {
		return false // FMA: the destination is an operand
	}
	if strings.Contains(name, "GATHER") {
		return false // masked merge into the destination; the mask register is written too
	}
	return true
}

func vecIsFma(op x86asm.Op) bool {
	name := op.String()
	return strings.HasPrefix(name, "VF") && (strings.Contains(name, "MADD") || strings.Contains(name, "MSUB"))
}

// vecFmaScalar returns the scalar lanes of a scalar FMA instruction (0 for the packed forms and
// for anything else).
func vecFmaScalar(op x86asm.Op) uint64 {
	name := op.String()
	if !strings.HasPrefix(name, "VF") || !(strings.Contains(name, "MADD") || strings.Contains(name, "MSUB")) {
		return 0
	}
	switch {
	case strings.HasSuffix(name, "SS"):
		return VecSS
	case strings.HasSuffix(name, "SD"):
		return VecSD
	}
	return 0
}

// vecImplicitXmm0 reports legacy instructions that read XMM0 without naming it.
func vecImplicitXmm0(op x86asm.Op) bool {
	switch op {
	case x86asm.BLENDVPS, x86asm.BLENDVPD, x86asm.PBLENDVB, x86asm.SHA256RNDS2,
		x86asm.PCMPISTRM, x86asm.PCMPESTRM, x86asm.VPCMPISTRM, x86asm.VPCMPESTRM:
		return true
	}
	return false
}

// vecTransfer returns the lanes live before inst given the lanes live after it.
//
// A vector register can also be read as the VSIB index of a gather's memory operand
// ([base + vidx*scale]); it is not an operand of its own, so the operand rules below never
// see it. Its lanes (the low 128 bits for an XMM index, all 256 for a YMM one) are added as
// reads here, after the instruction's own transfer: gathers fault (#UD) when the index equals
// the destination or the mask, so the read can never be one the instruction kills itself.
func vecTransfer(fn *Function, in Instruction, out uint64, mode VecLiveMode) uint64 {
	return vecTransferOps(fn, in, out, mode) | vecVsibReads(in.Inst)
}

// vecVsibReads returns the lanes read through a vector index register in a memory operand.
func vecVsibReads(inst x86asm.Inst) uint64 {
	var live uint64
	for _, arg := range inst.Args {
		if arg == nil {
			break
		}
		if m, ok := arg.(x86asm.Mem); ok {
			if n, ymm, ok := vecReg(m.Index); ok {
				live |= VecLanes(n, widthMask(ymm))
			}
		}
	}
	return live
}

func vecTransferOps(fn *Function, in Instruction, out uint64, mode VecLiveMode) uint64 {
	inst := in.Inst
	op := inst.Op
	switch op {
	case x86asm.CALL:
		if mode != VecLiveABI {
			return vecEverything
		}
		if rel, ok := inst.Args[0].(x86asm.Rel); ok {
			next := in.Address + uint64(inst.Len)
			t := uint64(int64(next) + int64(rel))
			// A call into the function itself (the get-PC idiom "call next; pop", a local
			// subroutine) follows no calling convention.
			if t == next || fn.Blocks[t] != nil || (t >= fn.EntryAddr && t < fn.EndAddr) {
				return vecEverything
			}
		}
		return vecArgs
	case x86asm.RET:
		if mode != VecLiveABI {
			return vecEverything
		}
		return vecRets
	case x86asm.VZEROUPPER:
		return out &^ vecAllUpper
	case x86asm.VZEROALL:
		return 0
	case x86asm.JMP, x86asm.NOP:
		return out
	case x86asm.INT:
		if isOrbisDebugBreak(inst) {
			return out
		}
		return vecEverything
	case x86asm.LRET, x86asm.UD2, x86asm.HLT, x86asm.INTO, x86asm.IRET, x86asm.IRETD, x86asm.IRETQ,
		x86asm.SYSRET, x86asm.SYSEXIT, x86asm.SYSENTER, x86asm.UD0, x86asm.UD1, x86asm.ICEBP, x86asm.RSM, x86asm.XABORT,
		// State save/restore: every register is read (and, restoring, written; no kill is claimed).
		x86asm.FXSAVE, x86asm.FXSAVE64, x86asm.FXRSTOR, x86asm.FXRSTOR64,
		x86asm.XSAVE, x86asm.XSAVE64, x86asm.XSAVEC, x86asm.XSAVEC64, x86asm.XSAVEOPT, x86asm.XSAVEOPT64,
		x86asm.XSAVES, x86asm.XSAVES64, x86asm.XRSTOR, x86asm.XRSTOR64, x86asm.XRSTORS, x86asm.XRSTORS64:
		return vecEverything
	}

	// Operands.
	type operand struct {
		n   int
		ymm bool
		ok  bool
	}
	var a [4]operand
	nargs, any := 0, false
	for i, arg := range inst.Args {
		if arg == nil {
			break
		}
		nargs = i + 1
		n, ymm, ok := vecReg(arg)
		a[i] = operand{n, ymm, ok}
		any = any || ok
	}
	live := out
	if vecImplicitXmm0(op) {
		live |= VecLanes(0, VecLow)
	}
	if !any {
		return live
	}
	lanes := func(o operand, m uint64) uint64 {
		if !o.ok {
			return 0
		}
		return VecLanes(o.n, m)
	}
	outOf := func(o operand) uint64 { return (out >> (4 * uint(o.n))) & VecAll }
	vex := strings.HasPrefix(op.String(), "V")

	// Scalar reads only.
	if m, ok := vecScalarReads[op]; ok {
		for i := 0; i < nargs; i++ {
			live |= lanes(a[i], m)
		}
		return live
	}

	if vex {
		d := a[0]
		// FMA: dst is read and written in place.
		if sc := vecFmaScalar(op); sc != 0 && d.ok && !d.ymm {
			live &^= VecLanes(d.n, VecU)
			for i := 0; i < nargs; i++ {
				live |= lanes(a[i], sc)
			}
			return live
		}
		if !d.ok && nargs == 2 && a[1].ok {
			// Scalar stores and moves to a GPR.
			switch op {
			case x86asm.VMOVSS, x86asm.VMOVD:
				return live | lanes(a[1], VecSS)
			case x86asm.VMOVSD, x86asm.VMOVQ:
				return live | lanes(a[1], VecSD)
			}
		}
		if !d.ok || !VecVexWritesArg0(op) {
			// No vector destination, or one that is also a source: everything named is read.
			// A packed FMA on XMM registers still zeroes the destination's upper half.
			if d.ok && !d.ymm && vecIsFma(op) {
				live &^= VecLanes(d.n, VecU)
			}
			for i := 0; i < nargs; i++ {
				live |= lanes(a[i], widthMask(a[i].ymm))
			}
			return live
		}
		dOut := outOf(d)
		live &^= VecLanes(d.n, VecAll)
		switch {
		case op == x86asm.VMOVSS || op == x86asm.VMOVSD:
			sc := uint64(VecSS)
			if op == x86asm.VMOVSD {
				sc = VecSD
			}
			if nargs >= 3 && a[1].ok && a[2].ok { // dst = {src2 scalar, src1 rest}
				live |= lanes(a[1], dOut&VecLow&^sc) | lanes(a[2], sc)
				return live
			}
			if nargs == 2 && !a[1].ok { // load: the rest is zeroed
				return live
			}
		case vecMoveOps[op]:
			if nargs == 2 {
				if a[1].ok {
					live |= lanes(a[1], dOut&widthMask(d.ymm))
				}
				return live
			}
		case op == x86asm.VMOVD || op == x86asm.VMOVQ:
			if nargs == 2 && !a[1].ok { // from a GPR or memory: zero-extended
				return live
			}
			if nargs == 2 && op == x86asm.VMOVQ {
				return live | lanes(a[1], VecSD)
			}
		case vecBitwiseOps[op]:
			if nargs == 3 {
				if vecIsXor(op) && a[1].ok && a[2].ok && a[1].n == a[2].n {
					return live // zero idiom
				}
				m := dOut & widthMask(d.ymm)
				return live | lanes(a[1], m) | lanes(a[2], m)
			}
		case vexLowReads[op] != 0:
			// The source is read in its low lanes only, whatever the destination width.
			if nargs == 2 && a[1].ok && !a[1].ymm {
				return live | lanes(a[1], vexLowReads[op])
			}
			if nargs == 2 && !a[1].ok {
				return live
			}
		case vexLowPairOps[op]:
			// XMM form: the low quadword of both sources is interleaved or concatenated.
			if nargs == 3 && !d.ymm && a[1].ok && !a[1].ymm {
				return live | lanes(a[1], VecSD) | lanes(a[2], VecSD)
			}
		default:
			if s, ok := vexScalarOps[op]; ok && nargs >= 3 && a[1].ok && !d.ymm {
				live |= lanes(a[1], s.s1|dOut&VecLow&^s.res)
				live |= lanes(a[2], s.s2)
				return live
			}
		}
		// Any other VEX write: the destination is rewritten in full, the sources read in full.
		for i := 1; i < nargs; i++ {
			live |= lanes(a[i], widthMask(a[i].ymm))
		}
		return live
	}

	// Legacy SSE: bits 128 and up are never touched.
	d := a[0]
	conservative := func() uint64 {
		for i := 0; i < nargs; i++ {
			live |= lanes(a[i], VecLow)
		}
		return live
	}
	if !d.ok {
		// Destination is a GPR or memory.
		switch {
		case (op == x86asm.MOVSS || op == x86asm.MOVD) && nargs == 2:
			return live | lanes(a[1], VecSS)
		case (op == x86asm.MOVSD_XMM || op == x86asm.MOVQ) && nargs == 2:
			return live | lanes(a[1], VecSD)
		}
		return conservative()
	}
	dOut := outOf(d)
	switch {
	case op == x86asm.MOVD || op == x86asm.MOVQ:
		if nargs != 2 {
			return conservative()
		}
		// Zero-extended into bits up to 127, from a GPR, memory or the low quadword of a register.
		live &^= VecLanes(d.n, VecLow)
		if a[1].ok {
			live |= lanes(a[1], VecSD)
		}
		return live
	case op == x86asm.MOVSS || op == x86asm.MOVSD_XMM:
		sc := uint64(VecSS)
		if op == x86asm.MOVSD_XMM {
			sc = VecSD
		}
		if nargs != 2 {
			return conservative()
		}
		if a[1].ok { // register to register: only the scalar moves
			return live&^VecLanes(d.n, sc) | lanes(a[1], sc)
		}
		if _, isMem := inst.Args[1].(x86asm.Mem); isMem { // load: bits up to 127 zeroed
			return live &^ VecLanes(d.n, VecLow)
		}
		return conservative()
	case vecMoveOps[op]:
		if nargs != 2 {
			return conservative()
		}
		live &^= VecLanes(d.n, VecLow)
		if a[1].ok {
			live |= lanes(a[1], dOut&VecLow)
		}
		return live
	case vecBitwiseOps[op]:
		if nargs != 2 || !a[1].ok {
			if _, isMem := inst.Args[1].(x86asm.Mem); nargs == 2 && isMem {
				return live // dst lanes stay as live as they are after
			}
			return conservative()
		}
		if vecIsXor(op) && a[1].n == d.n {
			return live &^ VecLanes(d.n, VecLow) // zero idiom
		}
		return live | lanes(a[1], dOut&VecLow)
	}
	if s, ok := sseScalarOps[op]; ok && nargs >= 2 {
		_, srcMem := inst.Args[1].(x86asm.Mem)
		_, srcGpr := inst.Args[1].(x86asm.Reg)
		if a[1].ok || srcMem || (srcGpr && s.s2 == 0) {
			if s.s1 == 0 && !(a[1].ok && a[1].n == d.n) {
				live &^= VecLanes(d.n, s.res)
			}
			live |= lanes(d, s.s1) | lanes(a[1], s.s2)
			return live
		}
	}
	return conservative()
}

// vecBlockSuccessors is blockSuccessors without the tail-call exemption: a direct branch out of
// the function may continue in a split-off part of the same source function (hot/cold
// splitting), where no calling convention holds.
func vecBlockSuccessors(fn *Function, b *BasicBlock) (succs []uint64, unknown bool) {
	succs, unknown = blockSuccessors(fn, b, nil)
	// A recovered jump table is not proof of the complete successor set: an index outside the recovered range, or a
	// table the recovery missed an entry of, goes to _recomp_indirect_jump, can land in a block that is not listed (or in
	// tier-1 code) and there every vector register is live. So an indirect jump always has an unknown successor too.
	if n := len(b.Insts); n > 0 {
		last := b.Insts[n-1]
		if last.Inst.Op == x86asm.JMP {
			if _, direct := last.Inst.Args[0].(x86asm.Rel); !direct {
				unknown = true
			}
		}
	}
	return succs, unknown
}


// AnalyzeVecLiveness sets Instruction.VecDead for every instruction of fn and marks fn as
// analysed. With VecLiveOff it only clears the marks.
func AnalyzeVecLiveness(fn *Function, mode VecLiveMode) {
	if fn == nil {
		return
	}
	fn.VecLive = false
	if mode == VecLiveOff || len(fn.Blocks) == 0 {
		for _, b := range fn.Blocks {
			for i := range b.Insts {
				b.Insts[i].VecDead = 0
			}
		}
		return
	}
	type info struct {
		succs   []uint64
		unknown bool
		in      uint64
	}
	infos := make(map[uint64]*info, len(fn.Blocks))
	for addr, b := range fn.Blocks {
		s, u := vecBlockSuccessors(fn, b)
		infos[addr] = &info{succs: s, unknown: u}
	}
	for changed := true; changed; {
		changed = false
		for i := len(fn.BlockOrder) - 1; i >= 0; i-- {
			addr := fn.BlockOrder[i]
			b, bi := fn.Blocks[addr], infos[addr]
			if b == nil || bi == nil {
				continue
			}
			var live uint64
			if bi.unknown {
				live = vecEverything
			}
			for _, s := range bi.succs {
				if si := infos[s]; si != nil {
					live |= si.in
				}
			}
			for k := len(b.Insts) - 1; k >= 0; k-- {
				in := &b.Insts[k]
				in.VecDead = ^live
				live = vecTransfer(fn, *in, live, mode)
			}
			if live != bi.in {
				bi.in = live
				changed = true
			}
		}
	}
	// Blocks that are not in BlockOrder (none are expected) keep everything live.
	seen := make(map[uint64]bool, len(fn.BlockOrder))
	for _, a := range fn.BlockOrder {
		seen[a] = true
	}
	for addr, b := range fn.Blocks {
		if !seen[addr] {
			for i := range b.Insts {
				b.Insts[i].VecDead = 0
			}
		}
	}
	fn.VecLive = true
}

// BuildTestFunction builds a function from a straight instruction sequence starting at entry,
// split into blocks at branches and their targets (tests and tools; no analysis is run).
func BuildTestFunction(entry uint64, insts []Instruction) *Function {
	end := entry
	if n := len(insts); n > 0 {
		end = insts[n-1].Address + uint64(insts[n-1].Inst.Len)
	}
	starts := map[uint64]bool{entry: true}
	for _, in := range insts {
		next := in.Address + uint64(in.Inst.Len)
		if rel, ok := in.Inst.Args[0].(x86asm.Rel); ok && in.Inst.Op != x86asm.CALL {
			starts[uint64(int64(next)+int64(rel))], starts[next] = true, true
		}
		if in.Inst.Op == x86asm.RET || in.Inst.Op == x86asm.JMP {
			starts[next] = true
		}
	}
	fn := &Function{EntryAddr: entry, EndAddr: end, Blocks: map[uint64]*BasicBlock{}, JumpTables: map[uint64][]uint64{}}
	var cur *BasicBlock
	for _, in := range insts {
		if starts[in.Address] || cur == nil {
			cur = &BasicBlock{StartAddr: in.Address}
			fn.Blocks[in.Address] = cur
			fn.BlockOrder = append(fn.BlockOrder, in.Address)
		}
		cur.Insts = append(cur.Insts, in)
		cur.EndAddr = in.Address + uint64(in.Inst.Len)
	}
	return fn
}
