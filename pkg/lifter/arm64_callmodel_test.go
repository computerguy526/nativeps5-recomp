package lifter

import (
	"encoding/hex"
	"reflect"
	"regexp"
	"strings"
	"testing"

	"ps4-recomp/pkg/disasm"

	"golang.org/x/arch/x86/x86asm"
)

func withCallModel(t *testing.T, m CallModel) {
	t.Helper()
	old := GetCallModel()
	SetCallModel(m)
	t.Cleanup(func() { SetCallModel(old) })
}

// liftAt lifts one instruction given as hex at addr inside fn.
func liftAt(t *testing.T, l *Arm64Lifter, fn *disasm.Function, addr uint64, h string) []string {
	t.Helper()
	code, err := hex.DecodeString(h)
	if err != nil {
		t.Fatal(err)
	}
	inst, err := x86asm.Decode(code, 64)
	if err != nil {
		t.Fatalf("%s: %v", h, err)
	}
	lines, err := l.LiftInstructionToBuf(disasm.Instruction{Address: addr, Inst: inst}, addr+uint64(inst.Len), fn, nil)
	if err != nil {
		t.Fatalf("%s: %v", h, err)
	}
	var code2 []string
	for _, ln := range lines {
		if s := strings.TrimSpace(ln); s != "" && !strings.HasPrefix(s, "//") {
			code2 = append(code2, s)
		}
	}
	return code2
}

func eqLines(t *testing.T, what string, got, want []string) {
	t.Helper()
	if strings.Join(got, "\n") != strings.Join(want, "\n") {
		t.Errorf("%s:\n got:\n  %s\n want:\n  %s", what, strings.Join(got, "\n  "), strings.Join(want, "\n  "))
	}
}

// The nested lowerings are what they were before the flat model existed (RET imm16 now pops
// its operand, which the old lowering dropped).
func TestNestedCallRetLowering(t *testing.T) {
	withCallModel(t, CallNested)
	fn := &disasm.Function{EntryAddr: 0x1000, Blocks: map[uint64]*disasm.BasicBlock{}}
	l := NewArm64Lifter(map[uint64]bool{0x2000: true})
	eqLines(t, "ret", liftAt(t, l, fn, 0x1010, "c3"),
		[]string{"ldr x16, [x28, x26]", "add x26, x26, #8", "ldr x30, [sp], #16", "ret"})
	eqLines(t, "ret 0x10", liftAt(t, l, fn, 0x1010, "c21000"),
		[]string{"ldr x16, [x28, x26]", "add x26, x26, #24", "ldr x30, [sp], #16", "ret"})
	eqLines(t, "call rel", liftAt(t, l, fn, 0x1010, "e8eb0f0000"), // call 0x2000
		[]string{"sub x26, x26, #8", "movz x16, #0x1015", "str x16, [x28, x26]", "bl _fn_0x2000"})
	eqLines(t, "jmp rel (tail call)", liftAt(t, l, fn, 0x1010, "e9eb0f0000"),
		[]string{"ldr x30, [sp], #16", "b _fn_0x2000"})
}

func TestFlatCallRetLowering(t *testing.T) {
	withCallModel(t, CallFlat)
	fn := &disasm.Function{EntryAddr: 0x1000, Blocks: map[uint64]*disasm.BasicBlock{0x1015: {}}}
	l := NewArm64Lifter(map[uint64]bool{0x2000: true})
	// RET: pop the guest return address, compare {address, rsp} with the shadow top (popped
	// with post-index), adjust rsp, mismatch -> slow path, else return to the continuation.
	eqLines(t, "ret", liftAt(t, l, fn, 0x1010, "c3"), []string{
		"ldr x7, [x28, x26]",
		"ldp x17, x0, [x29], #32",
		"cmp x7, x17",
		"ccmp x26, x0, #0, eq",
		"add x26, x26, #8",
		"b.ne L_fn_0x1000_retslow",
		"ldur x30, [x29, #-16]",
		"ret",
	})
	got := liftAt(t, l, fn, 0x1010, "c21000")
	if got[4] != "add x26, x26, #24" {
		t.Errorf("ret 0x10 adjusts rsp with %q", got[4])
	}
	// A big RET imm16 needs two adds; flags must be compared before, the branch after.
	got = liftAt(t, l, fn, 0x1010, "c2f8ff") // ret 0xfff8 -> 0x10000 in total
	if strings.Join(got[4:6], "|") != "add x26, x26, #0|add x26, x26, #16, lsl #12" && strings.Join(got[4:5], "|") != "add x26, x26, #16, lsl #12" {
		t.Errorf("ret 0xfff8: %v", got)
	}
	// Without a function (hand-assembled test code) the slow path is reached directly.
	if got := strings.Join(liftAt(t, l, nil, 0x1010, "c3"), "\n"); !strings.Contains(got, "b.eq 1f\nb _recomp_ret_slow\n1:") {
		t.Errorf("ret without a function:\n%s", got)
	}
	// CALL: the same shape as nested; the return address is named by FlatRAReg at the callee's
	// call entry (x16/x17 would not survive a linker veneer).
	eqLines(t, "call rel", liftAt(t, l, fn, 0x1010, "e8eb0f0000"),
		[]string{"sub x26, x26, #8", "movz x7, #0x1015", "str x7, [x28, x26]", "bl _fn_0x2000"})
	eqLines(t, "call reg", liftAt(t, l, fn, 0x1010, "ffd0"), // call rax
		[]string{"sub x26, x26, #8", "movz x7, #0x1012", "str x7, [x28, x26]", "mov x0, x19", "bl _recomp_indirect_call"})
	// CALL to a block of the same function (call next; pop): shadow entry pushed inline.
	eqLines(t, "call next", liftAt(t, l, fn, 0x1010, "e800000000"), []string{
		"sub x26, x26, #8", "movz x7, #0x1015", "str x7, [x28, x26]",
		"adr x30, 1f", "stp x7, x26, [x29, #-32]!", "str x30, [x29, #16]", "b L_fn_0x1000_loc_0x1015", "1:",
	})
	// Tail call: the callee's jump entry, no host frame to drop.
	eqLines(t, "jmp rel (tail call)", liftAt(t, l, fn, 0x1010, "e9eb0f0000"), []string{"b _fnj_0x2000"})
	if got := liftAt(t, l, fn, 0x1010, "ffe0"); got[len(got)-1] != "b _recomp_indirect_jump" || strings.Contains(strings.Join(got, "\n"), "[sp]") {
		t.Errorf("jmp rax: %v", got)
	}
}

// In the flat model x29 is the shadow stack pointer for as long as the pc is in guest code:
// nothing but the call entry, RET and the local-call push may write it, and the host stack
// pointer is only ever used as scratch that the same lowering releases.
func TestFlatModelLeavesX29Alone(t *testing.T) {
	withCallModel(t, CallFlat)
	writesX29 := regexp.MustCompile(`^\s*(mov|add|sub|ldr|ldp|adrp|adr)\s+x29\b|,\s*x29\s*,\s*\[sp|ldp\s+x\d+,\s*x29\b`)
	cases := append(memCorpus(),
		execCase{name: "vpcmpistri (helper call)", code: []string{"c4e37963d338"}},
		execCase{name: "vaeskeygenassist (helper call)", code: []string{"c4e379dfe501"}},
		execCase{name: "pcmpestri (helper call)", code: []string{"660f3a61c10c"}},
	)
	for _, tso := range []bool{true, false} {
		l := NewArm64Lifter(nil)
		l.SetTSO(tso)
		for i, c := range cases {
			lines, _ := liftSeq(t, l, c, uint64(0x10000+i*0x1000))
			for _, ln := range lines {
				code := ln
				if k := strings.Index(code, "//"); k >= 0 {
					code = code[:k]
				}
				// The helper lowerings save and restore x29 around the call ("stp x29, x30" /
				// "ldp x29, x30"): the restore writes back the value it had.
				if strings.Contains(code, "ldp  x29, x30, [sp") {
					continue
				}
				if writesX29.MatchString(code) {
					t.Errorf("%s (tso=%v): x29 written in a flat lowering: %q", c.name, tso, ln)
				}
			}
		}
	}
}

// Inline caches (docs/m3/callic.md): the sequence the indirect CALL forms end in.
func TestCallICLowering(t *testing.T) {
	withCallModel(t, CallFlat)
	old := callIC
	SetCallIC(true)
	t.Cleanup(func() { callIC = old })
	fn := &disasm.Function{EntryAddr: 0x1000, Blocks: map[uint64]*disasm.BasicBlock{}}
	l := NewArm64Lifter(map[uint64]bool{0x2000: true})
	want := []string{
		"sub x26, x26, #8", "movz x7, #0x1012", "str x7, [x28, x26]", "mov x0, x19",
		".pushsection __DATA,__nps5_ic,regular,no_dead_strip", ".p2align 4", "4:", ".quad -1, _recomp_indirect_call, -1, _recomp_indirect_call, 0, 0", ".popsection",
		"adrp x1, 4b@PAGE", "add x1, x1, 4b@PAGEOFF",
		"ldp x2, x3, [x1]", "cmp x2, x0", "b.eq 3f", "ldp x2, x3, [x1, #16]", "cmp x2, x0", "b.eq 3f",
		"bl _recomp_indirect_call_ic", "b 1f", "3:", "blr x3", "1:",
	}
	eqLines(t, "call reg + IC", liftAt(t, l, fn, 0x1010, "ffd0"), want)
	// A direct call to a known function is not an indirect site.
	eqLines(t, "call rel", liftAt(t, l, fn, 0x1010, "e8eb0f0000"),
		[]string{"sub x26, x26, #8", "movz x7, #0x1015", "str x7, [x28, x26]", "bl _fn_0x2000"})
	// Off, or nested: the plain dispatch call.
	SetCallIC(false)
	if got := liftAt(t, l, fn, 0x1010, "ffd0"); got[len(got)-1] != "bl _recomp_indirect_call" {
		t.Errorf("IC off: %v", got)
	}
	SetCallIC(true)
	withCallModel(t, CallNested)
	if got := liftAt(t, l, fn, 0x1010, "ffd0"); got[len(got)-1] != "bl _recomp_indirect_call" {
		t.Errorf("nested ignores the IC: %v", got)
	}
}

func TestJmpICLowering(t *testing.T) {
	withCallModel(t, CallFlat)
	oldC, oldJ := callIC, jmpIC
	SetCallIC(true)
	SetJmpIC(true)
	t.Cleanup(func() { callIC, jmpIC = oldC, oldJ })
	fn := &disasm.Function{EntryAddr: 0x1000, EndAddr: 0x1100, Blocks: map[uint64]*disasm.BasicBlock{0x1000: {StartAddr: 0x1000}}}
	l := NewArm64Lifter(map[uint64]bool{0x2000: true})
	site := []string{
		".pushsection __DATA,__nps5_ic,regular,no_dead_strip", ".p2align 4", "4:", ".quad -1, _recomp_indirect_call, -1, _recomp_indirect_call, 0, 0", ".popsection",
		"adrp x1, 4b@PAGE", "add x1, x1, 4b@PAGEOFF",
		"ldp x2, x3, [x1]", "cmp x2, x0", "b.ne 7f", "br x3", "7:",
		"ldp x2, x3, [x1, #16]", "cmp x2, x0", "b.ne 8f", "br x3", "8:",
	}
	want := append([]string{"mov x0, x19"}, site...)
	want = append(want, "b _recomp_indirect_jump_ic")
	eqLines(t, "jmp reg + IC", liftAt(t, l, fn, 0x1010, "ffe0"), want) // jmp rax
	// jmp [rax+8]: the pointer load first, then the same site.
	got := liftAt(t, l, fn, 0x1010, "ff6008")
	if got[len(got)-1] != "b _recomp_indirect_jump_ic" || !strings.Contains(strings.Join(got, "\n"), "br x3") {
		t.Errorf("jmp [mem] + IC: %v", got)
	}
	// Off (or no call IC): the plain dispatch.
	SetJmpIC(false)
	if got := liftAt(t, l, fn, 0x1010, "ffe0"); !reflect.DeepEqual(got, []string{"mov x0, x19", "b _recomp_indirect_jump"}) {
		t.Errorf("jmp IC off: %v", got)
	}
	SetJmpIC(true)
	SetCallIC(false)
	if got := liftAt(t, l, fn, 0x1010, "ffe0"); got[len(got)-1] != "b _recomp_indirect_jump" {
		t.Errorf("jmp IC needs the call IC: %v", got)
	}
	SetCallIC(true)
	withCallModel(t, CallNested)
	if got := liftAt(t, l, fn, 0x1010, "ffe0"); strings.Contains(strings.Join(got, "\n"), "_ic") {
		t.Errorf("nested ignores the IC: %v", got)
	}
}
