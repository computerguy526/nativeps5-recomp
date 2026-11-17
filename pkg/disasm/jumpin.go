package disasm

import (
	"golang.org/x/arch/x86/x86asm"
)

// Which functions are entered only by a call (-redzone-leaf, docs/m3/lift2.md).
//
// The stack privacy pass may treat the 128 bytes below the entry stack pointer as the function's
// own from its first instruction when the function is entered by a call: the call pushed the
// return address, so by the red zone rule nothing the caller left below its stack pointer is
// live. A fragment entered by a jump (a cold part, a shared tail) continues the frame of the
// code that jumped; if that code never lowered RSP it may still hold live data in the red zone
// and may have published its address, so the fragment gets no red zone.
//
// Discovery records every direct JMP/Jcc that leaves its function from a point where RSP may not
// have been lowered yet (no PUSH, CALL, SUB RSP on some path to it). A function that is the
// target of such a branch is "jump entered". Branches from code that has lowered RSP (an
// epilogue's tail call, a cold jump from a function with a frame) do not count: by the same
// red zone rule nothing below that RSP is live. The set is complete only after a discovery walk;
// an analysis that runs without one (a lone fragment build) treats every function as jump
// entered.

type jumpInfo struct {
	entered  map[uint64]struct{}
	complete bool
}

// callEntered reports whether the function at entry may use the red zone from its first
// instruction.
func (d *Disassembler) callEntered(entry uint64) bool {
	if !d.LeafRedZone || d.jumpIn == nil || !d.jumpIn.complete {
		return false
	}
	_, jumped := d.jumpIn.entered[entry]
	return !jumped
}

// CallEntered is callEntered for tools.
func (d *Disassembler) CallEntered(entry uint64) bool { return d.callEntered(entry) }

// JumpEnteredCount is the number of functions recorded as jump entered (statistics).
func (d *Disassembler) JumpEnteredCount() int {
	if d.jumpIn == nil {
		return 0
	}
	return len(d.jumpIn.entered)
}

// noteJumpOuts records the targets of the direct branches of fn that leave it from a point
// where RSP may be unlowered.
func (d *Disassembler) noteJumpOuts(fn *Function) {
	if d.jumpIn == nil {
		d.jumpIn = &jumpInfo{entered: map[uint64]struct{}{}}
	}
	in := make(map[uint64]bool, len(fn.Blocks)) // unlowered on some path into the block
	seen := make(map[uint64]bool, len(fn.Blocks))
	if fn.Blocks[fn.EntryAddr] == nil {
		return
	}
	in[fn.EntryAddr], seen[fn.EntryAddr] = true, true
	work := []uint64{fn.EntryAddr}
	for len(work) > 0 {
		a := work[len(work)-1]
		work = work[:len(work)-1]
		b := fn.Blocks[a]
		unlowered := in[a]
		for k := range b.Insts {
			inst := b.Insts[k].Inst
			next := b.Insts[k].Address + uint64(inst.Len)
			if unlowered && (inst.Op == x86asm.JMP || isJcc(inst.Op)) {
				if rel, ok := inst.Args[0].(x86asm.Rel); ok {
					if t := uint64(int64(next) + int64(rel)); fn.Blocks[t] == nil {
						d.jumpIn.entered[t] = struct{}{}
					}
				}
			}
			if unlowered && lowersRSP(inst) {
				unlowered = false
			}
		}
		succs, _ := blockSuccessors(fn, b, nil)
		for _, s := range succs {
			if fn.Blocks[s] == nil {
				continue
			}
			if !seen[s] || (unlowered && !in[s]) {
				seen[s] = true
				in[s] = in[s] || unlowered
				work = append(work, s)
			}
		}
	}
}

// lowersRSP: the instruction certainly moves RSP below its value at the function's entry, or
// calls (after which the caller's red zone is dead whatever RSP is).
func lowersRSP(inst x86asm.Inst) bool {
	switch inst.Op {
	case x86asm.PUSH, x86asm.PUSHF, x86asm.PUSHFD, x86asm.PUSHFQ, x86asm.CALL, x86asm.ENTER:
		return true
	case x86asm.SUB:
		r, ok := inst.Args[0].(x86asm.Reg)
		return ok && r == x86asm.RSP
	}
	return false
}
