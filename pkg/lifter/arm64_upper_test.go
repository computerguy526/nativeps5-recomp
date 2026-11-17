package lifter

import (
	"strings"
	"testing"

	"golang.org/x/arch/x86/x86asm"
)

// A lowering that uses vector scratch (v31) inside a YMM function and leaves the instruction
// by ret, br or a branch to a block label must restore the saved upper halves on that path
// too (regression: it used to skip the save altogether, leaving v31 clobbered).
func TestUpperRestoreOnControlLeave(t *testing.T) {
	inst, err := x86asm.Decode([]byte{0xc5, 0xf8, 0x28, 0xc1}, 64) // vmovaps xmm0, xmm1 (an XMM VEX op)
	if err != nil {
		t.Fatal(err)
	}
	in := []string{
		"    // 0x1000: test",
		"    mov v31.16b, v1.16b",
		"    b.eq L_fn_0x1000_loc_0x1020",
		"    cbz x3, L_fn_0x1000_loc_0x1030",
		"    tbnz x3, #5, L_fn_0x1000_loc_0x1040",
		"    ldr x30, [sp], #16",
		"    b _fn_0x2000",
		"    ret",
	}
	got := fixUpperHalves(inst, append([]string(nil), in...), 0, true)
	text := strings.Join(got, "\n")
	// Every exit is preceded by the restore of v31 ("ldr q31, [sp], #16").
	if strings.Count(text, "ldr q31, [sp], #16") < 5 { // 3 conditional + tail call + ret... end restore
		t.Fatalf("restore missing on exits:\n%s", text)
	}
	lines := got
	for i, ln := range lines {
		f := strings.Fields(ln)
		if len(f) == 0 {
			continue
		}
		switch {
		case f[0] == "ret" || (f[0] == "b" && strings.HasPrefix(f[1], "_fn_")):
			// restore must be before the epilogue ldr x30 (if any) on this straight-line exit
			j := i - 1
			if strings.TrimSpace(lines[j]) == "ldr x30, [sp], #16" {
				j--
			}
			if strings.TrimSpace(lines[j]) != "ldr q31, [sp], #16" {
				t.Errorf("exit %q not preceded by restore (prev %q):\n%s", ln, lines[j], text)
			}
		}
	}
	// The save is on top of the stack when the epilogue pops x30: restore comes first.
	iRestore := strings.Index(text, "ldr q31, [sp], #16\n    ldr x30, [sp], #16")
	if iRestore < 0 {
		t.Errorf("epilogue pop not preceded by restore:\n%s", text)
	}
	// Conditional exits are inverted around the restore.
	if !strings.Contains(text, "b.ne L_uh_skip_") || !strings.Contains(text, "cbnz x3, L_uh_skip_") || !strings.Contains(text, "tbz x3, #5, L_uh_skip_") {
		t.Errorf("conditional exits not inverted:\n%s", text)
	}
	if !strings.HasPrefix(strings.TrimSpace(got[1]), "str q31, [sp, #-16]!") {
		t.Errorf("save missing:\n%s", text)
	}
}
