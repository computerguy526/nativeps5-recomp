package lifter

import (
	"fmt"
	"strings"

	"ps4-recomp/pkg/disasm"

	"golang.org/x/arch/x86/x86asm"
)

// AVX2 / FMA3 forms (S2 census of Syobon and PS5 hello_cxx): VPBROADCAST{B,W,D,Q}, VPERMQ,
// VPERMPD, VPERMD, VPERMPS, VPSLLV/VPSRLV/VPSRAV, VPMULHUW and the VFMADD/VFMSUB/VFNMADD/
// VFNMSUB 132/213/231 families. VPERM2I128, VBROADCASTI128 and VPMASKMOVD/Q share the
// lowerings of their floating-point twins. Scratch vectors are v16-v20 and v30/v31, left to
// fixUpperHalves to rename or spill.

// avxSrc resolves a vector source operand. A register yields its own low/high names; a memory
// operand is loaded into the scratch registers lo (and hi for 256-bit).
func (l *Arm64Lifter) avxSrc(a x86asm.Arg, nextPC uint64, isYmm bool, lo, hi string, lines *[]string) (sLo, sHi string, ok bool) {
	switch v := a.(type) {
	case x86asm.Reg:
		if _, _, _, sl, sh, _, _, okv := getVectorDesc(v); okv {
			return sl, sh, true
		}
	case x86asm.Mem:
		*lines = append(*lines, computeEffAddr(v, nextPC, "x16")...)
		*lines = append(*lines, fmt.Sprintf("    ldr q%s, %s", lo[1:], l.gm("x16")))
		if isYmm {
			*lines = append(*lines, "    add x16, x16, #16", fmt.Sprintf("    ldr q%s, %s", hi[1:], l.gm("x16")))
		}
		return lo, hi, true
	}
	return "", "", false
}

func vecReg(a x86asm.Arg) (x86asm.Reg, bool) {
	r, ok := a.(x86asm.Reg)
	return r, ok
}

// finishHigh writes the YMM upper half of dst: copy of the low half result for broadcasts is
// done by callers; this only zeroes it for VEX.128 forms.
func zeroHighIfXmm(lines []string, isYmm bool, dHighV string) []string {
	if !isYmm {
		lines = append(lines, fmt.Sprintf("    movi %s.16b, #0", dHighV))
	}
	return lines
}

// liftVpbroadcast handles VPBROADCASTB/W/D/Q dst_xmm/ymm, src_xmm/m.
func (l *Arm64Lifter) liftVpbroadcast(inst disasm.Instruction, nextPC uint64, lines []string) ([]string, error) {
	var lane, elem string
	switch inst.Inst.Op {
	case x86asm.VPBROADCASTB:
		lane, elem = "16b", "b"
	case x86asm.VPBROADCASTW:
		lane, elem = "8h", "h"
	case x86asm.VPBROADCASTD:
		lane, elem = "4s", "s"
	default:
		lane, elem = "2d", "d"
	}
	dstReg, okDst := vecReg(inst.Inst.Args[0])
	isYmm, _, _, dLowV, dHighV, _, _, okD := getVectorDesc(dstReg)
	if !okDst || !okD {
		return append(lines, l.emitUnsupported(inst, "vpbroadcast dst")...), nil
	}
	switch src := inst.Inst.Args[1].(type) {
	case x86asm.Mem:
		lines = append(lines, computeEffAddr(src, nextPC, "x16")...)
		lines = append(lines, l.gAdd("x16", "x16"), fmt.Sprintf("    ld1r {%s.%s}, [x16]", dLowV, lane))
	case x86asm.Reg:
		_, _, _, sLowV, _, _, _, okS := getVectorDesc(src)
		if !okS {
			return append(lines, l.emitUnsupported(inst, "vpbroadcast src")...), nil
		}
		lines = append(lines, fmt.Sprintf("    dup %s.%s, %s.%s[0]", dLowV, lane, sLowV, elem))
	default:
		return append(lines, l.emitUnsupported(inst, "vpbroadcast src")...), nil
	}
	if isYmm {
		lines = append(lines, fmt.Sprintf("    mov %s.16b, %s.16b", dHighV, dLowV))
	} else {
		lines = append(lines, fmt.Sprintf("    movi %s.16b, #0", dHighV))
	}
	return lines, nil
}

// liftVpermq handles VPERMQ and VPERMPD dst_ymm, src_ymm/m256, imm8.
func (l *Arm64Lifter) liftVpermq(inst disasm.Instruction, nextPC uint64, lines []string) ([]string, error) {
	dstReg, okDst := vecReg(inst.Inst.Args[0])
	imm, okImm := inst.Inst.Args[2].(x86asm.Imm)
	_, _, _, dLowV, dHighV, _, _, okD := getVectorDesc(dstReg)
	if !okDst || !okImm || !okD {
		return append(lines, l.emitUnsupported(inst, "vpermq operands")...), nil
	}
	sLo, sHi, ok := l.avxSrc(inst.Inst.Args[1], nextPC, true, "v30", "v31", &lines)
	if !ok {
		return append(lines, l.emitUnsupported(inst, "vpermq src")...), nil
	}
	if srcReg, isReg := inst.Inst.Args[1].(x86asm.Reg); isReg && srcReg-x86asm.Y0 == dstReg-x86asm.Y0 {
		// dst == src: read the source from a copy, the halves are written one after the other.
		lines = append(lines, fmt.Sprintf("    mov v30.16b, %s.16b", sLo), fmt.Sprintf("    mov v31.16b, %s.16b", sHi))
		sLo, sHi = "v30", "v31"
	}
	qword := func(s int) (string, int) {
		if s < 2 {
			return sLo, s
		}
		return sHi, s - 2
	}
	for h, dv := range []string{dLowV, dHighV} {
		a, b := int(imm>>(4*uint(h)))&3, int(imm>>(4*uint(h)+2))&3
		ra, ia := qword(a)
		rb, ib := qword(b)
		switch {
		case ra == rb && ia == 0 && ib == 1:
			lines = append(lines, fmt.Sprintf("    mov %s.16b, %s.16b", dv, ra))
		case ra == rb && ia == 1 && ib == 0:
			lines = append(lines, fmt.Sprintf("    ext %s.16b, %s.16b, %s.16b, #8", dv, ra, ra))
		case ra == rb && ia == ib:
			lines = append(lines, fmt.Sprintf("    dup %s.2d, %s.d[%d]", dv, ra, ia))
		default:
			lines = append(lines, fmt.Sprintf("    mov %s.d[0], %s.d[%d]", dv, ra, ia), fmt.Sprintf("    mov %s.d[1], %s.d[%d]", dv, rb, ib))
		}
	}
	return lines, nil
}

// liftVpermd handles VPERMD and VPERMPS dst_ymm, idx_ymm, src_ymm/m256:
// dst.dword[i] = src.dword[idx.dword[i] & 7]. The 32-byte table lives in v30 (low) and v31 (high); the dword indexes become byte indexes (4*k + {0,1,2,3}).
func (l *Arm64Lifter) liftVpermd(inst disasm.Instruction, nextPC uint64, lines []string) ([]string, error) {
	dstReg, okDst := vecReg(inst.Inst.Args[0])
	idxReg, okIdx := vecReg(inst.Inst.Args[1])
	_, _, _, dLowV, dHighV, _, _, okD := getVectorDesc(dstReg)
	_, _, _, iLowV, iHighV, _, _, okI := getVectorDesc(idxReg)
	if !okDst || !okIdx || !okD || !okI {
		return append(lines, l.emitUnsupported(inst, "vpermd operands")...), nil
	}
	if mem, isMem := inst.Inst.Args[2].(x86asm.Mem); isMem {
		var s []string
		l.avxSrc(mem, nextPC, true, "v30", "v31", &s)
		lines = append(lines, s...)
	} else if tReg, isReg := vecReg(inst.Inst.Args[2]); isReg {
		if _, _, _, tl, th, _, _, okT := getVectorDesc(tReg); okT {
			lines = append(lines, fmt.Sprintf("    mov v30.16b, %s.16b", tl), fmt.Sprintf("    mov v31.16b, %s.16b", th))
		} else {
			return append(lines, l.emitUnsupported(inst, "vpermd src")...), nil
		}
	} else {
		return append(lines, l.emitUnsupported(inst, "vpermd src")...), nil
	}
	lines = append(lines,
		"    movi v17.4s, #7",
		"    movi v18.16b, #1",
		"    mov w16, #0x0100",
		"    movk w16, #0x0302, lsl #16",
		"    dup v20.4s, w16")
	for _, h := range [][2]string{{"v16", iLowV}, {"v19", iHighV}} {
		lines = append(lines,
			fmt.Sprintf("    and %s.16b, %s.16b, v17.16b", h[0], h[1]),
			fmt.Sprintf("    shl %s.4s, %s.4s, #2", h[0], h[0]),
			fmt.Sprintf("    mul %s.4s, %s.4s, v18.4s", h[0], h[0]),
			fmt.Sprintf("    add %s.16b, %s.16b, v20.16b", h[0], h[0]))
	}
	// Two single-register lookups (fixUpperHalves renames scratch registers one by one, so a
	// TBL register pair cannot be relied on): indexes 0-15 hit v30, 16-31 hit v31 after -16
	// (the rest wrap past 255 and give 0).
	lines = append(lines, "    movi v21.16b, #16")
	for _, h := range [][2]string{{dLowV, "v16"}, {dHighV, "v19"}} {
		lines = append(lines,
			fmt.Sprintf("    tbl v22.16b, {v30.16b}, %s.16b", h[1]),
			fmt.Sprintf("    sub v23.16b, %s.16b, v21.16b", h[1]),
			"    tbl v23.16b, {v31.16b}, v23.16b",
			fmt.Sprintf("    orr %s.16b, v22.16b, v23.16b", h[0]))
	}
	return lines, nil
}

// liftVpshiftv handles VPSLLVD/Q, VPSRLVD/Q and VPSRAVD: per-element variable shifts where a
// count >= the element width gives 0 (sign fill for arithmetic). NEON shifts take the low byte
// of the count as a signed amount, so the count is first clamped (dwords: UMIN with the width)
// or its out-of-range results masked (qwords).
func (l *Arm64Lifter) liftVpshiftv(inst disasm.Instruction, nextPC uint64, lines []string) ([]string, error) {
	op := inst.Inst.Op
	dstReg, okDst := vecReg(inst.Inst.Args[0])
	s1Reg, okS1 := vecReg(inst.Inst.Args[1])
	isYmm, _, _, dLowV, dHighV, _, _, okD := getVectorDesc(dstReg)
	_, _, _, s1Lo, s1Hi, _, _, okS := getVectorDesc(s1Reg)
	if !okDst || !okS1 || !okD || !okS {
		return append(lines, l.emitUnsupported(inst, "vpsllv operands")...), nil
	}
	cLo, cHi, ok := l.avxSrc(inst.Inst.Args[2], nextPC, isYmm, "v30", "v31", &lines)
	if !ok {
		return append(lines, l.emitUnsupported(inst, "vpsllv count")...), nil
	}
	right := op == x86asm.VPSRLVD || op == x86asm.VPSRLVQ || op == x86asm.VPSRAVD
	quad := op == x86asm.VPSLLVQ || op == x86asm.VPSRLVQ
	shift := "ushl"
	clamp := 32
	if op == x86asm.VPSRAVD {
		shift, clamp = "sshl", 31
	}
	if quad {
		lines = append(lines, "    mov x16, #64", "    dup v17.2d, x16")
	} else {
		lines = append(lines, fmt.Sprintf("    movi v17.4s, #%d", clamp))
	}
	halves := [][3]string{{dLowV, s1Lo, cLo}}
	if isYmm {
		halves = append(halves, [3]string{dHighV, s1Hi, cHi})
	}
	for _, h := range halves {
		d, s, c := h[0], h[1], h[2]
		if quad {
			lines = append(lines, fmt.Sprintf("    cmhi v18.2d, v17.2d, %s.2d", c))
			cnt := c
			if right {
				lines = append(lines, fmt.Sprintf("    neg v16.2d, %s.2d", c))
				cnt = "v16"
			}
			lines = append(lines, fmt.Sprintf("    %s %s.2d, %s.2d, %s.2d", shift, d, s, cnt),
				fmt.Sprintf("    and %s.16b, %s.16b, v18.16b", d, d))
			continue
		}
		lines = append(lines, fmt.Sprintf("    umin v16.4s, %s.4s, v17.4s", c))
		if right {
			lines = append(lines, "    neg v16.4s, v16.4s")
		}
		lines = append(lines, fmt.Sprintf("    %s %s.4s, %s.4s, v16.4s", shift, d, s))
	}
	return zeroHighIfXmm(lines, isYmm, dHighV), nil
}

// liftVpmulhuw handles VPMULHUW dst, src1, src2/m (unsigned 16x16 -> high 16).
func (l *Arm64Lifter) liftVpmulhuw(inst disasm.Instruction, nextPC uint64, lines []string) ([]string, error) {
	dstReg, okDst := vecReg(inst.Inst.Args[0])
	s1Reg, okS1 := vecReg(inst.Inst.Args[1])
	isYmm, _, _, dLowV, dHighV, _, _, okD := getVectorDesc(dstReg)
	_, _, _, s1Lo, s1Hi, _, _, okS := getVectorDesc(s1Reg)
	if !okDst || !okS1 || !okD || !okS {
		return append(lines, l.emitUnsupported(inst, "vpmulhuw operands")...), nil
	}
	s2Lo, s2Hi, ok := l.avxSrc(inst.Inst.Args[2], nextPC, isYmm, "v30", "v31", &lines)
	if !ok {
		return append(lines, l.emitUnsupported(inst, "vpmulhuw src2")...), nil
	}
	halves := [][3]string{{dLowV, s1Lo, s2Lo}}
	if isYmm {
		halves = append(halves, [3]string{dHighV, s1Hi, s2Hi})
	}
	for _, h := range halves {
		lines = append(lines,
			fmt.Sprintf("    umull v16.4s, %s.4h, %s.4h", h[1], h[2]),
			fmt.Sprintf("    umull2 v17.4s, %s.8h, %s.8h", h[1], h[2]),
			fmt.Sprintf("    uzp2 %s.8h, v16.8h, v17.8h", h[0]))
	}
	return zeroHighIfXmm(lines, isYmm, dHighV), nil
}

// fmaForm decodes VF[N]M{ADD,SUB}{132,213,231}{SS,SD,PS,PD}.
func fmaForm(op x86asm.Op) (neg, sub bool, order int, scalar, double, ok bool) {
	n := op.String()
	if !strings.HasPrefix(n, "VF") || len(n) < 10 {
		return
	}
	s := n[2:]
	if strings.HasPrefix(s, "N") {
		neg, s = true, s[1:]
	}
	switch {
	case strings.HasPrefix(s, "MADD") && !strings.HasPrefix(s, "MADDSUB"):
		s = s[4:]
	case strings.HasPrefix(s, "MSUB") && !strings.HasPrefix(s, "MSUBADD"):
		sub, s = true, s[4:]
	default:
		return
	}
	if len(s) != 5 {
		return
	}
	switch s[:3] {
	case "132":
		order = 132
	case "213":
		order = 213
	case "231":
		order = 231
	default:
		return
	}
	scalar = s[3] == 'S'
	double = s[4] == 'D'
	ok = (s[3] == 'S' || s[3] == 'P') && (s[4] == 'S' || s[4] == 'D')
	return
}

// liftFma handles the FMA3 families. With op1 = dst, op2 = src1, op3 = src2:
// 132: op1*op3 +- op2; 213: op2*op1 +- op3; 231: op2*op3 +- op1. FMADD = P+C, FMSUB = P-C,
// FNMADD = -P+C, FNMSUB = -P-C (single rounding, as ARM FMADD/FMLA).
func (l *Arm64Lifter) liftFma(inst disasm.Instruction, nextPC uint64, lines []string) ([]string, error) {
	neg, sub, order, scalar, double, _ := fmaForm(inst.Inst.Op)
	dstReg, okDst := vecReg(inst.Inst.Args[0])
	s2Reg, okS2 := vecReg(inst.Inst.Args[1])
	isYmm, _, _, dLowV, dHighV, dS, dD, okD := getVectorDesc(dstReg)
	_, _, _, s2Lo, s2Hi, s2S, s2D, okS := getVectorDesc(s2Reg)
	if !okDst || !okS2 || !okD || !okS {
		return append(lines, l.emitUnsupported(inst, "fma operands")...), nil
	}
	// op3: register, or memory loaded into v30 (scalar: ldr s/d) / v30:v31.
	var o3Lo, o3Hi, o3Sc string
	switch v := inst.Inst.Args[2].(type) {
	case x86asm.Reg:
		_, _, _, a, b, sc, dc, okv := getVectorDesc(v)
		if !okv {
			return append(lines, l.emitUnsupported(inst, "fma src")...), nil
		}
		o3Lo, o3Hi, o3Sc = a, b, sc
		if double {
			o3Sc = dc
		}
	case x86asm.Mem:
		lines = append(lines, computeEffAddr(v, nextPC, "x16")...)
		switch {
		case scalar && double:
			lines = append(lines, "    ldr d30, "+l.gm("x16"))
			o3Sc = "d30"
		case scalar:
			lines = append(lines, "    ldr s30, "+l.gm("x16"))
			o3Sc = "s30"
		default:
			lines = append(lines, "    ldr q30, "+l.gm("x16"))
			o3Lo = "v30"
			if isYmm {
				lines = append(lines, "    add x16, x16, #16", "    ldr q31, "+l.gm("x16"))
				o3Hi = "v31"
			}
		}
	default:
		return append(lines, l.emitUnsupported(inst, "fma src")...), nil
	}

	if scalar {
		pick := func(d, s2, s3 string) (a, b, c string) {
			switch order {
			case 132:
				return d, s3, s2
			case 213:
				return s2, d, s3
			}
			return s2, s3, d
		}
		d, s2n := dS, s2S
		if double {
			d, s2n = dD, s2D
		}
		a, b, c := pick(d, s2n, o3Sc)
		var armOp string
		switch {
		case !neg && !sub:
			armOp = "fmadd"
		case !neg && sub:
			armOp = "fnmsub"
		case neg && !sub:
			armOp = "fmsub"
		default:
			armOp = "fnmadd"
		}
		t := "s16"
		el := "s"
		if double {
			t, el = "d16", "d"
		}
		lines = append(lines, fmt.Sprintf("    %s %s, %s, %s, %s", armOp, t, a, b, c))
		lines = append(lines, fmt.Sprintf("    mov %s.%s[0], v16.%s[0]", dLowV, el, el))
		return zeroHighIfXmm(lines, false, dHighV), nil // VEX.128 zeroes the upper half
	}

	lane := "4s"
	if double {
		lane = "2d"
	}
	type half struct{ d, s2, o3 string }
	halves := []half{{dLowV, s2Lo, o3Lo}}
	if isYmm {
		halves = append(halves, half{dHighV, s2Hi, o3Hi})
	}
	for k, h := range halves {
		t := fmt.Sprintf("v%d", 16+k)
		var a, b, c string
		switch order {
		case 132:
			a, b, c = h.d, h.o3, h.s2
		case 213:
			a, b, c = h.s2, h.d, h.o3
		default:
			a, b, c = h.s2, h.o3, h.d
		}
		// t = +-C, then t +-= A*B fused. -C is negated first (not the sum afterwards) so that an
		// exact cancellation yields +0 as on x86.
		if sub {
			lines = append(lines, fmt.Sprintf("    fneg %s.%s, %s.%s", t, lane, c, lane))
		} else {
			lines = append(lines, fmt.Sprintf("    mov %s.16b, %s.16b", t, c))
		}
		acc := "fmla"
		if neg {
			acc = "fmls"
		}
		lines = append(lines, fmt.Sprintf("    %s %s.%s, %s.%s, %s.%s", acc, t, lane, a, lane, b, lane))
	}
	for k, h := range halves {
		lines = append(lines, fmt.Sprintf("    mov %s.16b, v%d.16b", h.d, 16+k))
	}
	return zeroHighIfXmm(lines, isYmm, dHighV), nil
}
