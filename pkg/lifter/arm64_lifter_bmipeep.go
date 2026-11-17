package lifter

import (
	"fmt"
	"sort"

	"golang.org/x/arch/x86/x86asm"

	"ps4-recomp/pkg/disasm"
)

// BMI and narrow-flag peepholes (-bmi-peep, docs/m3/lift2.md).
//
// Each function here lowers one family of instructions to fewer host instructions than the
// general lowering, for the cases where the general lowering's generality is unused: the
// source is a register, the flags it would compute are dead, or the instruction pairs with its
// neighbour. They return ok=false when the case does not apply; the caller then takes the
// general lowering, which stays the reference (the differential test of arm64_bmipeep_test.go
// runs both and compares registers and live flags).

// blockPos returns the index of the instruction at addr in the block being lowered (-1 when the
// lifter was not told the block).
func (l *Arm64Lifter) blockPos(addr uint64) int {
	b := l.x87Blk
	if b == nil {
		return -1
	}
	i := sort.Search(len(b.Insts), func(i int) bool { return b.Insts[i].Address >= addr })
	if i < len(b.Insts) && b.Insts[i].Address == addr {
		return i
	}
	return -1
}

// hostReg is the host register of a general register operand of size 4 or 8 (the whole register,
// not a sub-register), in the width of the operand.
func hostReg(a x86asm.Arg) (string, int, bool) {
	r, ok := a.(x86asm.Reg)
	if !ok {
		return "", 0, false
	}
	d, ok := arm64RegMap[r]
	if !ok || d.IsHigh8 {
		return "", 0, false
	}
	switch d.Size {
	case 4:
		return d.W, 4, true
	case 8:
		return d.X, 8, true
	}
	return "", 0, false
}

// liftBmiFast: BLSI, BLSR, BLSMSK with a 32/64-bit destination.
//
//	blsi:   neg t, src;          ands dst, src, t       CF = (src != 0)
//	blsr:   subs t, src, #1;     ands dst, src, t       CF = (src == 0)
//	blsmsk: subs t, src, #1;     eor dst, src, t        CF = (src == 0)
//
// ANDS gives SF and ZF of the result with CF = OF = 0; when CF is live it is inserted into C
// (carryAdd sense) from the instruction that computes it. Flags dead: the two plain instructions.
func (l *Arm64Lifter) liftBmiFast(inst disasm.Instruction, nextPC uint64, lines []string) ([]string, bool) {
	op := inst.Inst.Op
	args := inst.Inst.Args
	dst, sz, ok := hostReg(args[0])
	if !ok {
		return nil, false
	}
	r := "w"
	if sz == 8 {
		r = "x"
	}
	src := ""
	if s, ssz, ok := hostReg(args[1]); ok && ssz == sz {
		src = s
	} else if mem, ok := args[1].(x86asm.Mem); ok {
		lines = append(lines, l.readMemTo(mem, nextPC, sz, "x0", "w0")...)
		src = r + "0"
	} else {
		return nil, false
	}
	t := r + "1"
	flags := !inst.SkipFlags
	needCF := flags && !inst.CarryDead
	switch op {
	case x86asm.BLSI:
		lines = append(lines, fmt.Sprintf("    neg %s, %s", t, src))
		if flags {
			lines = append(lines, fmt.Sprintf("    ands %s, %s, %s", dst, src, t))
			if needCF {
				lines = append(lines, "    cset w2, ne", "    rmif x2, #63, #2")
			}
		} else {
			lines = append(lines, fmt.Sprintf("    and %s, %s, %s", dst, src, t))
		}
	case x86asm.BLSR:
		if needCF {
			lines = append(lines, fmt.Sprintf("    subs %s, %s, #1", t, src), "    cset w2, lo")
		} else {
			lines = append(lines, fmt.Sprintf("    sub %s, %s, #1", t, src))
		}
		if flags {
			lines = append(lines, fmt.Sprintf("    ands %s, %s, %s", dst, src, t))
			if needCF {
				lines = append(lines, "    rmif x2, #63, #2")
			}
		} else {
			lines = append(lines, fmt.Sprintf("    and %s, %s, %s", dst, src, t))
		}
	case x86asm.BLSMSK:
		if needCF {
			lines = append(lines, fmt.Sprintf("    subs %s, %s, #1", t, src), "    cset w2, lo")
		} else {
			lines = append(lines, fmt.Sprintf("    sub %s, %s, #1", t, src))
		}
		lines = append(lines, fmt.Sprintf("    eor %s, %s, %s", dst, src, t))
		if flags {
			lines = append(lines, fmt.Sprintf("    tst %s, %s", dst, dst))
			if needCF {
				lines = append(lines, "    rmif x2, #63, #2")
			}
		}
	default:
		return nil, false
	}
	if flags {
		l.cpol = carryAdd
	}
	return lines, true
}

// liftBitScanFast: LZCNT and TZCNT with a 32/64-bit destination and a register or memory source.
//
//	lzcnt: [cmp src, #0]; clz dst, src; [ccmp dst, #0, #0, ne]
//	tzcnt: [cmp src, #0]; rbit t, src; clz dst, t; [ccmp dst, #0, #0, ne]
//
// The CMP and CCMP (flags live) give ZF = (result == 0) and C = !CF = (src != 0), carrySub; the
// CMP reads the source before the destination is written.
func (l *Arm64Lifter) liftBitScanFast(inst disasm.Instruction, nextPC uint64, lines []string) ([]string, bool) {
	op := inst.Inst.Op
	args := inst.Inst.Args
	dst, sz, ok := hostReg(args[0])
	if !ok || (op != x86asm.LZCNT && op != x86asm.TZCNT) {
		return nil, false
	}
	r := "w"
	if sz == 8 {
		r = "x"
	}
	src := ""
	if s, ssz, ok := hostReg(args[1]); ok && ssz == sz {
		src = s
	} else if mem, ok := args[1].(x86asm.Mem); ok {
		lines = append(lines, l.readMemTo(mem, nextPC, sz, "x0", "w0")...)
		src = r + "0"
	} else {
		return nil, false
	}
	flags := !inst.SkipFlags
	if flags {
		lines = append(lines, fmt.Sprintf("    cmp %s, #0", src))
	}
	if op == x86asm.LZCNT {
		lines = append(lines, fmt.Sprintf("    clz %s, %s", dst, src))
	} else {
		lines = append(lines, fmt.Sprintf("    rbit %s1, %s", r, src), fmt.Sprintf("    clz %s, %s1", dst, r))
	}
	if flags {
		lines = append(lines, fmt.Sprintf("    ccmp %s, #0, #0, ne", dst))
		l.cpol = carrySub
	}
	return lines, true
}

// constCtl looks back in the block for the value of the 32/64-bit register reg: the nearest
// earlier instruction that writes it must be `mov reg, imm`.
func (l *Arm64Lifter) constReg(addr uint64, reg x86asm.Reg) (uint64, bool) {
	i := l.blockPos(addr)
	if i < 0 {
		return 0, false
	}
	d, ok := arm64RegMap[reg]
	if !ok {
		return 0, false
	}
	idx := int(reg-x86asm.RAX) & 15
	if reg >= x86asm.EAX && reg <= x86asm.R15L {
		idx = int(reg - x86asm.EAX)
	}
	_ = d
	b := l.x87Blk
	for k := i - 1; k >= 0 && k >= i-24; k-- {
		pi := b.Insts[k].Inst
		if disasm.RegWrites(pi)&(1<<uint(idx)) == 0 {
			continue
		}
		if pi.Op != x86asm.MOV {
			return 0, false
		}
		dr, ok := pi.Args[0].(x86asm.Reg)
		if !ok || dr != reg {
			return 0, false
		}
		imm, ok := pi.Args[1].(x86asm.Imm)
		if !ok {
			return 0, false
		}
		if reg >= x86asm.EAX && reg <= x86asm.R15L {
			return uint64(imm) & 0xffffffff, true
		}
		return uint64(imm), true
	}
	return 0, false
}

// liftBextrFast: BEXTR whose control register holds a constant set by a MOV earlier in the block:
// one UBFX. start >= size or len == 0 gives 0, a field that runs past the top is cut at it.
func (l *Arm64Lifter) liftBextrFast(inst disasm.Instruction, nextPC uint64, lines []string) ([]string, bool) {
	args := inst.Inst.Args
	dst, sz, ok := hostReg(args[0])
	ctl, okc := args[2].(x86asm.Reg)
	if !ok || !okc {
		return nil, false
	}
	v, okv := l.constReg(inst.Address, ctl)
	if !okv {
		return nil, false
	}
	start, length := int(v&0xff), int((v>>8)&0xff)
	bits := 8 * sz
	r := "w"
	if sz == 8 {
		r = "x"
	}
	src := ""
	if s, ssz, ok := hostReg(args[1]); ok && ssz == sz {
		src = s
	} else if mem, ok := args[1].(x86asm.Mem); ok {
		lines = append(lines, l.readMemTo(mem, nextPC, sz, "x0", "w0")...)
		src = r + "0"
	} else {
		return nil, false
	}
	if length > bits-start {
		length = bits - start
	}
	if start >= bits || length <= 0 {
		lines = append(lines, fmt.Sprintf("    mov %s, %szr", dst, r))
	} else {
		lines = append(lines, fmt.Sprintf("    ubfx %s, %s, #%d, #%d", dst, src, start, length))
	}
	if !inst.SkipFlags {
		lines = append(lines, fmt.Sprintf("    tst %s, %s", dst, dst))
		l.cpol = carryClear
	}
	return lines, true
}

// liftArithFast: narrow and same-register forms of TEST, XOR and friends, and the compare of a
// memory operand with zero that feeds a conditional branch.
func (l *Arm64Lifter) liftArithFast(inst disasm.Instruction, nextPC uint64, fn *disasm.Function, lines []string) ([]string, bool) {
	op := inst.Inst.Op
	args := inst.Inst.Args
	flags := !inst.SkipFlags
	dstReg, dstIsReg := args[0].(x86asm.Reg)
	switch {
	case op == x86asm.TEST && dstIsReg && flags:
		// test r8, r8 / test r16, r16 (low sub-register, same register).
		srcReg, ok := args[1].(x86asm.Reg)
		d, okd := arm64RegMap[dstReg]
		if !ok || !okd || srcReg != dstReg || d.IsHigh8 || (d.Size != 1 && d.Size != 2) {
			return nil, false
		}
		mask, sh := "0xff", 24
		if d.Size == 2 {
			mask, sh = "0xffff", 16
		}
		if inst.SignDead {
			lines = append(lines, fmt.Sprintf("    tst %s, #%s", d.W, mask))
		} else {
			lines = append(lines, fmt.Sprintf("    lsl w17, %s, #%d", d.W, sh), "    tst w17, w17")
		}
		l.cpol = carryClear
		return lines, true

	case op == x86asm.XOR && dstIsReg:
		d, okd := arm64RegMap[dstReg]
		if !okd {
			return nil, false
		}
		// xor r, r: the result is 0 and ZF = 1, SF = CF = OF = 0 (ANDS with the zero register).
		if srcReg, ok := args[1].(x86asm.Reg); ok && srcReg == dstReg && (d.Size == 4 || d.Size == 8) {
			dst, r := d.W, "w"
			if d.Size == 8 {
				dst, r = d.X, "x"
			}
			if flags {
				lines = append(lines, fmt.Sprintf("    ands %s, %szr, %szr", dst, r, r))
				l.cpol = carryClear
			} else {
				lines = append(lines, fmt.Sprintf("    mov %s, %szr", dst, r))
			}
			return lines, true
		}
		fallthrough
	case (op == x86asm.XOR || op == x86asm.OR || op == x86asm.AND) && dstIsReg && !flags:
		// xor/or/and r8, imm with dead flags: one logical instruction on the register, the low byte inserted;
		// a MOVZX of the result into the same register is folded in.
		d, okd := arm64RegMap[dstReg]
		imm, ok := args[1].(x86asm.Imm)
		if !okd || !ok || d.Size != 1 || d.IsHigh8 || flags {
			return nil, false
		}
		arm := map[x86asm.Op]string{x86asm.XOR: "eor", x86asm.OR: "orr", x86asm.AND: "and"}[op]
		v := uint64(imm) & 0xff
		var seq []string
		if logicalImm32(v) {
			seq = []string{fmt.Sprintf("    %s w16, %s, #%#x", arm, d.W, v)}
		} else {
			seq = []string{fmt.Sprintf("    mov w16, #%#x", v), fmt.Sprintf("    %s w16, %s, w16", arm, d.W)}
		}
		lines = append(lines, seq...)
		// Fold a following MOVZX r32/r64, <this register> in the block: the whole register is written.
		if i := l.blockPos(inst.Address); i >= 0 && i+1 < len(l.x87Blk.Insts) {
			nx := l.x87Blk.Insts[i+1]
			if nx.Inst.Op == x86asm.MOVZX {
				nd, ok1 := nx.Inst.Args[0].(x86asm.Reg)
				ns, ok2 := nx.Inst.Args[1].(x86asm.Reg)
				if ok1 && ok2 && ns == dstReg {
					if ndd, ok := arm64RegMap[nd]; ok && ndd.X == d.X && (ndd.Size == 4 || ndd.Size == 8) {
						l.fuseSkip = nx.Address
						return append(lines, fmt.Sprintf("    uxtb %s, w16", d.W)), true
					}
				}
			}
		}
		return append(lines, fmt.Sprintf("    bfi %s, x16, #0, #8", d.X)), true

	case op == x86asm.CMP && flags:
		// cmp [mem], 0 followed by je/jne (all flags dead after it): load, then CBZ/CBNZ at the branch.
		mem, ok := args[0].(x86asm.Mem)
		imm, oki := args[1].(x86asm.Imm)
		if !ok || !oki || imm != 0 {
			return nil, false
		}
		i := l.blockPos(inst.Address)
		if i < 0 || i+1 >= len(l.x87Blk.Insts) {
			return nil, false
		}
		j := l.x87Blk.Insts[i+1]
		if (j.Inst.Op != x86asm.JE && j.Inst.Op != x86asm.JNE) || !allFlagsDead(j) {
			return nil, false
		}
		rel, okr := j.Inst.Args[0].(x86asm.Rel)
		if !okr {
			return nil, false
		}
		target := uint64(int64(j.Address+uint64(j.Inst.Len)) + int64(rel))
		dist := int64(target) - int64(j.Address+uint64(j.Inst.Len))
		if dist < 0 {
			dist = -dist
		}
		if localLabel(fn, target) == "" || dist > 16384 {
			return nil, false
		}
		size := defMemSz(inst)
		if size != 1 && size != 2 && size != 4 && size != 8 {
			return nil, false
		}
		lines = append(lines, l.readMemTo(mem, nextPC, size, "x17", "w17")...)
		l.cbzAt = j.Address
		l.cbzReg = "w17"
		if size == 8 {
			l.cbzReg = "x17"
		}
		return lines, true
	}
	return nil, false
}

// allFlagsDead: no flag is live after the instruction.
func allFlagsDead(in disasm.Instruction) bool {
	return in.CarryDead && in.OverflowDead && in.ParityDead && in.AdjustDead && in.ZeroDead && in.SignDead
}

// logicalImm32 reports whether v (a value of at most 8 bits) is encodable as an ARM64 32-bit
// logical immediate: a rotated run of ones of an element size 2, 4, 8, 16 or 32, replicated.
func logicalImm32(v uint64) bool {
	if v == 0 || v == 0xffffffff {
		return false
	}
	for size := uint(2); size <= 32; size *= 2 {
		mask := uint64(1)<<size - 1
		e := v & mask
		// v must be the replication of e over 32 bits.
		rep := uint64(0)
		for sh := uint(0); sh < 32; sh += size {
			rep |= e << sh
		}
		if rep != v || e == 0 || e == mask {
			continue
		}
		// e is a rotated contiguous run of ones in size bits: e rotated so the run starts at bit 0
		// has the form 2^k - 1.
		for rot := uint(0); rot < size; rot++ {
			x := (e>>rot | e<<(size-rot)) & mask
			if x&(x+1) == 0 {
				return true
			}
		}
	}
	return false
}
