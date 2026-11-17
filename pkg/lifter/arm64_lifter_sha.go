package lifter

import (
	"fmt"

	"ps4-recomp/pkg/disasm"

	"golang.org/x/arch/x86/x86asm"
)

// SHA-NI (SHA1RNDS4, SHA1NEXTE, SHA1MSG1, SHA1MSG2, SHA256RNDS2, SHA256MSG1, SHA256MSG2).
//
// All seven map onto the arm64 FEAT_SHA1 / FEAT_SHA256 instructions (hw.optional.arm.FEAT_SHA1
// and FEAT_SHA256 are 1 on every Apple silicon part), so no runtime helper is needed. The
// sequences follow FEX-Emu's OpcodeDispatcher/Crypto.cpp (MIT, see NOTICE); the operand layouts
// differ between the two ISAs:
//
//   - x86 SHA1 keeps the state as dwords [3..0] = A,B,C,D and the schedule as W0..W3 in dwords
//     [3..0]; arm64 SHA1C/P/M/SU1 expect the opposite lane order, so the operands are reversed
//     (rev64 + ext #8) going in and the result coming out ("SHADataShuffle").
//   - SHA1RNDS4 has the round constant implicit in the immediate (K0..K3) and E already folded
//     into the schedule word, so arm64 SHA1x takes E = 0 and Vm = W+K.
//   - SHA256RNDS2 does two rounds with WK in XMM0 dwords 0,1; arm64 SHA256H/H2 do four rounds
//     with all four dwords. Rounds 3 and 4 only influence the halves of the state the result
//     does not read, so the extra rounds are harmless (FEX does the same).
//   - SHA256MSG2 maps onto SHA256SU1 with a zero accumulator: Vn = [d3,d0,d1,d2] and
//     Vm = [d3,d3,src2,src3] make T0 = [d0,d1,d2,d3], i.e. the destination dwords themselves.
//
// Scratch: v16-v20 and v30; the arm64 SHA ops read/write Q registers, which fixUpperHalves
// treats like any other scratch name.

var sha1K = [4]uint32{0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xCA62C1D6}

// shaSrc resolves the xmm/m128 source operand (index 1) to a vector register name.
func (l *Arm64Lifter) shaSrc(inst disasm.Instruction, nextPC uint64, lines *[]string) (string, bool) {
	switch s := inst.Inst.Args[1].(type) {
	case x86asm.Reg:
		if x, ok := arm64XmmMap[s]; ok {
			return x.V, true
		}
	case x86asm.Mem:
		*lines = append(*lines, computeEffAddr(s, nextPC, "x16")...)
		*lines = append(*lines, "    ldr q30, "+l.gm("x16"))
		return "v30", true
	}
	return "", false
}

func qOf(v string) string { return "q" + v[1:] }
func sOf(v string) string { return "s" + v[1:] }

// shaReverse emits dst = src with its four dwords reversed, using tmp.
func shaReverse(lines []string, dst, src, tmp string) []string {
	return append(lines,
		fmt.Sprintf("    rev64 %s.4s, %s.4s", tmp, src),
		fmt.Sprintf("    ext %s.16b, %s.16b, %s.16b, #8", dst, tmp, tmp))
}

func (l *Arm64Lifter) liftSha(inst disasm.Instruction, nextPC uint64, lines []string) ([]string, error) {
	dstReg, ok := inst.Inst.Args[0].(x86asm.Reg)
	dx, okD := arm64XmmMap[dstReg]
	if !ok || !okD {
		return append(lines, l.emitUnsupported(inst, "sha dst")...), nil
	}
	d := dx.V
	src, okS := l.shaSrc(inst, nextPC, &lines)
	if !okS {
		return append(lines, l.emitUnsupported(inst, "sha src")...), nil
	}
	switch inst.Inst.Op {
	case x86asm.SHA1NEXTE:
		// dst = src + [0,0,0,rol30(dst[3])] with dword 3 taken from the sum, 0..2 from src.
		lines = append(lines,
			fmt.Sprintf("    dup v16.4s, %s.s[3]", d),
			"    sha1h s16, s16",
			"    dup v16.4s, v16.s[0]",
			fmt.Sprintf("    add v16.4s, %s.4s, v16.4s", src),
			fmt.Sprintf("    mov v17.16b, %s.16b", src),
			"    mov v17.s[3], v16.s[3]",
			fmt.Sprintf("    mov %s.16b, v17.16b", d))
	case x86asm.SHA1MSG1:
		lines = append(lines,
			fmt.Sprintf("    ext v16.16b, %s.16b, %s.16b, #8", src, d),
			fmt.Sprintf("    eor %s.16b, %s.16b, v16.16b", d, d))
	case x86asm.SHA1MSG2:
		lines = shaReverse(lines, "v16", d, "v20")
		lines = shaReverse(lines, "v17", src, "v20")
		lines = append(lines, "    sha1su1 v16.4s, v17.4s")
		lines = shaReverse(lines, d, "v16", "v20")
	case x86asm.SHA1RNDS4:
		imm, okI := inst.Inst.Args[2].(x86asm.Imm)
		if !okI {
			return append(lines, l.emitUnsupported(inst, "sha1rnds4 imm")...), nil
		}
		k := sha1K[imm&3]
		op := []string{"sha1c", "sha1p", "sha1m", "sha1p"}[imm&3]
		lines = shaReverse(lines, "v16", d, "v20")
		lines = shaReverse(lines, "v17", src, "v20")
		lines = append(lines,
			fmt.Sprintf("    mov w17, #0x%x", k&0xffff),
			fmt.Sprintf("    movk w17, #0x%x, lsl #16", k>>16),
			"    dup v19.4s, w17",
			"    add v17.4s, v17.4s, v19.4s",
			"    movi v18.16b, #0",
			fmt.Sprintf("    %s q16, s18, v17.4s", op))
		lines = shaReverse(lines, d, "v16", "v20")
	case x86asm.SHA256MSG1:
		lines = append(lines,
			fmt.Sprintf("    mov v16.16b, %s.16b", d),
			fmt.Sprintf("    sha256su0 v16.4s, %s.4s", src),
			fmt.Sprintf("    mov %s.16b, v16.16b", d))
	case x86asm.SHA256MSG2:
		lines = append(lines,
			fmt.Sprintf("    ext v16.16b, %s.16b, %s.16b, #12", d, d),
			fmt.Sprintf("    dup v17.4s, %s.s[3]", d),
			fmt.Sprintf("    zip2 v17.2d, v17.2d, %s.2d", src),
			"    movi v18.16b, #0",
			"    sha256su1 v18.4s, v16.4s, v17.4s",
			fmt.Sprintf("    mov %s.16b, v18.16b", d))
	case x86asm.SHA256RNDS2:
		k := arm64XmmMap[x86asm.X0].V
		lines = append(lines,
			// abcd = rev64(zip2.2d(src, dst)); efgh = rev64(zip1.2d(src, dst))
			fmt.Sprintf("    zip2 v16.2d, %s.2d, %s.2d", src, d),
			"    rev64 v16.4s, v16.4s",
			fmt.Sprintf("    zip1 v17.2d, %s.2d, %s.2d", src, d),
			"    rev64 v17.4s, v17.4s",
			"    mov v18.16b, v16.16b",
			fmt.Sprintf("    sha256h q18, q17, %s.4s", k),
			"    mov v19.16b, v17.16b",
			fmt.Sprintf("    sha256h2 q19, q16, %s.4s", k),
			// result = rev64(zip2.2d(B, A))
			"    zip2 v16.2d, v19.2d, v18.2d",
			fmt.Sprintf("    rev64 %s.4s, v16.4s", d))
	default:
		return append(lines, l.emitUnsupported(inst, "sha op")...), nil
	}
	return lines, nil
}
