package disasm

import "golang.org/x/arch/x86/x86asm"

// PF and AF.
//
// The lifter keeps PF and AF lazily, the way FEX-Emu does (FEXCore OpcodeDispatcher/Flags.cpp,
// CalculatePF / CalculateAF / LoadPFRaw / LoadAF; MIT, see NOTICE): an integer flag writer
// stores the low byte of its result (GuestContext.pf) and the XOR of its operands
// (GuestContext.af); a reader computes PF as the even parity of the first and AF as bit 4 of the
// XOR of the two. After an FP compare PF is in host V instead (the unordered result).
//
// Two passes make this free where nobody reads the flags:
//   - PF/AF liveness (bits flagPF / flagAF in analyzeCarryOverflowLiveness): a writer stores the
//     bytes only when a reader is reachable.
//   - PF location (analyzeParityLocation): a forward pass that tells each instruction whether
//     PF is in host V (every path comes from an FP compare through instructions whose
//     lowerings keep NZCV) or in the context bytes, and where a V-held PF has to be stored
//     because a successor or a V-clobbering lowering needs it there.

// ParityClass says what an instruction does to PF/AF and to the host V that may hold PF.
type ParityClass uint8

const (
	// ParityKeep: PF/AF unchanged and the lowering keeps NZCV (it may read the flags).
	ParityKeep ParityClass = iota
	// ParityFPCompare: (U)COMIS*, F(U)COMI(P): PF = unordered, in host V; AF = 0.
	ParityFPCompare
	// ParityArith: PF/AF of an integer result; the lifter records them from the operands.
	ParityArith
	// ParityZero: PF = AF = 0 (POPCNT, PTEST family, PCMPxSTRx).
	ParityZero
	// ParityShift: SHL/SHR/SAR/SHLD/SHRD: PF from the result when the masked count is not 0
	// (AF undefined), nothing changes when it is 0.
	ParityShift
	// ParityLoad: SAHF, POPF: PF/AF come from the loaded value.
	ParityLoad
	// ParityStrCmp: SCAS/CMPS: PF/AF of the last comparison (none with REP and RCX = 0).
	ParityStrCmp
	// ParityUndefined: PF/AF undefined afterwards (MUL, DIV, bit scans, BMI, BT) or dead (CALL).
	ParityUndefined
	// ParityOther: PF/AF unchanged, but the lowering may rewrite host V (rotates), or an
	// instruction this pass does not know (treated as a PF/AF reader).
	ParityOther
)

// ClassifyParity returns the ParityClass of inst.
func ClassifyParity(inst x86asm.Inst) ParityClass {
	switch inst.Op {
	case x86asm.COMISS, x86asm.UCOMISS, x86asm.COMISD, x86asm.UCOMISD,
		x86asm.VCOMISS, x86asm.VUCOMISS, x86asm.VCOMISD, x86asm.VUCOMISD,
		x86asm.FCOMI, x86asm.FCOMIP, x86asm.FUCOMI, x86asm.FUCOMIP:
		return ParityFPCompare
	case x86asm.ADD, x86asm.SUB, x86asm.CMP, x86asm.ADC, x86asm.SBB,
		x86asm.AND, x86asm.OR, x86asm.XOR, x86asm.TEST, x86asm.NEG, x86asm.INC, x86asm.DEC,
		x86asm.XADD, x86asm.CMPXCHG:
		return ParityArith
	case x86asm.POPCNT, x86asm.PTEST, x86asm.VPTEST, x86asm.VTESTPS, x86asm.VTESTPD,
		x86asm.PCMPISTRI, x86asm.PCMPESTRI, x86asm.PCMPISTRM, x86asm.PCMPESTRM,
		x86asm.VPCMPISTRI, x86asm.VPCMPESTRI, x86asm.VPCMPISTRM, x86asm.VPCMPESTRM:
		return ParityZero
	case x86asm.SHL, x86asm.SHR, x86asm.SAR, x86asm.SHLD, x86asm.SHRD:
		return ParityShift
	case x86asm.SAHF, x86asm.POPF, x86asm.POPFD, x86asm.POPFQ:
		return ParityLoad
	case x86asm.CMPSB, x86asm.CMPSW, x86asm.CMPSD, x86asm.CMPSQ,
		x86asm.SCASB, x86asm.SCASW, x86asm.SCASD, x86asm.SCASQ:
		return ParityStrCmp
	case x86asm.MUL, x86asm.IMUL, x86asm.DIV, x86asm.IDIV,
		x86asm.BSF, x86asm.BSR, x86asm.TZCNT, x86asm.LZCNT,
		x86asm.ANDN, x86asm.BEXTR, x86asm.BLSI, x86asm.BLSR, x86asm.BLSMSK, x86asm.BZHI,
		x86asm.BT, x86asm.BTS, x86asm.BTR, x86asm.BTC,
		x86asm.CALL:
		return ParityUndefined
	case x86asm.CLC, x86asm.STC, x86asm.CMC, x86asm.LAHF,
		x86asm.PUSHF, x86asm.PUSHFD, x86asm.PUSHFQ,
		x86asm.FCMOVB, x86asm.FCMOVNB, x86asm.FCMOVE, x86asm.FCMOVNE,
		x86asm.FCMOVBE, x86asm.FCMOVNBE, x86asm.FCMOVU, x86asm.FCMOVNU:
		return ParityKeep
	}
	if isJcc(inst.Op) || isSetcc(inst.Op) || isCmovcc(inst.Op) || flagNeutral(inst) {
		return ParityKeep
	}
	return ParityOther
}

// parityAdjustEffect returns the PF/AF bits an instruction reads and the bits it defines (or
// leaves undefined). An unknown instruction reads both and defines nothing.
func parityAdjustEffect(inst x86asm.Inst) (reads, kills uint8) {
	both := flagPF | flagAF
	switch inst.Op {
	case x86asm.JP, x86asm.JNP, x86asm.SETP, x86asm.SETNP, x86asm.CMOVP, x86asm.CMOVNP,
		x86asm.FCMOVU, x86asm.FCMOVNU:
		return flagPF, 0
	case x86asm.LAHF, x86asm.PUSHF, x86asm.PUSHFD, x86asm.PUSHFQ:
		return both, 0
	}
	switch ClassifyParity(inst) {
	case ParityFPCompare, ParityArith, ParityZero, ParityLoad, ParityUndefined:
		return 0, both
	case ParityShift:
		if c, ok := shiftImmCount(inst); ok && c != 0 {
			return 0, both
		}
	case ParityStrCmp:
		if !hasRepPrefix(inst) {
			return 0, both
		}
	case ParityOther:
		switch inst.Op {
		case x86asm.ROL, x86asm.ROR, x86asm.RCL, x86asm.RCR:
			return 0, 0
		}
		return both, 0
	}
	return 0, 0
}

// parityInVAfter is the transfer function of the PF-location pass: whether PF is in host V
// after inst, given whether it was before.
func parityInVAfter(inst x86asm.Inst, before bool) bool {
	switch ClassifyParity(inst) {
	case ParityFPCompare:
		return true
	case ParityKeep:
		return before
	}
	return false
}

// analyzeParityLocation sets ParityInV, ParitySpillPre and ParitySpillExit (see Instruction).
// It runs after the liveness pass (it reads ParityDead). A block starts with PF in host V only
// when every way into it ends that way: the entry block, blocks without a known predecessor and
// every block of a function with an indirect jump that has no recovered table (the lifter lets
// such a jump reach any block) start with PF in the context.
func analyzeParityLocation(fn *Function, isFunc func(uint64) bool) {
	type info struct {
		succs   []uint64
		unknown bool
		preds   int
		in, out bool
	}
	infos := make(map[uint64]*info, len(fn.Blocks))
	wild := false
	for addr, b := range fn.Blocks {
		s, u := blockSuccessors(fn, b, isFunc)
		infos[addr] = &info{succs: s, unknown: u, in: true}
		if endsInWildJump(fn, b) {
			wild = true
		}
	}
	for _, bi := range infos {
		for _, s := range bi.succs {
			if si := infos[s]; si != nil {
				si.preds++
			}
		}
	}
	for addr, bi := range infos {
		if wild || bi.preds == 0 || addr == fn.EntryAddr {
			bi.in = false
		}
	}
	flow := func(b *BasicBlock, st bool) bool {
		for k := range b.Insts {
			st = parityInVAfter(b.Insts[k].Inst, st)
		}
		return st
	}
	for addr, bi := range infos {
		bi.out = flow(fn.Blocks[addr], bi.in)
	}
	// Greatest fixpoint: in[b] = AND of out[p] over the predecessors p.
	for changed := true; changed; {
		changed = false
		for _, addr := range fn.BlockOrder {
			bi := infos[addr]
			if bi == nil || bi.out {
				// Only a block that ends with PF in the context can lower a successor.
				continue
			}
			for _, s := range bi.succs {
				if si := infos[s]; si != nil && si.in {
					si.in = false
					if out := flow(fn.Blocks[s], false); out != si.out {
						si.out = out
					}
					changed = true
				}
			}
		}
	}
	for addr, b := range fn.Blocks {
		bi := infos[addr]
		st := bi.in
		for k := range b.Insts {
			in := &b.Insts[k]
			in.ParityInV = st
			after := parityInVAfter(in.Inst, st)
			in.ParitySpillPre = false
			if st && !after {
				// PF survives the instruction (or the instruction is unknown and may read it)
				// while the lowering may rewrite V.
				reads, kills := parityAdjustEffect(in.Inst)
				if kills&flagPF == 0 && (!in.ParityDead || reads&flagPF != 0) {
					in.ParitySpillPre = true
				}
			}
			in.ParitySpillExit = false
			st = after
		}
		if n := len(b.Insts); n > 0 && st && !b.Insts[n-1].ParityDead {
			need := bi.unknown
			for _, s := range bi.succs {
				if si := infos[s]; si == nil || !si.in {
					need = true
				}
			}
			b.Insts[n-1].ParitySpillExit = need
		}
	}
}

// AnalyzeFlags runs the flag passes on a function built outside the disassembler (tests, tools):
// the per-block SkipFlags pass, CF/OF/PF/AF liveness and the PF-location pass. isFunc reports
// function entries among branch targets outside fn (nil: none).
func AnalyzeFlags(fn *Function, isFunc func(uint64) bool) {
	(&Disassembler{}).analyzeBlockFlagLiveness(fn)
	analyzeCarryOverflowLiveness(fn, isFunc)
}
