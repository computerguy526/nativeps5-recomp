package lifter

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"golang.org/x/arch/x86/x86asm"
)

// Two guest sequences with a conditional branch at entry+3 (A) / entry+4 (B):
//
//	A: test r14b,r14b ; je +5 ; mov eax,1 ; mov ebx,7          (flags dead after the branch: compare-and-branch forms)
//	B: cmp r14,1 ; jb +5 ; mov eax,1 ; sbb ecx,ecx ; mov ebx,7  (CF live across the branch: carry sense at the exit)
var patchSeqs = []struct {
	name string
	code []string
	site uint64 // offset of the branch
}{
	{"test-je", []string{"4584f6", "7405", "b801000000", "bb07000000"}, 3},
	{"cmp-jb-sbb", []string{"4983fe01", "7205", "b801000000", "19c9", "bb07000000"}, 4},
}

func liftPatchSeq(t *testing.T, seq int, entry uint64, site *PatchSite) []string {
	t.Helper()
	l := NewArm64LifterMode(nil, AddrBase)
	if site != nil {
		l.SetPatchSites(map[uint64]PatchSite{entry + patchSeqs[seq].site: *site})
	} else {
		l.SetPatchSites(map[uint64]PatchSite{entry + 0x999: {Slot: 1, Always: true}}) // a site elsewhere changes nothing
	}
	return liftSeqCFG(t, l, execCase{name: patchSeqs[seq].name, code: patchSeqs[seq].code, cfg: true}, entry)
}

// The patched lowering is the unpatched one plus the flag test in front of the branch, and a
// profile with sites elsewhere leaves the text alone.
func TestPatchPointText(t *testing.T) {
	for si, s := range patchSeqs {
		const entry = 0x20000
		plain := NewArm64LifterMode(nil, AddrBase)
		base := liftSeqCFG(t, plain, execCase{name: s.name, code: s.code, cfg: true}, entry)
		if got := liftPatchSeq(t, si, entry, nil); strings.Join(got, "\n") != strings.Join(base, "\n") {
			t.Errorf("%s: a patch site at another address changed the lowering", s.name)
		}
		for _, always := range []bool{true, false} {
			slot := 5
			got := liftPatchSeq(t, si, entry, &PatchSite{Slot: slot, Always: always})
			dest := entry + s.site + 2 // fall-through
			if always {
				dest += 5
			}
			want := []string{
				"    adrp x16, _nps5_guest_patch_flags@PAGE",
				"    add x16, x16, _nps5_guest_patch_flags@PAGEOFF",
				fmt.Sprintf("    ldrb w16, [x16, #%d]", slot),
				"    cbz w16, L_pp_0x%x_N",
				fmt.Sprintf("    b L_fn_0x%x_loc_0x%x", entry, dest),
				"L_pp_0x%x_N:",
			}
			// Find the prefix and check that removing it gives the unpatched text.
			at := -1
			for i, ln := range got {
				if strings.Contains(ln, PatchFlagsSymbol+"@PAGE") && !strings.Contains(ln, "PAGEOFF") {
					at = i
					break
				}
			}
			if at < 0 || at+len(want) > len(got) {
				t.Fatalf("%s always=%t: no patch point in\n%s", s.name, always, strings.Join(got, "\n"))
			}
			for k := 0; k < len(want); k++ {
				g := got[at+k]
				w := want[k]
				if strings.Contains(w, "%x") {
					pre := strings.Split(fmt.Sprintf(w, entry+s.site), "_N")[0]
					if !strings.HasPrefix(g, pre) {
						t.Errorf("%s always=%t line %d: %q, want prefix %q", s.name, always, k, g, pre)
					}
				} else if g != w {
					t.Errorf("%s always=%t line %d: %q, want %q", s.name, always, k, g, w)
				}
			}
			rest := append(append([]string{}, got[:at]...), got[at+len(want):]...)
			if strings.Join(rest, "\n") != strings.Join(base, "\n") {
				t.Errorf("%s always=%t: the text around the patch point differs from the unpatched lowering:\n%s\n--- unpatched\n%s", s.name, always,
					strings.Join(rest, "\n"), strings.Join(base, "\n"))
			}
			for _, ln := range got[at : at+len(want)] {
				for _, bad := range []string{"cmp ", "cmn ", "tst ", "adds ", "subs ", "ands ", "msr ", "x17", "w17"} {
					if strings.Contains(ln, bad) {
						t.Errorf("patch point line %q touches flags or a register other than x16", ln)
					}
				}
			}
		}
	}
}

// A patch point on something that is not a relative conditional branch is refused at the site.
func TestPatchPointNotOnOtherInstructions(t *testing.T) {
	l := NewArm64LifterMode(nil, AddrBase)
	const entry = 0x30000
	// A site on "mov eax,1" is never consulted: the lowering has no patch point (the emitter turns that into an error).
	l.SetPatchSites(map[uint64]PatchSite{entry + 5: {Slot: 0, Always: true}})
	out := liftSeqCFG(t, l, execCase{name: "x", code: patchSeqs[0].code, cfg: true}, entry)
	if strings.Contains(strings.Join(out, "\n"), PatchFlagsSymbol) {
		t.Fatal("patch point emitted on a mov")
	}
}

// Runs the lowerings on the host: with the flag byte 0 the patched code behaves as the game's,
// with 1 the branch is always / never taken; registers and the carry the branch leaves live are
// what x86 gives on the path taken.
func TestPatchPointExec(t *testing.T) {
	if runtime.GOOS != "darwin" || runtime.GOARCH != "arm64" {
		t.Skip("needs an arm64 Mac (host lowering)")
	}
	if _, err := exec.LookPath("clang"); err != nil {
		t.Skip("clang not found")
	}
	dir := t.TempDir()
	var asm strings.Builder
	asm.WriteString(".text\n")
	variants := []*PatchSite{nil, {Slot: 3, Always: true}, {Slot: 3, Always: false}}
	regs := []x86asm.Reg{x86asm.RAX, x86asm.RBX, x86asm.RCX, x86asm.R14}
	nfn := 0
	for si := range patchSeqs {
		for _, v := range variants {
			lines := liftPatchSeq(t, si, uint64(0x40000+nfn*0x1000), v)
			fmt.Fprintf(&asm, ".globl _lifted_%d\n.p2align 2\n_lifted_%d:\n", nfn, nfn)
			asm.WriteString("    stp x29, x30, [sp, #-112]!\n    mov x29, sp\n    stp x19, x20, [sp, #16]\n    stp x21, x22, [sp, #32]\n" +
				"    stp x23, x24, [sp, #48]\n    stp x25, x26, [sp, #64]\n    stp x27, x28, [sp, #80]\n    str x0, [sp, #96]\n")
			for k, r := range regs {
				fmt.Fprintf(&asm, "    ldr %s, [x0, #%d]\n", arm64RegMap[r].X, 8*k)
			}
			asm.WriteString("    ldr x27, [x0, #32]\n    mov x28, #0\n") // FS base: a TCB whose +0x70 slot points at a context (lazy PF/AF bytes)
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
	var c strings.Builder
	c.WriteString("#include <stdint.h>\n#include <stdio.h>\nuint8_t nps5_guest_patch_flags[32];\n")
	for i := 0; i < nfn; i++ {
		fmt.Fprintf(&c, "void lifted_%d(uint64_t *);\n", i)
	}
	c.WriteString("static void (*fns[])(uint64_t *) = {")
	for i := 0; i < nfn; i++ {
		fmt.Fprintf(&c, "lifted_%d, ", i)
	}
	fmt.Fprintf(&c, `};
int main(void) {
    for (int f = 0; f < %d; f++)
        for (int flag = 0; flag < 2; flag++)
            for (uint64_t r14 = 0; r14 < 2; r14++) {
                static uint64_t tcb[32], ctx[512];
                tcb[0x70 / 8] = (uint64_t)ctx;
                uint64_t st[5] = {5, 0, 0x1234, r14, (uint64_t)tcb};
                nps5_guest_patch_flags[3] = (uint8_t)(flag * 0x80); /* any non-zero byte is "on" */
                nps5_guest_patch_flags[2] = nps5_guest_patch_flags[4] = (uint8_t)!flag; /* neighbours are not read */
                fns[f](st);
                printf("%%d %%d %%llu %%llx %%llx %%llx\n", f, flag, (unsigned long long)r14, (unsigned long long)st[0], (unsigned long long)st[1], (unsigned long long)st[2]);
            }
    return 0;
}
`, nfn)
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
	checks := 0
	f := 0
	for si := range patchSeqs {
		for _, v := range variants {
			for flag := 0; flag < 2; flag++ {
				for r14 := uint64(0); r14 < 2; r14++ {
					taken := r14 == 0 // je after test r14b / jb after cmp r14,1
					if v != nil && flag == 1 {
						taken = v.Always
					}
					rax := uint64(1)
					if taken {
						rax = 5
					}
					rcx := uint64(0x1234)
					if si == 1 { // sbb ecx,ecx: -CF of the compare, whatever path was forced
						rcx = 0
						if r14 == 0 {
							rcx = 0xffffffff
						}
					}
					fmt.Fprintf(&want, "%d %d %d %x %x %x\n", f, flag, r14, rax, 7, rcx)
					checks++
				}
			}
			f++
		}
	}
	if string(out) != want.String() {
		t.Fatalf("lifted patch points ran differently:\n got (fn flag r14 rax rbx rcx)\n%s want\n%s", out, want.String())
	}
	t.Logf("%d executed cases", checks)
}
