package lifter

import (
	"bytes"
	"encoding/hex"
	"fmt"
	"math/rand"
	"os"
	"strings"
	"testing"

	"ps4-recomp/pkg/disasm"

	"golang.org/x/arch/x86/x86asm"
)

// liftVec lifts the instructions (hex) as one function, split into blocks at branches, with the
// vector lane liveness pass in the given mode ("off": not run; the function then counts as a
// YMM function when ymm is set), and returns the lowering of each instruction.
func liftVec(t *testing.T, mode string, ymm bool, insts ...string) [][]string {
	t.Helper()
	l := NewArm64Lifter(nil)
	var seq []disasm.Instruction
	pc := uint64(0x1000)
	for _, h := range insts {
		b, err := hex.DecodeString(h)
		if err != nil {
			t.Fatalf("bad hex %q: %v", h, err)
		}
		inst, err := disasm.Decode(b)
		if err != nil {
			t.Fatalf("decode %s: %v", h, err)
		}
		seq = append(seq, disasm.Instruction{Address: pc, Inst: inst})
		pc += uint64(inst.Len)
	}
	fn := disasm.BuildTestFunction(0x1000, seq)
	fn.UsesYmm = ymm
	if mode != "off" {
		m, ok := disasm.ParseVecLiveMode(mode)
		if !ok {
			t.Fatalf("bad mode %q", mode)
		}
		disasm.AnalyzeVecLiveness(fn, m)
	}
	var out [][]string
	for _, addr := range fn.BlockOrder {
		l.ResetFlags()
		for _, in := range fn.Blocks[addr].Insts {
			lines, err := l.LiftInstructionToBuf(in, in.Address+uint64(in.Inst.Len), fn, nil)
			if err != nil {
				t.Fatalf("lift %v: %v", in.Inst, err)
			}
			out = append(out, lines)
		}
	}
	return out
}

// VECLIVE_DUMP="mode:hex hex ..." prints the lowering of a sequence (development aid).
func TestVecLiveDump(t *testing.T) {
	spec := os.Getenv("VECLIVE_DUMP")
	if spec == "" {
		t.Skip("VECLIVE_DUMP not set")
	}
	mode, rest, _ := strings.Cut(spec, ":")
	for _, ls := range liftVec(t, mode, os.Getenv("VECLIVE_YMM") != "", strings.Fields(rest)...) {
		for _, ln := range ls {
			t.Log(ln)
		}
	}
}

// vecSeqs reads testdata/veclive_seqs.txt (tools/gen_veclive_cases.py), or the file VECLIVE_SEQS names.
func vecSeqs(t *testing.T) [][]string {
	t.Helper()
	path := "testdata/veclive_seqs.txt"
	if p := os.Getenv("VECLIVE_SEQS"); p != "" { // a larger corpus from tools/gen_veclive_cases.py
		path = p
	}
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var seqs [][]string
	for _, ln := range strings.Split(string(b), "\n") {
		if ln = strings.TrimSpace(ln); ln != "" && ln[0] != '#' {
			seqs = append(seqs, strings.Fields(ln))
		}
	}
	return seqs
}

// TestVecLiveDifferential runs random SSE/AVX sequences lifted three ways on the host: without
// lane liveness in a function that keeps every YMM upper half (the reference: every lane of
// every register is architecturally exact after every instruction), and with the liveness pass
// in strict and in abi mode. General registers, flags and the memory window (which receives the
// registers each sequence stores at its end) must be identical. No x86 code is run.
func TestVecLiveDifferential(t *testing.T) {
	seqs := vecSeqs(t)
	r := rand.New(rand.NewSource(7))
	var base []execCase
	probe := NewArm64Lifter(nil)
	probe.SetAllowUnsupported(true)
	skipped := 0
	for i, code := range seqs {
		c := execCase{name: fmt.Sprintf("seq%d", i), code: code, ymm: true, cfg: true, noFlags: true,
			regs: map[x86asm.Reg]uint64{x86asm.RBX: execMemBase + 0x100, x86asm.RAX: r.Uint64(), x86asm.RCX: r.Uint64(),
				x86asm.RDX: r.Uint64(), x86asm.RSI: r.Uint64()},
			mem: map[int][]byte{}}
		in := randBytes(r, 0x200)
		scratch := randBytes(r, 0x220)
		if i%2 == 1 { // small floats and doubles instead of arbitrary bit patterns
			for k := 0; k < 16; k++ {
				copy(in[32*k:], floatLanes(r, 4+4*(k%2)))
				copy(scratch[32*k:], floatLanes(r, 8-4*(k%2)))
			}
		}
		c.mem[0x100], c.mem[0x800] = in, scratch
		// Sequences with an instruction the lifter does not lower are left out.
		unsup := false
		for _, ln := range liftSeqCFG(t, probe, c, 0x10000) {
			if strings.Contains(ln, "_recomp_unsupported") || strings.Contains(ln, "UNSUPPORTED") {
				unsup = true
				break
			}
		}
		if unsup {
			skipped++
			continue
		}
		base = append(base, c)
	}
	if len(base) < len(seqs)*3/4 {
		t.Fatalf("only %d of %d sequences are liftable", len(base), len(seqs))
	}
	t.Logf("%d sequences (%d skipped: unsupported instruction)", len(base), skipped)
	run := func(vec string) []byte {
		cases := make([]execCase, len(base))
		for i, c := range base {
			c.vec = vec
			cases[i] = c
		}
		var out []byte
		execCapture = &out
		defer func() { execCapture = nil }()
		runExecExpect(t, cases, make([]map[int][]byte, len(cases)))
		return out
	}
	ref := run("")
	if t.Failed() || len(ref) == 0 {
		t.Fatal("reference run failed")
	}
	rec := stSize + execMemSize
	for _, mode := range []string{"strict", "abi"} {
		got := run(mode)
		if len(got) != len(ref) {
			t.Fatalf("%s: output size %d, want %d", mode, len(got), len(ref))
		}
		bad := 0
		for i := range base {
			a, b := ref[i*rec:(i+1)*rec], got[i*rec:(i+1)*rec]
			// The flags are not compared (most sequences leave them undefined), the registers are.
			if bytes.Equal(a[:stFlags], b[:stFlags]) && bytes.Equal(a[stSize:], b[stSize:]) {
				continue
			}
			bad++
			if bad > 5 {
				continue
			}
			where := "registers"
			for k := stSize; k < rec; k++ {
				if a[k] != b[k] {
					where = fmt.Sprintf("memory +%#x (output slot of ymm%d, byte %d)", k-stSize, (k-stSize-0x400)/32, (k-stSize-0x400)%32)
					break
				}
			}
			t.Errorf("%s: %s differs from the reference at %s\n  %s", mode, base[i].name, where, strings.Join(base[i].code[16:], " "))
		}
		if bad > 5 {
			t.Errorf("%s: %d sequences differ", mode, bad)
		}
	}
}

// flat returns the instruction text of the lowerings (no comments, no indentation).
func flat(ls [][]string) []string {
	var out []string
	for _, l := range ls {
		for _, ln := range l {
			if c := code(ln); c != "" {
				out = append(out, c)
			}
		}
	}
	return out
}

func hasLine(lines []string, want string) bool {
	for _, l := range lines {
		if l == want {
			return true
		}
	}
	return false
}

const (
	vlLoadSd1      = "c5fb100f"   // vmovsd xmm1, qword ptr [rdi]
	vlLoadSd0      = "c5fb1007"   // vmovsd xmm0, qword ptr [rdi]
	vlLoadSs0      = "c5fa1007"   // vmovss xmm0, dword ptr [rdi]
	vlLoadSs8      = "c57a1007"   // vmovss xmm8, dword ptr [rdi]
	vlMulsd0_1_1   = "c5f359c1"   // vmulsd xmm0, xmm1, xmm1
	vlMulsd1_0_2   = "c5fb59ca"   // vmulsd xmm1, xmm0, xmm2
	vlMulss0_1_2   = "c5f259c2"   // vmulss xmm0, xmm1, xmm2
	vlStoreX1      = "c5f9110f"   // vmovupd [rdi], xmm1
	vlStoreX0      = "c5f91107"   // vmovupd [rdi], xmm0
	vlStoreSs0     = "c5fa1107"   // vmovss dword ptr [rdi], xmm0
	vlStoreSs8     = "c57a1107"   // vmovss dword ptr [rdi], xmm8
	vlAddss8_8_1   = "c53a58c1"   // vaddss xmm8, xmm8, xmm1
	vlLegacyLoad3  = "f30f101f"   // movss xmm3, dword ptr [rdi]
	vlStoreY3      = "c5fd111f"   // vmovupd [rdi], ymm3
	vlLoadY4       = "c5fc1027"   // vmovups ymm4, [rdi]
	vlPshufb5_6_7  = "c4e24900ef" // vpshufb xmm5, xmm6, xmm7 (lowering uses two scratch registers)
	vlStoreX5      = "c5f8112f"   // vmovups [rdi], xmm5
	vlCallFar      = "e8fb3f0000" // call out of the function
	vlRet          = "c3"
	vlJmpRax       = "ffe0"
	vlVzeroupper   = "c5f877"
	vlVzeroall     = "c5fc77"
	vlUcomisd0_1   = "c5f92ec1" // vucomisd xmm0, xmm1
	vlAddpsY1_2_3  = "c5ec58cb" // vaddps ymm1, ymm2, ymm3
	vlStoreY1      = "c5fc110f" // vmovups [rdi], ymm1
	vlAddsd0_0_1   = "c5fb58c1" // vaddsd xmm0, xmm0, xmm1
	vlXorpd0_0_mem = "c5f95707" // vxorpd xmm0, xmm0, [rdi]
)

// A scalar result whose upper lanes nobody reads is computed straight into the destination; the
// implied zeroing of the YMM upper half goes when that half is dead.
func TestVecLiveScalarFold(t *testing.T) {
	// xmm0 is stored as a scalar, then everything dies (vzeroall).
	got := flat(liftVec(t, "abi", false, vlMulss0_1_2, vlStoreSs0, vlVzeroall))
	if !hasLine(got, "fmul s0, s1, s2") {
		t.Errorf("no direct scalar multiply:\n%s", strings.Join(got, "\n"))
	}
	forbid(t, "dead lanes", got, "mov v0.16b", "mov v0.s[0]", "movi v16.16b", "movi v")
	// Off: the full merge and the zeroing.
	off := flat(liftVec(t, "off", false, vlMulss0_1_2, vlStoreSs0, vlVzeroall))
	requireInOrder(t, "off", off, "fmul s30, s1, s2", "mov v0.16b, v1.16b", "mov v0.s[0], v30.s[0]", "movi v16.16b, #0")
}

// The same value returned: every lane of xmm0 and its upper half are live at the return (the
// function may return __m128 or __m256), so the merge and the zeroing stay.
func TestVecLiveReturnKeepsLanes(t *testing.T) {
	got := flat(liftVec(t, "abi", false, vlMulss0_1_2, vlRet))
	requireInOrder(t, "returned", got, "fmul s30, s1, s2", "mov v0.16b, v1.16b", "mov v0.s[0], v30.s[0]", "movi v16.16b, #0")
}

// Lanes known to be zero in the host register make the direct form exact, live or not: a scalar
// load zeroes them, and the arm64 scalar multiply zeroes them again.
func TestVecLiveZeroLanes(t *testing.T) {
	got := flat(liftVec(t, "abi", false, vlLoadSd1, vlMulsd0_1_1, vlRet))
	if !hasLine(got, "fmul d0, d1, d1") {
		t.Errorf("no direct multiply after a scalar load:\n%s", strings.Join(got, "\n"))
	}
	forbid(t, "zero lanes", got, "mov v0.16b", "mov v0.d[0]")
	// The upper half of ymm0 is live at the return: its zeroing stays.
	if !hasLine(got, "movi v16.16b, #0") {
		t.Errorf("upper half of the returned register not zeroed:\n%s", strings.Join(got, "\n"))
	}
	// A packed operation with unknown lanes in between ends the knowledge.
	got = flat(liftVec(t, "abi", false, vlLoadSd0, vlXorpd0_0_mem, vlAddsd0_0_1, vlRet))
	if !hasLine(got, "mov v0.d[0], v30.d[0]") {
		t.Errorf("merge dropped although bits 64..127 are unknown and live:\n%s", strings.Join(got, "\n"))
	}
}

// The zero-lane knowledge does not survive a call (the callee may leave anything in the
// register) or a block boundary (another path may reach the block).
func TestVecLiveZeroLanesFlushed(t *testing.T) {
	got := flat(liftVec(t, "abi", false, vlLoadSd1, vlCallFar, vlMulsd0_1_1, vlRet))
	requireInOrder(t, "after call", got, "fmul d30, d1, d1", "mov v0.16b, v1.16b", "mov v0.d[0], v30.d[0]")
	// vmovsd xmm1,[rdi]; vucomisd; jbe +0 (to the next instruction, a new block); vmulsd xmm0,xmm1,xmm1; ret
	got = flat(liftVec(t, "abi", false, vlLoadSd1, vlUcomisd0_1, "7600", vlMulsd0_1_1, vlRet))
	requireInOrder(t, "after block boundary", got, "fmul d30, d1, d1", "mov v0.16b, v1.16b", "mov v0.d[0], v30.d[0]")
}

// Calls: an argument register (xmm0-7) keeps its upper-half zeroing before a call, a register
// that is not one loses it; after the call nothing is assumed.
func TestVecLiveCallFlush(t *testing.T) {
	got := flat(liftVec(t, "abi", false, vlLoadSs0, vlLoadSs8, vlCallFar, vlRet))
	if !hasLine(got, "movi v16.16b, #0") {
		t.Errorf("upper half of xmm0 (an argument register) not zeroed before the call:\n%s", strings.Join(got, "\n"))
	}
	forbid(t, "xmm8 before a call", got, "movi v24.16b")
	// Strict mode keeps both.
	got = flat(liftVec(t, "strict", false, vlLoadSs0, vlLoadSs8, vlCallFar, vlRet))
	if !hasLine(got, "movi v16.16b, #0") || !hasLine(got, "movi v24.16b, #0") {
		t.Errorf("strict mode dropped an upper-half zeroing before a call:\n%s", strings.Join(got, "\n"))
	}
	// A value computed before a call and used after it does not exist in System V code; a
	// register written and stored before the call is still exact at the store.
	got = flat(liftVec(t, "abi", false, vlLoadSs8, vlAddss8_8_1, vlStoreSs8, vlCallFar, vlRet))
	if !hasLine(got, "fadd s8, s8, s1") {
		t.Errorf("scalar add before the call:\n%s", strings.Join(got, "\n"))
	}
}

// Vector scratch registers are v16-v31, the YMM upper halves. With liveness a lowering borrows
// one whose upper half is dead and saves nothing; when every upper half is live (here: an
// indirect jump out of the function follows) it saves and restores what it borrows.
func TestVecLiveScratch(t *testing.T) {
	got := flat(liftVec(t, "abi", false, vlPshufb5_6_7, vlStoreX5, vlRet))
	forbid(t, "dead scratch", got, "stp q", "str q2", "str q3", "ldp q", "[sp, #-")
	got = flat(liftVec(t, "abi", false, vlPshufb5_6_7, vlStoreX5, vlJmpRax))
	save, restore := -1, -1
	for i, l := range got {
		if strings.HasPrefix(l, "stp q") && strings.Contains(l, "[sp, #-32]!") {
			save = i
		}
		if strings.HasPrefix(l, "ldp q") && strings.Contains(l, "[sp], #32") {
			restore = i
		}
	}
	if save < 0 || restore < save {
		t.Errorf("live upper halves borrowed without save/restore:\n%s", strings.Join(got, "\n"))
	}
	// ymm4's upper half is live (stored later), the others die at the vzeroupper: the scratch
	// registers must not be v20, and nothing is saved.
	got = flat(liftVec(t, "abi", false, vlLoadY4, vlPshufb5_6_7, vlStoreX5, "c5fc1127" /* vmovups [rdi], ymm4 */, vlVzeroupper, vlRet))
	forbid(t, "scratch avoids the live upper half", got, "stp q", "[sp, #-")
	for _, l := range got {
		if strings.HasPrefix(l, "tbl ") && strings.Contains(l, "v20.") {
			t.Errorf("scratch use of v20 (live upper half of ymm4): %s", l)
		}
	}
}

// A legacy SSE instruction keeps bits 128..255; VZEROUPPER zeroes only what can still be read.
func TestVecLiveLegacyAndVzeroupper(t *testing.T) {
	got := flat(liftVec(t, "abi", false, vlLegacyLoad3, vlStoreY3, vlVzeroupper, vlRet))
	forbid(t, "legacy load", got, "movi v19.16b", "movi v3.16b", "q19, [sp")
	if !hasLine(got, "ldr s3, [x28, x16]") {
		t.Errorf("legacy scalar load:\n%s", strings.Join(got, "\n"))
	}
	// After the vzeroupper only ymm0's upper half is read (by the return).
	n := 0
	for _, l := range got {
		if strings.HasPrefix(l, "movi v") {
			n++
			if l != "movi v16.16b, #0" {
				t.Errorf("vzeroupper zeroes a dead upper half: %s", l)
			}
		}
	}
	if n != 1 {
		t.Errorf("vzeroupper before ret: %d zeroings, want 1:\n%s", n, strings.Join(got, "\n"))
	}
	// With everything live (indirect jump out) all 16 are zeroed.
	got = flat(liftVec(t, "abi", false, vlVzeroupper, vlJmpRax))
	n = 0
	for _, l := range got {
		if strings.HasPrefix(l, "movi v") {
			n++
		}
	}
	if n != 16 {
		t.Errorf("vzeroupper before an indirect jump: %d zeroings, want 16", n)
	}
}

// Off mode and an unanalysed function produce exactly the previous lowering.
func TestVecLiveOffIsUnchanged(t *testing.T) {
	seq := []string{vlLoadSd1, vlMulsd0_1_1, vlAddpsY1_2_3, vlPshufb5_6_7, vlLegacyLoad3, vlVzeroupper, vlRet}
	for _, ymm := range []bool{false, true} {
		want := liftBlockIn(t, ymm, seq...)
		got := liftVec(t, "off", ymm, seq...)
		if strings.Join(flat(want), "\n") != strings.Join(flat(got), "\n") {
			t.Errorf("ymm=%v: off mode differs from the unanalysed lowering", ymm)
		}
		// The lifter switch alone also turns it off.
		l := NewArm64Lifter(nil)
		l.SetVecLiveness(false)
		if l.VecLiveness() {
			t.Error("SetVecLiveness(false)")
		}
	}
}

// The zero-lane tracker: what it learns from emitted text and what makes it forget.
func TestVecZeroTracker(t *testing.T) {
	const both = disasm.VecL1 | disasm.VecH
	var z vecZero
	step := func(lines ...string) { z.update(lines) }
	step("    ldr d1, [x28, x16]", "    ldr s2, [x28, x16]")
	if !z.has(1, disasm.VecH) || z.has(1, disasm.VecL1) || !z.has(2, disasm.VecL1) || !z.has(2, disasm.VecH) {
		t.Fatalf("scalar loads: %v", z.z)
	}
	step("    fmul d3, d1, d1", "    mov v4.16b, v2.16b", "    fcmp d1, d3", "    str d1, [x28, x16]", "    fcvtzs x19, d1")
	if !z.has(3, disasm.VecH) || uint64(z.z[4]) != both || !z.has(1, disasm.VecH) {
		t.Fatalf("defs, copies and pure reads: %v", z.z)
	}
	step("    mov v4.d[0], v30.d[0]") // lane 0 insert: bits 32..63 change, 64..127 stay
	if z.has(4, disasm.VecL1) || !z.has(4, disasm.VecH) {
		t.Fatalf("insert: %v", z.z)
	}
	step("    eor v5.16b, v1.16b, v9.16b", "    and v6.16b, v1.16b, v9.16b")
	if z.has(5, disasm.VecH) || !z.has(6, disasm.VecH) {
		t.Fatalf("bitwise: %v", z.z)
	}
	step("    eor v7.16b, v7.16b, v7.16b") // zero idiom
	if uint64(z.z[7]) != both {
		t.Fatalf("zero idiom: %v", z.z)
	}
	step("    fadd v3.2d, v3.2d, v1.2d") // not recognised: forgets what it names, also the source
	if z.has(3, disasm.VecH) || z.has(1, disasm.VecH) || !z.has(2, disasm.VecH) {
		t.Fatalf("unknown instruction: %v", z.z)
	}
	step("    cbz x3, 1f", "    fmov d2, d7", "1:") // two paths: forgets what it names
	if z.has(2, disasm.VecH) || !z.has(6, disasm.VecH) {
		t.Fatalf("internal control flow: %v", z.z)
	}
	for _, callLike := range [][]string{
		{"    bl _recomp_x87"},
		{"    adr x30, 1f", "    b L_shim", "1:"},
		{"    b _recomp_call_shim"},
		{"    tbl v20.16b, {v21.16b}, v22.16b"},
	} {
		z.z[6] = uint8(both)
		step(callLike...)
		if z.z != [16]uint8{} {
			t.Fatalf("%v: facts survive: %v", callLike, z.z)
		}
	}
	// Facts end at a block leader, at a gap in the addresses and in another function.
	fn := &disasm.Function{Blocks: map[uint64]*disasm.BasicBlock{0x1000: {}, 0x1010: {}}}
	z.begin(fn, 0x1000, 0x1004)
	z.z[1] = uint8(both)
	z.begin(fn, 0x1004, 0x1008)
	if !z.has(1, disasm.VecH) {
		t.Fatal("facts lost inside a block")
	}
	z.begin(fn, 0x100c, 0x1010) // 0x1008 was not lifted by this lifter
	if z.has(1, disasm.VecH) {
		t.Fatal("facts survive an address gap")
	}
	z.z[1] = uint8(both)
	z.begin(fn, 0x1010, 0x1014)
	if z.has(1, disasm.VecH) {
		t.Fatal("facts survive a block leader")
	}
	z.z[1] = uint8(both)
	z.begin(&disasm.Function{Blocks: map[uint64]*disasm.BasicBlock{}}, 0x1014, 0x1018)
	if z.has(1, disasm.VecH) {
		t.Fatal("facts survive a change of function")
	}
}

// Regression (found by the differential test on a 1500-sequence corpus): VRCPSS copies its first
// source into the destination at the start of its lowering and inserts the scalar at the end.
// What was known about the destination's lanes before the instruction (zero, after the scalar
// load) says nothing about them at the insert.
func TestVecLiveZeroFactStaleInsideLowering(t *testing.T) {
	got := flat(liftVec(t, "abi", false,
		"c57a1037",   // vmovss xmm14, dword ptr [rdi]
		"c4412a53f4", // vrcpss xmm14, xmm10, xmm12
		"c5781137",   // vmovups [rdi], xmm14
		vlVzeroall))
	if !hasLine(got, "mov v14.16b, v10.16b") {
		t.Fatalf("lowering changed; adapt the test:\n%s", strings.Join(got, "\n"))
	}
	for _, l := range got {
		if strings.HasPrefix(l, "fmov s14,") {
			t.Errorf("scalar written with the lanes above it zeroed, but they hold xmm10's: %s", l)
		}
	}
}

// The lifter's own switch: an analysed function lifted with liveness turned off on the lifter
// gets the previous lowering (the YMM rule of the function decides about spills again).
func TestVecLiveLifterSwitch(t *testing.T) {
	seq := []string{vlLoadSd1, vlMulsd0_1_1, vlPshufb5_6_7, vlStoreX5, vlRet}
	for _, ymm := range []bool{false, true} {
		want := flat(liftBlockIn(t, ymm, seq...))
		var insts []disasm.Instruction
		pc := uint64(0x1000)
		for _, h := range seq {
			b, _ := hex.DecodeString(h)
			inst, err := disasm.Decode(b)
			if err != nil {
				t.Fatal(err)
			}
			insts = append(insts, disasm.Instruction{Address: pc, Inst: inst})
			pc += uint64(inst.Len)
		}
		fn := disasm.BuildTestFunction(0x1000, insts)
		fn.UsesYmm = ymm
		disasm.AnalyzeVecLiveness(fn, disasm.VecLiveABI)
		l := NewArm64Lifter(nil)
		l.SetVecLiveness(false)
		var got []string
		for _, a := range fn.BlockOrder {
			for _, in := range fn.Blocks[a].Insts {
				lines, err := l.LiftInstructionToBuf(in, in.Address+uint64(in.Inst.Len), fn, nil)
				if err != nil {
					t.Fatal(err)
				}
				got = append(got, flat([][]string{lines})...)
			}
		}
		if strings.Join(want, "\n") != strings.Join(got, "\n") {
			t.Errorf("ymm=%v: lifter switch off differs from the previous lowering:\n%s\n--- want\n%s", ymm, strings.Join(got, "\n"), strings.Join(want, "\n"))
		}
	}
}
