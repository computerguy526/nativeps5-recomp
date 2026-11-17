package lifter

import (
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"math"
	"ps4-recomp/pkg/disasm"
	"strings"
	"testing"

	"golang.org/x/arch/x86/x86asm"
)

// FP compare fusion (arm64_lifter_fpfuse.go): UCOMIS/COMIS followed by J/SET/CMOV{A,AE,B,BE,E,NE,P,NP}
// consume the raw FCMP flags. Expectations come from the x86 definition: lt CF=1; eq ZF=1;
// gt none; unordered ZF=PF=CF=1.

type fpOutcome struct {
	name       string
	x, y       float64
	cf, zf, pf bool
}

var fpOutcomes = []fpOutcome{
	{"less", 1, 2, true, false, false},
	{"equal", 2, 2, false, true, false},
	{"greater", 3, 2, false, false, false},
	{"unordered x", math.NaN(), 1, true, true, true},
	{"unordered y", 1, math.NaN(), true, true, true},
	{"unordered both", math.NaN(), math.NaN(), true, true, true},
	{"zeros", 0, math.Copysign(0, -1), false, true, false},
	{"inf", math.Inf(1), math.Inf(1), false, true, false},
	{"neg inf < inf", math.Inf(-1), math.Inf(1), true, false, false},
}

type fpCC struct {
	name              string
	jcc, setcc, cmovc string // opcode bytes (hex)
	eval              func(o fpOutcome) bool
}

var fpCCs = []fpCC{
	{"b", "72", "92", "42", func(o fpOutcome) bool { return o.cf }},
	{"ae", "73", "93", "43", func(o fpOutcome) bool { return !o.cf }},
	{"e", "74", "94", "44", func(o fpOutcome) bool { return o.zf }},
	{"ne", "75", "95", "45", func(o fpOutcome) bool { return !o.zf }},
	{"be", "76", "96", "46", func(o fpOutcome) bool { return o.cf || o.zf }},
	{"a", "77", "97", "47", func(o fpOutcome) bool { return !o.cf && !o.zf }},
	{"p", "7a", "9a", "4a", func(o fpOutcome) bool { return o.pf }},
	{"np", "7b", "9b", "4b", func(o fpOutcome) bool { return !o.pf }},
}

func fpBytes(o fpOutcome, double bool) []byte {
	if double {
		b := binary.LittleEndian.AppendUint64(nil, math.Float64bits(o.x))
		return binary.LittleEndian.AppendUint64(b, math.Float64bits(o.y))
	}
	b := binary.LittleEndian.AppendUint32(nil, math.Float32bits(float32(o.x)))
	return binary.LittleEndian.AppendUint32(b, math.Float32bits(float32(o.y)))
}

func fpFuseCases(fuse bool) []execCase {
	b := uint64(execMemBase)
	var cases []execCase
	for _, double := range []bool{false, true} {
		// movs[sd] xmm0,[rsi]; movs[sd] xmm1,[rsi+k]; ucomis[sd] xmm0,xmm1 | comis[sd] xmm0,[rsi+k]
		var ld0, ld1, cmpReg, cmpMem string
		if double {
			ld0, ld1, cmpReg, cmpMem = "f20f1006", "f20f104e08", "660f2ec1", "660f2f4608"
		} else {
			ld0, ld1, cmpReg, cmpMem = "f30f1006", "f30f104e04", "0f2ec1", "0f2f4604"
		}
		for _, form := range []struct {
			name string
			code []string
		}{
			{"reg", []string{ld0, ld1, cmpReg}},
			{"mem", []string{ld0, cmpMem}},
		} {
			for _, o := range fpOutcomes {
				for _, cc := range fpCCs {
					want := cc.eval(o)
					tag := fmt.Sprintf("%s/%v/%s/%s", form.name, map[bool]string{false: "ss", true: "sd"}[double], o.name, cc.name)
					mem := map[int][]byte{0x100: fpBytes(o, double), 0x200: {0x78, 0x56, 0x34, 0x12}}
					base := map[x86asm.Reg]uint64{x86asm.RSI: b + 0x100, x86asm.RDI: b + 0x200,
						x86asm.RAX: 0x1111111122222222, x86asm.RBX: 0x3333333344444444, x86asm.RCX: 0x1234ff00}
					for _, cfg := range []bool{true, false} {
						_ = fuse
						// SETcc cl
						c := execCase{name: "set " + tag, cfg: cfg, code: append(append([]string{}, form.code...), "0f"+cc.setcc+"c1", "39d2"),
							regs: base, mem: mem}
						v := uint64(0)
						if want {
							v = 1
						}
						c.wantRegs = map[x86asm.Reg]uint64{x86asm.RCX: 0x1234ff00 | v}
						cases = append(cases, c)
						// CMOVcc eax,ebx (32), rax,rbx (64), eax,[rdi]
						for _, v := range []struct {
							n, enc string
							res    [2]uint64
						}{
							{"32", "0f" + cc.cmovc + "c3", [2]uint64{0x22222222, 0x44444444}},
							{"64", "480f" + cc.cmovc + "c3", [2]uint64{0x1111111122222222, 0x3333333344444444}},
							{"mem32", "0f" + cc.cmovc + "07", [2]uint64{0x22222222, 0x12345678}},
						} {
							r := v.res[0]
							if want {
								r = v.res[1]
							}
							cases = append(cases, execCase{name: "cmov" + v.n + " " + tag, cfg: cfg,
								code: append(append([]string{}, form.code...), v.enc, "39d2"),
								regs: base, mem: mem, wantRegs: map[x86asm.Reg]uint64{x86asm.RAX: r}})
						}
						// Jcc: taken skips "mov eax,1; cmp eax,eax"; the target is a block with its own kill.
						res := uint64(1)
						if want {
							res = 0
						}
						if !cfg {
							continue // branches need blocks
						}
						cases = append(cases, execCase{name: "j" + tag, cfg: cfg,
							code: append(append([]string{"b800000000"}, form.code...), cc.jcc+"07", "b801000000", "39c0", "39c9"),
							regs: base, mem: mem, wantRegs: map[x86asm.Reg]uint64{x86asm.RAX: res}})
					}
				}
			}
		}
	}
	return cases
}

func TestFpCmpFuseExec(t *testing.T) {
	old := fpCmpFuse
	defer func() { fpCmpFuse = old }()
	for _, on := range []bool{true, false} {
		fpCmpFuse = on
		cases := fpFuseCases(on)
		for i := range cases {
			cases[i].name = fmt.Sprintf("fuse=%v %s", on, cases[i].name)
		}
		want := make([]map[int][]byte, len(cases))
		runExecExpect(t, cases, want)
	}
}

// The fused form drops the flag conversion; the unfused (and the flag-off) lowering keeps it.
func TestFpCmpFuseShape(t *testing.T) {
	old := fpCmpFuse
	defer func() { fpCmpFuse = old }()
	code := []string{"f30f1006", "0f2e4604", "0f9ac1", "39d2"} // movss; ucomiss; setp cl; cmp edx,edx
	lift := func(code []string) string {
		l := NewArm64LifterMode(nil, execAddrMode)
		return strings.Join(liftSeqCFG(t, l, execCase{name: "shape", cfg: true, code: code}, 0x10000), "\n")
	}
	fpCmpFuse = true
	if s := lift(code); strings.Contains(s, "axflag") || !strings.Contains(s, "fcmp s0, s") || !strings.Contains(s, "cset w16, vs") {
		t.Errorf("fused lowering has the conversion or lacks fcmp/vs:\n%s", s)
	}
	// A reader the mapping cannot serve (adc) keeps the conversion.
	if s := lift([]string{"f30f1006", "0f2e4604", "11d2", "39d2"}); !strings.Contains(s, "axflag") {
		t.Errorf("adc after ucomiss must keep the flag conversion:\n%s", s)
	}
	// Flags live into the next block (setb in a successor): no fusion.
	if s := lift([]string{"f30f1006", "0f2e4604", "7301", "90", "0f92c1"}); !strings.Contains(s, "axflag") {
		t.Errorf("flags live into a successor must keep the conversion:\n%s", s)
	}
	// jne out; jp out: the window continues into the JP block (no conversion at all).
	if s := lift([]string{"f30f1006", "0f2e4604", "b800000000", "7507", "7a05", "b801000000", "39c9"}); strings.Contains(s, "axflag") || !strings.Contains(s, "b.vs") {
		t.Errorf("jne/jp chain must stay fused:\n%s", s)
	}
	// A pure move does not.
	if s := lift([]string{"f30f1006", "0f2e4604", "894e08", "0f92c2", "39c9"}); strings.Contains(s, "axflag") {
		t.Errorf("mov between compare and setb must stay fused:\n%s", s)
	}
	fpCmpFuse = false
	if s := lift(code); !strings.Contains(s, "axflag") {
		t.Errorf("flag off must keep the conversion:\n%s", s)
	}
}

// Windows that span blocks (`jne out; jp out`), and instructions between the compare and its
// readers whose lowering writes NZCV (the conversion is emitted in front of them).
func fpFuseWindowCases() []execCase {
	b := uint64(execMemBase)
	var cases []execCase
	for _, o := range fpOutcomes {
		mem := map[int][]byte{0x100: fpBytes(o, false)}
		regs := map[x86asm.Reg]uint64{x86asm.RSI: b + 0x100, x86asm.RAX: 0x55, x86asm.RDX: 0x77}
		// Two jumps to the same exit, then "mov eax, 1": eax = 1 when neither is taken.
		for _, c1 := range fpCCs {
			for _, c2 := range fpCCs {
				code := []string{"f30f1006", "0f2e4604", "b800000000", c1.jcc + "07", c2.jcc + "05", "b801000000", "39c9"}
				res := uint64(1)
				if c1.eval(o) || c2.eval(o) {
					res = 0
				}
				cases = append(cases, execCase{name: fmt.Sprintf("jj %s/%s/%s", o.name, c1.name, c2.name), cfg: true, block: true,
					code: code, regs: regs, mem: mem, wantRegs: map[x86asm.Reg]uint64{x86asm.RAX: res}})
			}
		}
		// A flag-neutral instruction whose lowering clobbers NZCV between the compare and SETcc/CMOVcc.
		for _, mid := range []struct{ name, hex string }{
			{"cvttss2si", "f30f2cc8"}, {"cvtsi2ss", "f30f2ad0"}, {"movmskps", "0f50c8"}, {"sqrtss", "f30f51d0"},
			{"maxss", "f30f5fd0"}, {"cvtss2sd", "f30f5ad0"}, {"vmovd", "c5f97ec9"}, {"vcvttss2si", "c5fa2cc8"},
			{"vmovapd", "c5f928d9"}, {"movapd", "660f28d9"}, {"movaps", "0f28d9"}, {"vmovsd", "c5fb10d9"}, {"vmovaps ymm", "c5fc28d9"}, {"lea", "488d4c0801"}, {"mov mem", "894e08"}, {"movzx", "0fb6c8"},
		} {
			for _, cc := range fpCCs {
				v := uint64(0)
				if cc.eval(o) {
					v = 1
				}
				code := []string{"f30f1006", "0f2e4604", mid.hex, "0f" + cc.setcc + "c2", "39c9"}
				cases = append(cases, execCase{name: fmt.Sprintf("mid %s/%s/%s", mid.name, o.name, cc.name), cfg: true, block: true,
					code: code, regs: regs, mem: mem, wantRegs: map[x86asm.Reg]uint64{x86asm.RDX: 0x77&^0xff | v}})
			}
		}
	}
	return cases
}

func TestFpCmpFuseWindows(t *testing.T) {
	old := fpCmpFuse
	defer func() { fpCmpFuse = old }()
	for _, on := range []bool{true, false} {
		fpCmpFuse = on
		cases := fpFuseWindowCases()
		for i := range cases {
			cases[i].name = fmt.Sprintf("fuse=%v %s", on, cases[i].name)
		}
		runExecExpect(t, cases, make([]map[int][]byte, len(cases)))
	}
}

// An instruction between a fused compare and its readers whose lowering writes NZCV gets the flag
// conversion in front of it; one that saves and restores NZCV, or writes none, does not.
func TestFpRawAfter(t *testing.T) {
	inst := disasmInst(t, "90") // nop: flag neutral
	for _, c := range []struct {
		name  string
		lines []string
		conv  bool
	}{
		{"cmp", []string{"    // x", "    mov x1, x2", "    cmp x1, #3", "    csel x1, x2, x3, lt"}, true},
		{"subs", []string{"    subs x1, x2, #1"}, true},
		{"bl helper", []string{"    bl _recomp_x"}, true},
		{"fcmp", []string{"    fcmp d0, d1"}, true},
		{"save/restore", []string{"    mrs x6, nzcv", "    cmp x1, #3", "    msr nzcv, x6"}, false},
		{"none", []string{"    add x1, x2, x3", "    ldr x4, [x28, x1]"}, false},
		{"msr fpcr", []string{"    msr fpcr, x4"}, false},
	} {
		l := NewArm64LifterMode(nil, execAddrMode)
		l.fpRawLive = true
		out := l.fpRawAfter(inst, append([]string{"    // head"}, c.lines...), 1)
		got := strings.Contains(strings.Join(out, "\n"), "axflag")
		if got != c.conv {
			t.Errorf("%s: conversion=%v want %v:\n%s", c.name, got, c.conv, strings.Join(out, "\n"))
		}
		if c.conv && (out[1] != "    cset w2, vs" || l.fpRawLive) {
			t.Errorf("%s: conversion must come first and end the raw state: %v", c.name, out)
		}
	}
}

func disasmInst(t *testing.T, hexs string) disasm.Instruction {
	b, _ := hex.DecodeString(hexs)
	in, err := disasm.Decode(b)
	if err != nil {
		t.Fatal(err)
	}
	return disasm.Instruction{Inst: in}
}

// Two fused windows back to back, the second in the fall-through block of the first one's branch
// (clang's clamp: `vucomisd lo, v; ja L; vucomisd v, hi; vmovapd; jbe L; vmovapd`).
func TestFpCmpFuseClamp(t *testing.T) {
	old := fpCmpFuse
	defer func() { fpCmpFuse = old }()
	b := uint64(execMemBase)
	vals := []float64{-3, -0.5, 0.25, 0.5, 2, math.NaN(), math.Inf(1), math.Inf(-1), math.Copysign(0, -1)}
	code := []string{"f20f1026", "f20f104e08", "f20f105610", "c5f928d9", "c5f92ecc", "770e", "c5f92ee2", "c5f928dc", "7604", "c5f928da", "39c9", "66480f7ed8"}
	for _, on := range []bool{true, false} {
		fpCmpFuse = on
		var cases []execCase
		for _, v := range vals {
			lo, hi := -1.0, 1.0
			r := v
			if lo > v {
				r = lo
			} else if v > hi {
				r = hi
			}
			mem := append(append(le64(math.Float64bits(v)), le64(math.Float64bits(lo))...), le64(math.Float64bits(hi))...)
			cases = append(cases, execCase{name: fmt.Sprintf("fuse=%v clamp %v", on, v), cfg: true, block: true, code: code,
				regs: map[x86asm.Reg]uint64{x86asm.RSI: b + 0x100}, mem: map[int][]byte{0x100: mem},
				wantRegs: map[x86asm.Reg]uint64{x86asm.RAX: math.Float64bits(r)}})
		}
		runExecExpect(t, cases, make([]map[int][]byte, len(cases)))
	}
}

// INC/DEC between the consumers and the end of the window: dead CF makes them end it (fused); a
// later reader of CF (setb after inc) must still see the compare's CF, so that stays unfused.
func TestFpCmpFuseInc(t *testing.T) {
	old := fpCmpFuse
	defer func() { fpCmpFuse = old }()
	b := uint64(execMemBase)
	for _, on := range []bool{true, false} {
		fpCmpFuse = on
		var cases []execCase
		for _, o := range fpOutcomes {
			mem := map[int][]byte{0x100: fpBytes(o, false)}
			regs := map[x86asm.Reg]uint64{x86asm.RSI: b + 0x100, x86asm.RCX: 41, x86asm.RDX: 0x77, x86asm.RBX: 0x99}
			for _, cc := range fpCCs {
				v := uint64(0)
				if cc.eval(o) {
					v = 1
				}
				// movss; ucomiss; setcc dl; inc ecx; cmp eax,eax
				cases = append(cases, execCase{name: fmt.Sprintf("fuse=%v inc-dead %s/%s", on, o.name, cc.name), cfg: true, block: true,
					code: []string{"f30f1006", "0f2e4604", "0f" + cc.setcc + "c2", "ffc1", "39c0"}, regs: regs, mem: mem,
					wantRegs: map[x86asm.Reg]uint64{x86asm.RDX: 0x77&^0xff | v, x86asm.RCX: 42}})
				// movss; ucomiss; setcc dl; inc ecx; setb bl  (CF live through the INC)
				cf := uint64(0)
				if o.cf {
					cf = 1
				}
				cases = append(cases, execCase{name: fmt.Sprintf("fuse=%v inc-live %s/%s", on, o.name, cc.name), cfg: true, block: true,
					code: []string{"f30f1006", "0f2e4604", "0f" + cc.setcc + "c2", "ffc1", "0f92c3", "39c0"}, regs: regs, mem: mem,
					wantRegs: map[x86asm.Reg]uint64{x86asm.RDX: 0x77&^0xff | v, x86asm.RCX: 42, x86asm.RBX: 0x99&^0xff | cf}})
			}
		}
		runExecExpect(t, cases, make([]map[int][]byte, len(cases)))
	}
}
