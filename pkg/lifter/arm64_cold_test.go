package lifter

import (
	"bytes"
	"encoding/binary"
	"regexp"
	"strings"
	"testing"

	"golang.org/x/arch/x86/x86asm"
)

// coldCorpus: scalar guest accesses whose address is unaligned or crosses a 16-byte boundary, in
// the folded (base + displacement) and the general form, loads and stores of every width.
func coldCorpus() []execCase {
	rb := uint64(execMemBase)
	regs := map[x86asm.Reg]uint64{
		x86asm.RAX: 0x1122334455667788, x86asm.RBX: rb + 0x10d, x86asm.RCX: 3,
		x86asm.RSI: rb + 0x20f, x86asm.RDI: rb + 0x300, x86asm.RBP: rb + 0x1000,
		x86asm.RDX: 0x80, x86asm.R8: rb + 0x41, x86asm.R9: rb + 0x501,
	}
	pat := make([]byte, 0x600)
	for i := range pat {
		pat[i] = byte(i*7 + 3)
	}
	mem := map[int][]byte{0: pat}
	mk := func(name string, code ...string) execCase {
		return execCase{name: name, code: code, regs: regs, mem: mem}
	}
	return []execCase{
		mk("load64 crossing", "488b03"),
		mk("load32 crossing", "8b06"),
		mk("load16 crossing", "668b06"),
		mk("store64 crossing", "488903"),
		mk("store32 crossing", "8906"),
		mk("store16 crossing", "668906"),
		mk("movzx16 crossing", "0fb706"),
		mk("movsx16 crossing", "480fbf06"),
		mk("movsxd crossing", "486306"),
		mk("load64 disp unaligned base", "488b4308"),
		mk("load64 disp neg", "488b43f8"),
		mk("load32 disp aligned result", "8b4303"),
		mk("store64 disp crossing", "48894308"),
		mk("store32 disp r8 crossing", "41894147"),
		mk("load64 r8 crossing", "498b4008"),
		mk("load64 aligned base", "488b4708"),
		mk("store64 aligned base", "48894708"),
		mk("load32 index", "8b448f10"),
		mk("store32 index", "89448f10"),
		mk("load64 r9 unaligned", "498b01"),
		mk("add mem crossing", "480103"),
		mk("cmp mem crossing", "483b06"),
		mk("two accesses", "488b03", "488906"),
		mk("byte", "8a03", "880e"),
	}
}

func runColdCorpus(t *testing.T, cold, tso bool) []byte {
	t.Helper()
	execCold = cold
	execNoTSO = !tso
	defer func() { execCold, execNoTSO = false, false }()
	var out []byte
	execCapture = &out
	defer func() { execCapture = nil }()
	cases := coldCorpus()
	runExecExpect(t, cases, make([]map[int][]byte, len(cases)))
	return out
}

// With outlining on, every access gives the same registers and memory as with it off, and as the
// plain access of the TSO-off lowering (a single thread cannot tell the orderings apart).
func TestColdOutlineExec(t *testing.T) {
	off := runColdCorpus(t, false, true)
	on := runColdCorpus(t, true, true)
	plain := runColdCorpus(t, false, false)
	if len(off) == 0 || len(off) != len(on) || len(off) != len(plain) {
		t.Fatalf("output sizes %d, %d, %d", len(off), len(on), len(plain))
	}
	rec := stSize + execMemSize
	// The first case loads 8 bytes across a 16-byte boundary: RAX must hold them (the test is
	// not comparing three runs that all did nothing).
	var want uint64
	for k := 7; k >= 0; k-- {
		want = want<<8 | uint64(byte((0x10d+k)*7+3))
	}
	if got := binary.LittleEndian.Uint64(on[stRegs:]); got != want {
		t.Fatalf("crossing load: RAX = %#x, want %#x", got, want)
	}
	for i, c := range coldCorpus() {
		o, n, p := off[i*rec:(i+1)*rec], on[i*rec:(i+1)*rec], plain[i*rec:(i+1)*rec]
		// The state block holds GPRs and flags (the FS base and scratch past stFlags differ by run).
		if !bytes.Equal(o[:stFlags], n[:stFlags]) || !bytes.Equal(o[stSize:], n[stSize:]) {
			t.Errorf("%s: outlined result differs from inline", c.name)
		}
		if !bytes.Equal(n[:stFlags], p[:stFlags]) || !bytes.Equal(n[stSize:], p[stSize:]) {
			t.Errorf("%s: outlined result differs from the plain (TSO off) access", c.name)
		}
	}
}

var (
	coldBranchRe = regexp.MustCompile(`(?m)^\s+b\s+(L_tso_cold_\d+)\s*$`)
	coldLabelRe  = regexp.MustCompile(`(?m)^(L_tso_cold_\d+):\s*$`)
	coldFenceRe  = regexp.MustCompile(`(?m)^\s+dmb ish`)
)

// The shape: the hot stream branches to each outlined body and never contains the fence; the cold
// buffer defines every label once and ends each body with a branch back to a hot label.
func TestColdOutlineShape(t *testing.T) {
	defer SetColdOutline(GetColdOutline())
	for _, code := range []string{"488b03", "488b4308", "488903", "48894308", "8b448f10"} {
		SetColdOutline(true)
		l := NewArm64Lifter(nil)
		hot := strings.Join(liftHex(t, l, code), "\n")
		cold := strings.Join(l.TakeCold(), "\n")
		if coldFenceRe.MatchString(hot) {
			t.Errorf("%s: fence left in the hot stream:\n%s", code, hot)
		}
		br := coldBranchRe.FindAllStringSubmatch(hot, -1)
		lb := coldLabelRe.FindAllStringSubmatch(cold, -1)
		if len(br) == 0 || len(br) != len(lb) {
			t.Fatalf("%s: %d branches to cold, %d cold labels\nhot:\n%s\ncold:\n%s", code, len(br), len(lb), hot, cold)
		}
		for i := range br {
			if br[i][1] != lb[i][1] {
				t.Errorf("%s: branch to %s, body %s", code, br[i][1], lb[i][1])
			}
		}
		if !coldFenceRe.MatchString(cold) || !strings.Contains(cold, "\n    b L_tso_done_") && !strings.Contains(cold, "    b L_tso_done_") {
			t.Errorf("%s: cold body lacks the fence or the branch back:\n%s", code, cold)
		}
		if again := l.TakeCold(); len(again) != 0 {
			t.Errorf("%s: TakeCold did not empty the buffer", code)
		}

		// Off: no cold lines, and the text is the inline form.
		SetColdOutline(false)
		l = NewArm64Lifter(nil)
		inline := strings.Join(liftHex(t, l, code), "\n")
		if len(l.TakeCold()) != 0 || strings.Contains(inline, "L_tso_cold_") || !coldFenceRe.MatchString(inline) {
			t.Errorf("%s: off must be the inline form:\n%s", code, inline)
		}
	}
}

// ConfigKey carries the switch, so a chunk cache cannot serve text lifted the other way.
func TestColdOutlineInConfigKey(t *testing.T) {
	defer SetColdOutline(GetColdOutline())
	SetColdOutline(true)
	on := NewArm64Lifter(nil).ConfigKey()
	SetColdOutline(false)
	off := NewArm64Lifter(nil).ConfigKey()
	if on == off || !strings.Contains(on, "cold=true") || !strings.Contains(off, "cold=false") {
		t.Errorf("ConfigKey does not follow -cold-outline:\n%s\n%s", on, off)
	}
}
