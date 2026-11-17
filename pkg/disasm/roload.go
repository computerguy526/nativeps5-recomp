package disasm

import (
	"sort"

	"golang.org/x/arch/x86/x86asm"

	"ps4-recomp/pkg/elfloader"
)

// Plain loads from read-only image memory (-ro-loads, docs/m3/lift2.md).
//
// An acquire load is needed only where another thread may write the location while this thread
// is running. Two kinds of constant address cannot be written then:
//
//   - RELRO and read-only PT_LOAD memory: the loader writes it (relocations, GOT) before it
//     starts the first guest thread and the program never does (it is mapped read-only on the
//     console); a module the program loads later is relocated before any code of it runs, and
//     the load that publishes its handle is an HLE call, which synchronises through the kernel.
//   - the stack guard value: the loader writes it into the image (elfloader.applyStackCanary,
//     a fixed value) and no guest code stores to it. The address reaches the code through the
//     __stack_chk_guard GOT slot, a RELRO load.
//
// The pass is block local: a register becomes a "guard pointer" by `mov r64, [rip+slot]` where
// the image holds the canary address in a read-only slot, and stays one until the next
// instruction that may write it.

// ROInfo is what AnalyzeROLoads needs of the loaded image.
type ROInfo struct {
	Ranges []elfloader.AddrRange // sorted, merged (elfloader.LoadedELF.ROAddrRanges)
	Canary uint64                // guest address of the stack guard value, or 0
	U64    func(addr uint64) (uint64, bool)
}

func (d *Disassembler) roInfo() *ROInfo {
	if d == nil || d.elf == nil {
		return nil
	}
	ri := &ROInfo{Ranges: d.elf.ROAddrRanges, Canary: d.elf.CanaryAddr}
	if d.elf.MemoryImage != nil {
		ri.U64 = d.elf.MemoryImage.U64
	}
	return ri
}

func (ri *ROInfo) inRO(addr uint64, size int64) bool {
	if size <= 0 {
		return false
	}
	end := addr + uint64(size)
	if end < addr {
		return false
	}
	i := sort.Search(len(ri.Ranges), func(i int) bool { return ri.Ranges[i].End > addr })
	return i < len(ri.Ranges) && ri.Ranges[i].Start <= addr && end <= ri.Ranges[i].End
}

// roMemIsSource reports whether the memory operand at index ai is only read by the instruction.
func roMemIsSource(inst x86asm.Inst, ai int) bool {
	switch inst.Op {
	case x86asm.LEA, x86asm.XCHG, x86asm.XADD, x86asm.CMPXCHG, x86asm.CMPXCHG8B, x86asm.CMPXCHG16B,
		x86asm.NOP, x86asm.CALL, x86asm.JMP, x86asm.LJMP, x86asm.LCALL, x86asm.PUSH,
		x86asm.PREFETCHNTA, x86asm.PREFETCHT0, x86asm.PREFETCHT1, x86asm.PREFETCHT2, x86asm.PREFETCHW,
		x86asm.CLFLUSH, x86asm.MOVNTI:
		return false
	case x86asm.CMP, x86asm.TEST, x86asm.BT:
		return true
	}
	return ai >= 1
}

// AnalyzeROLoads sets Instruction.ROLoad in every block of fn. The guard-pointer registers are a
// forward dataflow over the blocks (meet: intersection); a call clears the caller-saved registers
// and keeps the callee-saved ones, which the called function restores.
func AnalyzeROLoads(fn *Function, ri *ROInfo) {
	if ri == nil || len(ri.Ranges) == 0 && ri.Canary == 0 {
		return
	}
	const calleeSaved = 1<<rBX | 1<<rBP | 1<<rR12 | 1<<rR13 | 1<<rR14 | 1<<rR15
	in := make(map[uint64]uint16, len(fn.Blocks)) // guard registers on entry to the block
	seen := make(map[uint64]bool, len(fn.Blocks))
	succs := make(map[uint64][]uint64, len(fn.Blocks))
	wild := false
	for a, b := range fn.Blocks {
		succs[a], _ = blockSuccessors(fn, b, nil)
		if endsInWildJump(fn, b) {
			wild = true
		}
	}
	// step runs a block from the state g, optionally marking loads, and returns the state at its end.
	step := func(b *BasicBlock, g uint16, mark bool) uint16 {
		for k := range b.Insts {
			ins := &b.Insts[k]
			inst := ins.Inst
			next := ins.Address + uint64(inst.Len)
			mem, ai, ok := memArg(inst)
			isRO := false
			if ok && mem.Segment == 0 && !isStringOp(inst.Op) && roMemIsSource(inst, ai) && inst.MemBytes > 0 && !hasVecArg(inst) {
				switch {
				case mem.Base == x86asm.RIP && mem.Index == 0:
					a := next + uint64(int64(mem.Disp))
					if ri.inRO(a, int64(inst.MemBytes)) {
						isRO = true
					}
				case g != 0 && mem.Index == 0 && mem.Base >= x86asm.RAX && mem.Base <= x86asm.R15:
					if idx, _, ok := gprIndex(mem.Base); ok && g&(1<<uint(idx)) != 0 {
						a := ri.Canary + uint64(int64(mem.Disp))
						if a >= ri.Canary && a+uint64(inst.MemBytes) <= ri.Canary+8 {
							isRO = true
						}
					}
				}
			}
			if isRO && mark {
				ins.ROLoad = true
			}
			if g != 0 {
				if inst.Op == x86asm.CALL {
					g &= calleeSavedMask
				} else {
					g &^= RegWrites(inst)
				}
			}
			// mov r64, [rip+slot] where the read-only slot holds the guard's address.
			if isRO && ri.Canary != 0 && ri.U64 != nil && inst.Op == x86asm.MOV && mem.Base == x86asm.RIP && inst.MemBytes == 8 {
				if r, ok := inst.Args[0].(x86asm.Reg); ok && r >= x86asm.RAX && r <= x86asm.R15 {
					a := next + uint64(int64(mem.Disp))
					if v, ok := ri.U64(a); ok && v == ri.Canary {
						if idx, _, ok := gprIndex(r); ok {
							g |= 1 << uint(idx)
						}
					}
				}
			}
		}
		return g
	}
	if ri.Canary != 0 && !wild {
		// Fixpoint of the entry states: the entry block and blocks nobody reaches start with none.
		work := []uint64{}
		for a := range fn.Blocks {
			in[a] = 0
		}
		reached := map[uint64]bool{fn.EntryAddr: true}
		order := []uint64{fn.EntryAddr}
		for h := 0; h < len(order); h++ {
			for _, s := range succs[order[h]] {
				if fn.Blocks[s] != nil && !reached[s] {
					reached[s] = true
					order = append(order, s)
				}
			}
		}
		// Greatest fixpoint: start every reached block (but the entry) at "all", then intersect.
		for a := range reached {
			if a != fn.EntryAddr {
				in[a] = 0xffff
			}
		}
		work = append(work, fn.EntryAddr)
		for len(work) > 0 {
			a := work[len(work)-1]
			work = work[:len(work)-1]
			out := step(fn.Blocks[a], in[a], false)
			for _, s := range succs[a] {
				if fn.Blocks[s] == nil || !reached[s] {
					continue
				}
				if n := in[s] & out; n != in[s] || !seen[s] {
					in[s], seen[s] = n, true
					work = append(work, s)
				}
			}
		}
	}
	for a, b := range fn.Blocks {
		step(b, in[a], true)
	}
}

// RegWrites is the set of GPRs (bit i = register i) the instruction may write, conservatively:
// every instruction the table does not know writes all of them.
func RegWrites(inst x86asm.Inst) uint16 {
	const all = 0xffff
	op := inst.Op
	if readsOnly(op) {
		return 0
	}
	if known, _ := isRegALU(op); known || op == x86asm.MOV || op == x86asm.LEA || op == x86asm.POP {
		if r, ok := inst.Args[0].(x86asm.Reg); ok {
			if idx, _, ok := gprIndex(r); ok {
				if known, _ := isRegALU(op); known && op == x86asm.IMUL && inst.Args[1] == nil {
					return all
				}
				return 1 << uint(idx)
			}
			return 0 // a vector, segment or other register
		}
		return 0 // a store
	}
	if r, ok := inst.Args[0].(x86asm.Reg); ok && isVecOrFPReg(r) {
		return 0
	}
	if _, ok := inst.Args[0].(x86asm.Mem); ok {
		switch op {
		case x86asm.XCHG, x86asm.XADD, x86asm.CMPXCHG, x86asm.CMPXCHG8B, x86asm.CMPXCHG16B, x86asm.MUL, x86asm.IMUL, x86asm.DIV, x86asm.IDIV,
			x86asm.POP, x86asm.MOVSB, x86asm.MOVSW, x86asm.MOVSD, x86asm.MOVSQ, x86asm.STOSB, x86asm.STOSW, x86asm.STOSD, x86asm.STOSQ:
			return all
		}
		if !isStringOp(op) {
			return 0 // the instruction only writes its memory operand
		}
	}
	return all
}

// ROInfoOf is the ROInfo of a loaded image (tools).
func ROInfoOf(loaded *elfloader.LoadedELF) *ROInfo {
	return (&Disassembler{elf: loaded}).roInfo()
}
