package lifter

import (
	"fmt"
	"regexp"
	"strings"

	"golang.org/x/arch/x86/x86asm"
	"ps4-recomp/pkg/disasm"
)

// FP compare fusion (docs/m3/liftmisc.md item 1). The disasm pass marks an SSE FP compare whose
// flags are read only by J/SET/CMOV{A,AE,B,BE,E,NE,P,NP} of the same block and dead afterwards
// (Instruction.FpFlagsLocal) and those consumers (FpRaw). The compare then emits just `fcmp`, and
// NZCV holds ARM's FCMP result:
//
//	          N Z C V          x86 (ZF PF CF)
//	less      1 0 0 0          0 0 1
//	equal     0 1 1 0          1 0 0
//	greater   0 0 1 0          0 0 0
//	unordered 0 0 1 1          1 1 1
//
// V is the unordered flag in both models, so PF readers (vs/vc) and the PF spill are unchanged.
// Conditions with the same truth table over these four outcomes:
//
//	JA  CF=0,ZF=0 -> gt        JAE CF=0 -> ge       JB  CF=1 -> lt       JBE CF=1|ZF=1 -> le
//	JP -> vs   JNP -> vc
//	JE  ZF=1 -> eq or vs       JNE ZF=0 -> ordered and not eq
//
// (A NaN makes N!=V: gt/ge are false, lt/le true, as x86 CF=1 requires.) E and NE need two
// conditions; they are emitted as two branches, csinc/csel pairs. The other conditions (S, O, L,
// G ...) are never produced after an FP compare by compilers and are not fused.
var fpCmpFuse = true

// SetFpCmpFuse turns FP compare fusion on or off for all subsequent lifting (`-fp-cmp-fuse`).
func SetFpCmpFuse(on bool) { fpCmpFuse = on }

// GetFpCmpFuse reports whether FP compare fusion is on.
func GetFpCmpFuse() bool { return fpCmpFuse }

func fpRawCond(cc int) string {
	switch cc {
	case 7:
		return "gt"
	case 3:
		return "ge"
	case 2:
		return "lt"
	case 6:
		return "le"
	case 10:
		return "vs"
	case 11:
		return "vc"
	}
	return ""
}

// liftFpRawConsumer lowers a fused Jcc/SETcc/CMOVcc. ok is false when the form is not handled (the
// caller then uses the normal path, which would read the wrong flags: the disasm pass only marks
// forms handled here, so that cannot happen for marked instructions).
func (l *Arm64Lifter) liftFpRawConsumer(inst disasm.Instruction, nextPC uint64, fn *disasm.Function, cc int, lines []string) ([]string, bool, error) {
	opName := inst.Inst.Op.String()
	cond := fpRawCond(cc)
	switch {
	case strings.HasPrefix(opName, "SET"):
		out, err := l.liftSetccFp(inst, nextPC, cc, cond, lines)
		return out, true, err
	case strings.HasPrefix(opName, "CMOV"):
		out, ok := l.liftCmovFp(inst, nextPC, cc, cond, lines)
		return out, ok, nil
	}
	rel, ok := inst.Inst.Args[0].(x86asm.Rel)
	if !ok {
		return nil, false, nil
	}
	target := uint64(int64(nextPC) + int64(rel))
	lines = append(lines, l.exitFlags(inst)...)
	lbl := localLabel(fn, target)
	near := false
	if lbl != "" {
		d := int64(target) - int64(nextPC)
		if d < 0 {
			d = -d
		}
		near = d <= 16384
	}
	fe := uint64(0)
	if fn != nil {
		fe = fn.EntryAddr
	}
	l.labelCounter++
	skip := fmt.Sprintf("L_fn_0x%x_fpskip_%d", fe, l.labelCounter)
	switch {
	case cond != "" && near:
		return append(lines, fmt.Sprintf("    b.%s %s", cond, lbl)), true, nil
	case cond != "":
		lines = append(lines, fmt.Sprintf("    b.%s %s", invertArm64Cond(cond), skip))
		lines = append(lines, l.branchTo(fn, target)...)
		return append(lines, skip+":"), true, nil
	case cc == 4 && near: // JE: eq or unordered
		return append(lines, fmt.Sprintf("    b.eq %s", lbl), fmt.Sprintf("    b.vs %s", lbl)), true, nil
	case cc == 4:
		take := skip + "t"
		lines = append(lines, "    b.eq "+take, "    b.vs "+take, "    b "+skip, take+":")
		lines = append(lines, l.branchTo(fn, target)...)
		return append(lines, skip+":"), true, nil
	case cc == 5 && near: // JNE: ordered and not equal
		return append(lines, "    b.vs "+skip, fmt.Sprintf("    b.ne %s", lbl), skip+":"), true, nil
	case cc == 5:
		lines = append(lines, "    b.vs "+skip, "    b.eq "+skip)
		lines = append(lines, l.branchTo(fn, target)...)
		return append(lines, skip+":"), true, nil
	}
	return nil, false, nil
}

// liftSetccFp: SETcc from the raw FCMP flags. Result in w16 first, then the usual store.
func (l *Arm64Lifter) liftSetccFp(inst disasm.Instruction, nextPC uint64, cc int, cond string, lines []string) ([]string, error) {
	switch cc {
	case 4: // eq or unordered
		return l.liftSetccFpSeq(inst, nextPC, lines, "    cset w16, vs", "    csinc w16, w16, wzr, ne")
	case 5: // ne and ordered
		return l.liftSetccFpSeq(inst, nextPC, lines, "    cset w16, vc", "    csel w16, w16, wzr, ne")
	}
	return l.liftSetcc(inst, nextPC, cond, lines)
}

func (l *Arm64Lifter) liftSetccFpSeq(inst disasm.Instruction, nextPC uint64, lines []string, a, b string) ([]string, error) {
	// liftSetcc starts with `cset w16, cond`; run it with a harmless condition, then drop that
	// first instruction in favour of the two-instruction sequence.
	start := len(lines)
	out, err := l.liftSetcc(inst, nextPC, "vs", lines)
	if err != nil {
		return out, err
	}
	// out[start] is the cset w16, vs
	rest := append([]string{a, b}, out[start+1:]...)
	return append(out[:start], rest...), nil
}

// liftCmovFp: CMOVcc from the raw FCMP flags (32/64-bit register destination only).
func (l *Arm64Lifter) liftCmovFp(inst disasm.Instruction, nextPC uint64, cc int, cond string, lines []string) ([]string, bool) {
	dstReg, ok := inst.Inst.Args[0].(x86asm.Reg)
	if !ok {
		return nil, false
	}
	d, ok := arm64RegMap[dstReg]
	if !ok || d.Size == 2 {
		return nil, false
	}
	dn := d.X
	if d.Size == 4 {
		dn = d.W
	}
	var sn string
	switch src := inst.Inst.Args[1].(type) {
	case x86asm.Reg:
		s, ok := arm64RegMap[src]
		if !ok {
			return nil, false
		}
		sn = s.X
		if d.Size == 4 {
			sn = s.W
		}
	case x86asm.Mem:
		lines = append(lines, l.readMemTo(src, nextPC, d.Size, "x16", "w16")...)
		sn = "x16"
		if d.Size == 4 {
			sn = "w16"
		}
	default:
		return nil, false
	}
	t := "x17"
	if d.Size == 4 {
		t = "w17"
	}
	switch cc {
	case 4: // eq or unordered
		return append(lines, fmt.Sprintf("    csel %s, %s, %s, eq", dn, sn, dn), fmt.Sprintf("    csel %s, %s, %s, vs", dn, sn, dn)), true
	case 5: // ne and ordered
		return append(lines, fmt.Sprintf("    csel %s, %s, %s, ne", t, sn, dn), fmt.Sprintf("    csel %s, %s, %s, vs", dn, dn, t)), true
	}
	return append(lines, fmt.Sprintf("    csel %s, %s, %s, %s", dn, sn, dn, cond)), true
}

var nzcvWriterRe = regexp.MustCompile(`^(cmp|cmn|tst|ccmp|ccmn|fcmp|fcmpe|fccmp|fccmpe|adds|subs|ands|bics|negs|adcs|sbcs|ngcs|rmif|setf8|setf16|cfinv|axflag|xaflag|bl|blr|msr)\b`)

// fpConvert converts the raw FCMP flags into the x86 flag model (the lowering of an unfused
// compare): Z = ZF, C = !CF, V = PF, N = 0. Scratch w2.
func (l *Arm64Lifter) fpConvert() []string {
	l.cpol = carrySub
	l.fpRawLive = false
	return []string{"    cset w2, vs", "    axflag", "    rmif x2, #0, #1"}
}

// fpRawAfter runs after the lowering of an instruction (lines[start:]) while the raw FCMP flags are
// live: a consumer marked FpRawLast ends the window; an instruction that defines every flag ends
// it too (it overwrites them); any other instruction whose lowering may write NZCV gets the flag
// conversion in front of it, which keeps the later consumers (they then use the model flags) right.
func (l *Arm64Lifter) fpRawAfter(inst disasm.Instruction, lines []string, start int) []string {
	if !l.fpRawLive || inst.FpFlagsLocal {
		return lines // (a fused compare has just set the raw state itself)
	}
	if inst.FpRaw {
		if inst.FpRawLast {
			l.fpRawLive = false
		}
		return lines
	}
	if disasm.FpKillsFlags(inst) {
		l.fpRawLive = false
		return lines
	}
	hasNZCV := false
	saves := false
	for _, ln := range lines[start:] {
		t := strings.TrimSpace(ln)
		if i := strings.Index(t, "//"); i >= 0 {
			t = strings.TrimSpace(t[:i])
		}
		if t == "" {
			continue
		}
		if strings.HasPrefix(t, "mrs ") && strings.Contains(t, "nzcv") {
			saves = true
		}
		if nzcvWriterRe.MatchString(t) {
			if strings.HasPrefix(t, "msr ") && !strings.Contains(t, "nzcv") {
				continue
			}
			hasNZCV = true
		}
	}
	if hasNZCV && !saves {
		conv := l.fpConvert()
		out := append([]string{}, lines[:start]...)
		out = append(out, conv...)
		return append(out, lines[start:]...)
	}
	return lines
}
