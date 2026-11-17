package disasm

import (
	"encoding/binary"
	"strings"
	"testing"

	"golang.org/x/arch/x86/x86asm"
)

// spBuild assembles a function at 0x100 from byte fragments; a nil fragment is a CALL to a
// one-instruction function at 0x80.
func spBuild(t *testing.T, frags ...[]byte) (*Function, StackPrivInfo) {
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
	return fn, AnalyzeStackPrivacy(fn)
}

// spText is the instruction text without the decoder's "+Reg(0)" for an index-less SIB byte.
func spText(in Instruction) string { return strings.ReplaceAll(in.Inst.String(), "+Reg(0)", "") }

// spBits returns the StackPriv bits of the n-th instruction (in address order) whose text is s.
func spBits(t *testing.T, fn *Function, s string, n int) uint8 {
	t.Helper()
	for _, a := range fn.BlockOrder {
		for _, in := range fn.Blocks[a].Insts {
			if spText(in) == s {
				if n == 0 {
					if in.StackPriv&StackPrivSeen == 0 {
						t.Fatalf("%q not classified", s)
					}
					return in.StackPriv
				}
				n--
			}
		}
	}
	var all []string
	for _, a := range fn.BlockOrder {
		for _, in := range fn.Blocks[a].Insts {
			all = append(all, spText(in))
		}
	}
	t.Fatalf("instruction %q not found in %q", s, all)
	return 0
}

func wantMem(t *testing.T, fn *Function, s string, n int, private bool) {
	t.Helper()
	if got := spBits(t, fn, s, n)&StackPrivMem != 0; got != private {
		t.Errorf("%q #%d: private = %t, want %t", s, n, got, private)
	}
}

func wantPop(t *testing.T, fn *Function, s string, n int, private bool) {
	t.Helper()
	if got := spBits(t, fn, s, n)&StackPrivPop != 0; got != private {
		t.Errorf("%q #%d: pop private = %t, want %t", s, n, got, private)
	}
}

var (
	pushRBX   = []byte{0x53}
	popRBX    = []byte{0x5b}
	pushRBP   = []byte{0x55}
	popRBP    = []byte{0x5d}
	pushRAX   = []byte{0x50}
	popRAX    = []byte{0x58}
	ret       = []byte{0xc3}
	leave     = []byte{0xc9}
	movRbpRsp = []byte{0x48, 0x89, 0xe5}
	subRsp20  = []byte{0x48, 0x83, 0xec, 0x20}
	addRsp20  = []byte{0x48, 0x83, 0xc4, 0x20}
	stSp8Rdi  = []byte{0x48, 0x89, 0x7c, 0x24, 0x08} // mov [rsp+8], rdi
	ldRaxSp8  = []byte{0x48, 0x8b, 0x44, 0x24, 0x08} // mov rax, [rsp+8]
)

func TestStackPrivLeafFrame(t *testing.T) {
	fn, info := spBuild(t,
		pushRBX, subRsp20, stSp8Rdi, ldRaxSp8,
		[]byte{0x48, 0x8d, 0x4c, 0x24, 0x10}, // lea rcx, [rsp+0x10]
		[]byte{0x48, 0x89, 0x01},             // mov [rcx], rax        derived pointer
		[]byte{0x48, 0x8b, 0x54, 0x24, 0x30}, // mov rdx, [rsp+0x30]   stack argument
		[]byte{0x48, 0x8b, 0x44, 0x24, 0xf8}, // mov rax, [rsp-8]      red zone
		[]byte{0x48, 0x8b, 0x44, 0x24, 0x28}, // mov rax, [rsp+0x28]   return address
		[]byte{0x48, 0x8b, 0x04, 0xcc},       // mov rax, [rsp+rcx*8]  index (rcx is a frame pointer: not an index)
		[]byte{0x48, 0x8b, 0x44, 0xf4, 0x08}, // mov rax, [rsp+rsi*8+8] indexed local
		[]byte{0x48, 0x8b, 0x07},             // mov rax, [rdi]        not stack
		addRsp20, popRBX, ret)
	if info.Escaped || info.Poison != "" {
		t.Fatalf("info = %+v", info)
	}
	wantMem(t, fn, "MOV [RSP+0x8], RDI", 0, true)
	wantMem(t, fn, "MOV RAX, [RSP+0x8]", 0, true)
	wantMem(t, fn, "MOV [RCX], RAX", 0, true)
	wantMem(t, fn, "MOV RDX, [RSP+0x30]", 0, false)
	wantMem(t, fn, "MOV RAX, [RSP-0x8]", 0, true) // red zone of a function with a frame of its own
	wantMem(t, fn, "MOV RAX, [RSP+0x28]", 0, false)
	wantMem(t, fn, "MOV RAX, [RSP+8*RCX]", 0, false)
	wantMem(t, fn, "MOV RAX, [RSP+8*RSI+0x8]", 0, true)
	wantMem(t, fn, "MOV RAX, [RDI]", 0, false)
	wantPop(t, fn, "POP RBX", 0, true)
}

func TestStackPrivEscapeByCall(t *testing.T) {
	fn, info := spBuild(t,
		pushRBX, subRsp20, stSp8Rdi,
		[]byte{0x48, 0x8d, 0x7c, 0x24, 0x10}, // lea rdi, [rsp+0x10]
		nil,                                  // call
		ldRaxSp8, addRsp20, popRBX, ret)
	if !info.Escaped || info.EscapeWhy != "call" {
		t.Fatalf("info = %+v", info)
	}
	// Function-wide: the store before the call is ordered too (a longjmp may re-enter).
	wantMem(t, fn, "MOV [RSP+0x8], RDI", 0, false)
	wantMem(t, fn, "MOV RAX, [RSP+0x8]", 0, false)
	wantPop(t, fn, "POP RBX", 0, true) // register save slot
}

func TestStackPrivEscapeByStore(t *testing.T) {
	fn, info := spBuild(t,
		subRsp20,
		[]byte{0x48, 0x8d, 0x44, 0x24, 0x08}, // lea rax, [rsp+8]
		[]byte{0x48, 0x89, 0x07},             // mov [rdi], rax
		stSp8Rdi, addRsp20, ret)
	if !info.Escaped || info.EscapeWhy != "stored" {
		t.Fatalf("info = %+v", info)
	}
	wantMem(t, fn, "MOV [RSP+0x8], RDI", 0, false)
}

func TestStackPrivEscapeToVector(t *testing.T) {
	_, info := spBuild(t,
		subRsp20,
		[]byte{0x48, 0x8d, 0x44, 0x24, 0x08}, // lea rax, [rsp+8]
		[]byte{0x66, 0x48, 0x0f, 0x6e, 0xc0}, // movq xmm0, rax
		[]byte{0xf3, 0x0f, 0x7f, 0x07},       // movdqu [rdi], xmm0
		addRsp20, ret)
	if !info.Escaped {
		t.Fatalf("a frame address moved to a vector register must count as escaped: %+v", info)
	}
}

func TestStackPrivFramePointer(t *testing.T) {
	fn, info := spBuild(t,
		pushRBP, movRbpRsp,
		[]byte{0x48, 0x83, 0xec, 0x10}, // sub rsp, 0x10
		[]byte{0x48, 0x89, 0x7d, 0xf8}, // mov [rbp-8], rdi
		nil,                            // call: rbp is exempt, no argument register holds a frame address
		[]byte{0x48, 0x8b, 0x45, 0xf8}, // mov rax, [rbp-8]
		[]byte{0x48, 0x8b, 0x45, 0x10}, // mov rax, [rbp+0x10]   stack argument
		leave, ret)
	if info.Escaped || info.Poison != "" {
		t.Fatalf("info = %+v", info)
	}
	wantMem(t, fn, "MOV [RBP-0x8], RDI", 0, true)
	wantMem(t, fn, "MOV RAX, [RBP-0x8]", 0, true)
	wantMem(t, fn, "MOV RAX, [RBP+0x10]", 0, false)
	wantPop(t, fn, "LEAVE", 0, true)
}

// RBP as a general pointer: today's syntactic rule emits these loads plain.
func TestStackPrivRBPGeneralPointer(t *testing.T) {
	fn, info := spBuild(t,
		pushRBP,
		[]byte{0x48, 0x89, 0xfd},       // mov rbp, rdi
		[]byte{0x48, 0x8b, 0x45, 0x08}, // mov rax, [rbp+8]
		popRBP, ret)
	if info.Escaped {
		t.Fatalf("info = %+v", info)
	}
	wantMem(t, fn, "MOV RAX, [RBP+0x8]", 0, false)
	wantPop(t, fn, "POP RBP", 0, true)
}

// push rax; mov rdi, rsp; call f; pop rax: the popped slot was handed to the callee.
func TestStackPrivPopOfEscapedSlot(t *testing.T) {
	fn, info := spBuild(t,
		pushRAX,
		[]byte{0x48, 0x89, 0xe7}, // mov rdi, rsp
		nil, popRAX, ret)
	if !info.Escaped {
		t.Fatalf("info = %+v", info)
	}
	wantPop(t, fn, "POP RAX", 0, false)
}

// The same shape without the address: the pop is private.
func TestStackPrivPopOfPrivateSlot(t *testing.T) {
	fn, info := spBuild(t, pushRAX, nil, popRAX, ret)
	if info.Escaped {
		t.Fatalf("info = %+v", info)
	}
	wantPop(t, fn, "POP RAX", 0, true)
}

// A frame address kept in a callee-saved register across a call is treated as passed.
func TestStackPrivCalleeSavedAcrossCall(t *testing.T) {
	fn, info := spBuild(t,
		pushRBX, subRsp20,
		[]byte{0x48, 0x8d, 0x5c, 0x24, 0x08}, // lea rbx, [rsp+8]
		nil,
		[]byte{0x48, 0x8b, 0x03}, // mov rax, [rbx]
		addRsp20, popRBX, ret)
	if !info.Escaped {
		t.Fatalf("info = %+v", info)
	}
	wantMem(t, fn, "MOV RAX, [RBX]", 0, false)
	wantPop(t, fn, "POP RBX", 0, true)
}

func TestStackPrivRealignedFrame(t *testing.T) {
	fn, info := spBuild(t,
		pushRBP, movRbpRsp,
		[]byte{0x48, 0x83, 0xe4, 0xe0}, // and rsp, -32
		subRsp20,
		[]byte{0x48, 0x89, 0x7d, 0xf8}, // mov [rbp-8], rdi     below the saved rbp, above the aligned rsp
		stSp8Rdi,                       // mov [rsp+8], rdi     offset unknown
		leave, ret)
	if info.Escaped || info.Poison != "" {
		t.Fatalf("info = %+v", info)
	}
	wantMem(t, fn, "MOV [RBP-0x8], RDI", 0, true)
	wantMem(t, fn, "MOV [RSP+0x8], RDI", 0, false)
	wantPop(t, fn, "LEAVE", 0, true)
}

// The red zone is private once the code has had a frame of its own (push rbp), not before.
func TestStackPrivRedZone(t *testing.T) {
	fn, info := spBuild(t,
		[]byte{0x48, 0x89, 0x7c, 0x24, 0xf8}, // mov [rsp-8], rdi     no frame yet: could be an enclosing leaf's
		pushRBP, movRbpRsp,
		[]byte{0x48, 0x89, 0x7d, 0xf8},                   // mov [rbp-8], rdi     red zone of a frame-pointer leaf
		[]byte{0x48, 0x8b, 0x45, 0xf8},                   // mov rax, [rbp-8]
		[]byte{0x48, 0x8b, 0x85, 0x70, 0xff, 0xff, 0xff}, // mov rax, [rbp-0x90]  beyond the red zone
		popRBP, ret)
	if info.Escaped || info.Poison != "" {
		t.Fatalf("info = %+v", info)
	}
	wantMem(t, fn, "MOV [RSP-0x8], RDI", 0, false)
	wantMem(t, fn, "MOV [RBP-0x8], RDI", 0, true)
	wantMem(t, fn, "MOV RAX, [RBP-0x8]", 0, true)
	wantMem(t, fn, "MOV RAX, [RBP+0xffffff70]", 0, false)
	wantPop(t, fn, "POP RBP", 0, true)
}

// A red-zone address that leaves the registers is an escape like any other.
func TestStackPrivRedZoneEscape(t *testing.T) {
	fn, info := spBuild(t,
		pushRBP, movRbpRsp,
		[]byte{0x48, 0x8d, 0x45, 0xf8}, // lea rax, [rbp-8]
		[]byte{0x48, 0x89, 0x07},       // mov [rdi], rax
		[]byte{0x48, 0x89, 0x7d, 0xf8}, // mov [rbp-8], rdi
		popRBP, ret)
	if !info.Escaped {
		t.Fatalf("info = %+v", info)
	}
	wantMem(t, fn, "MOV [RBP-0x8], RDI", 0, false)
}

// call next; pop: the stack depth after the pair is what it was before.
func TestStackPrivGetPC(t *testing.T) {
	fn, info := spBuild(t,
		pushRBX, subRsp20,
		[]byte{0xe8, 0, 0, 0, 0}, // call next
		popRAX,
		stSp8Rdi,
		[]byte{0x48, 0x8b, 0x44, 0x24, 0x20}, // mov rax, [rsp+0x20]  the saved rbx, in the frame
		[]byte{0x48, 0x8b, 0x44, 0x24, 0x28}, // mov rax, [rsp+0x28]  the return address
		addRsp20, popRBX, ret)
	if info.Escaped || info.Poison != "" {
		t.Fatalf("info = %+v", info)
	}
	wantMem(t, fn, "MOV [RSP+0x8], RDI", 0, true)
	wantMem(t, fn, "MOV RAX, [RSP+0x20]", 0, true)
	wantMem(t, fn, "MOV RAX, [RSP+0x28]", 0, false)
	wantPop(t, fn, "POP RBX", 0, true)
}

// pop [rsp+8] stores relative to the incremented stack pointer: never classified.
func TestStackPrivPopToStack(t *testing.T) {
	fn, _ := spBuild(t, subRsp20, pushRAX, []byte{0x8f, 0x44, 0x24, 0x08}, addRsp20, ret)
	wantMem(t, fn, "POP [RSP+0x8]", 0, false)
	wantPop(t, fn, "POP [RSP+0x8]", 0, true)
}

// Region rule: in an escaped function a slot below every escaping address keeps the region bit.
func TestStackPrivRegion(t *testing.T) {
	fn, info := spBuild(t,
		pushRBX, subRsp20, stSp8Rdi, // slot [-0x20, -0x18)
		[]byte{0x48, 0x8d, 0x7c, 0x24, 0x10}, // lea rdi, [rsp+0x10]   escapes -0x18
		nil,
		ldRaxSp8,
		[]byte{0x48, 0x8b, 0x44, 0x24, 0x18}, // mov rax, [rsp+0x18]   above the escaping address
		[]byte{0x48, 0x8b, 0x04, 0xf4},       // mov rax, [rsp+rsi*8]  indexed: no known extent
		addRsp20, popRBX, ret)
	if !info.Escaped || info.EscapeLow != -0x18 {
		t.Fatalf("info = %+v", info)
	}
	for _, c := range []struct {
		text   string
		region bool
	}{{"MOV [RSP+0x8], RDI", true}, {"MOV RAX, [RSP+0x8]", true}, {"MOV RAX, [RSP+0x18]", false}, {"MOV RAX, [RSP+8*RSI]", false}} {
		bits := spBits(t, fn, c.text, 0)
		if bits&StackPrivMem != 0 {
			t.Errorf("%s: strictly private in an escaped function", c.text)
		}
		if got := bits&StackPrivMemRegion != 0; got != c.region {
			t.Errorf("%s: region private = %t, want %t", c.text, got, c.region)
		}
	}
}

// An escaping address of unknown offset leaves no region.
func TestStackPrivRegionUnknownOffset(t *testing.T) {
	fn, info := spBuild(t,
		pushRBX, subRsp20, stSp8Rdi,
		[]byte{0x48, 0x8d, 0x7c, 0xf4, 0x10}, // lea rdi, [rsp+rsi*8+0x10]
		nil, addRsp20, popRBX, ret)
	if !info.Escaped {
		t.Fatalf("info = %+v", info)
	}
	if spBits(t, fn, "MOV [RSP+0x8], RDI", 0)&StackPrivMemRegion != 0 {
		t.Error("region private below an address of unknown offset")
	}
}

// A register that is a frame pointer on one path only is not one after the join.
func TestStackPrivJoin(t *testing.T) {
	fn, info := spBuild(t,
		subRsp20,
		[]byte{0x48, 0x8d, 0x44, 0x24, 0x08}, // lea rax, [rsp+8]
		[]byte{0x48, 0x85, 0xff},             // test rdi, rdi
		[]byte{0x74, 0x03},                   // je +3
		[]byte{0x48, 0x89, 0xf0},             // mov rax, rsi
		[]byte{0x48, 0x8b, 0x08},             // mov rcx, [rax]
		[]byte{0x48, 0x8b, 0x4c, 0x24, 0x10}, // mov rcx, [rsp+0x10]
		addRsp20, ret)
	if info.Escaped {
		t.Fatalf("info = %+v", info)
	}
	wantMem(t, fn, "MOV RCX, [RAX]", 0, false)
	wantMem(t, fn, "MOV RCX, [RSP+0x10]", 0, true)
}

// Different stack depths at a join: nothing rsp-relative is private afterwards.
func TestStackPrivDepthMismatch(t *testing.T) {
	fn, _ := spBuild(t,
		subRsp20,
		[]byte{0x48, 0x85, 0xff}, // test rdi, rdi
		[]byte{0x74, 0x01},       // je +1
		pushRAX,
		stSp8Rdi, ret)
	wantMem(t, fn, "MOV [RSP+0x8], RDI", 0, false)
}

// An indirect jump with a live frame and no recovered table: the function is not analysed.
func TestStackPrivWildJump(t *testing.T) {
	fn, info := spBuild(t,
		subRsp20, stSp8Rdi,
		[]byte{0xff, 0xe0}, // jmp rax
	)
	if info.Poison == "" {
		t.Fatalf("info = %+v", info)
	}
	wantMem(t, fn, "MOV [RSP+0x8], RDI", 0, false)
}

// An indirect tail call (frame released) is an exit.
func TestStackPrivIndirectTailCall(t *testing.T) {
	fn, info := spBuild(t,
		subRsp20, stSp8Rdi, addRsp20,
		[]byte{0xff, 0xe0}, // jmp rax
	)
	if info.Poison != "" || info.Escaped {
		t.Fatalf("info = %+v", info)
	}
	wantMem(t, fn, "MOV [RSP+0x8], RDI", 0, true)
}

// Implicit register writes: after CPUID, RBX is no frame pointer.
func TestStackPrivImplicitWrite(t *testing.T) {
	fn, _ := spBuild(t,
		pushRBX, subRsp20,
		[]byte{0x48, 0x8d, 0x5c, 0x24, 0x08}, // lea rbx, [rsp+8]
		[]byte{0x48, 0x8b, 0x03},             // mov rax, [rbx]    private
		[]byte{0x0f, 0xa2},                   // cpuid
		[]byte{0x48, 0x8b, 0x03},             // mov rax, [rbx]    not stack
		addRsp20, popRBX, ret)
	wantMem(t, fn, "MOV RAX, [RBX]", 0, true)
	wantMem(t, fn, "MOV RAX, [RBX]", 1, false)
}

func TestStackPrivGPRIndex(t *testing.T) {
	for r, want := range map[x86asm.Reg]int{
		x86asm.AL: rAX, x86asm.BL: rBX, x86asm.AH: rAX, x86asm.BH: rBX, x86asm.SPB: rSP, x86asm.DIB: rDI,
		x86asm.R8B: rR8, x86asm.R15B: rR15, x86asm.SP: rSP, x86asm.EBP: rBP, x86asm.R11L: rR11, x86asm.RSP: rSP, x86asm.R15: rR15,
	} {
		if i, _, ok := gprIndex(r); !ok || i != want {
			t.Errorf("gprIndex(%v) = %d, %t; want %d", r, i, ok, want)
		}
	}
	if _, _, ok := gprIndex(x86asm.X3); ok {
		t.Error("xmm3 is not a GPR")
	}
}

// spBuildOpts is spBuild with the pass options (the callee at 0x80 stands in for a PLT stub).
func spBuildOpts(t *testing.T, opts StackPrivOpts, frags ...[]byte) *Function {
	t.Helper()
	fn, _ := spBuild(t, frags...)
	for _, b := range fn.Blocks {
		for i := range b.Insts {
			b.Insts[i].StackPriv = 0
		}
	}
	AnalyzeStackPrivacyOpts(fn, opts, nil)
	return fn
}

// A frame address passed to a clean import (memcpy-like) is not an escape; to any other callee it is.
func TestStackPrivCleanImport(t *testing.T) {
	sub := []byte{0x48, 0x83, 0xec, 0x28}
	add := []byte{0x48, 0x83, 0xc4, 0x28}
	lea := []byte{0x48, 0x8d, 0x7c, 0x24, 0x08}      // lea rdi,[rsp+8]
	load := []byte{0x8b, 0x44, 0x24, 0x08}           // mov eax,[rsp+8]
	storeRAX := []byte{0x48, 0x89, 0x44, 0x24, 0x20} // mov [rsp+0x20],rax
	ret := []byte{0xc3}
	clean := StackPrivOpts{ImportClean: func(a uint64) bool { return a == 0x80 }}

	fn := spBuildOpts(t, StackPrivOpts{}, sub, lea, nil, load, add, ret)
	wantMem(t, fn, "MOV EAX, [RSP+0x8]", 0, false)
	fn = spBuildOpts(t, clean, sub, lea, nil, load, add, ret)
	wantMem(t, fn, "MOV EAX, [RSP+0x8]", 0, true)
	// A different callee is still an escape.
	fn = spBuildOpts(t, StackPrivOpts{ImportClean: func(a uint64) bool { return a == 0x1234 }}, sub, lea, nil, load, add, ret)
	wantMem(t, fn, "MOV EAX, [RSP+0x8]", 0, false)
	// The result of memcpy/strchr points into the argument: RAX stays tainted, storing it escapes.
	fn = spBuildOpts(t, clean, sub, lea, nil, storeRAX, load, add, ret)
	wantMem(t, fn, "MOV EAX, [RSP+0x8]", 0, false)
	// Without a frame address in an argument register the result is clean.
	fn = spBuildOpts(t, clean, sub, nil, storeRAX, load, add, ret)
	wantMem(t, fn, "MOV EAX, [RSP+0x8]", 0, true)
	// The GOT form: call [rip+disp] with the slot at 0x500 (disp relative to the next instruction).
	pc := 0x100 + len(sub) + len(lea)
	got := []byte{0xff, 0x15, 0, 0, 0, 0}
	binary.LittleEndian.PutUint32(got[2:], uint32(int32(0x500-(pc+6))))
	fn = spBuildOpts(t, StackPrivOpts{ImportClean: func(a uint64) bool { return a == 0x500 }}, sub, lea, got, load, add, ret)
	wantMem(t, fn, "MOV EAX, [RSP+0x8]", 0, true)
}

// The rules id (part of the chunk cache key) changes with the import table and the pass version.
func TestStackPrivRulesID(t *testing.T) {
	base := stackPrivRulesIDFor(stackPrivRulesVersion, cleanImportNames)
	if base != StackPrivRulesID() {
		t.Fatal("StackPrivRulesID is not the id of the live table")
	}
	if stackPrivRulesIDFor(stackPrivRulesVersion, append(append([]string(nil), cleanImportNames...), "strtok")) == base {
		t.Error("adding an import must change the id")
	}
	if stackPrivRulesIDFor(stackPrivRulesVersion, cleanImportNames[1:]) == base {
		t.Error("removing an import must change the id")
	}
	if stackPrivRulesIDFor(stackPrivRulesVersion+1, cleanImportNames) == base {
		t.Error("a new pass version must change the id")
	}
}
