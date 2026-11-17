package disasm

import "testing"

// PF/AF liveness and the PF-location pass (parity.go).
func TestParityLivenessAndLocation(t *testing.T) {
	const (
		addRR    = "4801d8"   // add rax, rbx
		movRR    = "4889d8"   // mov rax, rbx
		ucomiss  = "0f2ec1"   // ucomiss xmm0, xmm1
		cvt      = "f20f5ac0" // cvtsd2ss xmm0, xmm0 (flag neutral)
		setp     = "0f9ac0"   // setp al
		lahf     = "9f"
		pushfq   = "9c"
		rol      = "48d1c0"   // rol rax, 1: keeps PF, its lowering may write V
		shlCl    = "48d3e2"   // shl rdx, cl: PF only when the count is not 0
		shlImm   = "48c1e203" // shl rdx, 3
		imul     = "480fafc3" // imul rax, rbx: PF undefined
		repeCmps = "f3a6"
		cmps     = "a6"
		jmpRax   = "ffe0"
		ret      = "c3"
		syscall  = "0f05" // not classified: reads PF/AF
	)
	type want struct {
		label          string
		idx            int
		pfDead, afDead bool
		inV            bool
		pre, exit      bool
	}
	for _, tc := range []struct {
		name   string
		blocks []testBlock
		want   []want
	}{
		{"no reader: PF and AF dead", []testBlock{
			{"entry", []string{addRR, movRR, ret}},
		}, []want{{label: "entry", idx: 0, pfDead: true, afDead: true}}},
		{"SETP reads PF, not AF", []testBlock{
			{"entry", []string{addRR, movRR, setp, ret}},
		}, []want{{label: "entry", idx: 0, pfDead: false, afDead: true}, {label: "entry", idx: 2, pfDead: true, afDead: true}}},
		{"LAHF and PUSHFQ read both", []testBlock{
			{"entry", []string{addRR, lahf, addRR, pushfq, ret}},
		}, []want{{label: "entry", idx: 0}, {label: "entry", idx: 2}}},
		{"a later writer kills", []testBlock{
			{"entry", []string{addRR, addRR, setp, ret}},
		}, []want{{label: "entry", idx: 0, pfDead: true, afDead: true}, {label: "entry", idx: 1, afDead: true}}},
		{"undefined PF (IMUL) kills", []testBlock{
			{"entry", []string{addRR, imul, setp, ret}},
		}, []want{{label: "entry", idx: 0, pfDead: true, afDead: true}}},
		{"variable shift and rotate pass PF through; immediate shift kills", []testBlock{
			{"entry", []string{addRR, shlCl, rol, setp, addRR, shlImm, setp, ret}},
		}, []want{{label: "entry", idx: 0, afDead: true}, {label: "entry", idx: 4, pfDead: true, afDead: true}}},
		{"REPE CMPSB passes PF through, CMPSB kills", []testBlock{
			{"entry", []string{addRR, repeCmps, setp, addRR, cmps, setp, ret}},
		}, []want{{label: "entry", idx: 0, afDead: true}, {label: "entry", idx: 3, pfDead: true, afDead: true}}},
		{"unknown instruction reads", []testBlock{
			{"entry", []string{addRR, syscall, ret}},
		}, []want{{label: "entry", idx: 0}}},
		{"reader in a successor", []testBlock{
			{"entry", []string{addRR, "jcc:JE:other"}},
			{"fall", []string{"jcc:JP:other"}},
			{"other", []string{ret}},
		}, []want{{label: "entry", idx: 0, afDead: true}, {label: "fall", idx: 0, pfDead: true, afDead: true}}},
		// An indirect jump without a table reaches the blocks of the function or a function
		// entry: PF/AF are live across it only if a block reads them.
		{"indirect jump: no reader in the function", []testBlock{
			{"entry", []string{addRR, jmpRax}},
			{"other", []string{ret}},
		}, []want{{label: "entry", idx: 0, pfDead: true, afDead: true}}},
		{"indirect jump: a block reads PF", []testBlock{
			{"entry", []string{addRR, jmpRax}},
			{"other", []string{setp, ret}},
		}, []want{{label: "entry", idx: 0, afDead: true}}},
		{"jump to an unknown address keeps PF/AF live", []testBlock{
			{"entry", []string{addRR, "jmp:nofn"}},
		}, []want{{label: "entry", idx: 0}}},
		{"tail call: dead", []testBlock{
			{"entry", []string{addRR, "jmp:out"}},
		}, []want{{label: "entry", idx: 0, pfDead: true, afDead: true}}},

		// clang's x == y (the Syobon JP sites of the mapping table's appendix): the JP is in its
		// own block, entered only from the JNE; PF stays in V and nothing is stored.
		{"ucomiss; jne; jp: PF in V across the block boundary", []testBlock{
			{"entry", []string{ucomiss, cvt, "jcc:JNE:out2"}},
			{"jp", []string{"jcc:JP:out2"}},
			{"eq", []string{movRR}},
			{"out2", []string{ret}},
		}, []want{
			{label: "entry", idx: 0, afDead: true},
			{label: "entry", idx: 2, afDead: true, inV: true},
			{label: "jp", idx: 0, pfDead: true, afDead: true, inV: true},
		}},
		{"join of an FP and an integer path: stored on the FP side", []testBlock{
			{"entry", []string{"jcc:JE:int"}},
			{"fp", []string{ucomiss, "jmp:join"}},
			{"int", []string{addRR}},
			{"join", []string{setp, ret}},
		}, []want{
			{label: "fp", idx: 1, afDead: true, inV: true, exit: true},
			{label: "int", idx: 0, afDead: true},
			{label: "join", idx: 0, pfDead: true, afDead: true},
		}},
		{"rotate between the compare and the reader: stored before it", []testBlock{
			{"entry", []string{ucomiss, rol, setp, ret}},
		}, []want{{label: "entry", idx: 1, afDead: true, inV: true, pre: true}, {label: "entry", idx: 2, pfDead: true, afDead: true}}},
		{"rotate with PF dead: nothing stored", []testBlock{
			{"entry", []string{ucomiss, rol, ret}},
		}, []want{{label: "entry", idx: 1, pfDead: true, afDead: true, inV: true}}},
		{"integer writer after the compare: PF back in the context", []testBlock{
			{"entry", []string{ucomiss, addRR, setp, ret}},
		}, []want{{label: "entry", idx: 1, afDead: true, inV: true}, {label: "entry", idx: 2, pfDead: true, afDead: true}}},
		{"function with an indirect jump: every block starts with PF in the context", []testBlock{
			{"entry", []string{ucomiss, "jcc:JNE:sw"}},
			{"jp", []string{"jcc:JP:sw"}},
			{"sw", []string{jmpRax}},
		}, []want{
			{label: "entry", idx: 1, afDead: true, inV: true, exit: true},
			{label: "jp", idx: 0, afDead: true},
		}},
		{"PUSHFQ after a compare: AF live at the compare", []testBlock{
			{"entry", []string{ucomiss, pushfq, ret}},
		}, []want{{label: "entry", idx: 0}, {label: "entry", idx: 1, pfDead: true, afDead: true, inV: true}}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			fn, addrs := buildFunction(t, tc.blocks, nil)
			analyzeCarryOverflowLiveness(fn, testIsFunc)
			for _, w := range tc.want {
				in := fn.Blocks[addrs[w.label]].Insts[w.idx]
				got := want{w.label, w.idx, in.ParityDead, in.AdjustDead, in.ParityInV, in.ParitySpillPre, in.ParitySpillExit}
				if got != w {
					t.Errorf("%s[%d] %v: got %+v, want %+v", w.label, w.idx, in.Inst, got, w)
				}
			}
		})
	}
}
