package lifter

import (
	"bytes"
	"fmt"
	"strings"
	"testing"

	"golang.org/x/arch/x86/x86asm"
)

func liftPeep(t *testing.T, h string) []string {
	t.Helper()
	l := NewArm64Lifter(nil)
	l.SetPeephole(true)
	return liftHex(t, l, h)
}

// The copies of the effective-address and result scratch registers are gone, and an access at
// base + small displacement uses LDAPUR/STLUR without materialising the guest address.
func TestPeepholeShapes(t *testing.T) {
	// movzx eax, byte [rsi+0x24]: two instructions.
	lines := liftPeep(t, "0fb64624")
	requireInOrder(t, "movzx byte", lines, "add x7, x28, x23", "ldapurb w19, [x7, #36]")
	forbid(t, "movzx byte", lines, "mov x16, x23", "add x16, ...")
	// mov r9d, [rsi+0x14]: aligned fast path of two instructions before the access, exact check behind it.
	lines = liftPeep(t, "448b4e14")
	requireInOrder(t, "mov dword", lines, "add x7, x28, x23", "and x16, x7, #3", "cbz x16, L_tso_al_...", "add x16, x7, #20",
		"add x7, x16, #3", "eor x7, x7, x16", "tbz x7, #4, L_tso_ok_...", "ldr w9, [x16]", "dmb ishld", "b L_tso_done_...",
		"ldapr w9, [x16]", "b L_tso_done_...", "ldapur w9, [x7, #20]")
	// mov [rdi+8], rax: release store with the same shape.
	lines = liftPeep(t, "48894708")
	requireInOrder(t, "store qword", lines, "add x7, x28, x24", "and x16, x7, #7", "cbz x16, ...", "dmb ish", "str x19, [x16]",
		"stlr x19, [x16]", "stlur x19, [x7, #8]")
	// movsxd rax, dword [rdi+4]: the sign-extending acquire load.
	requireInOrder(t, "movsxd", liftPeep(t, "48634704"), "ldapursw x19, [x7, #4]")
	// A displacement that is not a multiple of the access size keeps the general form (one add for the address).
	lines = liftPeep(t, "8b4705") // mov eax, [rdi+5]
	requireInOrder(t, "odd disp", lines, "add x16, x24, #5", "add x7, x16, #3", "eor x7, x7, x16", "tbz x7, #4, ...", "ldapr w19, [x7]")
	forbid(t, "odd disp", lines, "mov x16, x24")
	// mov rax, [rdi+rcx*8]: one add for the address.
	requireInOrder(t, "index", liftPeep(t, "488b04cf"), "add x16, x24, x21, lsl #3", "ldapr x19, [x7]")
	// A stack access: no copy of rsp.
	lines = liftPeep(t, "488b442408") // mov rax, [rsp+8]
	requireInOrder(t, "rsp load", lines, "add x16, x26, #8", "ldr x19, [x28, x16]")
	forbid(t, "rsp load", lines, "mov x16, x26", "ldapr ...", "ldapur ...")
	// lea rdi, [rip+0x100]: the constant is built in the guest register.
	lines = liftPeep(t, "488d3d00010000")
	forbid(t, "lea rip", lines, "mov x24, x16")
	if got := strings.Join(lines, "\n"); !strings.Contains(got, "x24, #") {
		t.Errorf("lea rip: constant not built in x24:\n%s", got)
	}
	// mov eax, [rip+0x100] at 0x10000: the address 0x10106 is a constant, the access cannot cross: no check.
	lines = liftPeep(t, "8b0500010000")
	requireInOrder(t, "rip load", lines, "movk x16, ...", "add x7, x28, x16", "ldapr w19, [x7]")
	forbid(t, "rip load", lines, "tbz ...", "eor ...", "dmb ishld")
	// mov rax, [rip+0x107] -> 0x1010e, 8 bytes: crosses a 16-byte boundary, so the fenced plain load, nothing else.
	lines = liftPeep(t, "488b0507010000")
	requireInOrder(t, "rip load crossing", lines, "ldr x19, [x28, x16]", "dmb ishld")
	forbid(t, "rip load crossing", lines, "ldapr ...", "tbz ...")
	// A read-modify-write keeps its address register: it is used twice.
	lines = liftPeep(t, "014708") // add [rdi+8], eax
	requireInOrder(t, "rmw", lines, "add x16, x24, #8")
	forbid(t, "rmw", lines, "ldapur ...", "stlur ...")
}

// alignCorpus: loads and stores of every width at bases of every alignment within a 16-byte block,
// so the aligned fast path, the unaligned in-block path and the boundary-crossing fenced path of
// the folded access forms all run, next to the forms the copy propagation rewrites.
func alignCorpus() []execCase {
	rb := uint64(execMemBase)
	pat := make([]byte, 0x600)
	for i := range pat {
		pat[i] = byte(i*13 + 0x85)
	}
	var cases []execCase
	codes := []struct{ name, hex string }{
		{"mov eax,[rdi+8]", "8b4708"}, {"mov rax,[rdi+8]", "488b4708"}, {"movzx eax,word[rdi+8]", "0fb74708"},
		{"movsx eax,word[rdi+8]", "0fbf4708"}, {"movsx rax,byte[rdi+9]", "480fbe4709"}, {"movsxd rax,[rdi+8]", "48634708"},
		{"mov rax,[rdi-8]", "488b47f8"}, {"mov eax,[rdi+5]", "8b4705"}, {"movzx eax,byte[rdi+0x24]", "0fb64724"},
		{"mov [rdi+8],eax", "894708"}, {"mov [rdi+8],rax", "48894708"}, {"mov [rdi+8],ax", "66894708"}, {"mov [rdi+3],al", "884703"},
		{"mov [rdi-16],rsi", "488977f0"}, {"mov rdi,[rdi+8]", "488b7f08"}, {"mov eax,[rdi]", "8b07"}, {"mov [rdi],rax", "488907"},
		{"mov rax,[rdi+rcx*8+16]", "488b44cf10"}, {"add [rdi+8],eax", "014708"}, {"add eax,[rdi+8]", "034708"},
		{"cmp dword[rdi+8],3", "837f0803"}, {"lea rax,[rdi+rcx*4+7]", "488d448f07"}, {"shrx rax,rsi,rcx", "c4e2f3f7c6"},
		{"shlx eax,esi,ecx", "c4e271f7c6"}, {"sarx rax,rsi,rcx", "c4e2f2f7c6"},
	}
	for _, c := range codes {
		for _, off := range []uint64{0, 1, 2, 3, 4, 7, 8, 9, 12, 13, 14, 15} {
			cases = append(cases, execCase{
				name: fmt.Sprintf("%s base+%d", c.name, off),
				code: []string{c.hex},
				regs: map[x86asm.Reg]uint64{x86asm.RAX: 0x1122334455667788, x86asm.RDI: rb + 0x100 + off, x86asm.RSI: 0x8fedcba987654321,
					x86asm.RCX: 0x43},
				mem:     map[int][]byte{0: pat},
				noFlags: true,
			})
		}
	}
	return cases
}

// The peephole pass and the folded acquire/release forms change no result: the same code lifted
// with and without them, run on the host, leaves identical registers, flags and memory.
func TestPeepholeMatchesPlainLowering(t *testing.T) {
	for _, tso := range []bool{true, false} {
		cases := append(memCorpus(), alignCorpus()...)
		run := func(noPeep bool) []byte {
			execNoPeephole = noPeep
			defer func() { execNoPeephole = false }()
			var out []byte
			execCapture = &out
			defer func() { execCapture = nil }()
			runExecExpectTSO(t, cases, tso)
			return out
		}
		with, without := run(false), run(true)
		if len(with) == 0 || len(with) != len(without) {
			t.Fatalf("tso=%v: output sizes %d vs %d", tso, len(with), len(without))
		}
		rec := stSize + execMemSize
		for i, c := range cases {
			a, b := with[i*rec:(i+1)*rec], without[i*rec:(i+1)*rec]
			if !bytes.Equal(a[:stFlags+8], b[:stFlags+8]) || !bytes.Equal(a[stSize:], b[stSize:]) {
				t.Errorf("tso=%v %s: result with the peephole pass differs from the plain lowering", tso, c.name)
			}
		}
	}
}
