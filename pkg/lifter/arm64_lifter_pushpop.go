package lifter

import (
	"fmt"

	"ps4-recomp/pkg/disasm"

	"golang.org/x/arch/x86/x86asm"
)

// Push/pop fusion (docs/m3/liftmisc.md item 2). A run of consecutive PUSH r64 (or POP r64) of one
// basic block lowers to one stack-pointer update and STP/LDP pairs instead of `sub x26, x26, #8;
// str` / `ldr; add x26, x26, #8` per register.
//
//	push rbp; push r15; push r14; push rbx      sub x26, x26, #32
//	                                            add x16, x28, x26        (base mode; identity: x26 itself)
//	                                            stp r15, rbp, [x16, #16]
//	                                            stp rbx, r14, [x16]
//
// The memory image is exactly the sequence's: the first-pushed register at the highest address, the
// stores issued from the highest address down like the originals, and the final x26 is the
// original's. Pops load in order and update x26 once at the end.
//
// Faulting semantics. The stack is thread-private and its guard page makes an access below the
// stack a fatal fault, so the only visible differences are at a fault inside a run, which the
// guard page turns into the end of the process:
//   - push: x26 is lowered by the whole run before the first store (the original lowers it one slot
//     per store), so a fault reports the run's final rsp, and the stores of the pair that contains
//     the faulting address are all or nothing (an STP that crosses a page boundary may not perform
//     the half that is in the mapped page). Which address faults is unchanged: the same stores
//     are issued, highest first, and the first one that touches the guard page faults.
//   - pop: x26 is raised after the last load, so a fault leaves the original rsp (the original
//     raises it after each load: a fault in the k-th leaves rsp+8k); the destination registers of
//     the loads issued before the fault may or may not be written.
// A guest fault handler that resumes after a stack fault inside such a run would see this; the
// guard-page policy has none (a stack fault is fatal and reported with the guest pc of the run's
// first instruction). Runs are limited to maxPushPopRun registers (STP/LDP reach) and never
// contain PUSH RSP / POP RSP, a destination that appears twice, or a 16-bit/memory/immediate form. A POP run also stops before a pop that
// -tso-elide wants ordered (popOrdered): such a pop is an acquire load on its own.
// (A function marked TSO-relaxed skips ordering altogether; fusion is still declined there, which only costs the fusion.)

var pushPopFuse = true

// SetPushPopFuse turns push/pop fusion on or off for all subsequent lifting (`-pushpop-fuse`).
func SetPushPopFuse(on bool) { pushPopFuse = on }

// GetPushPopFuse reports whether push/pop fusion is on.
func GetPushPopFuse() bool { return pushPopFuse }

const maxPushPopRun = 16

// ppRegOf returns the host register of a PUSH/POP r64 operand that can be part of a run.
func ppRegOf(in x86asm.Inst, op x86asm.Op) (string, bool) {
	if in.Op != op || in.Args[0] == nil {
		return "", false
	}
	r, ok := in.Args[0].(x86asm.Reg)
	if !ok || r == x86asm.RSP {
		return "", false
	}
	d, ok := arm64RegMap[r]
	if !ok || d.Size != 8 || d.IsHigh8 {
		return "", false
	}
	return d.X, true
}

// popOrderedInsn is popOrdered for an instruction of the current block (its own privacy bits), with the same gates as
// LiftInstructionToBuf's call of applyTSO (-no-tso lowers nothing ordered).
func (l *Arm64Lifter) popOrderedInsn(in disasm.Instruction) bool {
	if l.tsoElide == TSOElideOff || l.noTSO {
		return false
	}
	return in.StackPriv&l.tsoElide.popBit() == 0
}

// ppRun returns the host registers of the run of op starting at the instruction at address addr
// in the current block, or nil when it is shorter than two or the block is unknown.
func (l *Arm64Lifter) ppRun(addr uint64, op x86asm.Op) []string {
	b := l.x87Blk
	if b == nil {
		return nil
	}
	i := 0
	for i < len(b.Insts) && b.Insts[i].Address != addr {
		i++
	}
	if i == len(b.Insts) {
		return nil
	}
	var regs []string
	seen := map[string]bool{}
	for ; i < len(b.Insts) && len(regs) < maxPushPopRun; i++ {
		r, ok := ppRegOf(b.Insts[i].Inst, op)
		if !ok || (op == x86asm.POP && seen[r]) {
			break
		}
		// A pop whose slot must be an acquire load (-tso-elide: neither private nor a register save slot) is lowered alone, where
		// applyTSO rewrites it; the fused LDP is addressed through x16/x26 and would stay plain.
		if op == x86asm.POP && l.popOrderedInsn(b.Insts[i]) {
			break
		}
		seen[r] = true
		regs = append(regs, r)
	}
	if len(regs) < 2 {
		return nil
	}
	return regs
}

// liftPushPopRun lowers the run starting at inst if it is the first of one, and reports the
// instructions it covers. For a later instruction of a run already lowered it emits nothing.
func (l *Arm64Lifter) liftPushPopRun(inst disasm.Instruction, lines []string) ([]string, bool) {
	if !pushPopFuse {
		return lines, false
	}
	op := inst.Inst.Op
	if l.ppSkipUntil != 0 && inst.Address <= l.ppSkipUntil && inst.Address > l.ppSkipFrom {
		if !l.noComments {
			lines = append(lines, "    // fused into the run above")
		}
		return lines, true
	}
	regs := l.ppRun(inst.Address, op)
	if regs == nil {
		return lines, false
	}
	// The instructions covered after this one.
	b := l.x87Blk
	j := 0
	for b.Insts[j].Address != inst.Address {
		j++
	}
	l.ppSkipFrom = inst.Address
	l.ppSkipUntil = b.Insts[j+len(regs)-1].Address
	n := len(regs)
	base := "x26"
	if l.AddrMode() != AddrIdentity {
		base = "x16"
	}
	if op == x86asm.PUSH {
		lines = append(lines, fmt.Sprintf("    sub x26, x26, #%d", 8*n))
		if base == "x16" {
			lines = append(lines, "    add x16, x28, x26")
		}
		for k := 0; k < n; k += 2 {
			if k+1 < n {
				lines = append(lines, fmt.Sprintf("    stp %s, %s, [%s, #%d]", regs[k+1], regs[k], base, 8*(n-2-k)))
			} else {
				lines = append(lines, fmt.Sprintf("    str %s, [%s, #%d]", regs[k], base, 8*(n-1-k)))
			}
		}
		return lines, true
	}
	if base == "x16" {
		lines = append(lines, "    add x16, x28, x26")
	}
	for k := 0; k < n; k += 2 {
		if k+1 < n {
			lines = append(lines, fmt.Sprintf("    ldp %s, %s, [%s, #%d]", regs[k], regs[k+1], base, 8*k))
		} else {
			lines = append(lines, fmt.Sprintf("    ldr %s, [%s, #%d]", regs[k], base, 8*k))
		}
	}
	return append(lines, fmt.Sprintf("    add x26, x26, #%d", 8*n)), true
}
