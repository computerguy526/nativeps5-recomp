package disasm

import (
	"strings"

	"golang.org/x/arch/x86/x86asm"
)

// FP compare fusion (docs/m3/liftmisc.md, item 1).
//
// After UCOMISS/UCOMISD/COMISS/COMISD the lifter normally converts the ARM FCMP result into the x86
// flag model (four instructions: cset/axflag/rmif plus the PF store). When every reader of those
// flags is a Jcc/SETcc/CMOVcc in the same block, and the flags are dead afterwards, the raw FCMP
// NZCV can be consumed directly with the ARM condition that has the same truth table, NaN included.
// This pass decides, per compare, whether that holds: Instruction.FpFlagsLocal on the compare and
// Instruction.FpRaw on its consumers. The lifter ignores both unless the feature is enabled.

type fpFlagKind uint8

const (
	fpNeutral fpFlagKind = iota // reads and writes no flag, and the lowering keeps NZCV
	fpKill                      // defines every flag (CF OF SF ZF PF AF)
	fpRead                      // reads a flag, or may (conservative)
)

// fpClassifyIn is fpClassify with the liveness facts of the instruction: INC/DEC preserve CF, so
// they end a window only when CF is dead after them (they define every other flag).
func fpClassifyIn(in *Instruction) fpFlagKind {
	if (in.Inst.Op == x86asm.INC || in.Inst.Op == x86asm.DEC) && in.CarryDead {
		return fpKill
	}
	return fpClassify(in.Inst)
}

func fpClassify(in x86asm.Inst) fpFlagKind {
	switch in.Op {
	case x86asm.ADD, x86asm.SUB, x86asm.CMP, x86asm.TEST, x86asm.AND, x86asm.OR, x86asm.XOR, x86asm.NEG,
		x86asm.COMISS, x86asm.UCOMISS, x86asm.COMISD, x86asm.UCOMISD,
		x86asm.VCOMISS, x86asm.VUCOMISS, x86asm.VCOMISD, x86asm.VUCOMISD,
		x86asm.FCOMI, x86asm.FCOMIP, x86asm.FUCOMI, x86asm.FUCOMIP,
		x86asm.POPF, x86asm.POPFD, x86asm.POPFQ, x86asm.CALL, x86asm.PTEST, x86asm.VPTEST:
		return fpKill
	case x86asm.SHL, x86asm.SHR, x86asm.SAR:
		if c, ok := shiftImmCount(in); ok && c != 0 {
			return fpKill
		}
		return fpRead
	case x86asm.CMPXCHG8B, x86asm.CMPXCHG16B:
		return fpRead // writes ZF only
	}
	if flagNeutral(in) {
		return fpNeutral
	}
	return fpRead
}

// fpWindowNeutral: instructions allowed between a compare and its consumers. A subset of
// flagNeutral: plain moves and address arithmetic, whose lowerings are known to leave NZCV alone.
func fpWindowNeutral(in x86asm.Inst) bool {
	switch in.Op {
	case x86asm.MOV, x86asm.MOVZX, x86asm.MOVSX, x86asm.MOVSXD, x86asm.LEA, x86asm.NOP,
		x86asm.MOVSS, x86asm.MOVAPS, x86asm.MOVUPS, x86asm.MOVAPD, x86asm.MOVUPD, x86asm.MOVD, x86asm.MOVQ,
		x86asm.MOVDQA, x86asm.MOVDQU, x86asm.XORPS, x86asm.PXOR:
		return true
	case x86asm.MOVSD:
		// SSE scalar move only (not the string instruction): an XMM operand is present.
		for _, a := range in.Args {
			if a == nil {
				break
			}
			if r, ok := a.(x86asm.Reg); ok && r >= x86asm.X0 && r <= x86asm.X15 {
				return true
			}
		}
	}
	return false
}

// fpConsumer reports whether in is a flag consumer the lifter can map from the raw FCMP flags:
// J/SET/CMOV with cc in {A, AE, B, BE, E, NE, P, NP}; CMOV only with a 32/64-bit register
// destination.
func fpConsumer(in x86asm.Inst) bool {
	switch in.Op {
	case x86asm.JA, x86asm.JAE, x86asm.JB, x86asm.JBE, x86asm.JE, x86asm.JNE, x86asm.JP, x86asm.JNP:
		_, rel := in.Args[0].(x86asm.Rel)
		return rel
	case x86asm.SETA, x86asm.SETAE, x86asm.SETB, x86asm.SETBE, x86asm.SETE, x86asm.SETNE, x86asm.SETP, x86asm.SETNP:
		return true
	case x86asm.CMOVA, x86asm.CMOVAE, x86asm.CMOVB, x86asm.CMOVBE, x86asm.CMOVP, x86asm.CMOVNP:
		// CMOVE/CMOVNE are left out: their raw form is a chain of two CSELs on the destination
		// (measured 30% slower than the converted flags on a cmovne;cmovp loop-carried select).
		r, ok := in.Args[0].(x86asm.Reg)
		return ok && ((r >= x86asm.EAX && r <= x86asm.EDI) || (r >= x86asm.R8L && r <= x86asm.R15L) || (r >= x86asm.RAX && r <= x86asm.R15))
	}
	return false
}

func fpIsParityCons(op x86asm.Op) bool {
	switch op {
	case x86asm.JP, x86asm.JNP, x86asm.SETP, x86asm.SETNP, x86asm.CMOVP, x86asm.CMOVNP:
		return true
	}
	return false
}

// analyzeFpCmpFusion sets FpFlagsLocal / FpRaw. It runs after the PF-location pass.
func analyzeFpCmpFusion(fn *Function, isFunc func(uint64) bool) {
	type info struct {
		succs   []uint64
		unknown bool
		in      bool // some flag may be read before being redefined, on entry
	}
	infos := make(map[uint64]*info, len(fn.Blocks))
	for addr, b := range fn.Blocks {
		s, u := blockSuccessors(fn, b, isFunc)
		infos[addr] = &info{succs: s, unknown: u || endsInWildJump(fn, b)}
	}
	for changed := true; changed; {
		changed = false
		for i := len(fn.BlockOrder) - 1; i >= 0; i-- {
			addr := fn.BlockOrder[i]
			b, bi := fn.Blocks[addr], infos[addr]
			if b == nil || bi == nil {
				continue
			}
			live := bi.unknown
			for _, s := range bi.succs {
				if si := infos[s]; si != nil && si.in {
					live = true
				}
			}
			for k := len(b.Insts) - 1; k >= 0; k-- {
				switch fpClassifyIn(&b.Insts[k]) {
				case fpKill:
					live = false
				case fpRead:
					live = true
				}
			}
			if live != bi.in {
				bi.in = live
				changed = true
			}
		}
	}
	for _, b := range fn.Blocks {
		for k := range b.Insts {
			b.Insts[k].FpFlagsLocal = false
			b.Insts[k].FpRaw = false
			b.Insts[k].FpRawIn = false
			b.Insts[k].FpRawLast = false
			b.Insts[k].FpWhy = 0
		}
	}
	// Predecessor counts (a block entered by a wild indirect jump has unknown predecessors).
	preds := map[uint64]int{}
	wild := false
	for addr, b := range fn.Blocks {
		for _, sa := range infos[addr].succs {
			preds[sa]++
		}
		if endsInWildJump(fn, b) {
			wild = true
		}
	}
	type mark struct {
		b     *BasicBlock
		idx   int
		raw   bool // a consumer
		rawIn bool // first instruction of a continuation block
	}
	for addr, b := range fn.Blocks {
		for i := range b.Insts {
			if !isSSEFpCompare(b.Insts[i].Inst.Op) {
				continue
			}
			var marks []mark
			curB, curAddr, idx := b, addr, i+1
			nCons := 0
			why := FpWhyFused
		chain:
			for hops := 0; ; hops++ {
				ok, ended := true, false
				for j := idx; j < len(curB.Insts) && ok && !ended; j++ {
					in := &curB.Insts[j]
					x := in.Inst
					switch {
					case fpConsumer(x):
						if fpIsParityCons(x.Op) && !in.ParityInV {
							ok = false
							break
						}
						marks = append(marks, mark{b: curB, idx: j, raw: true})
						nCons++
					case fpClassifyIn(in) == fpKill:
						ended = true
					case fpClassifyIn(in) == fpNeutral && !isJcc(x.Op):
						if in.ParitySpillPre {
							ok = false
						}
					default:
						ok = false
						why = FpWhyNoConsumer
					}
				}
				if !ok {
					if why == FpWhyFused {
						why = FpWhyOther
					}
					break chain
				}
				if ended {
					break chain
				}
				// End of the block with the flags still raw: every exit must be flag-dead, except
				// the fall-through of a conditional branch on them (a consumer) into a block that has
				// no other way in, where the window continues (the `jne L; jp L` shape).
				bi := infos[curAddr]
				last := curB.Insts[len(curB.Insts)-1]
				if bi.unknown && last.Inst.Op != x86asm.RET {
					why = FpWhyLiveOut
					break chain
				}
				var next uint64
				cont := false
				if isJcc(last.Inst.Op) && fpConsumer(last.Inst) && !wild && hops < 4 {
					next = last.Address + uint64(last.Inst.Len)
					if nb := fn.Blocks[next]; nb != nil && preds[next] == 1 && len(nb.Insts) > 0 && next != fn.EntryAddr {
						cont = true
					}
				}
				for _, sa := range bi.succs {
					if cont && sa == next {
						continue
					}
					if si := infos[sa]; si == nil || si.in {
						why = FpWhyLiveOut
						break chain
					}
				}
				if !cont {
					break chain
				}
				curB, curAddr, idx = fn.Blocks[next], next, 0
				marks = append(marks, mark{b: curB, idx: 0, rawIn: true})
			}
			if why != FpWhyFused || nCons == 0 {
				if why == FpWhyFused {
					why = FpWhyNoConsumer
				}
				b.Insts[i].FpWhy = why
				continue
			}
			b.Insts[i].FpFlagsLocal = true
			b.Insts[i].FpWhy = FpWhyFused
			lastCons := -1
			for k, m := range marks {
				if m.raw {
					lastCons = k
				}
			}
			for k, m := range marks {
				if m.raw {
					m.b.Insts[m.idx].FpRaw = true
					m.b.Insts[m.idx].FpRawLast = k == lastCons
				}
				if m.rawIn {
					m.b.Insts[m.idx].FpRawIn = true
				}
			}
		}
	}
}

// FpWhy values: why a compare was or was not fused (statistics, tests).
const (
	FpWhyNone       uint8 = iota
	FpWhyFused            // fused
	FpWhyNoConsumer       // the next flag-relevant instruction is not a mappable consumer (or none before a kill)
	FpWhyLiveOut          // flags may be live into a successor / unknown exit
	FpWhyOther            // an instruction in the window the lowering cannot be proven to leave NZCV alone
)

func isSSEFpCompare(op x86asm.Op) bool {
	n := op.String()
	return strings.HasSuffix(n, "COMISS") || strings.HasSuffix(n, "COMISD")
}

// FpKillsFlags reports whether in defines every arithmetic flag without reading any (the
// instructions that end a fusion window).
func FpKillsFlags(in Instruction) bool { return fpClassifyIn(&in) == fpKill }
