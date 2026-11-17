package lifter

import (
	"fmt"

	"ps4-recomp/pkg/disasm"

	"golang.org/x/arch/x86/x86asm"
)

// Value patch points (pkg/emitter/patchpoints.go, site operations "value" and "value-f32"): a
// guest instruction that loads a constant into a register, where a run-time setting chooses the
// constant. As with the branch patch points the translated code is never modified: the lowering
// of the instruction is the ordinary one followed by a load of the register from one 64-bit slot
// of nps5_guest_patch_values. The slot starts as the constant of the setting's default option
// and is written once, before the guest starts.

// PatchValuesSymbol is the host array of 64-bit value slots the lowering reads.
const PatchValuesSymbol = "_nps5_guest_patch_values"

// PatchValueKind is the shape of a value site.
type PatchValueKind uint8

const (
	// PatchValueImm: "mov r64, imm" / "mov r32, imm"; the slot holds the integer.
	PatchValueImm PatchValueKind = iota
	// PatchValueF32: a scalar single load "movss/vmovss/movd/vmovd xmm, m32"; the low 32 bits of
	// the slot hold the float's bits.
	PatchValueF32
)

// PatchValueSite is one value site: the register the instruction at that address writes is
// loaded from slot Slot afterwards.
type PatchValueSite struct {
	Slot int
	Kind PatchValueKind
}

// SetPatchValueSites installs the value sites by guest address. The map is shared read-only by
// the clones of l.
func (l *Arm64Lifter) SetPatchValueSites(sites map[uint64]PatchValueSite) { l.patchValueSites = sites }

// PatchValueTarget checks that inst is an instruction a value site of this kind can sit on and
// returns the host register the slot is loaded into, plus the instruction's own immediate
// (PatchValueImm; 0 for PatchValueF32, whose constant is in guest memory).
//
// Both shapes only write their destination: no flags, no memory, no other register. That is what
// makes "ordinary lowering, then load the destination again" equal to the same instruction with
// another constant.
func PatchValueTarget(inst x86asm.Inst, kind PatchValueKind) (reg string, imm uint64, err error) {
	switch kind {
	case PatchValueImm:
		if inst.Op != x86asm.MOV {
			return "", 0, fmt.Errorf("%s is not a mov", inst.Op)
		}
		r, ok := inst.Args[0].(x86asm.Reg)
		v, ok2 := inst.Args[1].(x86asm.Imm)
		if !ok || !ok2 {
			return "", 0, fmt.Errorf("not a register <- immediate move")
		}
		desc, ok := arm64RegMap[r]
		if !ok || desc.IsHigh8 || (desc.Size != 4 && desc.Size != 8) {
			return "", 0, fmt.Errorf("destination %s is not a 32- or 64-bit register", r)
		}
		if desc.Size == 4 {
			return desc.W, uint64(uint32(v)), nil // a 32-bit move zero-extends, and so does "ldr w"
		}
		return desc.X, uint64(v), nil
	case PatchValueF32:
		switch inst.Op {
		case x86asm.MOVSS, x86asm.VMOVSS, x86asm.MOVD, x86asm.VMOVD:
		default:
			return "", 0, fmt.Errorf("%s is not a scalar single load", inst.Op)
		}
		r, ok := inst.Args[0].(x86asm.Reg)
		if _, isMem := inst.Args[1].(x86asm.Mem); !ok || !isMem || inst.Args[2] != nil {
			return "", 0, fmt.Errorf("not an xmm <- m32 load")
		}
		isYmm, _, _, _, _, lowS, _, ok := getVectorDesc(r)
		if !ok || isYmm {
			return "", 0, fmt.Errorf("destination %s is not an xmm register", r)
		}
		// "ldr sN" writes bits 0-31 and clears 32-127 of vN, as the load itself does; the upper
		// YMM half (v16+N) keeps what the ordinary lowering left there.
		return lowS, 0, nil
	}
	return "", 0, fmt.Errorf("unknown value kind %d", kind)
}

// patchValueSuffix returns the lines that go behind the lowering of inst:
//
//	adrp x16, values@PAGE ; add x16, x16, values@PAGEOFF ; ldr <reg>, [x16, #8*slot]
//
// NZCV is not touched and x16 is the only other register written (free between guest
// instructions, as for the branch patch points).
func (l *Arm64Lifter) patchValueSuffix(inst disasm.Instruction, vs PatchValueSite) ([]string, error) {
	reg, _, err := PatchValueTarget(inst.Inst, vs.Kind)
	if err != nil {
		return nil, fmt.Errorf("value patch point at 0x%x: %v", inst.Address, err)
	}
	if vs.Slot < 0 || vs.Slot*8 > 32760 {
		return nil, fmt.Errorf("value patch point at 0x%x: slot %d", inst.Address, vs.Slot)
	}
	return []string{
		fmt.Sprintf("    adrp x16, %s@PAGE", PatchValuesSymbol),
		fmt.Sprintf("    add x16, x16, %s@PAGEOFF", PatchValuesSymbol),
		fmt.Sprintf("    ldr %s, [x16, #%d]", reg, vs.Slot*8),
	}, nil
}
