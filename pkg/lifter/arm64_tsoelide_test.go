package lifter

import (
	"bytes"
	"encoding/hex"
	"fmt"
	"strings"
	"testing"

	"ps4-recomp/pkg/disasm"

	"golang.org/x/arch/x86/x86asm"
)

// liftElide lowers one instruction (hex) with the given stack privacy bits.
func liftElide(t *testing.T, l *Arm64Lifter, h string, bits uint8) string {
	t.Helper()
	b, err := hex.DecodeString(h)
	if err != nil {
		t.Fatal(err)
	}
	inst, err := x86asm.Decode(b, 64)
	if err != nil {
		t.Fatalf("decode %s: %v", h, err)
	}
	fn := &disasm.Function{EntryAddr: 0x1000, EndAddr: 0x1100, Blocks: map[uint64]*disasm.BasicBlock{0x1000: {StartAddr: 0x1000}}}
	out, err := l.LiftInstructionToBuf(disasm.Instruction{Address: 0x1000, Inst: inst, ParityDead: true, AdjustDead: true, StackPriv: bits}, 0x1000+uint64(inst.Len), fn, nil)
	if err != nil {
		t.Fatalf("lift %s: %v", h, err)
	}
	return strings.Join(out, "\n")
}

func isOrdered(asm string) bool {
	return strings.Contains(asm, "ldapr") || strings.Contains(asm, "ldapur") || strings.Contains(asm, "stlr") || strings.Contains(asm, "stlur")
}

// -tso-elide stack: the privacy bits decide, not the base register.
func TestTSOElideStackShapes(t *testing.T) {
	const seen, mem, pop = disasm.StackPrivSeen, disasm.StackPrivSeen | disasm.StackPrivMem, disasm.StackPrivSeen | disasm.StackPrivPop
	for _, mode := range []AddrMode{AddrBase, AddrIdentity} {
		l := NewArm64LifterMode(nil, mode)
		l.SetTSOElide(TSOElideStack)
		for _, c := range []struct {
			name, hex string
			bits      uint8
			ordered   bool
		}{
			{"mov rax,[rsp+8] shared", "488b442408", seen, true},
			{"mov rax,[rsp+8] private", "488b442408", mem, false},
			{"mov rax,[rsp+8] not analysed", "488b442408", 0, true},
			{"mov rax,[rsp] shared", "488b0424", seen, true},
			{"mov rax,[rsp] private", "488b0424", mem, false},
			{"mov [rsp],rax shared", "48890424", seen, true},
			{"mov [rbp-8],rdi shared", "48897df8", seen, true},
			{"mov [rbp-8],rdi private", "48897df8", mem, false},
			{"mov al,[rsp+1] shared", "8a442401", seen, true},
			{"add [rsp+8],rax shared", "4801442408", seen, true},
			{"add [rsp+8],rax private", "4801442408", mem, false},
			{"mov eax,[rdi+8] derived private", "8b4708", mem, false},
			{"mov eax,[rdi+8] shared", "8b4708", seen, true},
			{"pop rbx save slot", "5b", pop, false},
			{"pop rbx escaped slot", "5b", seen, true},
			{"pop rbx not analysed", "5b", 0, true},
			{"leave save slot", "c9", pop, false},
			{"leave unknown slot", "c9", seen, true},
			{"push rax", "50", seen, false},
			{"push rax not analysed", "50", 0, false},
			{"push [rsp+8] shared", "ff742408", seen, true},
			{"push [rsp+8] private", "ff742408", mem, false},
			{"push [rsp] shared", "ff3424", seen, true},
			{"pop [rdi] save slot, shared target", "8f07", pop, true},
			{"pushfq", "9c", seen, false},
			{"popfq", "9d", seen, false},
			{"movaps xmm0,[rsp+16] (vector: never ordered)", "0f28442410", seen, false},
		} {
			got := liftElide(t, l, c.hex, c.bits)
			if isOrdered(got) != c.ordered {
				t.Errorf("%v %s: ordered = %t, want %t\n%s", mode, c.name, !c.ordered, c.ordered, got)
			}
		}
		// A push stores through the stack pointer with a plain store even when its operand is
		// loaded with an acquire load; a pop into shared memory loads plain from a save slot.
		if got := liftElide(t, l, "ff742408", seen); strings.Contains(got, "stlr") || strings.Contains(got, "stlur") {
			t.Errorf("%v push [rsp+8]: the push itself must stay plain\n%s", mode, got)
		}
		if got := liftElide(t, l, "8f07", pop); strings.Contains(got, "ldapr") || strings.Contains(got, "ldapur") {
			t.Errorf("%v pop [rdi] from a save slot: the pop load must stay plain\n%s", mode, got)
		}
		if got := liftElide(t, l, "8f07", seen); !strings.Contains(got, "ldapr") || !strings.Contains(got, "stl") {
			t.Errorf("%v pop [rdi] from an escaped slot: both accesses ordered\n%s", mode, got)
		}
	}
}

// -tso-elide stack-region reads the region bits, -tso-elide stack does not.
func TestTSOElideRegionBits(t *testing.T) {
	const region = disasm.StackPrivSeen | disasm.StackPrivMemRegion | disasm.StackPrivPopRegion
	st, rg := NewArm64Lifter(nil), NewArm64Lifter(nil)
	st.SetTSOElide(TSOElideStack)
	rg.SetTSOElide(TSOElideRegion)
	for _, h := range []string{"488b442408", "48897df8", "5b", "c9"} {
		if !isOrdered(liftElide(t, st, h, region)) {
			t.Errorf("%s: stack mode must not use the region bits", h)
		}
		if isOrdered(liftElide(t, rg, h, region)) {
			t.Errorf("%s: stack-region mode ignores the region bits", h)
		}
		if !isOrdered(liftElide(t, rg, h, disasm.StackPrivSeen)) {
			t.Errorf("%s: stack-region mode without bits must be ordered", h)
		}
	}
	if m, err := ParseTSOElide("stack-region"); err != nil || m != TSOElideRegion || m.String() != "stack-region" {
		t.Error("ParseTSOElide(stack-region)")
	}
	if st.ConfigKey() == rg.ConfigKey() {
		t.Error("the two modes must have different chunk cache keys")
	}
}

// -tso-elide off is the lowering the lifter had before the option: the bits change nothing, and
// the configuration key is unchanged.
func TestTSOElideOffIgnoresBits(t *testing.T) {
	def := NewArm64Lifter(nil)
	off := NewArm64Lifter(nil)
	off.SetTSOElide(TSOElideOff)
	if def.ConfigKey() != off.ConfigKey() || strings.Contains(def.ConfigKey(), "elide") {
		t.Errorf("default key %q, off key %q", def.ConfigKey(), off.ConfigKey())
	}
	st := NewArm64Lifter(nil)
	st.SetTSOElide(TSOElideStack)
	if st.ConfigKey() == def.ConfigKey() {
		t.Error("the stack mode must change the chunk cache key")
	}
	if c := st.Clone(); c.TSOElide() != TSOElideStack {
		t.Error("Clone drops the mode")
	}
	for _, h := range []string{"488b442408", "48897df8", "5b", "c9", "50", "8b4708", "ff742408", "8f07", "4801442408"} {
		// Fresh lifters: the label counter is part of the text.
		a := liftElide(t, NewArm64Lifter(nil), h, 0)
		for _, bits := range []uint8{disasm.StackPrivSeen, disasm.StackPrivSeen | disasm.StackPrivMem, disasm.StackPrivSeen | disasm.StackPrivPop, 0xff} {
			off := NewArm64Lifter(nil)
			off.SetTSOElide(TSOElideOff)
			if b := liftElide(t, off, h, bits); a != b {
				t.Errorf("%s bits %#x: off mode output depends on the bits\n%s\n---\n%s", h, bits, a, b)
			}
		}
	}
	if isOrdered(liftElide(t, def, "488b442408", 0)) || !isOrdered(liftElide(t, def, "8b4708", 0)) {
		t.Error("off mode: rsp-relative plain, other bases ordered")
	}
	if m, err := ParseTSOElide("stack"); err != nil || m != TSOElideStack {
		t.Error("ParseTSOElide(stack)")
	}
	if _, err := ParseTSOElide("all"); err == nil {
		t.Error("ParseTSOElide accepts an unknown mode")
	}
}

// stackCorpus: stack-pointer and frame-pointer accesses of every size, pushes and pops, with the
// stack pointer at every alignment inside a 16-byte block (the ordered forms check for an access
// that crosses a 16-byte boundary).
func stackCorpus() []execCase {
	pat := make([]byte, 0x600)
	for i := range pat {
		pat[i] = byte(i*29 + 0x41)
	}
	seqs := []struct {
		name string
		code []string
	}{
		{"push/pop", []string{"50", "51", "5a", "5e"}},
		{"store/load 64", []string{"4889442408", "488b4c2408"}},
		{"store/load 64 [rsp]", []string{"48890424", "488b1424"}},
		{"store 32 / movsxd", []string{"89442408", "48634c2408"}},
		{"store 16 / movzx / movsx", []string{"6689442402", "0fb74c2402", "0fbf542402"}},
		{"store 8 / movsx", []string{"88442401", "480fbe4c2401"}},
		{"rmw add", []string{"4801442408", "48034c2408"}},
		{"frame", []string{"55", "4889e5", "4883ec20", "488945f8", "488b4df8", "894dec", "8b55ec", "c9"}},
		{"pop [rsp+8]", []string{"50", "8f442408"}},
		{"pop [rsp]", []string{"50", "8f0424"}},
		{"push [rsp+8]", []string{"ff742408", "5a"}},
		{"push [rsp]", []string{"ff3424", "5a"}},
		{"cmp [rsp+8],3", []string{"48837c240803"}},
		{"indexed", []string{"488b44cc08", "48894cd410"}},
		{"xchg", []string{"4887442408"}},
		{"lock add", []string{"f04801442408"}},
		{"pushfq/popfq", []string{"9c", "58", "50", "9d"}},
		{"pop rsp-relative chain", []string{"50", "4889e1", "58", "488b11"}}, // push rax; mov rcx,rsp; pop rax; mov rdx,[rcx]
	}
	var cases []execCase
	for _, s := range seqs {
		for _, off := range []uint64{0, 1, 4, 7, 8, 9, 12, 15} {
			if (strings.HasPrefix(s.name, "lock") || s.name == "xchg") && off%8 != 0 {
				continue // a locked access that crosses a 16-byte boundary faults by design
			}
			c := execCase{
				name: fmt.Sprintf("%s rsp+%d", s.name, off),
				code: s.code,
				regs: map[x86asm.Reg]uint64{x86asm.RAX: 0x1122334455667788, x86asm.RCX: 1, x86asm.RDX: 2,
					x86asm.RSI: 0x8fedcba987654321, x86asm.RSP: execRSP + off, x86asm.RBP: execMemBase + 0x200},
				mem: map[int][]byte{0x1500: pat},
			}
			if strings.HasPrefix(s.name, "cmp") || strings.HasPrefix(s.name, "rmw") || strings.HasPrefix(s.name, "lock") {
				c.cmpFlags = fCF | fZF | fSF | fOF
			} else {
				c.noFlags = !strings.HasPrefix(s.name, "pushfq")
			}
			cases = append(cases, c)
		}
	}
	return cases
}

// The ordered forms of stack accesses compute what the plain ones do: the stack corpus and the
// memory corpus run on the host with -tso-elide off, with -tso-elide stack and nothing private
// (every access ordered, pops included), and with -tso-elide stack and everything private.
func TestTSOElideStackExec(t *testing.T) {
	cases := append(stackCorpus(), memCorpus()...)
	run := func(mode TSOElideMode, bits uint8) []byte {
		execTSOElide = mode
		defer func() { execTSOElide = TSOElideOff }()
		cs := make([]execCase, len(cases))
		for i, c := range cases {
			prev := c.prep
			c.prep = func(k int, in *disasm.Instruction) {
				if prev != nil {
					prev(k, in)
				}
				in.StackPriv = bits
			}
			cs[i] = c
		}
		var out []byte
		execCapture = &out
		defer func() { execCapture = nil }()
		runExecExpectTSO(t, cs, true)
		return out
	}
	ref := run(TSOElideOff, 0)
	rec := stSize + execMemSize
	if len(ref) != len(cases)*rec {
		t.Fatalf("reference output: %d bytes for %d cases", len(ref), len(cases))
	}
	for _, v := range []struct {
		name string
		bits uint8
	}{
		{"all ordered", disasm.StackPrivSeen},
		{"all private", disasm.StackPrivSeen | disasm.StackPrivMem | disasm.StackPrivPop},
	} {
		got := run(TSOElideStack, v.bits)
		if len(got) != len(ref) {
			t.Fatalf("%s: output sizes %d vs %d", v.name, len(got), len(ref))
		}
		for i, c := range cases {
			a, b := ref[i*rec:(i+1)*rec], got[i*rec:(i+1)*rec]
			if !bytes.Equal(a[:stFlags+8], b[:stFlags+8]) || !bytes.Equal(a[stSize:], b[stSize:]) {
				t.Errorf("%s: %s: result differs from -tso-elide off", v.name, c.name)
			}
		}
	}
}

// The privacy rules id is in the key of every non-off mode and absent from off.
func TestTSOElideConfigKeyHasRules(t *testing.T) {
	st := NewArm64Lifter(nil)
	st.SetTSOElide(TSOElideStack)
	if !strings.Contains(st.ConfigKey(), "priv="+disasm.StackPrivRulesID()) {
		t.Errorf("key %q lacks the rules id", st.ConfigKey())
	}
	if strings.Contains(NewArm64Lifter(nil).ConfigKey(), "priv=") {
		t.Error("off key must stay unchanged")
	}
}
