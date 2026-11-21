package lifter

import (
	"fmt"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"ps4-recomp/pkg/disasm"

	"golang.org/x/arch/x86/x86asm"
)

// Override sites (live choice settings): a 32-bit register move whose result the slot replaces
// unless the slot is "keep" (bit 63) or, with KeepNegative, the own result is negative.
var overrideSeqs = []struct {
	name    string
	code    []string
	site    uint64
	keepNeg bool
	rcx     uint64 // RCX before the sequence
	own     uint64 // RAX without a patch
	rcxOut  func(r14 uint64) uint64
}{
	// mov eax, dword ptr [rbx]            (rbx -> 1920.0f)
	{"load", []string{"8b03"}, 0, false, 0x1234, uint64(math.Float32bits(1920)), nil},
	// mov eax, ecx
	{"reg", []string{"89c8"}, 0, false, 0x1234, 0x1234, nil},
	// mov eax, ecx with keep_negative, a positive and a negative own value
	{"reg-kn-pos", []string{"89c8"}, 0, true, 0x1234, 0x1234, nil},
	{"reg-kn-neg", []string{"89c8"}, 0, true, 0x80000005, 0x80000005, nil},
	// cmp r14, 1 ; mov eax, [rbx] ; sbb ecx, ecx    (CF of the compare is live across the site)
	{"cmp-load-sbb", []string{"4983fe01", "8b03", "19c9"}, 4, false, 0x1234, uint64(math.Float32bits(1920)), func(r14 uint64) uint64 {
		if r14 == 0 {
			return 0xffffffff
		}
		return 0
	}},
}

func TestPatchOverrideTarget(t *testing.T) {
	for _, c := range []struct {
		code string
		ok   bool
	}{
		{"8b03", true},        // mov eax, [rbx]
		{"89c8", true},        // mov eax, ecx
		{"448b7b0c", true},    // mov r15d, [rbx+0xc]
		{"488b03", false},     // mov rax, [rbx]
		{"8a03", false},       // mov al, [rbx]
		{"b878563412", false}, // mov eax, imm (a "value" site)
		{"0303", false},       // add eax, [rbx]
		{"8903", false},       // mov [rbx], eax
	} {
		_, _, err := PatchValueTarget(decodeHex(t, c.code), PatchValueOverride)
		if (err == nil) != c.ok {
			t.Errorf("%s: %v", c.code, err)
		}
	}
	l := NewArm64LifterMode(nil, AddrBase)
	l.SetPatchValueSites(map[uint64]PatchValueSite{0x30000: {Slot: 1, Kind: PatchValueData32}})
	in := disasm.Instruction{Address: 0x30000, Inst: decodeHex(t, "8b03")}
	if _, err := l.LiftInstructionToBuf(in, 0x30002, nil, nil); err == nil {
		t.Errorf("a data site on an instruction must fail the lift")
	}
}

func TestPatchOverrideExec(t *testing.T) {
	if runtime.GOOS != "darwin" || runtime.GOARCH != "arm64" {
		t.Skip("needs an arm64 Mac (host lowering)")
	}
	if _, err := exec.LookPath("clang"); err != nil {
		t.Skip("clang not found")
	}
	const slot = 5
	dir := t.TempDir()
	var asm strings.Builder
	asm.WriteString(".text\n")
	regs := []x86asm.Reg{x86asm.RAX, x86asm.RBX, x86asm.RCX, x86asm.RDX, x86asm.R14}
	// One lifter for all functions (its labels are numbered per lifter), the sites of the "on" copies.
	l := NewArm64LifterMode(nil, AddrBase)
	sites := map[uint64]PatchValueSite{}
	for i, s := range overrideSeqs {
		sites[uint64(0x40000+(2*i+1)*0x1000)+s.site] = PatchValueSite{Slot: slot, Kind: PatchValueOverride, KeepNegative: s.keepNeg}
	}
	l.SetPatchValueSites(sites)
	nfn := 0
	for _, s := range overrideSeqs {
		for range []bool{false, true} {
			entry := uint64(0x40000 + nfn*0x1000)
			lines := liftSeqCFG(t, l, execCase{name: s.name, code: s.code, cfg: true}, entry)
			fmt.Fprintf(&asm, ".globl _lifted_%d\n.p2align 2\n_lifted_%d:\n", nfn, nfn)
			asm.WriteString("    stp x29, x30, [sp, #-112]!\n    mov x29, sp\n    stp x19, x20, [sp, #16]\n    stp x21, x22, [sp, #32]\n" +
				"    stp x23, x24, [sp, #48]\n    stp x25, x26, [sp, #64]\n    stp x27, x28, [sp, #80]\n    str x0, [sp, #96]\n")
			for k, r := range regs {
				fmt.Fprintf(&asm, "    ldr %s, [x0, #%d]\n", arm64RegMap[r].X, 8*k)
			}
			asm.WriteString("    ldr x27, [x0, #40]\n    mov x28, #0\n")
			for _, ln := range lines {
				asm.WriteString(ln + "\n")
			}
			asm.WriteString("    ldr x16, [sp, #96]\n")
			for k, r := range regs {
				fmt.Fprintf(&asm, "    str %s, [x16, #%d]\n", arm64RegMap[r].X, 8*k)
			}
			asm.WriteString("    ldp x19, x20, [sp, #16]\n    ldp x21, x22, [sp, #32]\n    ldp x23, x24, [sp, #48]\n" +
				"    ldp x25, x26, [sp, #64]\n    ldp x27, x28, [sp, #80]\n    ldp x29, x30, [sp], #112\n    ret\n")
			nfn++
		}
	}
	// keep, 7, keep with junk in the low half, 9 with junk in the upper half (bit 63 clear), 0
	slotVals := []uint64{PatchOverrideKeep, 7, 0xdeadbeef00000003, 0x1234567800000009, 0}
	var c strings.Builder
	c.WriteString("#include <stdint.h>\n#include <stdio.h>\nuint64_t nps5_guest_patch_values[64];\n")
	for i := 0; i < nfn; i++ {
		fmt.Fprintf(&c, "void lifted_%d(uint64_t *);\n", i)
	}
	c.WriteString("static void (*fns[])(uint64_t *) = {")
	for i := 0; i < nfn; i++ {
		fmt.Fprintf(&c, "lifted_%d, ", i)
	}
	c.WriteString("};\nstatic const uint64_t vals[] = {")
	for _, v := range slotVals {
		fmt.Fprintf(&c, "0x%xull, ", v)
	}
	c.WriteString("};\nstatic const uint64_t rcx0[] = {")
	for _, s := range overrideSeqs {
		fmt.Fprintf(&c, "0x%xull, 0x%xull, ", s.rcx, s.rcx)
	}
	fmt.Fprintf(&c, `};
int main(void) {
    static float konst = 1920.0f;
    for (int f = 0; f < %d; f++)
        for (int v = 0; v < %d; v++)
            for (uint64_t r14 = 0; r14 < 2; r14++) {
                static uint64_t tcb[32], ctx[512];
                tcb[0x70 / 8] = (uint64_t)ctx;
                uint64_t st[6] = {0x1111, (uint64_t)&konst, rcx0[f], 0x2222, r14, (uint64_t)tcb};
                for (int i = 0; i < 64; i++) nps5_guest_patch_values[i] = 0x5a5a5a5a5a5a5a5aull + i;
                nps5_guest_patch_values[%d] = vals[v];
                fns[f](st);
                printf("%%d %%d %%llu %%llx %%d %%llx %%llx %%llx\n", f, v, (unsigned long long)r14, (unsigned long long)st[0], st[1] == (uint64_t)&konst,
                       (unsigned long long)st[2], (unsigned long long)st[3], (unsigned long long)st[4]);
            }
    return 0;
}
`, nfn, len(slotVals), slot)
	cp, sp, bin := filepath.Join(dir, "d.c"), filepath.Join(dir, "l.s"), filepath.Join(dir, "t")
	if err := os.WriteFile(cp, []byte(c.String()), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(sp, []byte(asm.String()), 0o644); err != nil {
		t.Fatal(err)
	}
	if o, err := exec.Command("clang", "-arch", "arm64", "-O1", "-w", "-o", bin, cp, sp).CombinedOutput(); err != nil {
		t.Fatalf("clang: %v\n%s\n%s", err, o, asm.String())
	}
	out, err := exec.Command(bin).Output()
	if err != nil {
		t.Fatal(err)
	}
	var want strings.Builder
	f := 0
	for _, s := range overrideSeqs {
		for _, on := range []bool{false, true} {
			for v, sv := range slotVals {
				for r14 := uint64(0); r14 < 2; r14++ {
					rax := s.own
					if on && sv>>63 == 0 && !(s.keepNeg && s.own&0x80000000 != 0) {
						rax = sv & 0xffffffff
					}
					rcx := s.rcx
					if s.rcxOut != nil {
						rcx = s.rcxOut(r14)
					}
					fmt.Fprintf(&want, "%d %d %d %x 1 %x 2222 %x\n", f, v, r14, rax, rcx, r14)
				}
			}
			f++
		}
	}
	if string(out) != want.String() {
		t.Fatalf("lifted override sites ran differently:\n got (fn value r14 rax rbx-kept rcx rdx r14)\n%s want\n%s", out, want.String())
	}
}
