package disasm

import (
	"encoding/binary"
	"testing"
)

// rzBuild: like spBuild, with AnalyzeStackPrivacyOpts(CallEntered) on the function at 0x100.
func rzBuild(t *testing.T, callEntered bool, frags ...[]byte) *Function {
	t.Helper()
	img := make([]byte, 0x1000)
	img[0x80] = 0xc3
	pc := 0x100
	for _, f := range frags {
		if f == nil {
			img[pc] = 0xe8
			binary.LittleEndian.PutUint32(img[pc+1:], uint32(int32(0x80-(pc+5))))
			pc += 5
			continue
		}
		copy(img[pc:], f)
		pc += len(f)
	}
	d, err := NewDisassembler(switchTestELF(img))
	if err != nil {
		t.Fatal(err)
	}
	d.SkipFlagLiveness = true
	fn, _, err := d.DisasmFunction(0x100)
	if err != nil || fn == nil {
		t.Fatalf("disasm: %v", err)
	}
	AnalyzeStackPrivacyOpts(fn, StackPrivOpts{CallEntered: callEntered}, nil)
	return fn
}

func TestLeafRedZone(t *testing.T) {
	var (
		st  = []byte{0x48, 0x89, 0x7c, 0x24, 0xf8} // mov [rsp-8], rdi
		ld  = []byte{0x48, 0x8b, 0x44, 0x24, 0xf8} // mov rax, [rsp-8]
		far = []byte{0x48, 0x89, 0x7c, 0x24, 0x80} // mov [rsp-128], rdi
		out = []byte{0x48, 0x89, 0x7c, 0x24, 0x78} // mov [rsp-136] would need disp32; see below
		ret = []byte{0xc3}
	)
	_ = out
	below := []byte{0x48, 0x89, 0xbc, 0x24, 0x77, 0xff, 0xff, 0xff} // mov [rsp-137], rdi
	above := []byte{0x48, 0x89, 0x7c, 0x24, 0x08}                   // mov [rsp+8], rdi (stack argument)
	retaddr := []byte{0x48, 0x89, 0x3c, 0x24}                       // mov [rsp], rdi (the return address)

	for _, c := range []struct {
		name string
		call bool
		code [][]byte
		want map[string]bool
	}{
		{"leaf entered by call", true, [][]byte{st, ld, far, ret}, map[string]bool{"MOV [RSP-0x8], RDI": true, "MOV RAX, [RSP-0x8]": true, "MOV [RSP-0x80], RDI": true}},
		{"leaf entered by jump", false, [][]byte{st, ld, ret}, map[string]bool{"MOV [RSP-0x8], RDI": false, "MOV RAX, [RSP-0x8]": false}},
		{"beyond the red zone", true, [][]byte{below, ret}, map[string]bool{"MOV [RSP+0xffffff77], RDI": false}},
		{"stack argument and return address stay shared", true, [][]byte{above, retaddr, ret}, map[string]bool{"MOV [RSP+0x8], RDI": false, "MOV [RSP], RDI": false}},
		// A call ends the leaf property only for the frame address escape rule: passing a red zone
		// address to the callee is an escape, so nothing in the function is private.
		{"red zone address passed to a call", true, [][]byte{{0x48, 0x8d, 0x7c, 0x24, 0xf8}, nil, st, ret}, map[string]bool{"MOV [RSP-0x8], RDI": false}},
	} {
		fn := rzBuild(t, c.call, c.code...)
		for text := range c.want {
			got := spBits(t, fn, text, 0)&StackPrivMem != 0
			if got != c.want[text] {
				t.Errorf("%s: %q private = %t, want %t", c.name, text, got, c.want[text])
			}
		}
	}
}

func TestNoteJumpOuts(t *testing.T) {
	jmp := "e9fb0f0000" // jmp 0x2000 from 0x1000 (rel32 = 0xffb)
	jmpPush := []string{"55", "e9fb0f0000"}
	for _, c := range []struct {
		name    string
		hexes   []string
		entered bool
	}{
		{"frameless leaf jumps out", []string{jmp}, true},
		{"jump after push", jmpPush, false},
		{"jump after call", []string{"e800000000", "e9f60f0000"}, false},
	} {
		fn, _ := roBlock(t, c.hexes...)
		d := &Disassembler{}
		d.noteJumpOuts(fn)
		if _, got := d.jumpIn.entered[0x2000]; got != c.entered {
			t.Errorf("%s: target jump-entered = %t, want %t", c.name, got, c.entered)
		}
	}
	// callEntered needs a complete walk and no recorded jump.
	d := &Disassembler{LeafRedZone: true}
	if d.callEntered(0x2000) {
		t.Error("no discovery: must not be call-entered")
	}
	d.jumpIn = &jumpInfo{entered: map[uint64]struct{}{0x2000: {}}, complete: true}
	if d.callEntered(0x2000) || !d.callEntered(0x3000) {
		t.Error("callEntered must follow the jump-entered set")
	}
}
