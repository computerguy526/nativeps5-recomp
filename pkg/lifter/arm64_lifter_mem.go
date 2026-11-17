package lifter

import (
	"fmt"
	"regexp"

	"golang.org/x/arch/x86/x86asm"
)

// Guest memory operands.
//
// Every guest memory access of a lowering goes through this file. Two address modes are selected
// at translate time (per lifter: NewArm64LifterMode, SetAddrMode):
//
//	AddrBase      guest address a is reached as [x28, a]: x28 holds the host base of guest
//	              memory (the original PS4Recomp scheme).
//	AddrIdentity  guest VA == host VA: the access is [a, #0]; x28 is not read by lowerings
//	              (the hand-written trampolines are converted by RewriteGuestMem). The address
//	              register may be the guest register itself, and a small displacement can be
//	              folded into the operand ([x26, #disp]) for stack accesses.
//
// The identity form is spelled [xN, #0] rather than [xN] on purpose: it is textually distinct
// from the host-pointer accesses ([xN], [xN, #imm] on the context or the x87 area), so the TSO
// rewriter (arm64_lifter_tso.go) can tell a guest access from a host one by shape, as it does
// for [x28, xN] in base mode.
//
// What L2 must change for identity mode to run: the loader and runtime must place guest memory
// at the host VA the guest image was linked/rebased for (0x70_0000_0000 and up); x28 may stay 0
// (lowerings never read it in identity mode; the trampolines load it from the context but only
// use it through RewriteGuestMem'd forms); and every emitted absolute constant (return
// addresses, lea rip, dispatch keys, relocation addends, MemoryImage indexing) must carry that
// bias. The lifter emits the guest address it is given; it does not rebase.

// AddrMode selects how guest addresses map to host addresses.
type AddrMode int

const (
	AddrBase AddrMode = iota
	AddrIdentity
)

func (m AddrMode) String() string {
	if m == AddrIdentity {
		return "identity"
	}
	return "base"
}

// ParseAddrMode parses "base" or "identity".
func ParseAddrMode(s string) (AddrMode, error) {
	switch s {
	case "", "base":
		return AddrBase, nil
	case "identity":
		return AddrIdentity, nil
	}
	return AddrBase, fmt.Errorf("unknown address mode %q (want base or identity)", s)
}

// SetAddrMode selects this lifter's address mode (per lifter: lifters in the same process may
// use different modes). Call it before lifting.
func (l *Arm64Lifter) SetAddrMode(m AddrMode) { l.addrMode = m }

// AddrMode returns this lifter's address mode (AddrBase for a nil lifter).
func (l *Arm64Lifter) AddrMode() AddrMode {
	if l == nil {
		return AddrBase
	}
	return l.addrMode
}

// NewArm64LifterMode creates a lifter with the given address mode.
func NewArm64LifterMode(knownFuncs map[uint64]bool, m AddrMode) *Arm64Lifter {
	l := NewArm64Lifter(knownFuncs)
	l.addrMode = m
	return l
}

// gm is the addressing operand of a guest memory access whose guest address is in addrReg.
func (l *Arm64Lifter) gm(addrReg string) string {
	if l.AddrMode() == AddrIdentity {
		return "[" + addrReg + ", #0]"
	}
	return "[x28, " + addrReg + "]"
}

// gAdd returns the line that turns the guest address in src into a host pointer in dst (for
// instructions that take a plain base register, such as ldxr or ld1). In identity mode the
// pointer is the address itself.
func (l *Arm64Lifter) gAdd(dst, src string) string {
	if l.AddrMode() == AddrIdentity {
		if dst == src {
			return "    // guest==host"
		}
		return fmt.Sprintf("    mov %s, %s", dst, src)
	}
	return fmt.Sprintf("    add %s, x28, %s", dst, src)
}

var (
	rewriteAddRe = regexp.MustCompile(`(?m)^(\s*)add\s+(x\d+),\s*x28,\s*(x\d+)(\s*(?://.*)?)$`)
	rewriteRefRe = regexp.MustCompile(`\[x28,\s*(x\d+)\]`)
)

// RewriteGuestMem converts hand-written asm text (the runtime trampolines) from base form to the
// current address mode. In base mode it returns the text unchanged.
func (l *Arm64Lifter) RewriteGuestMem(text string) string {
	if l.AddrMode() != AddrIdentity {
		return text
	}
	text = rewriteAddRe.ReplaceAllStringFunc(text, func(s string) string {
		m := rewriteAddRe.FindStringSubmatch(s)
		if m[2] == m[3] {
			return m[1] + "// guest==host" + m[4]
		}
		return fmt.Sprintf("%smov %s, %s%s", m[1], m[2], m[3], m[4])
	})
	return rewriteRefRe.ReplaceAllString(text, "[$1]")
}

// foldableDisp reports whether disp fits the unscaled (ldur/stur) form every access size takes.
func foldableDisp(disp int64) bool { return disp >= -256 && disp <= 255 }

// memOperand lowers an x86 memory operand to an ARM addressing operand plus the setup lines it
// needs. scratch receives the computed guest address when one is needed (many lowerings rely on
// the address staying in it for a second access, so it is only skipped when direct is set: the
// caller promises not to read the scratch register afterwards). stack marks accesses that are
// thread-private stack or frame data and never subject to the TSO rewrite.
//
// In base mode the result is always [x28, scratch]. In identity mode with direct set, a plain
// [base] operand needs no address computation at all, and a stack [base+disp] folds the displacement into the
// operand. (A guest access with a folded nonzero displacement would not be recognised by the
// TSO rewriter, so non-stack accesses never fold.)
func (l *Arm64Lifter) memOperand(mem x86asm.Mem, nextPC uint64, scratch string, direct, stack bool) (string, []string) {
	if direct && l.AddrMode() == AddrIdentity && mem.Segment == 0 && mem.Index == 0 && mem.Base != 0 && mem.Base != x86asm.RIP {
		if d, ok := arm64RegMap[mem.Base]; ok && d.Size == 8 && !d.IsHigh8 {
			disp := int64(int32(mem.Disp))
			if disp == 0 {
				return "[" + d.X + ", #0]", nil
			}
			if stack && foldableDisp(disp) {
				return fmt.Sprintf("[%s, #%d]", d.X, disp), nil
			}
		}
	}
	return l.gm(scratch), computeEffAddr(mem, nextPC, scratch)
}

func isStackBase(r x86asm.Reg) bool {
	switch r {
	case x86asm.RSP, x86asm.ESP, x86asm.RBP, x86asm.EBP:
		return true
	}
	return false
}

const scalarOps = `ldr|ldrb|ldrh|ldrsb|ldrsh|ldrsw|str|strb|strh`

var (
	guestScalarAccessBaseRe  = regexp.MustCompile(`^(\s+)(` + scalarOps + `)\s+([wx](?:\d+|zr)),\s*\[x28,\s*(x\d+)\]\s*(//.*)?$`)
	guestScalarAccessIdentRe = regexp.MustCompile(`^(\s+)(` + scalarOps + `)\s+([wx](?:\d+|zr)),\s*\[(x\d+),\s*#0\]\s*(//.*)?$`)
)

// guestAccessRe matches a scalar GPR guest access in the current address mode and captures
// (indent, op, data register, address register, trailing comment).
func (l *Arm64Lifter) guestAccessRe() *regexp.Regexp {
	if l.AddrMode() == AddrIdentity {
		return guestScalarAccessIdentRe
	}
	return guestScalarAccessBaseRe
}
