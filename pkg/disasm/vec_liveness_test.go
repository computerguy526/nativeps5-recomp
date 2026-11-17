package disasm

import (
	"encoding/hex"
	"testing"
)

// vecFn builds a function at 0x1000 from hex-encoded instructions and runs the lane liveness
// pass. It returns the instructions in address order.
func vecFn(t *testing.T, mode VecLiveMode, code ...string) []*Instruction {
	t.Helper()
	var seq []Instruction
	pc := uint64(0x1000)
	for _, h := range code {
		b, err := hex.DecodeString(h)
		if err != nil {
			t.Fatalf("bad hex %q", h)
		}
		inst, err := Decode(b)
		if err != nil {
			t.Fatalf("decode %s: %v", h, err)
		}
		seq = append(seq, Instruction{Address: pc, Inst: inst})
		pc += uint64(inst.Len)
	}
	fn := BuildTestFunction(0x1000, seq)
	AnalyzeVecLiveness(fn, mode)
	if !fn.VecLive {
		t.Fatal("function not marked as analysed")
	}
	var out []*Instruction
	for _, a := range fn.BlockOrder {
		b := fn.Blocks[a]
		for i := range b.Insts {
			out = append(out, &b.Insts[i])
		}
	}
	return out
}

func dead(in *Instruction, reg int, lanes uint64) bool {
	m := VecLanes(reg, lanes)
	return in.VecDead&m == m
}

func live(in *Instruction, reg int, lanes uint64) bool {
	return in.VecDead&VecLanes(reg, lanes) == 0
}

const (
	hVmulsd8_1_2   = "c57359c2"     // vmulsd xmm8, xmm1, xmm2
	hVaddsd0_8_3   = "c5bb58c3"     // vaddsd xmm0, xmm8, xmm3
	hCallFar       = "e8fb3f0000"   // call +0x3ffb (outside the function)
	hCallNext      = "e800000000"   // call next (get-PC idiom)
	hCallRax       = "ffd0"         // call rax
	hRet           = "c3"           // ret
	hJmpFar        = "e9fb3f0000"   // jmp out of the function
	hJmpRax        = "ffe0"         // jmp rax
	hUd2           = "0f0b"         // ud2
	hFxsave        = "0fae07"       // fxsave [rdi]
	hVaddpsY1      = "c5ec58cb"     // vaddps ymm1, ymm2, ymm3
	hMovaps1_4     = "0f28cc"       // movaps xmm1, xmm4
	hVzeroupper    = "c5f877"       // vzeroupper
	hVzeroall      = "c5fc77"       // vzeroall
	hStoreY1       = "c5fc110f"     // vmovups [rdi], ymm1
	hVmulsd0_1_2   = "c5f359c2"     // vmulsd xmm0, xmm1, xmm2
	hStoreSd0      = "c5fb1107"     // vmovsd [rdi], xmm0
	hStoreX0       = "c5f91107"     // vmovupd [rdi], xmm0
	hVucomisd0_1   = "c5f92ec1"     // vucomisd xmm0, xmm1
	hVmovaps5_0    = "c5f828e8"     // vmovaps xmm5, xmm0
	hVxorps3       = "c5e057db"     // vxorps xmm3, xmm3, xmm3
	hAddss0_1      = "f30f58c1"     // addss xmm0, xmm1
	hMovss0_1      = "f30f10c1"     // movss xmm0, xmm1
	hBroadcastY2_0 = "c4e27d18d0"   // vbroadcastss ymm2, xmm0
	hLoadSs0       = "c5fa1007"     // vmovss xmm0, dword ptr [rdi]
	hVaddps9       = "c4413058ca"   // vaddps xmm9, xmm9, xmm10
	hFmaSd3_2_1    = "c4e2e9b9d9"   // vfmadd231sd xmm3, xmm2, xmm1
	hVaddsd0_0_1   = "c5fb58c1"     // vaddsd xmm0, xmm0, xmm1
	hSyscall       = "0f05"         // syscall
	hPcmpistri     = "c4e37963c100" // vpcmpistri xmm0, xmm1, 0
	hBlendvps2_3   = "660f3814d3"   // blendvps xmm2, xmm3 (implicit xmm0)
	hDecEcx        = "ffc9"         // dec ecx
	hVaddss0_0_mem = "c5fa5807"     // vaddss xmm0, xmm0, dword ptr [rdi]
)

// A call reads xmm0-7 in full and leaves nothing behind: what is computed before it into a
// register that is not an argument is dead, and so is the upper half of a scratch destination.
func TestVecLiveCallABI(t *testing.T) {
	in := vecFn(t, VecLiveABI, hVmulsd8_1_2, hVaddsd0_8_3, hCallFar, hRet)
	// after vmulsd xmm8: U[8] dead (xmm8 is not an argument); H[8] flows into xmm0, an argument.
	if !dead(in[0], 8, VecU) || !live(in[0], 8, VecLow) {
		t.Errorf("after vmulsd xmm8: dead=%#x", in[0].VecDead)
	}
	// after vaddsd xmm0: xmm8 dead in full, xmm0 live in full (it may be a __m256 argument),
	// xmm9-15 dead, xmm1-7 live.
	if !dead(in[1], 8, VecAll) || !live(in[1], 0, VecAll) || !dead(in[1], 15, VecAll) || !live(in[1], 7, VecAll) {
		t.Errorf("after vaddsd xmm0: dead=%#x", in[1].VecDead)
	}
	// after the call: only what the return reads.
	if !live(in[2], 0, VecAll) || !live(in[2], 1, VecLow) || !dead(in[2], 1, VecU) || !dead(in[2], 2, VecAll) {
		t.Errorf("after call: dead=%#x", in[2].VecDead)
	}
}

// Strict mode assumes no calling convention: nothing dies at a call or a return.
func TestVecLiveCallStrict(t *testing.T) {
	in := vecFn(t, VecLiveStrict, hVmulsd8_1_2, hVaddsd0_8_3, hCallFar, hRet)
	// Only the rewrite of xmm0 by the second instruction makes anything dead.
	for i, want := range []uint64{VecLanes(0, VecAll), 0, 0} {
		if in[i].VecDead != want {
			t.Errorf("instruction %d: dead=%#x, want %#x", i, in[i].VecDead, want)
		}
	}
}

// Every other way out keeps everything live: a branch out of the function (it may continue in a
// split-off part of the same source function), an indirect jump, a trap, a call into the
// function itself (get-PC), an indirect call is a call.
func TestVecLiveExits(t *testing.T) {
	for _, c := range []struct {
		name string
		exit []string
		all  bool
	}{
		{"direct jump out", []string{hJmpFar}, true},
		{"indirect jump", []string{hJmpRax}, true},
		{"ud2", []string{hUd2}, true},
		{"call next", []string{hCallNext, hRet}, true},
		{"fxsave", []string{hFxsave, hRet}, true},
		{"indirect call", []string{hCallRax, hRet}, false},
		{"ret", []string{hRet}, false},
	} {
		in := vecFn(t, VecLiveABI, append([]string{hVmulsd8_1_2}, c.exit...)...)
		if c.all && in[0].VecDead != 0 {
			t.Errorf("%s: dead=%#x after the instruction before it, want nothing dead", c.name, in[0].VecDead)
		}
		if !c.all && !dead(in[0], 8, VecAll) {
			t.Errorf("%s: xmm8 should be dead before it (dead=%#x)", c.name, in[0].VecDead)
		}
	}
	// A raw SYSCALL preserves the vector registers: it is transparent.
	in := vecFn(t, VecLiveABI, hVmulsd8_1_2, hSyscall, hJmpRax)
	if in[0].VecDead != 0 {
		t.Errorf("syscall: dead=%#x", in[0].VecDead)
	}
}

// YMM upper halves: a VEX write kills, VZEROUPPER kills, a legacy SSE write does not.
func TestVecLiveUpperHalves(t *testing.T) {
	in := vecFn(t, VecLiveABI, hVaddpsY1, hMovaps1_4, hStoreY1, hRet)
	if !live(in[0], 1, VecU) || !dead(in[0], 1, VecLow) {
		t.Errorf("vaddps ymm1 then movaps xmm1: dead=%#x (upper must stay live, low is rewritten)", in[0].VecDead)
	}
	if !live(in[1], 1, VecAll) {
		t.Errorf("after movaps xmm1: dead=%#x", in[1].VecDead)
	}
	in = vecFn(t, VecLiveABI, hVaddpsY1, hVzeroupper, hStoreY1, hRet)
	if !dead(in[0], 1, VecU) || !live(in[0], 1, VecLow) {
		t.Errorf("vaddps ymm1 then vzeroupper: dead=%#x", in[0].VecDead)
	}
	// vzeroupper itself: the upper half it zeroes for ymm1 is read by the store, the others are not
	// (ymm0's is: the return).
	if !live(in[1], 1, VecU) || !live(in[1], 0, VecU) || !dead(in[1], 5, VecU) {
		t.Errorf("after vzeroupper: dead=%#x", in[1].VecDead)
	}
	in = vecFn(t, VecLiveABI, hVaddpsY1, hVzeroall, hRet)
	if in[0].VecDead != ^uint64(0) {
		t.Errorf("before vzeroall: dead=%#x, want everything", in[0].VecDead)
	}
	// A VEX.128 write zeroes the upper half: what was there before is dead.
	in = vecFn(t, VecLiveABI, hVaddpsY1, "c5f058cb" /* vaddps xmm1, xmm1, xmm3 */, hStoreY1, hRet)
	if !dead(in[0], 1, VecU) || !live(in[1], 1, VecAll) {
		t.Errorf("VEX.128 rewrite: dead=%#x / %#x", in[0].VecDead, in[1].VecDead)
	}
}

// Scalar instructions pass the lanes above the scalar through from their first source.
func TestVecLiveScalarChain(t *testing.T) {
	in := vecFn(t, VecLiveABI, hVmulsd0_1_2, hStoreSd0, hVzeroall, hRet)
	if !dead(in[0], 0, VecH|VecU) || !live(in[0], 0, VecSD) {
		t.Errorf("scalar store: dead=%#x", in[0].VecDead)
	}
	in = vecFn(t, VecLiveABI, hLoadSs0, hVmulsd0_1_2, hStoreX0, hVzeroall, hRet)
	if !live(in[1], 0, VecLow) || !dead(in[1], 0, VecU) {
		t.Errorf("full store: dead=%#x", in[1].VecDead)
	}
	// before vmulsd (= after the load): xmm1's H is what ends up in xmm0's H; xmm2 is read as a scalar.
	if !live(in[0], 1, VecLow) || !live(in[0], 2, VecSD) || !dead(in[0], 2, VecH) || !dead(in[0], 0, VecAll) {
		t.Errorf("sources: dead=%#x", in[0].VecDead)
	}
	// Scalar FMA merges into its own destination.
	in = vecFn(t, VecLiveABI, hVxorps3, hFmaSd3_2_1, hStoreSd0, "c5f828c3" /* vmovaps xmm0, xmm3 */, hStoreX0, hVzeroall)
	if !live(in[0], 3, VecLow) || !dead(in[0], 3, VecU) {
		t.Errorf("fma destination: dead=%#x", in[0].VecDead)
	}
	// Legacy scalar: the destination's other lanes stay as live as they are after.
	in = vecFn(t, VecLiveABI, hLoadSs0, hAddss0_1, hStoreSd0, hVzeroall)
	if !live(in[0], 0, VecSD) || !dead(in[0], 0, VecH) || !live(in[0], 1, VecSS) || !dead(in[0], 1, VecL1|VecH) {
		t.Errorf("addss: dead=%#x", in[0].VecDead)
	}
	// Broadcast reads the low element only; a compare reads the scalars.
	in = vecFn(t, VecLiveABI, hVmulsd0_1_2, hBroadcastY2_0, hVucomisd0_1, hVzeroall)
	if !live(in[0], 0, VecSD) || !dead(in[0], 0, VecH) {
		t.Errorf("broadcast source: dead=%#x", in[0].VecDead)
	}
}

// Liveness joins over branches and loops.
func TestVecLiveControlFlow(t *testing.T) {
	// vmulsd xmm0; vucomisd; jbe +4 (over the full store); vmovupd [rdi], xmm0; vmovsd [rdi], xmm0; vzeroall
	in := vecFn(t, VecLiveABI, hVmulsd0_1_2, hVucomisd0_1, "7604", hStoreX0, hStoreSd0, hVzeroall)
	if !live(in[0], 0, VecLow) {
		t.Errorf("one successor reads the full register: dead=%#x", in[0].VecDead)
	}
	// loop: L: vaddsd xmm0, xmm0, xmm1; dec ecx; jne L; vmovsd [rdi], xmm0; vzeroall
	in = vecFn(t, VecLiveABI, hVaddsd0_0_1, hDecEcx, "75f8", hStoreSd0, hVzeroall)
	if !live(in[0], 0, VecSD) || !dead(in[0], 0, VecH|VecU) || !live(in[2], 1, VecSD) {
		t.Errorf("loop: dead=%#x / %#x", in[0].VecDead, in[2].VecDead)
	}
}

// Instructions without an exact rule read everything they name and kill nothing; implicit
// operands are known.
func TestVecLiveConservativeDefault(t *testing.T) {
	in := vecFn(t, VecLiveABI, hVmulsd0_1_2, hPcmpistri, hVzeroall)
	if !live(in[0], 0, VecLow) || !live(in[0], 1, VecLow) {
		t.Errorf("pcmpistri sources: dead=%#x", in[0].VecDead)
	}
	in = vecFn(t, VecLiveABI, hVmulsd0_1_2, hBlendvps2_3, hVzeroall)
	if !live(in[0], 0, VecLow) {
		t.Errorf("blendvps reads xmm0 implicitly: dead=%#x", in[0].VecDead)
	}
}

func TestVecLiveOff(t *testing.T) {
	var seq []Instruction
	b, _ := hex.DecodeString(hVmulsd0_1_2)
	inst, _ := Decode(b)
	seq = append(seq, Instruction{Address: 0x1000, Inst: inst, VecDead: 5})
	fn := BuildTestFunction(0x1000, seq)
	AnalyzeVecLiveness(fn, VecLiveOff)
	if fn.VecLive || fn.Blocks[0x1000].Insts[0].VecDead != 0 {
		t.Error("off mode must clear the marks")
	}
	if m, ok := ParseVecLiveMode("strict"); !ok || m != VecLiveStrict {
		t.Error("parse strict")
	}
	if _, ok := ParseVecLiveMode("bogus"); ok {
		t.Error("parse bogus")
	}
}

// A recovered jump table is not the complete successor set: an index the table does not cover goes to
// _recomp_indirect_jump (and can enter tier-1 code) with every vector register live. The only listed target here is a
// RET, where xmm8 is dead; the jmp must still keep it live.
func TestVecLiveJumpTableHasUnknownSuccessor(t *testing.T) {
	var seq []Instruction
	pc := uint64(0x1000)
	for _, h := range []string{hVmulsd8_1_2, hJmpRax, hRet} {
		b, _ := hex.DecodeString(h)
		inst, err := Decode(b)
		if err != nil {
			t.Fatal(err)
		}
		seq = append(seq, Instruction{Address: pc, Inst: inst})
		pc += uint64(inst.Len)
	}
	fn := BuildTestFunction(0x1000, seq)
	fn.JumpTables[seq[1].Address] = []uint64{seq[2].Address} // the table lists only the ret block
	AnalyzeVecLiveness(fn, VecLiveABI)
	first := &fn.Blocks[fn.BlockOrder[0]].Insts[0]
	if first.VecDead != 0 {
		t.Fatalf("dead=%#x before a jump-table jmp, want everything live", first.VecDead)
	}
	// without the table the result is the same (already conservative), and a direct jmp to a local block keeps its precision
	fn2 := BuildTestFunction(0x1000, seq)
	AnalyzeVecLiveness(fn2, VecLiveABI)
	if fn2.Blocks[fn2.BlockOrder[0]].Insts[0].VecDead != 0 {
		t.Fatal("no table: want everything live")
	}
}

// A gather reads a vector register through the VSIB index of its memory operand, which is not
// an operand of its own. PPSA30552 fn_0x331c760: "vpermd ymm8, ymm4, ymm2" followed by
// "vpgatherqd xmm15, [rcx+ymm10], xmm2", ymm10 not read again; the upper half of ymm10 looked
// dead after the vpermd, whose lowering then used its host register as scratch.
func TestVecLiveGatherIndex(t *testing.T) {
	const (
		vpermd8_4_2 = "c4625d36c2"     // vpermd ymm8, ymm4, ymm2
		gatherQdY10 = "c4226d913c11"   // vpgatherqd xmm15, [rcx+ymm10*1], xmm2
		movapsY2_0  = "c5fc28d0"       // vmovaps ymm2, ymm0
		gatherQqX2  = "c4e2e1914c5600" // vpgatherqq xmm1, [rsi+xmm2*2+0], xmm3
		gatherDdY2  = "c4e265904c9600" // vpgatherdd ymm1, [rsi+ymm2*4+0], ymm3
	)
	in := vecFn(t, VecLiveABI, vpermd8_4_2, gatherQdY10, hVzeroall)
	if !live(in[0], 10, VecAll) {
		t.Errorf("ymm index of vpgatherqd: dead=%#x", in[0].VecDead)
	}
	if !live(in[0], 2, VecLow) || !live(in[0], 15, VecLow) {
		t.Errorf("mask and destination of vpgatherqd: dead=%#x", in[0].VecDead)
	}
	if !dead(in[1], 10, VecAll) {
		t.Errorf("index after the gather (vzeroall follows): dead=%#x", in[1].VecDead)
	}
	// An XMM index is read in its low 128 bits only.
	in = vecFn(t, VecLiveABI, movapsY2_0, gatherQqX2, hVzeroall)
	if !live(in[0], 2, VecLow) || !dead(in[0], 2, VecU) {
		t.Errorf("xmm index of vpgatherqq: dead=%#x", in[0].VecDead)
	}
	in = vecFn(t, VecLiveStrict, movapsY2_0, gatherDdY2, hVzeroall)
	if !live(in[0], 2, VecAll) || !live(in[0], 1, VecAll) || !live(in[0], 3, VecAll) {
		t.Errorf("vpgatherdd ymm: dead=%#x", in[0].VecDead)
	}
}

// Other registers an instruction reads without it being obvious from the operand order: the
// mask of the AVX masked loads and stores, the /is4 selector of the VEX blends, MASKMOVDQU.
func TestVecLiveMaskOperands(t *testing.T) {
	cases := []struct {
		name, hex string
		reads     map[int]uint64
	}{
		{"vpmaskmovd ymm1, ymm2, [rdi]", "c4e26d8c0f", map[int]uint64{2: VecAll}},
		{"vpmaskmovd [rdi], ymm2, ymm1", "c4e26d8e0f", map[int]uint64{1: VecAll, 2: VecAll}},
		{"vpmaskmovq xmm1, xmm2, [rdi]", "c4e2e98c0f", map[int]uint64{2: VecLow}},
		{"vmaskmovps ymm1, ymm2, [rdi]", "c4e26d2c0f", map[int]uint64{2: VecAll}},
		{"vmaskmovps [rdi], ymm2, ymm1", "c4e26d2e0f", map[int]uint64{1: VecAll, 2: VecAll}},
		{"vmaskmovpd [rdi], xmm2, xmm1", "c4e2692f0f", map[int]uint64{1: VecLow, 2: VecLow}},
		{"vblendvps xmm1, xmm2, xmm3, xmm4", "c4e3694acb40", map[int]uint64{2: VecLow, 3: VecLow, 4: VecLow}},
		{"vpblendvb ymm1, ymm2, ymm3, ymm4", "c4e36d4ccb40", map[int]uint64{2: VecAll, 3: VecAll, 4: VecAll}},
		{"maskmovdqu xmm1, xmm2", "660ff7ca", map[int]uint64{1: VecLow, 2: VecLow}},
		{"vmaskmovdqu xmm1, xmm2", "c5f9f7ca", map[int]uint64{1: VecLow, 2: VecLow}},
		{"pblendvb xmm1, xmm2 (xmm0)", "660f3810ca", map[int]uint64{0: VecLow, 1: VecLow, 2: VecLow}},
	}
	for _, c := range cases {
		in := vecFn(t, VecLiveABI, "90", c.hex, hVzeroall)
		for reg, lanes := range c.reads {
			if !live(in[0], reg, lanes) {
				t.Errorf("%s: xmm%d lanes %#x not live before it (dead=%#x)", c.name, reg, lanes, in[0].VecDead)
			}
		}
	}
}
