package analyzer

import (
	"testing"

	"golang.org/x/arch/x86/x86asm"

	"ps4-recomp/pkg/disasm"
)

func insts(t *testing.T, code []byte) []disasm.Instruction {
	t.Helper()
	var out []disasm.Instruction
	for pc := 0; pc < len(code); {
		in, err := x86asm.Decode(code[pc:], 64)
		if err != nil {
			t.Fatal(err)
		}
		out = append(out, disasm.Instruction{Address: uint64(pc), Inst: in})
		pc += in.Len
	}
	return out
}

// A jmp through a register right after the frame is torn down is a tail call, not a switch.
func TestClassifyTailEpilogue(t *testing.T) {
	// mov rax,[rip+8]; pop rbp; jmp rax
	in := insts(t, []byte{0x48, 0x8b, 0x05, 0x08, 0, 0, 0, 0x5d, 0xff, 0xe0})
	jmp := in[len(in)-1]
	if got := classifyTail(in, jmp); got != "epilogue" {
		t.Fatalf("kind = %q", got)
	}
	// mov rax,[rax+rcx*8]; jmp rax: not an epilogue
	in = insts(t, []byte{0x48, 0x8b, 0x04, 0xc8, 0xff, 0xe0})
	if got := classifyTail(in, in[len(in)-1]); got == "epilogue" {
		t.Fatalf("switch dispatch classified as %q", got)
	}
}
