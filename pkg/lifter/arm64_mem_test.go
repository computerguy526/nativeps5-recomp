package lifter

import (
	"bytes"
	"regexp"
	"strings"
	"testing"

	"golang.org/x/arch/x86/x86asm"
)

// memCorpus: instruction sequences touching guest memory in every way the lowerings handle.
func memCorpus() []execCase {
	rb := uint64(execMemBase)
	regs := map[x86asm.Reg]uint64{
		x86asm.RAX: 0x1122334455667788, x86asm.RBX: rb + 0x100, x86asm.RCX: 3,
		x86asm.RSI: rb + 0x200, x86asm.RDI: rb + 0x300, x86asm.RBP: rb + 0x1000,
		x86asm.RDX: 0x80, x86asm.R8: rb + 0x400,
	}
	pat := make([]byte, 0x600)
	for i := range pat {
		pat[i] = byte(i*7 + 3)
	}
	mem := map[int][]byte{0: pat}
	mk := func(name string, code ...string) execCase {
		return execCase{name: name, code: code, regs: regs, mem: mem, noFlags: false}
	}
	return []execCase{
		mk("load64 disp", "488b4308"),
		mk("load32 index", "8b448b10"),
		mk("store32 index", "89448b10"),
		mk("store8 imm", "c6037f"),
		mk("movzx16", "0fb74302"),
		mk("movsx8", "480fbe03"),
		mk("movsxd", "486313"),
		mk("add mem,reg", "0103"),
		mk("add reg,mem big disp", "48038300010000"),
		mk("push pop", "5059"),
		mk("rsp load", "488b442408"),
		mk("rbp store", "488945f8"),
		mk("rbp load neg", "488b45f0"),
		mk("xchg mem", "8703"),
		mk("lock cmpxchg", "f00fb10b"),
		mk("lock xadd", "f00fc103"),
		mk("cmp mem imm", "48833b05"),
		mk("inc mem", "ff03"),
		mk("not mem8", "f613"),
		mk("movups", "0f1003", "0f114310"),
		mk("movsd", "f20f100b"),
		mk("addss mem", "f30f5803"),
		mk("rep movsb", "f3a4"),
		mk("rep stosq", "f348ab"),
		mk("x87 fld/fstp", "d903", "dd5b08"),
		mk("fnstcw", "d97b00"),
		mk("stmxcsr", "0fae5b04"),
		mk("vmovdqu ymm", "c5fe6f03", "c5fe7f4320"),
		mk("fs load", "64488b042510000000"),
		mk("split 16B access", "488b8bf8ffffff", "488983ffffffff"),
	}
}

func liftAll(t *testing.T, cases []execCase) string {
	t.Helper()
	l := NewArm64Lifter(nil)
	var sb strings.Builder
	for i, c := range cases {
		lines, _ := liftSeq(t, l, c, uint64(0x10000+i*0x1000))
		sb.WriteString(strings.Join(lines, "\n"))
		sb.WriteString("\n")
	}
	return sb.String()
}

var x28Re = regexp.MustCompile(`(^|[^0-9A-Za-z_])x28([^0-9A-Za-z_]|$)`)

// identityViolations returns the lines of lifted code that use x28 (the base-mode memory base).
// Comments are ignored; a `[x28,` guest-memory form is the case this lint exists for. (A plain
// substring search for "x28" also hits every #0x28.. immediate and "0x28" in comments: the
// token must stand alone.)
func identityViolations(lines []string) []string {
	var bad []string
	for _, ln := range lines {
		code := ln
		if k := strings.Index(code, "//"); k >= 0 {
			code = code[:k]
		}
		if x28Re.MatchString(code) {
			bad = append(bad, ln)
		}
	}
	return bad
}

func TestIdentityViolationsLint(t *testing.T) {
	if len(identityViolations([]string{"    mov w24, #0x289", "    // [x28, x1]", "    movk w10, #0x28d, lsl #16"})) != 0 {
		t.Error("false positive on x28 as a substring of an immediate or in a comment")
	}
	for _, s := range []string{"    ldr x0, [x28, x1]", "    add x16, x28, x2", "    stp x27, x28, [sp, #80]"} {
		if len(identityViolations([]string{s})) != 1 {
			t.Errorf("missed %q", s)
		}
	}
}

// In identity mode no lowering reads x28.
func TestIdentityModeNoX28(t *testing.T) {
	for _, tso := range []bool{true, false} {
		l := NewArm64LifterMode(nil, AddrIdentity)
		l.SetTSO(tso)
		for i, c := range memCorpus() {
			lines, _ := liftSeq(t, l, c, uint64(0x10000+i*0x1000))
			for _, ln := range identityViolations(lines) {
				t.Errorf("%s (tso=%v): x28 in identity lowering: %q", c.name, tso, ln)
			}
		}
	}
}

// Base and identity lowerings of the same code, run on the host with the memory base at 0,
// must leave identical registers and memory (the guest address is the host address in both).
func TestIdentityMatchesBase(t *testing.T) {
	for _, tso := range []bool{true, false} {
		runBoth := func(m AddrMode) []byte {
			execAddrMode = m
			defer func() { execAddrMode = AddrBase }()
			var out []byte
			execCapture = &out
			defer func() { execCapture = nil }()
			cases := memCorpus()
			runExecExpectTSO(t, cases, tso)
			return out
		}
		base := runBoth(AddrBase)
		ident := runBoth(AddrIdentity)
		if len(base) == 0 || len(base) != len(ident) {
			t.Fatalf("tso=%v: output sizes %d vs %d", tso, len(base), len(ident))
		}
		rec := stSize + execMemSize
		cases := memCorpus()
		for i, c := range cases {
			b, id := base[i*rec:(i+1)*rec], ident[i*rec:(i+1)*rec]
			// The state block also holds the FS base and scratch; compare GPRs + window.
			if !bytes.Equal(b[:stFlags], id[:stFlags]) || !bytes.Equal(b[stSize:], id[stSize:]) {
				t.Errorf("tso=%v %s: identity result differs from base", tso, c.name)
			}
		}
	}
}

func TestIdentityFoldsStackDisp(t *testing.T) {
	l := NewArm64LifterMode(nil, AddrIdentity)
	lines := liftHex(t, l, "488b442408") // mov rax, [rsp+8]
	got := strings.Join(lines, "\n")
	if !strings.Contains(got, "[x26, #8]") {
		t.Errorf("rsp+8 not folded:\n%s", got)
	}
	lines = liftHex(t, l, "488b4308") // mov rax, [rbx+8]: not folded (TSO)
	got = strings.Join(lines, "\n")
	if strings.Contains(got, "[x20, #8]") || !strings.Contains(got, "ldapr") {
		t.Errorf("heap access must stay TSO-recognisable:\n%s", got)
	}
}

func TestRewriteGuestMem(t *testing.T) {
	in := "    ldr  x2, [x28, x2]           // ctx\n    add  x16, x28, x24\n    add  x16, x28, x16\n    stp x27, x28, [sp, #80]\n"
	if NewArm64Lifter(nil).RewriteGuestMem(in) != in {
		t.Error("base mode must not change the text")
	}
	got := NewArm64LifterMode(nil, AddrIdentity).RewriteGuestMem(in)
	want := "    ldr  x2, [x2]           // ctx\n    mov x16, x24\n    // guest==host\n    stp x27, x28, [sp, #80]\n"
	if got != want {
		t.Errorf("got:\n%s\nwant:\n%s", got, want)
	}
}

func liftHex(t *testing.T, l *Arm64Lifter, h string) []string {
	t.Helper()
	lines, _ := liftSeq(t, l, execCase{name: h, code: []string{h}}, 0x10000)
	return lines
}

func runExecExpectTSO(t *testing.T, cases []execCase, tso bool) {
	t.Helper()
	execNoTSO = !tso
	defer func() { execNoTSO = false }()
	runExecExpect(t, cases, make([]map[int][]byte, len(cases)))
}

// Two lifters with different address modes coexist in one process and lift interleaved without
// affecting each other (the mode is per lifter, not a package global).
func TestAddrModePerLifter(t *testing.T) {
	base := NewArm64LifterMode(nil, AddrBase)
	ident := NewArm64LifterMode(nil, AddrIdentity)
	if base.AddrMode() != AddrBase || ident.AddrMode() != AddrIdentity {
		t.Fatal("modes not stored per lifter")
	}
	for i := 0; i < 3; i++ {
		b := strings.Join(liftHex(t, base, "488b4308"), "\n")
		id := strings.Join(liftHex(t, ident, "488b4308"), "\n")
		if !strings.Contains(b, "[x28, ") || strings.Contains(id, "x28") || !strings.Contains(id, ", #0]") {
			t.Fatalf("iteration %d: wrong forms\nbase:\n%s\nidentity:\n%s", i, b, id)
		}
	}
	in := "    ldr  x2, [x28, x2]\n"
	if base.RewriteGuestMem(in) != in || ident.RewriteGuestMem(in) == in {
		t.Error("RewriteGuestMem follows the lifter's mode")
	}
}
