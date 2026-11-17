package lifter

import (
	"fmt"

	"golang.org/x/arch/x86/x86asm"
	"ps4-recomp/pkg/disasm"
)

// instHasRep reports F3 (REP/REPE); instHasRepn reports F2 (REPNE). On MOVS/STOS/LODS both
// prefixes mean REP (instHasAnyRep).
func instHasRep(inst x86asm.Inst) bool {
	return instRepPrefix(inst) == 0xF3
}

func instHasRepn(inst x86asm.Inst) bool {
	return instRepPrefix(inst) == 0xF2
}

func instHasAnyRep(inst x86asm.Inst) bool {
	return instRepPrefix(inst) != 0
}

func instRepPrefix(inst x86asm.Inst) byte {
	for _, p := range inst.Prefix {
		if p == 0 {
			continue
		}
		switch p & 0xFF {
		case 0xF3, 0xF2:
			return byte(p & 0xFF)
		}
	}
	return 0
}

// liftStringOp handles string instructions: MOVS*, STOS*, LODS*, SCAS*, CMPS*.
//
// Direction: RSI/RDI advance by the element width when DF = 0 and go back by it when DF = 1.
// DF is guest state (GuestContext.df, written by STD/CLD/POPFQ), read here every time; each
// lowering starts by putting the signed step in x5 (strStep). FEX reads DF the same way per
// string instruction (OpcodeDispatcher.h ForeachDirection:286, LoadDir; STD/CLD in
// OpcodeDispatcher.cpp FLAGControlOp:1208).
func (l *Arm64Lifter) liftStringOp(inst disasm.Instruction, width int, lines []string) ([]string, error) {
	op := inst.Inst.Op
	lines = append(lines, l.strStep(width)...)
	switch op {
	case x86asm.MOVSB, x86asm.MOVSW, x86asm.MOVSD, x86asm.MOVSQ:
		return l.liftMovs(inst, width, lines)
	case x86asm.STOSB, x86asm.STOSW, x86asm.STOSD, x86asm.STOSQ:
		return l.liftStos(inst, width, lines)
	case x86asm.LODSB, x86asm.LODSW, x86asm.LODSD, x86asm.LODSQ:
		return l.liftLods(inst, width, lines)
	case x86asm.SCASB, x86asm.SCASW, x86asm.SCASD, x86asm.SCASQ:
		return l.liftScas(inst, width, lines)
	case x86asm.CMPSB, x86asm.CMPSW, x86asm.CMPSD, x86asm.CMPSQ:
		return l.liftCmps(inst, width, lines)
	default:
		lines = append(lines, l.emitUnsupported(inst, "unsupported string op")...)
		return lines, nil
	}
}

// strStep sets x5 to the string step: +width when DF = 0, -width when DF = 1. No flags change.
func (l *Arm64Lifter) strStep(width int) []string {
	shift := 1 // 2 * width = 1 << shift
	switch width {
	case 2:
		shift = 2
	case 4:
		shift = 3
	case 8:
		shift = 4
	}
	lines := l.emitLoadCtxPtr("x5")
	return append(lines,
		fmt.Sprintf("    ldrb w5, [x5, #0x%x] // df", ctxDF),
		fmt.Sprintf("    mov x17, #%d", width),
		fmt.Sprintf("    sub x5, x17, x5, lsl #%d", shift))
}

func strStoreInst(width int) string {
	switch width {
	case 1:
		return "strb"
	case 2:
		return "strh"
	}
	return "str"
}

func strReg(width int, n string) string {
	if width == 8 {
		return "x" + n
	}
	return "w" + n
}

func (l *Arm64Lifter) liftMovs(inst disasm.Instruction, width int, lines []string) ([]string, error) {
	body := []string{
		l.strLoad(width, "x16", "x23"),
		fmt.Sprintf("    %s %s, %s", strStoreInst(width), strReg(width, "16"), l.gm("x24")),
		"    add x23, x23, x5",
		"    add x24, x24, x5",
	}
	return l.strRep(inst, "movs", body, lines), nil
}

func (l *Arm64Lifter) liftStos(inst disasm.Instruction, width int, lines []string) ([]string, error) {
	body := []string{
		fmt.Sprintf("    %s %s, %s", strStoreInst(width), strReg(width, "19"), l.gm("x24")),
		"    add x24, x24, x5",
	}
	return l.strRep(inst, "stos", body, lines), nil
}

// strRep emits body once, or under REP (F3 or F2) RCX times. REP MOVS/STOS keep the flags: the
// count goes down with SUB, not SUBS.
func (l *Arm64Lifter) strRep(inst disasm.Instruction, name string, body, lines []string) []string {
	if !instHasAnyRep(inst.Inst) {
		return append(lines, body...)
	}
	l.labelCounter++
	id := l.labelCounter
	lblLoop := fmt.Sprintf("L_rep_%s_loop_%x_%d", name, inst.Address, id)
	lblDone := fmt.Sprintf("L_rep_%s_done_%x_%d", name, inst.Address, id)
	lines = append(lines, fmt.Sprintf("    cbz x21, %s", lblDone), lblLoop+":")
	lines = append(lines, body...)
	return append(lines,
		"    sub x21, x21, #1",
		fmt.Sprintf("    cbnz x21, %s", lblLoop),
		lblDone+":")
}

func (l *Arm64Lifter) liftLods(inst disasm.Instruction, width int, lines []string) ([]string, error) {
	hasRep := instHasAnyRep(inst.Inst)
	var lblDone string
	if hasRep {
		// Only the last element read stays in the accumulator: skip to it.
		l.labelCounter++
		lblDone = fmt.Sprintf("L_rep_lods_done_%x_%d", inst.Address, l.labelCounter)
		lines = append(lines,
			fmt.Sprintf("    cbz x21, %s", lblDone),
			"    sub x16, x21, #1",
			"    mul x16, x16, x5",
			"    add x23, x23, x16",
		)
	}

	switch width {
	case 1:
		lines = append(lines,
			"    ldrb w16, "+l.gm("x23"),
			"    bfi x19, x16, #0, #8",
		)
	case 2:
		lines = append(lines,
			"    ldrh w16, "+l.gm("x23"),
			"    bfi x19, x16, #0, #16",
		)
	case 4:
		lines = append(lines, "    ldr w19, "+l.gm("x23"))
	case 8:
		lines = append(lines, "    ldr x19, "+l.gm("x23"))
	}
	lines = append(lines, "    add x23, x23, x5")

	if hasRep {
		lines = append(lines,
			"    mov x21, #0",
			fmt.Sprintf("%s:", lblDone),
		)
	}
	return lines, nil
}

// strCompare sets NZCV from lhs - rhs at the operand width (x86 CF in the sub sense). 8/16-bit
// operands are compared in the top bits of a W register, so N, C and V are those of the narrow
// subtraction. lhs is "x19" (AL/AX/EAX/RAX) or a loaded scratch value; scratch x4.
func strCompare(width int, lhs, rhs string) []string {
	switch width {
	case 1, 2:
		sh := 32 - 8*width
		return []string{
			fmt.Sprintf("    lsl w4, w%s, #%d", lhs[1:], sh),
			fmt.Sprintf("    cmp w4, w%s, lsl #%d", rhs[1:], sh),
		}
	case 4:
		return []string{fmt.Sprintf("    cmp w%s, w%s", lhs[1:], rhs[1:])}
	}
	return []string{fmt.Sprintf("    cmp x%s, x%s", lhs[1:], rhs[1:])}
}

// strLoad returns the zero-extending load of one element.
func (l *Arm64Lifter) strLoad(width int, dst, addr string) string {
	switch width {
	case 1:
		return fmt.Sprintf("    ldrb w%s, %s", dst[1:], l.gm(addr))
	case 2:
		return fmt.Sprintf("    ldrh w%s, %s", dst[1:], l.gm(addr))
	case 4:
		return fmt.Sprintf("    ldr w%s, %s", dst[1:], l.gm(addr))
	}
	return fmt.Sprintf("    ldr x%s, %s", dst[1:], l.gm(addr))
}

// liftScas handles SCAS: flags of AL/AX/EAX/RAX - [RDI], RDI steps. liftCmps handles CMPS:
// flags of [RSI] - [RDI], RSI and RDI step. With REPE/REPNE the comparison repeats while RCX
// is not 0 and ZF is 1 (REPE) or 0 (REPNE); RCX = 0 on entry leaves the flags of the previous
// instruction, so the carry is brought into the sub sense first and both paths end in it.
func (l *Arm64Lifter) liftScas(inst disasm.Instruction, width int, lines []string) ([]string, error) {
	body := []string{l.strLoad(width, "x16", "x24"), "    add x24, x24, x5"}
	return l.liftRepCompare(inst, "scas", body, width, "x19", "x16", lines), nil
}

func (l *Arm64Lifter) liftCmps(inst disasm.Instruction, width int, lines []string) ([]string, error) {
	body := []string{
		l.strLoad(width, "x16", "x23"),
		l.strLoad(width, "x17", "x24"),
		"    add x23, x23, x5",
		"    add x24, x24, x5",
	}
	return l.liftRepCompare(inst, "cmps", body, width, "x16", "x17", lines), nil
}

// strParity records the lazy PF/AF of the comparison lhs - rhs (see arm64_lifter_parity.go)
// when a reader can follow. Scratch: x2, x3, x4.
func (l *Arm64Lifter) strParity(inst disasm.Instruction, lhs, rhs string) []string {
	if inst.ParityDead && inst.AdjustDead {
		return nil
	}
	lines := []string{
		fmt.Sprintf("    sub w4, w%s, w%s", lhs[1:], rhs[1:]),
		fmt.Sprintf("    eor w3, w%s, w%s", lhs[1:], rhs[1:]),
	}
	lines = append(lines, l.emitLoadCtxPtr("x2")...)
	return append(lines,
		fmt.Sprintf("    strb w4, [x2, #0x%x] // pf", ctxPF),
		fmt.Sprintf("    strb w3, [x2, #0x%x] // af", ctxAF))
}

func (l *Arm64Lifter) liftRepCompare(inst disasm.Instruction, name string, body []string, width int, lhs, rhs string, lines []string) []string {
	cmp := strCompare(width, lhs, rhs)
	rec := l.strParity(inst, lhs, rhs)
	stopOnMatch := instHasRepn(inst.Inst)
	if !stopOnMatch && !instHasRep(inst.Inst) {
		lines = append(lines, body...)
		lines = append(lines, cmp...)
		lines = append(lines, rec...)
		l.cpol = carrySub
		return lines
	}
	l.labelCounter++
	id := l.labelCounter
	lblLoop := fmt.Sprintf("L_rep_%s_loop_%x_%d", name, inst.Address, id)
	lblDone := fmt.Sprintf("L_rep_%s_done_%x_%d", name, inst.Address, id)
	lblStop := lblDone
	if len(rec) > 0 {
		lblStop = fmt.Sprintf("L_rep_%s_stop_%x_%d", name, inst.Address, id)
	}
	stopBranch := "b.ne" // REPE stops when not equal
	if stopOnMatch {
		stopBranch = "b.eq" // REPNE stops when equal
	}
	lines = append(lines, l.normalizeCarry()...)
	lines = append(lines, fmt.Sprintf("    cbz x21, %s", lblDone), lblLoop+":")
	lines = append(lines, body...)
	lines = append(lines, "    sub x21, x21, #1")
	lines = append(lines, cmp...)
	lines = append(lines,
		fmt.Sprintf("    %s %s", stopBranch, lblStop),
		fmt.Sprintf("    cbnz x21, %s", lblLoop),
	)
	if len(rec) > 0 {
		// At least one comparison ran: its operands are still in the registers.
		lines = append(lines, lblStop+":")
		lines = append(lines, rec...)
	}
	lines = append(lines, lblDone+":")
	l.cpol = carrySub
	return lines
}
