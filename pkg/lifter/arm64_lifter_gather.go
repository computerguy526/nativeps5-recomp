package lifter

import (
	"fmt"

	"ps4-recomp/pkg/disasm"

	"golang.org/x/arch/x86/x86asm"
)

// AVX2 gathers: VGATHER{D,Q}P{S,D} and VPGATHER{D,Q}{D,Q}, xmm and ymm.
//
//	vgather* dst, [base + vidx*scale + disp], mask
//
// Per element i (in order): if the mask element's MSB is set, dst[i] = [base + sext(idx[i])*scale
// + disp]; masked-off elements keep dst. On completion the whole mask register (and, for
// xmm forms, the upper halves of dst and mask) is zero; dst elements beyond the number of
// gathered elements (VGATHERQPS/VPGATHERQD with a 128-bit index: 2 of 4) are zeroed.
//
// The lowering is element-wise through memOperand-style addressing (gm, so base and identity
// mode and the TSO rewriter apply): base+disp is computed once into x6 (computeEffAddr with the
// index removed), each element adds the sign-extended index with the scale as an LSL, wrapping
// at 64 bits like the hardware ("overflow" FEX tests), then loads one element. Elements fault in
// order. Differences: the mask is cleared once at the end instead of per element, so a fault
// in element k leaves earlier mask bits set (architecturally cleared) and dst partly updated;
// guest code cannot resume from a recompiled fault, so this is unobservable. Cost: ~6 ops and
// a branch per element (8 elements max).
//
// Scratch GPRs: x6, x16, x17.

func isGatherOp(op x86asm.Op) bool {
	switch op {
	case x86asm.VGATHERDPS, x86asm.VGATHERDPD, x86asm.VGATHERQPS, x86asm.VGATHERQPD,
		x86asm.VPGATHERDD, x86asm.VPGATHERDQ, x86asm.VPGATHERQD, x86asm.VPGATHERQQ:
		return true
	}
	return false
}

func gatherShape(op x86asm.Op) (idxBits, dataBits int) {
	switch op {
	case x86asm.VGATHERDPS, x86asm.VPGATHERDD:
		return 32, 32
	case x86asm.VGATHERDPD, x86asm.VPGATHERDQ:
		return 32, 64
	case x86asm.VGATHERQPS, x86asm.VPGATHERQD:
		return 64, 32
	}
	return 64, 64
}

// vecLane names element i (of bits-wide elements) of a vector held as lowV (+ highV for the
// upper 128 bits): "v3.s[2]" style.
func vecLane(lowV, highV string, i, bits int) string {
	per := 128 / bits
	v := lowV
	if i >= per {
		v, i = highV, i-per
	}
	suf := "s"
	if bits == 64 {
		suf = "d"
	}
	return fmt.Sprintf("%s.%s[%d]", v, suf, i)
}

func (l *Arm64Lifter) liftGather(inst disasm.Instruction, nextPC uint64, lines []string) ([]string, error) {
	dstReg, okD := inst.Inst.Args[0].(x86asm.Reg)
	mem, okM := inst.Inst.Args[1].(x86asm.Mem)
	maskReg, okK := inst.Inst.Args[2].(x86asm.Reg)
	dYmm, _, _, dLo, dHi, _, _, okDD := getVectorDesc(dstReg)
	_, _, _, kLo, kHi, _, _, okKD := getVectorDesc(maskReg)
	_, _, _, iLo, iHi, _, _, okID := getVectorDesc(mem.Index)
	if !okD || !okM || !okK || !okDD || !okKD || !okID || mem.Index == 0 {
		return append(lines, l.emitUnsupported(inst, "gather operands")...), nil
	}
	idxBits, dataBits := gatherShape(inst.Inst.Op)
	idxYmm := mem.Index >= x86asm.Y0 && mem.Index <= x86asm.Y15
	n := (128 << b2i(idxYmm)) / idxBits
	if m := (128 << b2i(dYmm)) / dataBits; m < n {
		n = m
	}
	shift := map[uint8]int{0: 0, 1: 0, 2: 1, 4: 2, 8: 3}[mem.Scale]

	base := mem
	base.Index, base.Scale = 0, 1
	lines = append(lines, computeEffAddr(base, nextPC, "x6")...)

	l.labelCounter++
	id := l.labelCounter
	ldr, tmp, tbz := "ldr w17", "w17", 31
	if dataBits == 64 {
		ldr, tmp, tbz = "ldr x17", "x17", 63
	}
	for i := 0; i < n; i++ {
		skip := fmt.Sprintf("L_gather_0x%x_%d_%d", inst.Address, id, i)
		lines = append(lines,
			fmt.Sprintf("    umov %s, %s", tmp, vecLane(kLo, kHi, i, dataBits)),
			fmt.Sprintf("    tbz %s, #%d, %s", tmp, tbz, skip))
		if idxBits == 32 {
			lines = append(lines, fmt.Sprintf("    smov x17, %s", vecLane(iLo, iHi, i, 32)))
		} else {
			lines = append(lines, fmt.Sprintf("    umov x17, %s", vecLane(iLo, iHi, i, 64)))
		}
		lines = append(lines,
			fmt.Sprintf("    add x16, x6, x17, lsl #%d", shift),
			fmt.Sprintf("    %s, %s", ldr, l.gm("x16")),
			fmt.Sprintf("    mov %s, %s", vecLane(dLo, dHi, i, dataBits), tmp),
			skip+":")
	}
	// Elements beyond n in the destination are zeroed (they exist only when 2 indices fill an xmm of dwords).
	if per := 128 / dataBits; n < per && !dYmm {
		for i := n; i < per; i++ {
			lines = append(lines, fmt.Sprintf("    mov %s, wzr", vecLane(dLo, dHi, i, dataBits)))
		}
	}
	lines = append(lines, fmt.Sprintf("    movi %s.16b, #0", kLo), fmt.Sprintf("    movi %s.16b, #0", kHi))
	if !dYmm {
		lines = append(lines, fmt.Sprintf("    movi %s.16b, #0", dHi))
	}
	return lines, nil
}

func b2i(b bool) int {
	if b {
		return 1
	}
	return 0
}
