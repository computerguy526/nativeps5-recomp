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
	// PatchValueOverride: "mov r32, r32" / "mov r32, m32" whose result a setting may replace while
	// the game runs. The slot holds either the replacement (bits 0-31, bit 63 clear) or "keep"
	// (bit 63 set: the instruction's own result stays). With KeepNegative an own result with bit 31
	// set (a negative int32) stays too. The runtime may rewrite the slot at any time (a live
	// setting: nps5_rscale.c), the translated code reads it every time the instruction runs.
	PatchValueOverride
	// PatchValueData32: not an instruction: a 32-bit word of the image's data that the runtime
	// writes (the slot holds the word; bit 63 = keep). No lowering exists for it.
	PatchValueData32
)

// PatchOverrideKeep is the slot content of an override or data site that keeps the game's value.
const PatchOverrideKeep = ^uint64(0)

// PatchValueSite is one value site: the register the instruction at that address writes is
// loaded from slot Slot afterwards.
type PatchValueSite struct {
	Slot         int
	Kind         PatchValueKind
	KeepNegative bool // PatchValueOverride only
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
	case PatchValueOverride:
		if inst.Op != x86asm.MOV {
			return "", 0, fmt.Errorf("%s is not a mov", inst.Op)
		}
		r, ok := inst.Args[0].(x86asm.Reg)
		if !ok {
			return "", 0, fmt.Errorf("the destination is not a register")
		}
		desc, ok := arm64RegMap[r]
		if !ok || desc.IsHigh8 || desc.Size != 4 {
			return "", 0, fmt.Errorf("destination %s is not a 32-bit register", r)
		}
		switch src := inst.Args[1].(type) {
		case x86asm.Reg:
			if sd, ok := arm64RegMap[src]; !ok || sd.Size != 4 || sd.IsHigh8 {
				return "", 0, fmt.Errorf("source %s is not a 32-bit register", src)
			}
		case x86asm.Mem:
			if inst.MemBytes != 4 {
				return "", 0, fmt.Errorf("not a 32-bit load")
			}
		default:
			return "", 0, fmt.Errorf("not a register <- register or register <- m32 move")
		}
		// A 32-bit mov writes the register zero-extended and nothing else: no flags, no memory.
		return desc.W, 0, nil
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
	if vs.Kind == PatchValueOverride {
		// adrp/add x16 ; ldr x16, [x16, #8*slot] ; tbnz x16, #63, keep ; [tbnz wR, #31, keep] ;
		// mov wR, w16 ; keep:   (branches only: NZCV untouched, x16 the only other register)
		keep := fmt.Sprintf("L_pv_0x%x_%d", inst.Address, l.labelCounter)
		l.labelCounter++
		out := []string{
			fmt.Sprintf("    adrp x16, %s@PAGE", PatchValuesSymbol),
			fmt.Sprintf("    add x16, x16, %s@PAGEOFF", PatchValuesSymbol),
			fmt.Sprintf("    ldr x16, [x16, #%d]", vs.Slot*8),
			"    tbnz x16, #63, " + keep,
		}
		if vs.KeepNegative {
			out = append(out, fmt.Sprintf("    tbnz %s, #31, %s", reg, keep))
		}
		return append(out, fmt.Sprintf("    mov %s, w16", reg), keep+":"), nil
	}
	if vs.Kind == PatchValueData32 {
		return nil, fmt.Errorf("value patch point at 0x%x: a data site is not an instruction", inst.Address)
	}
	return []string{
		fmt.Sprintf("    adrp x16, %s@PAGE", PatchValuesSymbol),
		fmt.Sprintf("    add x16, x16, %s@PAGEOFF", PatchValuesSymbol),
		fmt.Sprintf("    ldr %s, [x16, #%d]", reg, vs.Slot*8),
	}, nil
}
