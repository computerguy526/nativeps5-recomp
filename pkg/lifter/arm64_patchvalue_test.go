package lifter

import (
	"encoding/hex"
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

// Guest sequences with a value site (offset site): the instruction that loads a constant, then
// instructions that make the loaded register visible in RAX / RCX.
var valueSeqs = []struct {
	name string
	code []string
	site uint64
	kind PatchValueKind
	reg  string // host register the slot is loaded into
	// own is what the sequence leaves in RAX without a patch (the float loads read 1920.0f).
	own uint64
	// out maps a slot value to RAX.
	out func(slot uint64) uint64
}{
	// movabs rdx, 0x43800000780 ; mov rax, rdx
	{"movabs-r64", []string{"48ba8007000038040000", "4889d0"}, 0, PatchValueImm, "x22", 0x43800000780, func(s uint64) uint64 { return s }},
	// mov edx, 0x12345678 ; mov rax, rdx       (a 32-bit move zero-extends: so does the slot load)
	{"mov-r32", []string{"ba78563412", "4889d0"}, 0, PatchValueImm, "w22", 0x12345678, func(s uint64) uint64 { return s & 0xffffffff }},
	// vmovss xmm0, [rbx] ; vmovq rax, xmm0      (bits 32-63 of xmm0 are cleared)
	{"vmovss", []string{"c5fa1003", "c4e1f97ec0"}, 0, PatchValueF32, "s0", uint64(math.Float32bits(1920)), func(s uint64) uint64 { return s & 0xffffffff }},
	// movss xmm0, [rbx] ; movq rax, xmm0
	{"movss", []string{"f30f1003", "66480f7ec0"}, 0, PatchValueF32, "s0", uint64(math.Float32bits(1920)), func(s uint64) uint64 { return s & 0xffffffff }},
	// vmovd xmm0, [rbx] ; vmovq rax, xmm0
	{"vmovd", []string{"c5f96e03", "c4e1f97ec0"}, 0, PatchValueF32, "s0", uint64(math.Float32bits(1920)), func(s uint64) uint64 { return s & 0xffffffff }},
	// cmp r14, 1 ; movabs rdx, imm ; sbb ecx, ecx ; mov rax, rdx   (CF of the compare is live across the site)
	{"cmp-movabs-sbb", []string{"4983fe01", "48ba8007000038040000", "19c9", "4889d0"}, 4, PatchValueImm, "x22", 0x43800000780, func(s uint64) uint64 { return s }},
}

func liftValueSeq(t *testing.T, seq int, entry uint64, slot int, on bool) []string {
	t.Helper()
	s := valueSeqs[seq]
	l := NewArm64LifterMode(nil, AddrBase)
	if on {
		l.SetPatchValueSites(map[uint64]PatchValueSite{entry + s.site: {Slot: slot, Kind: s.kind}})
	} else {
		l.SetPatchValueSites(map[uint64]PatchValueSite{entry + 0x999: {Slot: 1, Kind: s.kind}}) // a site elsewhere changes nothing
	}
	return liftSeqCFG(t, l, execCase{name: s.name, code: s.code, cfg: true}, entry)
}

// The patched lowering is the unpatched one plus three lines behind the instruction: the slot's
// address in x16 and one load of the destination. Nothing else moves, no flags, no other register.
func TestPatchValueText(t *testing.T) {
	for si, s := range valueSeqs {
		const entry = 0x20000
		base := liftSeqCFG(t, NewArm64LifterMode(nil, AddrBase), execCase{name: s.name, code: s.code, cfg: true}, entry)
		if got := liftValueSeq(t, si, entry, 0, false); strings.Join(got, "\n") != strings.Join(base, "\n") {
			t.Errorf("%s: a value site at another address changed the lowering", s.name)
		}
		for _, slot := range []int{0, 5, 63} {
			got := liftValueSeq(t, si, entry, slot, true)
			want := []string{
				"    adrp x16, _nps5_guest_patch_values@PAGE",
				"    add x16, x16, _nps5_guest_patch_values@PAGEOFF",
				fmt.Sprintf("    ldr %s, [x16, #%d]", s.reg, 8*slot),
			}
			at := -1
			for i, ln := range got {
				if ln == want[0] {
					if at >= 0 {
						t.Fatalf("%s: two value loads", s.name)
					}
					at = i
				}
			}
			if at < 0 || at+3 > len(got) || got[at+1] != want[1] || got[at+2] != want[2] {
				t.Fatalf("%s slot %d: want\n%s\nin\n%s", s.name, slot, strings.Join(want, "\n"), strings.Join(got, "\n"))
			}
			rest := append(append([]string{}, got[:at]...), got[at+3:]...)
			if strings.Join(rest, "\n") != strings.Join(base, "\n") {
				t.Errorf("%s: the text around the value load differs from the unpatched lowering:\n%s\n--- unpatched\n%s", s.name,
					strings.Join(rest, "\n"), strings.Join(base, "\n"))
			}
		}
	}
}

func decodeHex(t *testing.T, h string) x86asm.Inst {
	t.Helper()
	b, err := hex.DecodeString(h)
	if err != nil {
		t.Fatal(err)
	}
	in, err := disasm.Decode(b)
	if err != nil {
		t.Fatalf("decode %s: %v", h, err)
	}
	return in
}

// The shapes a value site accepts, the register and the instruction's own constant it reports,
// and the refusals: anything that is not a plain constant load into a 32/64-bit or xmm register.
func TestPatchValueTarget(t *testing.T) {
	ok := []struct {
		hex  string
		kind PatchValueKind
		reg  string
		imm  uint64
	}{
		{"48ba8007000038040000", PatchValueImm, "x22", 0x43800000780}, // movabs rdx
		{"48b98007000038040000", PatchValueImm, arm64RegMap[x86asm.RCX].X, 0x43800000780},
		{"49bfefcdab8967452301", PatchValueImm, arm64RegMap[x86asm.R15].X, 0x0123456789abcdef},
		{"ba78563412", PatchValueImm, "w22", 0x12345678},
		{"b8ffffffff", PatchValueImm, arm64RegMap[x86asm.EAX].W, 0xffffffff}, // mov eax, -1: zero-extended
		{"48c7c0ffffffff", PatchValueImm, arm64RegMap[x86asm.RAX].X, 0xffffffffffffffff},
		{"c5fa10051e15f900", PatchValueF32, "s0", 0}, // vmovss xmm0, [rip+..]
		{"c5f96e05a8fb2701", PatchValueF32, "s0", 0}, // vmovd xmm0, [rip+..]
		{"f30f100d10000000", PatchValueF32, "s1", 0}, // movss xmm1, [rip+0x10]
		{"c57a103d10000000", PatchValueF32, "s15", 0},
	}
	for _, c := range ok {
		reg, imm, err := PatchValueTarget(decodeHex(t, c.hex), c.kind)
		if err != nil || reg != c.reg || imm != c.imm {
			t.Errorf("%s: %q 0x%x %v, want %q 0x%x", c.hex, reg, imm, err, c.reg, c.imm)
		}
	}
	bad := []struct {
		hex  string
		kind PatchValueKind
	}{
		{"4889d0", PatchValueImm},               // mov rax, rdx: no immediate
		{"48c70301000000", PatchValueImm},       // mov qword [rbx], 1: memory destination
		{"b001", PatchValueImm},                 // mov al, 1
		{"b401", PatchValueImm},                 // mov ah, 1
		{"66b80100", PatchValueImm},             // mov ax, 1
		{"4883c001", PatchValueImm},             // add rax, 1
		{"7405", PatchValueImm},                 // je
		{"48ba8007000038040000", PatchValueF32}, // a mov is not a float load
		{"c5fa10c1", PatchValueF32},             // vmovss xmm0, xmm0, xmm1: register form
		{"c5fa1103", PatchValueF32},             // vmovss [rbx], xmm0: a store
		{"c5fb1003", PatchValueF32},             // vmovsd: a double
		{"c5f96ec0", PatchValueF32},             // vmovd xmm0, eax: no memory
		{"c5fa10051e15f900", PatchValueImm},     // a float load is not a mov
		{"c5fa10051e15f900", PatchValueKind(9)}, // unknown kind
	}
	for _, c := range bad {
		if reg, _, err := PatchValueTarget(decodeHex(t, c.hex), c.kind); err == nil {
			t.Errorf("%s kind %d accepted (%s)", c.hex, c.kind, reg)
		}
	}
}

// A value site on an instruction of another shape fails the lift (the setting would do nothing),
// and so does a slot outside the table.
func TestPatchValueRefusedAtLift(t *testing.T) {
	lift := func(code string, vs PatchValueSite) error {
		l := NewArm64LifterMode(nil, AddrBase)
		l.SetPatchValueSites(map[uint64]PatchValueSite{0x30000: vs})
		in := disasm.Instruction{Address: 0x30000, Inst: decodeHex(t, code)}
		_, err := l.LiftInstructionToBuf(in, 0x30000+uint64(in.Inst.Len), nil, nil)
		return err
	}
	if err := lift("48ba8007000038040000", PatchValueSite{Slot: 2, Kind: PatchValueImm}); err != nil {
		t.Fatalf("a good site: %v", err)
	}
	for _, c := range []struct {
		code string
		vs   PatchValueSite
	}{
		{"4889d0", PatchValueSite{Slot: 0, Kind: PatchValueImm}},
		{"48ba8007000038040000", PatchValueSite{Slot: 0, Kind: PatchValueF32}},
		{"48ba8007000038040000", PatchValueSite{Slot: -1, Kind: PatchValueImm}},
		{"48ba8007000038040000", PatchValueSite{Slot: 5000, Kind: PatchValueImm}},
	} {
		if err := lift(c.code, c.vs); err == nil || !strings.Contains(err.Error(), "value patch point at 0x30000") {
			t.Errorf("%s %+v: %v", c.code, c.vs, err)
		}
	}
	// An instruction the one before folded away cannot carry a site.
	l := NewArm64LifterMode(nil, AddrBase)
	l.SetPatchValueSites(map[uint64]PatchValueSite{0x30000: {Slot: 0, Kind: PatchValueImm}})
	l.fuseSkip = 0x30000
	in := disasm.Instruction{Address: 0x30000, Inst: decodeHex(t, "48ba8007000038040000")}
	if _, err := l.LiftInstructionToBuf(in, 0x3000a, nil, nil); err == nil || !strings.Contains(err.Error(), "folded") {
		t.Errorf("fused site: %v", err)
	}
}

// Runs the lowerings on the host. Without a site, and with a site whose slot holds the
// instruction's own constant, the sequence gives what x86 gives; with another value in the slot
// the destination register has that value (32-bit and float destinations: its low 32 bits, upper
// bits clear, whatever the slot's upper half holds). The neighbouring slots are not read, the
// other registers and the carry of a compare before the site are untouched.
func TestPatchValueExec(t *testing.T) {
	if runtime.GOOS != "darwin" || runtime.GOARCH != "arm64" {
		t.Skip("needs an arm64 Mac (host lowering)")
	}
	if _, err := exec.LookPath("clang"); err != nil {
		t.Skip("clang not found")
	}
	const slot = 3
	dir := t.TempDir()
	var asm strings.Builder
	asm.WriteString(".text\n")
	regs := []x86asm.Reg{x86asm.RAX, x86asm.RBX, x86asm.RCX, x86asm.RDX, x86asm.R14}
	nfn := 0
	for si := range valueSeqs {
		for _, on := range []bool{false, true} {
			lines := liftValueSeq(t, si, uint64(0x40000+nfn*0x1000), slot, on)
			fmt.Fprintf(&asm, ".globl _lifted_%d\n.p2align 2\n_lifted_%d:\n", nfn, nfn)
			asm.WriteString("    stp x29, x30, [sp, #-112]!\n    mov x29, sp\n    stp x19, x20, [sp, #16]\n    stp x21, x22, [sp, #32]\n" +
				"    stp x23, x24, [sp, #48]\n    stp x25, x26, [sp, #64]\n    stp x27, x28, [sp, #80]\n    str x0, [sp, #96]\n")
			for k, r := range regs {
				fmt.Fprintf(&asm, "    ldr %s, [x0, #%d]\n", arm64RegMap[r].X, 8*k)
			}
			// xmm0 starts as all ones (the loads must clear bits 32-127); FS base: a TCB whose +0x70 slot points at a context.
			asm.WriteString("    movi v0.16b, #0xff\n    ldr x27, [x0, #40]\n    mov x28, #0\n")
			for _, ln := range lines {
				asm.WriteString(ln + "\n")
			}
			asm.WriteString("    ldr x16, [sp, #96]\n")
			for k, r := range regs {
				fmt.Fprintf(&asm, "    str %s, [x16, #%d]\n", arm64RegMap[r].X, 8*k)
			}
			asm.WriteString("    mov x17, v0.d[1]\n    str x17, [x16, #48]\n")
			asm.WriteString("    ldp x19, x20, [sp, #16]\n    ldp x21, x22, [sp, #32]\n    ldp x23, x24, [sp, #48]\n" +
				"    ldp x25, x26, [sp, #64]\n    ldp x27, x28, [sp, #80]\n    ldp x29, x30, [sp], #112\n    ret\n")
			nfn++
		}
	}
	slotVals := []uint64{0, 0x43800000780, 0x2d000000500, uint64(math.Float32bits(1280)), 0xdeadbeef00000000 | uint64(math.Float32bits(1600)), ^uint64(0)}
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
	fmt.Fprintf(&c, `};
int main(void) {
    static float konst = 1920.0f;
    for (int f = 0; f < %d; f++)
        for (int v = 0; v < %d; v++)
            for (uint64_t r14 = 0; r14 < 2; r14++) {
                static uint64_t tcb[32], ctx[512];
                tcb[0x70 / 8] = (uint64_t)ctx;
                /* rax rbx rcx rdx r14, tcb, out: upper half of xmm0 */
                uint64_t st[7] = {0x1111, (uint64_t)&konst, 0x1234, 0x2222, r14, (uint64_t)tcb, 0};
                for (int i = 0; i < 64; i++) nps5_guest_patch_values[i] = 0x5a5a5a5a5a5a5a5aull + i; /* neighbours are not read */
                nps5_guest_patch_values[%d] = vals[v];
                fns[f](st);
                printf("%%d %%d %%llu %%llx %%d %%llx %%llx %%llx\n", f, v, (unsigned long long)r14, (unsigned long long)st[0], st[1] == (uint64_t)&konst,
                       (unsigned long long)st[2], (unsigned long long)st[4], (unsigned long long)st[6]);
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
	checks, f := 0, 0
	for _, s := range valueSeqs {
		for _, on := range []bool{false, true} {
			for v, sv := range slotVals {
				for r14 := uint64(0); r14 < 2; r14++ {
					rax := s.own
					if on {
						rax = s.out(sv)
					}
					rcx := uint64(0x1234)
					if s.name == "cmp-movabs-sbb" { // sbb ecx, ecx = -CF of "cmp r14, 1"
						rcx = 0
						if r14 == 0 {
							rcx = 0xffffffff
						}
					}
					upper := ^uint64(0) // xmm0 bits 64-127: untouched by the integer sequences, cleared by the float loads
					if s.kind == PatchValueF32 {
						upper = 0
					}
					fmt.Fprintf(&want, "%d %d %d %x 1 %x %x %x\n", f, v, r14, rax, rcx, r14, upper)
					checks++
				}
			}
			f++
		}
	}
	if string(out) != want.String() {
		t.Fatalf("lifted value sites ran differently:\n got (fn value r14 rax rbx-kept rcx r14 xmm0-upper)\n%s want\n%s", out, want.String())
	}
	t.Logf("%d executed cases", checks)
}
