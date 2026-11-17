package emitter

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"ps4-recomp/pkg/lifter"
)

// The trampoline of every (call model, address mode) pair assembles, and the flat one keeps the
// invariants the runtime relies on.
func TestTrampolineAssemblesInEveryMode(t *testing.T) {
	if _, err := exec.LookPath("clang"); err != nil {
		t.Skip("clang not found")
	}
	oldCall := lifter.GetCallModel()
	defer lifter.SetCallModel(oldCall)
	dir := t.TempDir()
	for _, cm := range []lifter.CallModel{lifter.CallNested, lifter.CallFlat} {
		for _, am := range []lifter.AddrMode{lifter.AddrBase, lifter.AddrIdentity} {
			lifter.SetCallModel(cm)
			p := filepath.Join(dir, "tramp_"+cm.String()+"_"+am.String()+".s")
			if err := (&AsmEmitter{lifter: lifter.NewArm64LifterMode(nil, am)}).EmitTrampoline(p); err != nil {
				t.Fatal(err)
			}
			if out, err := exec.Command("clang", "-target", "arm64-apple-darwin", "-c", p, "-o", p+".o").CombinedOutput(); err != nil {
				t.Errorf("%s/%s: %v\n%s", cm, am, err, out)
			}
			src, _ := os.ReadFile(p)
			if am == lifter.AddrIdentity && strings.Contains(string(src), "[x28, x") {
				t.Errorf("%s/identity: a guest access still goes through x28", cm)
			}
		}
	}
}

func TestFlatTrampolineShape(t *testing.T) {
	flat := TrampolineSource(lifter.CallFlat)
	if TrampolineSource(lifter.CallNested) != trampolineAsm {
		t.Error("the nested trampoline is trampolineAsm, unchanged")
	}
	for _, sym := range []string{"_recomp_enter_guest:", "_recomp_enter_guest_ret:", "_recomp_call_shim:", "_recomp_indirect_call:",
		"_recomp_indirect_jump:", "_recomp_ret_slow:", "_recomp_dispatch_continue:", "_recomp_resume_guest:", "_g_recomp_call_model:", "_recomp_indirect_call_ic:", "_g_recomp_ic:",
		"_recomp_vpcmpistri:", "_recomp_vpcmpe_stri:", "_recomp_vaeskeygenassist:"} {
		if n := strings.Count(flat, "\n"+sym); n != 1 {
			t.Errorf("flat trampoline defines %s %d times", sym, n)
		}
	}
	// x29 is the shadow stack pointer for as long as the pc is in guest code: only host-section
	// code may point it at a frame record.
	section := "host"
	for _, ln := range strings.Split(flat, "\n") {
		switch strings.TrimSpace(ln) {
		case "GUEST_SECTION":
			section = "guest"
		case "HOST_SECTION", ".text":
			section = "host"
		}
		if section == "guest" && strings.Contains(ln, "mov  x29, sp") {
			t.Errorf("guest-section code makes x29 a frame pointer: %q", ln)
		}
	}
	// The host stack is flat: no guest-section code of the flat trampoline keeps a host frame
	// across a branch into guest code, and the nested function epilogue does not appear.
	if strings.Contains(flat, "ldr  x30, [sp], #16") {
		t.Error("flat trampoline pops a per-function host frame")
	}
}

// The flat shim stub is a call entry followed by the jump entry at FlatJumpEntryOffset.
func TestFlatShimStub(t *testing.T) {
	old := lifter.GetCallModel()
	lifter.SetCallModel(lifter.CallFlat)
	defer lifter.SetCallModel(old)
	var b strings.Builder
	writeShimStub(&b, "_fn_0x1000", "shim_foo", 0x1000)
	lines := strings.Split(strings.TrimSpace(b.String()), "\n")
	want := append([]string{".globl _fn_0x1000", "_fn_0x1000:"}, lifter.FlatCallEntry()...)
	for i, w := range want {
		if i >= len(lines) || lines[i] != w {
			t.Fatalf("stub line %d = %q, want %q\n%s", i, lines[i], w, b.String())
		}
	}
	if len(lifter.FlatCallEntry())*4 != lifter.FlatJumpEntryOffset {
		t.Errorf("call entry is %d instructions, FlatJumpEntryOffset = %d", len(lifter.FlatCallEntry()), lifter.FlatJumpEntryOffset)
	}
	if !strings.HasSuffix(strings.TrimSpace(b.String()), "b    _recomp_call_shim") {
		t.Errorf("stub does not end in the bridge:\n%s", b.String())
	}
}

func TestShimStubViaGOT(t *testing.T) {
	ShimStubViaGOT = true
	defer func() { ShimStubViaGOT = false }()
	var b strings.Builder
	writeShimStub(&b, "_fn_0x1000", "shim_foo", 0x1000)
	if !strings.Contains(b.String(), "_shim_foo@GOTPAGE") || strings.Contains(b.String(), "@PAGEOFF") {
		t.Fatalf("stub does not use the GOT:\n%s", b.String())
	}
}
