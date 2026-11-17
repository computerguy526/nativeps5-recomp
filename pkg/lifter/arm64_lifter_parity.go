package lifter

import (
	"fmt"
	"regexp"
	"strings"

	"ps4-recomp/pkg/disasm"

	"golang.org/x/arch/x86/x86asm"
)

// PF, AF and DF.
//
// PF and AF are lazy, after FEX-Emu's scheme (FEXCore/Source/Interface/Core/OpcodeDispatcher/
// Flags.cpp: CalculatePF:222, CalculateAF:227, LoadPFRaw:174, LoadAF:182; MIT, see NOTICE):
//
//	GuestContext.pf (0x9c): a byte whose parity is the flag: PF = 1 iff it has an even number
//	                        of set bits. Integer flag writers store the low byte of the result.
//	GuestContext.af (0x9d): AF = bit 4 of (af ^ pf). Writers store the XOR of their operands.
//	GuestContext.df (0x9e): DF as 0 or 1 (STD/CLD/POPFQ write it, string instructions and
//	                        PUSHFQ read it).
//
// A writer stores the bytes only when the liveness pass says a reader can follow
// (disasm.Instruction.ParityDead / AdjustDead), so code that never reads PF or AF pays nothing.
// V stays OF. After an FP compare PF is in host V (the unordered result), as before; the
// disasm PF-location pass (Instruction.ParityInV / ParitySpillPre / ParitySpillExit) tells each
// reader where PF is and where a V-held PF must be stored to the context byte. Nothing here
// changes NZCV.
const (
	ctxPF = 0x9c
	ctxAF = 0x9d
	ctxDF = 0x9e
)

// spillParity stores the PF that host V holds after an FP compare: a byte with even parity (0)
// when PF = 1, odd parity (1) when PF = 0. Scratch: x16, x17.
func (l *Arm64Lifter) spillParity() []string {
	lines := []string{"    cset w16, vc"}
	lines = append(lines, l.emitLoadCtxPtr("x17")...)
	return append(lines, fmt.Sprintf("    strb w16, [x17, #0x%x] // pf", ctxPF))
}

// exitFlags returns what a block-ending instruction does before control leaves the block: C
// goes to the block-boundary sense when CF is live, and a V-held PF is stored when a successor
// reads it from the context.
func (l *Arm64Lifter) exitFlags(inst disasm.Instruction) []string {
	var lines []string
	if !inst.CarryDead {
		lines = append(lines, l.normalizeCarry()...)
	}
	if inst.ParitySpillExit && !l.pfSpilled {
		l.pfSpilled = true
		lines = append(lines, l.spillParity()...)
	}
	return lines
}

// loadParityBit leaves in bit 0 of w16 the XOR of the bits of GuestContext.pf: 1 when PF = 0.
// Scratch: x16, x17 (x17 keeps the context pointer).
func (l *Arm64Lifter) loadParityBit() []string {
	lines := l.emitLoadCtxPtr("x17")
	return append(lines,
		fmt.Sprintf("    ldrb w16, [x17, #0x%x] // pf", ctxPF),
		"    eor w16, w16, w16, lsr #4",
		"    eor w16, w16, w16, lsr #2",
		"    eor w16, w16, w16, lsr #1")
}

// parityToV wraps a lowering that tests PF through host V (cond vs/vc) when PF is in the
// context: NZCV is saved in x6, V becomes PF, and after the lowering NZCV is restored.
func (l *Arm64Lifter) parityToV() (pre, post []string) {
	pre = append([]string{"    mrs x6, nzcv"}, l.loadParityBit()...)
	pre = append(pre, "    eor w16, w16, #1", "    rmif x16, #0, #1") // V = PF
	return pre, []string{"    msr nzcv, x6"}
}

// isParityCond reports the x86 conditions P (10) and NP (11).
func isParityCond(cc int) bool { return cc == 10 || cc == 11 }

// flagBitsPFAF ORs the PF (bit 2) and AF (bit 4) of the guest into w0, for LAHF and PUSHFQ.
// Scratch: w1, w2, x16, x17; x17 is left holding the context pointer.
func (l *Arm64Lifter) flagBitsPFAF(inst disasm.Instruction) []string {
	var lines []string
	if inst.ParityInV {
		lines = append(lines, "    cset w1, vs")
		lines = append(lines, l.emitLoadCtxPtr("x17")...)
	} else {
		lines = append(lines, l.loadParityBit()...)
		lines = append(lines, "    mvn w1, w16", "    and w1, w1, #1")
	}
	return append(lines,
		"    orr w0, w0, w1, lsl #2",
		fmt.Sprintf("    ldrb w1, [x17, #0x%x] // af", ctxAF),
		fmt.Sprintf("    ldrb w2, [x17, #0x%x] // pf", ctxPF),
		"    eor w1, w1, w2",
		"    and w1, w1, #0x10",
		"    orr w0, w0, w1")
}

// storeFlagBitsPFAF stores the PF (bit 2) and AF (bit 4) of the flags image in w0 as the lazy
// bytes, when they are live after inst. Scratch: w1; ctx is the register with the context
// pointer.
func storeFlagBitsPFAF(inst disasm.Instruction, ctx string) []string {
	if inst.ParityDead && inst.AdjustDead {
		return nil
	}
	return []string{
		"    ubfx w1, w0, #2, #1",
		"    eor w1, w1, #1",
		fmt.Sprintf("    strb w1, [%s, #0x%x] // pf", ctx, ctxPF),
		"    and w1, w0, #0x10", // bit 4 of pf is 0, so af ^ pf gives AF back
		fmt.Sprintf("    strb w1, [%s, #0x%x] // af", ctx, ctxAF),
	}
}

// lowByte returns code that makes the low byte of an operand available in a W register
// (only bits 0-7 are meaningful) and that register. Memory goes through x16 into w17.
func (l *Arm64Lifter) lowByte(arg x86asm.Arg, nextPC uint64, scratch string) ([]string, string, bool) {
	switch v := arg.(type) {
	case x86asm.Reg:
		d, ok := arm64RegMap[v]
		if !ok {
			return nil, "", false
		}
		if d.IsHigh8 {
			return []string{fmt.Sprintf("    ubfx %s, %s, #8, #8", scratch, d.W)}, scratch, true
		}
		return nil, d.W, true
	case x86asm.Imm:
		return []string{fmt.Sprintf("    mov %s, #%d", scratch, uint64(v)&0xff)}, scratch, true
	case x86asm.Mem:
		return l.readMemTo(v, nextPC, 1, "x"+scratch[1:], scratch), scratch, true
	}
	return nil, "", false
}

// parityPre runs before the lowering of inst: it stores a V-held PF that the lowering would
// lose, and records PF/AF of an integer operation from its operands (the result's low byte
// depends only on the operands' low bytes and the carry in).
func (l *Arm64Lifter) parityPre(inst disasm.Instruction, nextPC uint64, lines []string) []string {
	if inst.ParitySpillPre {
		lines = append(lines, l.spillParity()...)
	}
	if inst.ParityDead && inst.AdjustDead {
		return lines
	}
	if disasm.ClassifyParity(inst.Inst) != disasm.ParityArith {
		return lines
	}
	op := inst.Inst.Op
	a0, a1 := inst.Inst.Args[0], inst.Inst.Args[1]
	if op == x86asm.CMPXCHG {
		a0, a1 = x86asm.AL, a0 // flags of CMP accumulator, destination (low bytes)
	}
	unary := op == x86asm.NEG || op == x86asm.INC || op == x86asm.DEC
	// A memory operand is read first (its address uses x16); at most one operand is memory.
	var code []string
	var ra, rb string
	var ok bool
	if _, mem := a1.(x86asm.Mem); mem && !unary {
		var c []string
		if c, rb, ok = l.lowByte(a1, nextPC, "w17"); !ok {
			return lines
		}
		code = append(code, c...)
		if c, ra, ok = l.lowByte(a0, nextPC, "w16"); !ok {
			return lines
		}
		code = append(code, c...)
	} else {
		var c []string
		if c, ra, ok = l.lowByte(a0, nextPC, "w17"); !ok {
			return lines
		}
		code = append(code, c...)
		if !unary {
			if c, rb, ok = l.lowByte(a1, nextPC, "w16"); !ok {
				return lines
			}
			code = append(code, c...)
		}
	}
	// Result byte in w5, AF source in w6.
	switch op {
	case x86asm.ADD, x86asm.XADD:
		code = append(code, fmt.Sprintf("    add w5, %s, %s", ra, rb))
	case x86asm.ADC:
		code = append(code, fmt.Sprintf("    cset w5, %s", l.carryCond(true)),
			fmt.Sprintf("    add w5, w5, %s", ra), fmt.Sprintf("    add w5, w5, %s", rb))
	case x86asm.SUB, x86asm.CMP, x86asm.CMPXCHG:
		code = append(code, fmt.Sprintf("    sub w5, %s, %s", ra, rb))
	case x86asm.SBB:
		code = append(code, fmt.Sprintf("    cset w5, %s", l.carryCond(true)),
			fmt.Sprintf("    sub w5, %s, w5", ra), fmt.Sprintf("    sub w5, w5, %s", rb))
	case x86asm.AND, x86asm.TEST:
		code = append(code, fmt.Sprintf("    and w5, %s, %s", ra, rb))
	case x86asm.OR:
		code = append(code, fmt.Sprintf("    orr w5, %s, %s", ra, rb))
	case x86asm.XOR:
		code = append(code, fmt.Sprintf("    eor w5, %s, %s", ra, rb))
	case x86asm.NEG:
		code = append(code, fmt.Sprintf("    neg w5, %s", ra))
	case x86asm.INC:
		code = append(code, fmt.Sprintf("    add w5, %s, #1", ra))
	case x86asm.DEC:
		code = append(code, fmt.Sprintf("    sub w5, %s, #1", ra))
	default:
		return lines
	}
	af := "w6"
	switch op {
	case x86asm.AND, x86asm.TEST, x86asm.OR, x86asm.XOR:
		af = "w5" // AF = 0 (undefined in the SDM, 0 on hardware and in FEX): af ^ pf = 0
	case x86asm.NEG, x86asm.INC, x86asm.DEC:
		af = ra // the other operand is 0 or 1: bit 4 of the XOR is the operand's
	default:
		code = append(code, fmt.Sprintf("    eor w6, %s, %s", ra, rb))
	}
	code = append(code, l.emitLoadCtxPtr("x16")...)
	code = append(code, fmt.Sprintf("    strb w5, [x16, #0x%x] // pf", ctxPF))
	if !inst.AdjustDead {
		code = append(code, fmt.Sprintf("    strb %s, [x16, #0x%x] // af", af, ctxAF))
	}
	return append(lines, code...)
}

var w6Re = regexp.MustCompile(`\b[wx]6\b`)

// parityShiftCount saves the masked CL count of a shift whose destination is RCX itself, for
// parityPost. The count goes to w6, which the register forms of the shift lowerings do not use
// (checked in parityPost).
func (l *Arm64Lifter) parityShiftCount(inst disasm.Instruction, lines []string) []string {
	if inst.ParityDead || disasm.ClassifyParity(inst.Inst) != disasm.ParityShift || !shiftDstIsRCX(inst.Inst) {
		return lines
	}
	if _, cl := shiftCountArg(inst.Inst).(x86asm.Reg); !cl {
		return lines
	}
	return append(lines, fmt.Sprintf("    and w6, w21, #%d", shiftMask(inst.Inst)))
}

func shiftCountArg(inst x86asm.Inst) x86asm.Arg {
	if inst.Op == x86asm.SHLD || inst.Op == x86asm.SHRD {
		return inst.Args[2]
	}
	return inst.Args[1]
}

func shiftMask(inst x86asm.Inst) int {
	if r, ok := inst.Args[0].(x86asm.Reg); ok && arm64RegMap[r].Size == 8 {
		return 63
	}
	if _, ok := inst.Args[0].(x86asm.Mem); ok && inst.MemBytes == 8 {
		return 63
	}
	return 31
}

func shiftDstIsRCX(inst x86asm.Inst) bool {
	r, ok := inst.Args[0].(x86asm.Reg)
	return ok && arm64RegMap[r].X == "x21"
}

// parityPost runs after the lowering of inst (lines[start:] is the lowering):
//   - FP compare: PF is in host V. When AF is live (a LAHF or PUSHFQ can follow) both bytes are
//     stored too, so that AF reads as 0.
//   - PF = AF = 0 instructions store a byte of odd parity and af = 0.
//   - shifts store the low byte of the destination when the masked count is not 0.
func (l *Arm64Lifter) parityPost(inst disasm.Instruction, nextPC uint64, lines []string, start int) []string {
	if fpCmpFuse && inst.FpFlagsLocal {
		return lines // PF is read only inside the fused window, straight from host V
	}
	if inst.ParityDead && inst.AdjustDead {
		return lines
	}
	for _, ln := range lines[start:] {
		if strings.Contains(ln, "_recomp_unsupported_instruction") {
			return lines
		}
	}
	switch disasm.ClassifyParity(inst.Inst) {
	case disasm.ParityFPCompare:
		if inst.AdjustDead {
			return lines
		}
		lines = append(lines, l.spillParity()...)
		return append(lines, fmt.Sprintf("    strb wzr, [x17, #0x%x] // af", ctxAF))
	case disasm.ParityZero:
		lines = append(lines, l.emitLoadCtxPtr("x17")...)
		return append(lines, "    mov w16, #1",
			fmt.Sprintf("    strb w16, [x17, #0x%x] // pf", ctxPF),
			fmt.Sprintf("    strb wzr, [x17, #0x%x] // af", ctxAF))
	case disasm.ParityShift:
		if inst.ParityDead {
			return lines
		}
		count := shiftCountArg(inst.Inst)
		skip := ""
		switch c := count.(type) {
		case x86asm.Imm:
			if int(c)&shiftMask(inst.Inst) == 0 {
				return lines
			}
		case x86asm.Reg:
			if c != x86asm.CL {
				return lines
			}
			l.labelCounter++
			skip = fmt.Sprintf("L_shift_pf_0x%x_%d", inst.Address, l.labelCounter)
			if shiftDstIsRCX(inst.Inst) {
				// The count was saved in w6 before the lowering (parityShiftCount).
				n := 0
				for _, ln := range lines[start:] {
					if w6Re.MatchString(ln) {
						n++
					}
				}
				if n != 1 {
					return append(lines, l.emitUnsupported(inst, "PF after a shift of RCX by CL")...)
				}
				lines = append(lines, fmt.Sprintf("    cbz w6, %s", skip))
			} else {
				lines = append(lines, fmt.Sprintf("    and w17, w21, #%d", shiftMask(inst.Inst)),
					fmt.Sprintf("    cbz w17, %s", skip))
			}
		default:
			return lines
		}
		c, r, ok := l.lowByte(inst.Inst.Args[0], nextPC, "w17")
		if !ok {
			return lines
		}
		lines = append(lines, c...)
		lines = append(lines, l.emitLoadCtxPtr("x16")...)
		lines = append(lines, fmt.Sprintf("    strb %s, [x16, #0x%x] // pf", r, ctxPF))
		if skip != "" {
			lines = append(lines, skip+":")
		}
	}
	return lines
}
