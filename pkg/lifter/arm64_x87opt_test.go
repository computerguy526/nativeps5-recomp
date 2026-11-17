package lifter

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"math"
	"math/rand"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strings"
	"testing"

	"ps4-recomp/pkg/disasm"

	"golang.org/x/arch/x86/x86asm"
)

// Tests of the x87=f80 performance work (arm64_lifter_x87inline.go, the bridge in
// pkg/emitter/asm_x87.go): the inline fast paths and the batching are compared, over a corpus of
// random instruction sequences, with the reference lowering that calls the helper for every
// instruction; the vector registers and NZCV are checked across the bridge.

// x87Prog is one program: Intel-syntax lines, the initial memory window and control word.
type x87Prog struct {
	asm []string
	cw  uint16
}

// x87Result is the state after a program: the raw record written by the driver.
type x87Result struct {
	raw []byte
}

const (
	x87RecCore = 16 + 128 + 8 + 256 // top, sw, cw, valid, fpu80, rax, window[0:256]
	x87RecLen  = x87RecCore + 512 + 8
)

// Guest registers of the harness: rdx = the window at 0x100. The vector registers are loaded with
// a pattern before the program and stored after it; NZCV starts as 0xa0000000.
const x87OptDriver = `
#include "recomp_context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t vpat[512], vout[512];
uint64_t nzcv_out;
static GuestContext ctx;

int main(int argc, char **argv) {
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
    uint8_t *window = calloc(1, 1 << 20);
    typedef void (*fn_t)(uint8_t *, uint64_t *);
    extern fn_t lifted_table[];
    extern int lifted_n;
    uint8_t mem[256];
    for (int i = 0; i < 512; i++) vpat[i] = (uint8_t)(i * 37 + 11);
    for (int i = 0; i < lifted_n; i++) {
        uint16_t cw;
        fread(&cw, 2, 1, in); fread(mem, 1, 256, in);
        memset(window, 0, 1 << 20);
        memset(&ctx, 0, sizeof ctx);
        ctx.fpu_cw = cw;
        ctx.mxcsr = 0x1f80;
        GuestContext *cp = &ctx;
        memcpy(window + 0x80000 + 0x70, &cp, 8);
        memcpy(window + 0x100, mem, 256);
        uint64_t rax = 0;
        memset(vout, 0, sizeof vout);
        lifted_table[i](window, &rax);
        uint32_t top = (uint32_t)ctx.fpu_top, sw = ctx.fpu_sw, c = ctx.fpu_cw, valid = ctx.fpu_valid;
        fwrite(&top, 4, 1, out); fwrite(&sw, 4, 1, out); fwrite(&c, 4, 1, out); fwrite(&valid, 4, 1, out);
        for (int r = 0; r < 8; r++) fwrite(&ctx.fpu80[r], 16, 1, out);
        fwrite(&rax, 8, 1, out);
        fwrite(window + 0x100, 1, 256, out);
        fwrite(vout, 1, 512, out);
        fwrite(&nzcv_out, 8, 1, out);
    }
    return 0;
}
`

// x87OptWindow is the initial memory (rdx): values of every format the menu reads, including
// NaNs, infinities, denormals, a pseudo-denormal and the integer extremes.
func x87OptWindow() []byte {
	w := make([]byte, 256)
	copy(w[0:], mem80(0xC90FDAA22168C235, 0x4000))  // pi
	copy(w[16:], mem80(0x8000000000000000, 0xbfff)) // -1
	copy(w[32:], mem64(1.0000001))
	copy(w[40:], mem64(-0.37))
	copy(w[48:], mem64(math.Inf(1)))
	copy(w[56:], mem64(5e-324)) // denormal double
	copy(w[64:], mem32(2.5))
	copy(w[68:], mem32(float32(math.NaN())))
	copy(w[72:], mem32(1e-40)) // denormal float
	copy(w[80:], memI(2, -12345))
	copy(w[84:], memI(4, math.MinInt32))
	copy(w[88:], memI(8, math.MinInt64))
	copy(w[96:], mem80(0xC000000000000001, 0x7fff))  // quiet NaN
	copy(w[112:], mem80(0x0000000000000001, 0x0000)) // denormal extended
	copy(w[240:], mem64(1e10))
	copy(w[248:], mem64(40000.5))
	copy(w[208:], memI(2, 0x027f))
	copy(w[210:], memI(2, 0x0c7f))
	copy(w[212:], memI(2, 0x037f))
	return w
}

var x87OptMenu = []string{
	"fld1", "fldz", "fldpi", "fldl2e", "fld st(0)", "fld st(1)", "fld st(3)",
	"fld tbyte ptr [rdx]", "fld tbyte ptr [rdx+16]", "fld tbyte ptr [rdx+96]", "fld tbyte ptr [rdx+112]",
	"fld qword ptr [rdx+32]", "fld qword ptr [rdx+48]", "fld qword ptr [rdx+56]", "fld dword ptr [rdx+64]", "fld dword ptr [rdx+68]",
	"fild word ptr [rdx+80]", "fild dword ptr [rdx+84]", "fild qword ptr [rdx+88]", "fild dword ptr [rdx+4]", "fild qword ptr [rdx+120]",
	"fstp tbyte ptr [rdx+128]", "fstp qword ptr [rdx+160]", "fst dword ptr [rdx+176]",
	"fstp st(1)", "fst st(2)", "fstp st(0)", "fst st(0)", "fstp st(3)",
	"fxch st(1)", "fxch st(2)", "fxch", "fxch st(7)", "fxch st(0)",
	"fchs", "fabs", "ffree st(0)", "ffree st(2)", "ffree st(7)", "ffreep st(1)", "ffreep st(0)", "fincstp", "fdecstp",
	"fadd st, st(1)", "faddp", "fmul qword ptr [rdx+32]", "fsubp st(1), st", "fdivr st(1), st", "fdiv dword ptr [rdx+64]",
	"fiadd word ptr [rdx+80]", "fmul st(2), st", "fsub tbyte ptr [rdx]" /* invalid: replaced below */, "fsqrt", "frndint", "fscale", "fprem", "fxtract",
	"fcom st(1)", "fcomp", "ftst", "fxam", "fucompp", "fcomi st, st(1)", "fucomip st, st(1)", "fcmovb st, st(1)", "fcmovne st, st(2)",
	"fnstsw ax", "fnstsw word ptr [rdx+192]", "fnstcw word ptr [rdx+200]", "fldcw word ptr [rdx+208]", "fldcw word ptr [rdx+210]", "fldcw word ptr [rdx+212]",
	"fistp dword ptr [rdx+216]", "fisttp qword ptr [rdx+224]", "fisttp word ptr [rdx+234]", "fisttp dword ptr [rdx+236]",
	"fld qword ptr [rdx+240]", "fld qword ptr [rdx+248]", "fld qword ptr [rdx+40]", "fchs", "fist word ptr [rdx+232]", "fninit", "fnclex",
	"mov eax, 1", "add rax, rdx",
}

func x87OptCorpus() []x87Prog {
	var menu []string
	for _, m := range x87OptMenu {
		if strings.HasPrefix(m, "fsub tbyte") {
			continue // there is no FSUB m80
		}
		menu = append(menu, m)
	}
	rng := rand.New(rand.NewSource(87))
	cws := []uint16{0x037f, 0x037f, 0x037f, 0x027f, 0x007f, 0x0c7f, 0x077f, 0x0b7f}
	progs := []x87Prog{
		// The first program uses the helper first: its thread-local state is created while all 32
		// vector registers hold the pattern.
		{asm: []string{"fld1", "fld1", "faddp", "fmul qword ptr [rdx+32]"}, cw: 0x37f},
		// Directed: runs of helper operations with memory operands and inline operations in between.
		{asm: []string{"fld qword ptr [rdx+32]", "fmul qword ptr [rdx+32]", "fxch", "fadd dword ptr [rdx+64]", "fstp qword ptr [rdx+160]", "fistp dword ptr [rdx+216]"}, cw: 0x37f},
		{asm: []string{"fild dword ptr [rdx+84]", "fchs", "fld qword ptr [rdx+40]", "fmulp", "fabs", "faddp st(1), st"}, cw: 0x37f},
		{asm: []string{"fldz", "fld1", "fldpi", "fldl2e", "fsqrt", "frndint", "fxch st(2)", "fst st(3)", "fprem", "fstp tbyte ptr [rdx+128]"}, cw: 0x37f},
		{asm: []string{"fld1", "fld1", "fld1", "fld1", "fld1", "fld1", "fld1", "fld1", "fld1", "fld1", "fadd st, st(1)", "fnstsw ax"}, cw: 0x37f},
		{asm: []string{"fld qword ptr [rdx+240]", "fisttp qword ptr [rdx+128]", "fld qword ptr [rdx+240]", "fisttp dword ptr [rdx+136]", "fld qword ptr [rdx+248]", "fisttp word ptr [rdx+140]", "fld qword ptr [rdx+248]", "fchs", "fisttp dword ptr [rdx+144]", "fld qword ptr [rdx+40]", "fisttp dword ptr [rdx+148]", "fldz", "fisttp dword ptr [rdx+152]", "fnstsw ax"}, cw: 0x37f},
		{asm: []string{"faddp", "fmul st(1), st", "fxch", "fchs", "fstp st(0)", "fnstsw word ptr [rdx+192]"}, cw: 0x37f},
	}
	for i := 0; i < 600; i++ {
		n := 2 + rng.Intn(14)
		p := x87Prog{cw: cws[rng.Intn(len(cws))]}
		for j := 0; j < n; j++ {
			p.asm = append(p.asm, menu[rng.Intn(len(menu))])
		}
		progs = append(progs, p)
	}
	return progs
}

// x87OptRun lowers every program with the given options and runs them natively against the real
// runtime. It returns the records and the lowered text of every program.
func x87OptRun(t *testing.T, dir string, lib string, progs []x87Prog, inline, batch bool) ([]x87Result, []string) {
	t.Helper()
	l := NewArm64Lifter(nil)
	if err := l.SetX87Mode("f80"); err != nil {
		t.Fatal(err)
	}
	l.SetTSO(false)
	l.SetX87Opt(inline, batch)
	var asm strings.Builder
	asm.WriteString(".text\n")
	var table strings.Builder
	var in bytes.Buffer
	var lowered []string
	enc := map[string]string{}
	for i, p := range progs {
		var insts []disasm.Instruction
		pc := uint64(0x1000)
		for _, line := range p.asm {
			h, ok := enc[line]
			if !ok {
				h = assemble(t, line)[0]
				enc[line] = h
			}
			inst, err := x86asm.Decode(mustHex(t, h), 64)
			if err != nil {
				t.Fatalf("decode %q: %v", line, err)
			}
			insts = append(insts, disasm.Instruction{Address: pc, Inst: inst})
			pc += uint64(inst.Len)
		}
		blk := &disasm.BasicBlock{StartAddr: 0x1000, EndAddr: pc, Insts: insts}
		fn := &disasm.Function{EntryAddr: 0x1000, EndAddr: 0x9000, Blocks: map[uint64]*disasm.BasicBlock{0x1000: blk}}
		l.SetBlock(blk)
		fmt.Fprintf(&asm, ".p2align 2\n_case_%d:\n", i)
		asm.WriteString("    stp x29, x30, [sp, #-112]!\n    stp x19, x20, [sp, #16]\n    stp x21, x22, [sp, #32]\n    stp x23, x24, [sp, #48]\n" +
			"    stp x25, x26, [sp, #64]\n    stp x27, x28, [sp, #80]\n    str x1, [sp, #96]\n")
		asm.WriteString("    mov x28, x0\n    mov x19, #0\n    mov x20, #0\n    mov x21, #0\n    mov x22, #0x100\n    mov x23, #0\n    mov x24, #0\n    mov x25, #0\n    mov x26, #0x70000\n    mov x27, #0x80000\n")
		asm.WriteString("    adrp x16, _vpat@PAGE\n    add  x16, x16, _vpat@PAGEOFF\n")
		for r := 0; r < 32; r += 2 {
			fmt.Fprintf(&asm, "    ldp q%d, q%d, [x16, #%d]\n", r, r+1, 16*r)
		}
		asm.WriteString("    mov x2, #0xa0000000\n    msr nzcv, x2\n")
		var body strings.Builder
		for _, in := range insts {
			next := in.Address + uint64(in.Inst.Len)
			lines, err := l.LiftInstructionToBuf(in, next, fn, nil)
			if err != nil {
				t.Fatalf("lift %s: %v", in.Inst, err)
			}
			for _, ln := range lines {
				if strings.Contains(ln, "_recomp_unsupported") {
					t.Fatalf("%s not lifted", in.Inst)
				}
				body.WriteString(ln + "\n")
			}
		}
		asm.WriteString(body.String())
		lowered = append(lowered, body.String())
		asm.WriteString("    mrs x2, nzcv\n    adrp x16, _vout@PAGE\n    add  x16, x16, _vout@PAGEOFF\n")
		for r := 0; r < 32; r += 2 {
			fmt.Fprintf(&asm, "    stp q%d, q%d, [x16, #%d]\n", r, r+1, 16*r)
		}
		asm.WriteString("    adrp x16, _nzcv_out@PAGE\n    add x16, x16, _nzcv_out@PAGEOFF\n    str x2, [x16]\n")
		asm.WriteString("    ldr x1, [sp, #96]\n    str x19, [x1]\n    ldp x19, x20, [sp, #16]\n    ldp x21, x22, [sp, #32]\n    ldp x23, x24, [sp, #48]\n" +
			"    ldp x25, x26, [sp, #64]\n    ldp x27, x28, [sp, #80]\n    ldp x29, x30, [sp], #112\n    ret\n")
		fmt.Fprintf(&table, "    .quad _case_%d\n", i)
		cw := p.cw
		binary.Write(&in, binary.LittleEndian, cw)
		in.Write(x87OptWindow())
	}
	asm.WriteString(".data\n.p2align 3\n.globl _lifted_table\n_lifted_table:\n" + table.String())
	fmt.Fprintf(&asm, ".globl _lifted_n\n_lifted_n:\n    .long %d\n", len(progs))
	tag := fmt.Sprintf("%v_%v", inline, batch)
	asmPath := filepath.Join(dir, "cases_"+tag+".s")
	os.WriteFile(asmPath, []byte(asm.String()), 0o644)
	drv := filepath.Join(dir, "driver.c")
	os.WriteFile(drv, []byte(x87OptDriver), 0o644)
	exe := filepath.Join(dir, "x87opt_"+tag)
	core, _ := filepath.Abs(filepath.Join("..", "runtime", "core"))
	if o, err := exec.Command("clang", "-arch", "arm64", "-O1", "-w", "-I"+core, drv, asmPath, lib, "-o", exe).CombinedOutput(); err != nil {
		t.Fatalf("link: %v\n%s", err, o)
	}
	inPath, outPath := filepath.Join(dir, "in_"+tag+".bin"), filepath.Join(dir, "out_"+tag+".bin")
	os.WriteFile(inPath, in.Bytes(), 0o644)
	if o, err := exec.Command(exe, inPath, outPath).CombinedOutput(); err != nil {
		t.Fatalf("run: %v\n%s", err, o)
	}
	res, err := os.ReadFile(outPath)
	if err != nil {
		t.Fatal(err)
	}
	if len(res) != x87RecLen*len(progs) {
		t.Fatalf("output is %d bytes, want %d", len(res), x87RecLen*len(progs))
	}
	out := make([]x87Result, len(progs))
	for i := range progs {
		out[i] = x87Result{raw: res[i*x87RecLen : (i+1)*x87RecLen]}
	}
	return out, lowered
}

func x87OptSkip(t *testing.T) {
	if runtime.GOOS != "darwin" || runtime.GOARCH != "arm64" {
		t.Skip("needs an arm64 Mac (host lowering)")
	}
	for _, tool := range []string{"clang", "ar"} {
		if _, err := exec.LookPath(tool); err != nil {
			t.Skip(tool + " not found")
		}
	}
	if _, err := os.Stat(filepath.Join(llvmBin(), "llvm-mc")); err != nil {
		t.Skip("llvm-mc not found (set LLVM_BIN)")
	}
}

// TestX87OptMatchesReference: the inline fast paths, the batching and both together leave the
// same x87 state (TOP, status and control word, tags, all eight 80-bit registers), RAX, memory,
// NZCV and vector registers as the lowering that calls the helper for every instruction, over
// 600 random sequences (which overflow and underflow the stack, free registers, change the
// control word, hit NaNs and denormals) and a few directed ones. It also requires that the
// vector registers v0-v31 and the start NZCV survive programs without a compare.
func TestX87OptMatchesReference(t *testing.T) {
	x87OptSkip(t)
	dir := t.TempDir()
	lib := buildX87Runtime(t, dir)
	progs := x87OptCorpus()
	// The reference is the plain lowering on a runtime without the integer fast paths (everything on SoftFloat):
	// the modes below, with the fast paths and the lean bridge entries, must agree with it bit for bit.
	refDir := filepath.Join(dir, "ref")
	os.MkdirAll(refDir, 0o755)
	refLib := buildX87RuntimeFlags(t, refDir, "-DNPS5_X87_NOFAST")
	ref, _ := x87OptRun(t, dir, refLib, progs, false, false)
	vpat := make([]byte, 512)
	for i := range vpat {
		vpat[i] = byte(i*37 + 11)
	}
	for _, mode := range []struct {
		name          string
		inline, batch bool
	}{{"inline", true, false}, {"batch", false, true}, {"inline+batch", true, true}} {
		got, lowered := x87OptRun(t, dir, lib, progs, mode.inline, mode.batch)
		runs, inl := 0, 0
		for i, p := range progs {
			if !bytes.Equal(got[i].raw, ref[i].raw) {
				t.Errorf("%s: %v\n  state differs from the reference:\n  got  % x\n  want % x", mode.name, p.asm, got[i].raw[:x87RecCore], ref[i].raw[:x87RecCore])
				if len(got[i].raw) > x87RecCore && !bytes.Equal(got[i].raw[x87RecCore:], ref[i].raw[x87RecCore:]) {
					t.Errorf("  vector/NZCV: got % x want % x", got[i].raw[x87RecCore:x87RecCore+16], ref[i].raw[x87RecCore:x87RecCore+16])
				}
				break
			}
			runs += strings.Count(lowered[i], "bl   _recomp_x87_run")
			inl += strings.Count(lowered[i], "// tcb: ctx")
		}
		if mode.batch && runs == 0 {
			t.Errorf("%s: no batch was emitted", mode.name)
		}
		if mode.inline && inl == 0 {
			t.Errorf("%s: no inline sequence was emitted", mode.name)
		}
		t.Logf("%s: %d batches, %d context loads over %d programs", mode.name, runs, inl, len(progs))
	}
	// The vector registers survive the bridge and the inline paths.
	nzcvOK := 0
	for i, p := range progs {
		v := ref[i].raw[x87RecCore : x87RecCore+512]
		if !bytes.Equal(v, vpat) {
			t.Errorf("%v: the vector registers changed:\n got % x\nwant % x", p.asm, v, vpat)
			break
		}
		flagsOp := false
		for _, a := range p.asm {
			flagsOp = flagsOp || strings.HasPrefix(a, "fcomi") || strings.HasPrefix(a, "fucomi") || strings.HasPrefix(a, "add ")
		}
		if nz := binary.LittleEndian.Uint64(ref[i].raw[x87RecCore+512:]); !flagsOp {
			nzcvOK++
			if nz != 0xa0000000 {
				t.Errorf("%v: NZCV = %#x, want 0xa0000000", p.asm, nz)
				break
			}
		}
	}
	if nzcvOK == 0 {
		t.Errorf("NZCV never survived (no program without a compare/flags instruction?)")
	}
}

// TestX87InlineKeepsFlags: the inline sequences must not set NZCV (the guest flags live there)
// and must not touch the vector registers; the lowering of the register/memory moves has no
// helper call on its fast path.
func TestX87InlineKeepsFlags(t *testing.T) {
	x87OptSkip(t)
	l := NewArm64Lifter(nil)
	l.SetX87Mode("f80")
	flagSetting := regexp.MustCompile(`^\s*(adds|subs|ands|bics|negs|adcs|sbcs|cmp|cmn|tst|ccmp|ccmn)\b`)
	vector := regexp.MustCompile(`\b[vqdshb](\d|[12]\d|3[01])\b`)
	for _, w := range []x87Word{
		{kind: x87KFldSt, a: 2}, {kind: x87KFldConst, a: 0}, {kind: x87KFldConst, a: 6}, {kind: x87KFldM, sz: 2}, {kind: x87KFild, sz: 0},
		{kind: x87KFild, sz: 1}, {kind: x87KFild, sz: 2}, {kind: x87KFstM, sz: 2, pop: true}, {kind: x87KFstSt, a: 1, pop: true},
		{kind: x87KFstSt, a: 0, pop: true}, {kind: x87KXch, a: 3}, {kind: x87KChs}, {kind: x87KAbs}, {kind: x87KFree, a: 1},
		{kind: x87KFist, sz: 0, aux: 1, pop: true}, {kind: x87KFist, sz: 1, aux: 1, pop: true}, {kind: x87KFist, sz: 2, aux: 1, pop: true},
		{kind: x87KFldM, sz: 0}, {kind: x87KFldM, sz: 1},
		{kind: x87KFree, a: 1, pop: true}, {kind: x87KIncstp}, {kind: x87KDecstp},
	} {
		body, _ := l.x87Inline(w)
		if body == nil {
			t.Errorf("%+v has no inline path", w)
			continue
		}
		for _, ln := range body {
			if flagSetting.MatchString(ln) {
				t.Errorf("%+v: flag-setting instruction %q", w, ln)
			}
			if vector.MatchString(ln) {
				t.Errorf("%+v: vector register in %q", w, ln)
			}
		}
	}
	// FSQRT and the other arithmetic stay with SoftFloat.
	if b, _ := l.x87Inline(x87Word{kind: x87KArithSt, aux: x87Mul}); b != nil {
		t.Errorf("arithmetic must stay in the helper")
	}
}

// TestX87RuntimeVectorRegs: the bridge saves only v0-v7 because nps5_x87.c and the SoftFloat subset
// it calls use no other vector register. Disassemble them as the runtime is built (-O1 and -O2
// with the translator's -mcpu) and fail on any use of v8-v31, on any call out of the objects other
// than the helpers themselves, and on SIMD in the 8-15 range (callee-saved halves).
func TestX87RuntimeVectorRegs(t *testing.T) {
	x87OptSkip(t)
	dump := filepath.Join(llvmBin(), "llvm-objdump")
	if _, err := os.Stat(dump); err != nil {
		t.Skip("llvm-objdump not found")
	}
	core, _ := filepath.Abs(filepath.Join("..", "runtime", "core"))
	srcs, _ := filepath.Glob(filepath.Join(core, "softfloat", "*.c"))
	srcs = append(srcs, filepath.Join(core, "nps5_x87.c"))
	use := regexp.MustCompile(`\b[vqdshb](8|9|1\d|2\d|3[01])\b`)
	for _, opt := range []string{"-O1", "-O2", "-O3", "-Os"} {
		dir := t.TempDir()
		for _, src := range srcs {
			obj := filepath.Join(dir, filepath.Base(src)+".o")
			if o, err := exec.Command("clang", "-target", "arm64-apple-darwin", "-mcpu=apple-m1", opt, "-w", "-ffp-contract=off",
				"-I"+core, "-I"+filepath.Join(core, "softfloat"), "-c", src, "-o", obj).CombinedOutput(); err != nil {
				t.Fatalf("%s: %v\n%s", src, err, o)
			}
			out, err := exec.Command(dump, "-d", "--no-show-raw-insn", obj).CombinedOutput()
			if err != nil {
				t.Fatalf("objdump: %v\n%s", err, out)
			}
			// recomp_x87_fast / recomp_x87_run_fast (preserve_most, no host state saved by the bridge): no vector
			// register at all (v0-v7 are not preserved by preserve_most) and no call out.
			any := regexp.MustCompile(`\b[vqdsb]([0-9]|[12][0-9]|3[01])\b`)
			fast := ""
			sawFast := 0
			for _, ln := range strings.Split(string(out), "\n") {
				if strings.HasSuffix(ln, ">:") {
					fast = ""
					if strings.Contains(ln, "<_recomp_x87_fast>") || strings.Contains(ln, "<_recomp_x87_run_fast>") {
						fast = ln
						sawFast++
					}
				}
				if i := strings.Index(ln, ";"); i >= 0 {
					ln = ln[:i]
				}
				if f := strings.Fields(ln); len(f) > 1 && strings.HasSuffix(f[0], ":") && use.MatchString(strings.Join(f[1:], " ")) {
					t.Errorf("%s %s uses a vector register above v7: %s", opt, filepath.Base(src), strings.TrimSpace(ln))
				}
				if f := strings.Fields(ln); fast != "" && len(f) > 1 && strings.HasSuffix(f[0], ":") {
					body := strings.Join(f[1:], " ")
					if any.MatchString(body) || strings.HasPrefix(body, "bl ") || strings.HasPrefix(body, "blr ") {
						t.Errorf("%s %s: %s must be integer-only and call-free: %s", opt, filepath.Base(src), strings.TrimSpace(fast), strings.TrimSpace(ln))
					}
				}
			}
			if strings.HasSuffix(src, "nps5_x87.c") && sawFast != 2 {
				t.Errorf("%s: found %d of the 2 lean entry points", opt, sawFast)
			}
		}
	}
}

// TestX87FastConversions: the inline conversions in nps5_x87.c (float32/float64 normal numbers and
// integers to extended) give exactly SoftFloat's result, including the denormal, zero, infinity and
// NaN inputs that still go through SoftFloat.
func TestX87FastConversions(t *testing.T) {
	x87OptSkip(t)
	dir := t.TempDir()
	core, _ := filepath.Abs(filepath.Join("..", "runtime", "core"))
	src := `#include "nps5_x87.c"
#include <stdio.h>
#include <stdlib.h>
static uint64_t s = 88172645463325252ull;
static uint64_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
static int same(f80 a, f80 b) { return a.signif == b.signif && a.signExp == b.signExp; }
int main(void) {
  long bad = 0;
  for (long i = 0; i < 20000000; i++) {
    uint64_t r = rnd();
    if (i % 4 == 0) r = r & 0x800fffffffffffffull | (rnd() % 4 ? 0 : 0x7ff0000000000000ull); // denormals, zero, inf, NaN
    if (i % 4 == 1) r = r >> (rnd() % 64);
    f80 a = load_f64((uint64_t)&r), b = f64_to_extF80((float64_t){r});
    if (!same(a, b)) { if (bad++ < 5) printf("f64 %016llx\n", (unsigned long long)r); }
    uint32_t w = (uint32_t)r;
    if (i % 4 == 0) w = w & 0x807fffff;
    f80 c = load_f32((uint64_t)&w), d = f32_to_extF80((float32_t){w});
    if (!same(c, d)) { if (bad++ < 5) printf("f32 %08x\n", w); }
    int64_t v = (int64_t)(r >> (rnd() % 64));
    if (rnd() & 1) v = -v;
    f80 e = int_to_f80(v), f = i64_to_extF80(v);
    if (!same(e, f)) { if (bad++ < 5) printf("i64 %lld\n", (long long)v); }
  }
  for (int64_t v = -70000; v <= 70000; v++) if (!same(int_to_f80(v), i64_to_extF80(v))) bad++;
  int64_t ex[] = {INT64_MIN, INT64_MAX, INT32_MIN, INT32_MAX, 0, 1, -1};
  for (int i = 0; i < 7; i++) if (!same(int_to_f80(ex[i]), i64_to_extF80(ex[i]))) bad++;
  printf("bad %ld\n", bad);
  return bad != 0;
}
`
	cpath := filepath.Join(dir, "conv.c")
	os.WriteFile(cpath, []byte(src), 0o644)
	srcs, _ := filepath.Glob(filepath.Join(core, "softfloat", "*.c"))
	args := append([]string{"-arch", "arm64", "-O2", "-w", "-I" + core, "-I" + filepath.Join(core, "softfloat"), cpath}, srcs...)
	args = append(args, "-o", filepath.Join(dir, "conv"))
	if o, err := exec.Command("clang", args...).CombinedOutput(); err != nil {
		t.Fatalf("build: %v\n%s", err, o)
	}
	if o, err := exec.Command(filepath.Join(dir, "conv")).CombinedOutput(); err != nil {
		t.Fatalf("conversions differ from SoftFloat:\n%s", o)
	}
}

// TestX87FastArith: the exact integer fast paths of nps5_x87.c (FADD/FSUB/FSUBR/FMUL, compares) give
// SoftFloat's value and its inexact flag for every PC and RC wherever they take the fast path (the
// operands are zero or normal and the result is in the normal range); everywhere else they decline.
// 1.5e6 random operand pairs per operation, PC and RC, with the exponent differences, significand
// patterns (ties, all ones, representable doubles/floats) and range limits that matter.
func TestX87FastArith(t *testing.T) {
	x87OptSkip(t)
	n := 1500000
	if testing.Short() {
		n = 50000
	}
	dir := t.TempDir()
	core, _ := filepath.Abs(filepath.Join("..", "runtime", "core"))
	src := `#include "nps5_x87.c"
#include <stdio.h>
#include <stdlib.h>
static uint64_t s = 88172645463325252ull;
static uint64_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
static uint64_t mant(void) {
  uint64_t r = rnd(), m;
  switch (rnd() % 9) {
  case 0: m = r; break;
  case 1: m = r & ~0x7ffull; break;                       // a double's significand
  case 2: m = r & ~0xffffffffffull; break;                // a float's
  case 3: m = 0xffffffffffffffffull; break;
  case 4: m = 0x8000000000000000ull; break;
  case 5: m = 0x8000000000000000ull | (rnd() % 3 ? 0x400 : 0x3ff) | (rnd() % 4 ? 0 : r & 0x7ff); break;
  case 6: m = 0x8000000000000000ull | (rnd() % 3 ? 0x8000000000 : 0x7fffffffff) ; break;
  case 7: m = 0x8000000000000000ull | (r >> 40 << 20); break;
  default: m = r & 0xfffffffffffff800ull | (r & 1 ? 0x400 : 0); break;
  }
  return m | 0x8000000000000000ull;
}
static f80 operand(uint16_t base, int spread) {
  uint64_t r = rnd();
  uint16_t sign = (r & 1) << 15;
  int e;
  switch ((r >> 1) % 16) {
  case 0: e = 1 + rnd() % 3; break;
  case 1: e = 0x7ffe - rnd() % 3; break;
  case 2: e = 0x4000 + rnd() % 100; break;
  default: e = base + (int)(rnd() % (2 * spread + 1)) - spread; break;
  }
  f80 v = mk(mant(), (uint16_t)(sign | e));
  switch ((r >> 5) % 64) {                          // special operands
  case 0: v = mk(0, sign); break;
  case 1: v = mk(rnd() & 0x7fffffffffffffffull, sign); break;       // denormal / pseudo
  case 2: v = mk(0x8000000000000000ull, sign | 0x7fff); break;     // infinity
  case 3: v = mk(0xc000000000000000ull | rnd(), sign | 0x7fff); break;
  case 4: v.signif &= 0x7fffffffffffffffull; break;                  // unnormal
  }
  return v;
}
int main(void) {
  long bad = 0, fast = 0, total = 0, cmpfast = 0;
  static const unsigned pcs[3] = {0x000, 0x200, 0x300};
  for (int op = 0; op < 4; op++)
  for (int pi = 0; pi < 3; pi++)
  for (int rc = 0; rc < 4; rc++) {
    unsigned cw = 0x7f | pcs[pi] | rc << 10;
    GuestContext c; memset(&c, 0, sizeof c);
    c.fpu_cw = cw;
    env_begin(&c);
    for (long i = 0; i < NNN; i++) {
      uint16_t base = (rnd() % 4 == 0) ? 0x3fff : (uint16_t)(1 + rnd() % 0x7ffe);
      int spread = (int)(rnd() % 4 == 0 ? 80 : rnd() % 4 == 0 ? 3 : 1 + rnd() % 20);
      f80 a = operand(base, spread), b = operand(base, spread), r;
      if (rnd() % 8 == 0) { b = a; b.signExp ^= (rnd() & 1) << 15; if (rnd() % 2) b.signExp ^= 0x8000; }
      if (rnd() % 16 == 0) b = a; 
      bool inex;
      total++;
      if (!fast_arith(op, a, b, cw, &r, &inex)) continue;
      fast++;
      softfloat_exceptionFlags = 0;
      f80 e = arith(op, a, b);
      unsigned fl = softfloat_exceptionFlags;
      softfloat_exceptionFlags = 0;
      if (e.signif != r.signif || e.signExp != r.signExp || fl != (inex ? softfloat_flag_inexact : 0)) {
        if (bad++ < 10) printf("op %%d cw %%04x a %%04x:%%016llx b %%04x:%%016llx fast %%04x:%%016llx inex %%d soft %%04x:%%016llx flags %%x\n", op, cw, a.signExp,
          (unsigned long long)a.signif, b.signExp, (unsigned long long)b.signif, r.signExp, (unsigned long long)r.signif, inex, e.signExp, (unsigned long long)e.signif, fl);
      }
    }
  }
  GuestContext c; memset(&c, 0, sizeof c);
  c.fpu_cw = 0x37f; env_begin(&c);
  for (long i = 0; i < 6000000; i++) {
    uint16_t base = (rnd() %% 3 == 0) ? 0x3fff : (uint16_t)(1 + rnd() %% 0x7ffe);
    f80 a = operand(base, 2), b = operand(base, 2);
    if (rnd() %% 4 == 0) b = a;
    if (rnd() %% 8 == 0) { b = a; b.signExp ^= 0x8000; }
    unsigned cc;
    if (!fast_cmp(a, b, &cc)) continue;
    cmpfast++;
    softfloat_exceptionFlags = 0;
    unsigned e = compare(a, b, false);
    if (e != cc || softfloat_exceptionFlags) { if (bad++ < 10) printf("cmp %%04x:%%016llx %%04x:%%016llx fast %%x soft %%x\n", a.signExp, (unsigned long long)a.signif, b.signExp, (unsigned long long)b.signif, cc, e); }
  }
  printf("arith: %%ld of %%ld took the fast path, compares %%ld, bad %%ld\n", fast, total, cmpfast, bad);
  return bad != 0 || fast < total / 3 || cmpfast < 2000000;
}
`
	src = strings.ReplaceAll(src, "NNN", fmt.Sprint(n))
	src = strings.ReplaceAll(src, "%%", "%")
	cpath := filepath.Join(dir, "fast.c")
	os.WriteFile(cpath, []byte(src), 0o644)
	srcs, _ := filepath.Glob(filepath.Join(core, "softfloat", "*.c"))
	args := append([]string{"-arch", "arm64", "-O2", "-w", "-I" + core, "-I" + filepath.Join(core, "softfloat"), cpath}, srcs...)
	args = append(args, "-o", filepath.Join(dir, "fast"))
	if o, err := exec.Command("clang", args...).CombinedOutput(); err != nil {
		t.Fatalf("build: %v\n%s", err, o)
	}
	o, err := exec.Command(filepath.Join(dir, "fast")).CombinedOutput()
	t.Logf("%s", o)
	if err != nil {
		t.Fatalf("fast arithmetic differs from SoftFloat:\n%s", o)
	}
}
