package lifter

import (
	"fmt"
	"sort"
	"strings"

	"ps4-recomp/pkg/disasm"

	"golang.org/x/arch/x86/x86asm"
)

// Unsupported-instruction policy.
//
// A guest instruction the lifter has no lowering for becomes a diagnostic stub (a
// "// UNSUPPORTED:" marker followed by a call to _recomp_unsupported_instruction). Every stub is
// also recorded on the lifter. The emitter fails the translation with the list unless the
// lifter allows unsupported instructions (SetAllowUnsupported(true), CLI -allow-unsupported):
// a guest binary with holes must not silently build.

// UnsupportedSite is one instruction that was lowered to a stub.
type UnsupportedSite struct {
	Addr     uint64 // guest address of the instruction
	Func     uint64 // entry of the containing function (0 if unknown)
	Mnemonic string // e.g. VPERMQ
	Form     string // mnemonic and operand classes, e.g. "VPERMQ ymm,ymm,imm"
	Reason   string // what the lifter said (the instruction text unless it gave a reason)
}

// SetAllowUnsupported selects whether unsupported instructions are tolerated (brk-style stubs
// kept) or make the translation fail. The default is to fail.
func (l *Arm64Lifter) SetAllowUnsupported(allow bool) { l.allowUnsupported = allow }

// AllowUnsupported reports whether unsupported instructions are tolerated.
func (l *Arm64Lifter) AllowUnsupported() bool { return l.allowUnsupported }

// Unsupported returns the stubs emitted so far.
func (l *Arm64Lifter) Unsupported() []UnsupportedSite { return l.unsupported }

// RecordUnsupported records a stub emitted outside the lifter (the emitter's lift-error path).
func (l *Arm64Lifter) RecordUnsupported(inst disasm.Instruction, fn uint64, reason string) {
	l.unsupported = append(l.unsupported, siteOf(inst, fn, reason))
}

func siteOf(inst disasm.Instruction, fn uint64, reason string) UnsupportedSite {
	mn := strings.ToUpper(strings.TrimSuffix(inst.Inst.Op.String(), "_XMM"))
	if inst.Inst.Op == disasm.OpEXTRQ {
		mn = "EXTRQ"
	} else if inst.Inst.Op == disasm.OpINSERTQ {
		mn = "INSERTQ"
	}
	form := mn
	if ops := operandClasses(inst.Inst); ops != "" {
		form += " " + ops
	}
	return UnsupportedSite{Addr: inst.Address, Func: fn, Mnemonic: mn, Form: form, Reason: reason}
}

// operandClasses summarises an instruction's operands: xmm, ymm, mm, st, r8/r16/r32/r64,
// m (memory), imm, rel.
func operandClasses(inst x86asm.Inst) string {
	var out []string
	for _, a := range inst.Args {
		if a == nil {
			break
		}
		switch v := a.(type) {
		case x86asm.Mem:
			out = append(out, "m")
		case x86asm.Imm:
			out = append(out, "imm")
		case x86asm.Rel:
			out = append(out, "rel")
		case x86asm.Reg:
			switch {
			case v >= x86asm.X0 && v <= x86asm.X15:
				out = append(out, "xmm")
			case v >= x86asm.Y0 && v <= x86asm.Y15:
				out = append(out, "ymm")
			case v >= x86asm.M0 && v <= x86asm.M7:
				out = append(out, "mm")
			case v >= x86asm.F0 && v <= x86asm.F7:
				out = append(out, "st")
			case v >= x86asm.AL && v <= x86asm.R15B:
				out = append(out, "r8")
			case v >= x86asm.AX && v <= x86asm.R15W:
				out = append(out, "r16")
			case v >= x86asm.EAX && v <= x86asm.R15L:
				out = append(out, "r32")
			case v >= x86asm.RAX && v <= x86asm.R15:
				out = append(out, "r64")
			default:
				out = append(out, "r")
			}
		default:
			out = append(out, "?")
		}
	}
	return strings.Join(out, ",")
}

// FormatUnsupported renders the failure message: a per-form summary followed by up to max
// individual sites (address, function, form, reason).
func FormatUnsupported(sites []UnsupportedSite, max int) string {
	counts := map[string]int{}
	for _, s := range sites {
		counts[s.Form]++
	}
	forms := make([]string, 0, len(counts))
	for f := range counts {
		forms = append(forms, f)
	}
	sort.Slice(forms, func(i, j int) bool {
		if counts[forms[i]] != counts[forms[j]] {
			return counts[forms[i]] > counts[forms[j]]
		}
		return forms[i] < forms[j]
	})
	var sb strings.Builder
	fmt.Fprintf(&sb, "%d unsupported instruction(s), %d distinct form(s):\n", len(sites), len(forms))
	for _, f := range forms {
		fmt.Fprintf(&sb, "  %6d  %s\n", counts[f], f)
	}
	sorted := append([]UnsupportedSite(nil), sites...)
	sort.Slice(sorted, func(i, j int) bool { return sorted[i].Addr < sorted[j].Addr })
	fmt.Fprintf(&sb, "sites (address, function, form):\n")
	for i, s := range sorted {
		if max > 0 && i >= max {
			fmt.Fprintf(&sb, "  ... %d more\n", len(sorted)-max)
			break
		}
		fmt.Fprintf(&sb, "  0x%x  fn 0x%x  %s  [%s]\n", s.Addr, s.Func, s.Form, s.Reason)
	}
	sb.WriteString("use -allow-unsupported to emit diagnostic stubs for them instead\n")
	return sb.String()
}

// Probe lifts inst (in the state the lifter is in) and reports whether it lowered to an
// unsupported stub, with the site. The recorded stub is removed again: the analyzer uses it to
// count what the lifter really cannot lower, instead of the stale IsOpcodeSupported list.
func (l *Arm64Lifter) Probe(inst disasm.Instruction, fn *disasm.Function) (UnsupportedSite, bool) {
	n := len(l.unsupported)
	nextPC := inst.Address + uint64(inst.Inst.Len)
	_, _ = l.LiftInstructionToBuf(inst, nextPC, fn, nil)
	l.cold = nil // the lowering is discarded
	if len(l.unsupported) == n {
		return UnsupportedSite{}, false
	}
	s := l.unsupported[n]
	l.unsupported = l.unsupported[:n]
	return s, true
}
