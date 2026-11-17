package disasm

import (
	"encoding/hex"
	"testing"

	"golang.org/x/arch/x86/x86asm"

	"ps4-recomp/pkg/elfloader"
)

// block decodes the hex instructions (one string each) into a one-block function at 0x1000.
func roBlock(t *testing.T, hexes ...string) (*Function, *BasicBlock) {
	t.Helper()
	b := &BasicBlock{StartAddr: 0x1000}
	pc := uint64(0x1000)
	for _, h := range hexes {
		raw, err := hex.DecodeString(h)
		if err != nil {
			t.Fatal(err)
		}
		inst, err := x86asm.Decode(raw, 64)
		if err != nil {
			t.Fatalf("%s: %v", h, err)
		}
		b.Insts = append(b.Insts, Instruction{Address: pc, Inst: inst})
		pc += uint64(inst.Len)
	}
	return &Function{EntryAddr: 0x1000, Blocks: map[uint64]*BasicBlock{0x1000: b}}, b
}

func TestAnalyzeROLoads(t *testing.T) {
	const canary = 0x9000
	// slot 0x5000 holds the canary address; 0x5008 holds another pointer; both in RELRO.
	ri := &ROInfo{
		Ranges: []elfloader.AddrRange{{Start: 0x5000, End: 0x6000}},
		Canary: canary,
		U64: func(a uint64) (uint64, bool) {
			switch a {
			case 0x5000:
				return canary, true
			case 0x5008:
				return 0x7777, true
			}
			return 0, false
		},
	}
	// Instruction lengths: mov rax,[rip+d32] is 7 bytes; the displacement is relative to the next pc.
	rip := func(op string, next, target uint64) string {
		d := int32(int64(target) - int64(next))
		return op + hex.EncodeToString([]byte{byte(d), byte(d >> 8), byte(d >> 16), byte(d >> 24)})
	}
	cases := []struct {
		name string
		hex  []string
		want []bool
	}{
		{"guard pointer then value", []string{rip("488b05", 0x1007, 0x5000), "488b00", "483b4508"}, []bool{true, true, false}},
		{"other pointer: value not guard", []string{rip("488b05", 0x1007, 0x5008), "488b00"}, []bool{true, false}},
		{"guard pointer overwritten", []string{rip("488b05", 0x1007, 0x5000), "4889f8", "488b00"}, []bool{true, false, false}},
		{"guard pointer overwritten by 32-bit write", []string{rip("488b05", 0x1007, 0x5000), "89f8", "488b00"}, []bool{true, false, false}},
		{"unknown instruction clears", []string{rip("488b05", 0x1007, 0x5000), "f7e1", "488b00"}, []bool{true, false, false}},
		{"load past the guard value", []string{rip("488b05", 0x1007, 0x5000), "488b4008"}, []bool{true, false}},
		{"store to a RELRO slot is not a load", []string{rip("488905", 0x1007, 0x5010)}, []bool{false}},
		{"lea is no access", []string{rip("488d05", 0x1007, 0x5010)}, []bool{false}},
		{"outside RELRO", []string{rip("488b05", 0x1007, 0x6000)}, []bool{false}},
		{"straddles the end", []string{rip("488b05", 0x1007, 0x5ffc)}, []bool{false}},
	}
	for _, c := range cases {
		fn, b := roBlock(t, c.hex...)
		AnalyzeROLoads(fn, ri)
		for i, in := range b.Insts {
			if in.ROLoad != c.want[i] {
				t.Errorf("%s: inst %d (%v): ROLoad = %t, want %t", c.name, i, in.Inst, in.ROLoad, c.want[i])
			}
		}
	}
}

// A guard pointer kept in a callee-saved register is still one in the blocks after it (epilogues at
// several exits), through calls, and not in a block another path reaches without it.
func TestAnalyzeROLoadsAcrossBlocks(t *testing.T) {
	const canary = 0x9000
	ri := &ROInfo{
		Ranges: []elfloader.AddrRange{{Start: 0x5000, End: 0x6000}},
		Canary: canary,
		U64: func(a uint64) (uint64, bool) {
			if a == 0x5000 {
				return canary, true
			}
			return 0, false
		},
	}
	mk := func(pc uint64, hexes ...string) *BasicBlock {
		b := &BasicBlock{StartAddr: pc}
		for _, h := range hexes {
			raw, _ := hex.DecodeString(h)
			inst, err := x86asm.Decode(raw, 64)
			if err != nil {
				t.Fatal(err)
			}
			b.Insts = append(b.Insts, Instruction{Address: pc, Inst: inst})
			pc += uint64(inst.Len)
		}
		b.EndAddr = pc
		return b
	}
	d32 := func(next, target uint64) string {
		d := int32(int64(target) - int64(next))
		return hex.EncodeToString([]byte{byte(d), byte(d >> 8), byte(d >> 16), byte(d >> 24)})
	}
	// A: mov r12,[rip+0x5000]; jne B   C: call; mov rax,[r12]; ret   B: mov rax,[r12]; ret
	A := mk(0x1000, "4c8b25"+d32(0x1007, 0x5000), "750a")
	C := mk(0x1009, "e800000000", "498b0424", "c3")
	B := mk(0x1013, "498b0424", "c3")
	fn := &Function{EntryAddr: 0x1000, Blocks: map[uint64]*BasicBlock{0x1000: A, 0x1009: C, 0x1013: B}, BlockOrder: []uint64{0x1000, 0x1009, 0x1013}}
	AnalyzeROLoads(fn, ri)
	if !A.Insts[0].ROLoad || !C.Insts[1].ROLoad || !B.Insts[0].ROLoad {
		t.Errorf("guard pointer in r12 across blocks and a call: A=%t C=%t B=%t", A.Insts[0].ROLoad, C.Insts[1].ROLoad, B.Insts[0].ROLoad)
	}
	// X is reached from P without the pointer and from A with it: not a guard load there.
	P := mk(0x1000, "85c0", "7505") // test eax,eax; jne A (0x1009)
	X := mk(0x1004, "498b0424", "c3")
	A2 := mk(0x1009, "4c8b25"+d32(0x1010, 0x5000), "e9"+d32(0x1015, 0x1004))
	fn2 := &Function{EntryAddr: 0x1000, Blocks: map[uint64]*BasicBlock{0x1000: P, 0x1004: X, 0x1009: A2}, BlockOrder: []uint64{0x1000, 0x1004, 0x1009}}
	AnalyzeROLoads(fn2, ri)
	if X.Insts[0].ROLoad || !A2.Insts[0].ROLoad {
		t.Errorf("merge without the pointer: X=%t (want false) A2 pointer load=%t (want true)", X.Insts[0].ROLoad, A2.Insts[0].ROLoad)
	}
}
