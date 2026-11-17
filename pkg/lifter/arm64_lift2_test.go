package lifter

import (
	"encoding/hex"
	"strings"
	"testing"

	"golang.org/x/arch/x86/x86asm"

	"ps4-recomp/pkg/disasm"
)

func liftROLoad(t *testing.T, h string, ro bool) string {
	t.Helper()
	raw, _ := hex.DecodeString(h)
	inst, err := x86asm.Decode(raw, 64)
	if err != nil {
		t.Fatal(err)
	}
	l := NewArm64Lifter(nil)
	fn := &disasm.Function{EntryAddr: 0x1000, EndAddr: 0x1100, Blocks: map[uint64]*disasm.BasicBlock{0x1000: {StartAddr: 0x1000}}}
	out, err := l.LiftInstructionToBuf(disasm.Instruction{Address: 0x1000, Inst: inst, ParityDead: true, AdjustDead: true, ROLoad: ro}, 0x1000+uint64(inst.Len), fn, nil)
	if err != nil {
		t.Fatal(err)
	}
	return strings.Join(out, "\n")
}

func TestROLoadPlain(t *testing.T) {
	defer SetROLoads(GetROLoads())
	const movRaxRip = "488b05f0ffffff" // mov rax,[rip-16]
	const cmpRaxRip = "483b05f0ffffff"
	for _, h := range []string{movRaxRip, cmpRaxRip, "8b05f0ffffff", "0fb605f0ffffff"} {
		SetROLoads(true)
		if got := liftROLoad(t, h, true); isOrdered(got) || strings.Contains(got, "dmb") {
			t.Errorf("%s: read-only load is ordered:\n%s", h, got)
		}
		if got := liftROLoad(t, h, false); !isOrdered(got) && !strings.Contains(got, "dmb") {
			t.Errorf("%s: unflagged load is plain:\n%s", h, got)
		}
		SetROLoads(false)
		if got := liftROLoad(t, h, true); !isOrdered(got) && !strings.Contains(got, "dmb") {
			t.Errorf("%s: -ro-loads=false must keep the ordering:\n%s", h, got)
		}
	}
}
