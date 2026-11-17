package lifter

import (
	"fmt"

	"ps4-recomp/pkg/disasm"

	"golang.org/x/arch/x86/x86asm"
)

// Patch points (pkg/emitter/patchpoints.go): a guest conditional branch whose outcome a run-time
// setting can force. The translated code is never modified after it is linked (a signed text page
// that changes kills the process on macOS); instead the lowering of the branch starts with a test
// of one byte of nps5_guest_patch_flags, so the same binary runs with the patch on or off.

// PatchFlagsSymbol is the host byte array the lowering reads: one byte per setting (slot).
const PatchFlagsSymbol = "_nps5_guest_patch_flags"

// PatchSite is one patched conditional branch. With the flag byte of Slot non-zero the branch is
// always taken (Always) or never taken; with the byte zero it is the game's own branch.
type PatchSite struct {
	Slot   int
	Always bool
}

// SetPatchSites installs the patch points by guest address. The map is shared read-only by the
// clones of l.
func (l *Arm64Lifter) SetPatchSites(sites map[uint64]PatchSite) { l.patchSites = sites }

// patchPointPrefix returns the lines that go in front of the lowering of the conditional branch
// inst (after its exitFlags lines, so both successors get the flags they expect):
//
//	adrp x16, flags@PAGE ; add x16, x16, flags@PAGEOFF ; ldrb w16, [x16, #slot]
//	cbz  w16, 1f ; b <forced successor> ; 1:
//
// The forced successor is the branch target (Always) or the fall-through address. NZCV is not
// touched and x16 is the only register written: it is free between guest instructions, and the
// register of a fused compare-and-branch is x17 (bmi-peep).
func (l *Arm64Lifter) patchPointPrefix(inst disasm.Instruction, nextPC uint64, fn *disasm.Function, ps PatchSite) ([]string, error) {
	rel, ok := inst.Inst.Args[0].(x86asm.Rel)
	if !ok {
		return nil, fmt.Errorf("patch point at 0x%x: %s is not a relative conditional branch", inst.Address, inst.Inst.Op)
	}
	dest := nextPC
	if ps.Always {
		dest = uint64(int64(nextPC) + int64(rel))
	}
	skip := fmt.Sprintf("L_pp_0x%x_%d", inst.Address, l.labelCounter)
	l.labelCounter++
	lines := []string{
		fmt.Sprintf("    adrp x16, %s@PAGE", PatchFlagsSymbol),
		fmt.Sprintf("    add x16, x16, %s@PAGEOFF", PatchFlagsSymbol),
		fmt.Sprintf("    ldrb w16, [x16, #%d]", ps.Slot),
		"    cbz w16, " + skip,
	}
	lines = append(lines, l.branchTo(fn, dest)...)
	return append(lines, skip+":"), nil
}
