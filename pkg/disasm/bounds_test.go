package disasm

import (
	"debug/elf"
	"encoding/binary"
	"testing"

	"ps4-recomp/pkg/elfloader"

	"golang.org/x/arch/x86/x86asm"
)

func imageELF(img []byte, bounds []elfloader.AddrRange) *elfloader.LoadedELF {
	return &elfloader.LoadedELF{
		MemoryImage:  elfloader.NewImage(0, img),
		ExecRanges:   []elfloader.AddrRange{{Start: 0, End: uint64(len(img))}},
		FuncBounds:   bounds,
		SymbolByAddr: map[uint64]elfloader.Symbol{},
		SymbolByName: map[string]elfloader.Symbol{},
	}
}

func TestUnwindCeilingStopsBeforeFollowingCode(t *testing.T) {
	img := make([]byte, 0x100)
	img[0x40] = 0x90 // nop
	// Looks like a real instruction, but it belongs to the next extent.
	img[0x41] = 0x48
	img[0x42] = 0x89
	img[0x43] = 0xc0 // mov rax, rax
	loaded := imageELF(img, []elfloader.AddrRange{{Start: 0x40, End: 0x41}})
	d, err := NewDisassembler(loaded)
	if err != nil {
		t.Fatal(err)
	}
	fn, _, err := d.DisasmFunction(0x40)
	if err != nil {
		t.Fatal(err)
	}
	n := 0
	for _, b := range fn.Blocks {
		n += len(b.Insts)
		for _, inst := range b.Insts {
			if inst.Address != 0x40 {
				t.Fatalf("decoded 0x%x %s past unwind end", inst.Address, inst.Inst)
			}
		}
	}
	if n != 1 {
		t.Fatalf("instructions = %d", n)
	}
}

func TestPrivilegedOpcodeEndsBlock(t *testing.T) {
	img := make([]byte, 0x100)
	img[0x40] = 0x90 // nop
	img[0x41] = 0x6e // outsb
	img[0x42] = 0x6e
	loaded := imageELF(img, nil)
	d, err := NewDisassembler(loaded)
	if err != nil {
		t.Fatal(err)
	}
	fn, _, err := d.DisasmFunction(0x40)
	if err != nil {
		t.Fatal(err)
	}
	for _, b := range fn.Blocks {
		for _, inst := range b.Insts {
			if inst.Inst.Op == x86asm.OUTSB || inst.Inst.Op == x86asm.OUTSD || inst.Inst.Op == x86asm.INSD {
				t.Fatalf("privileged opcode kept at 0x%x", inst.Address)
			}
		}
	}
	if d.PrivilegedStops != 1 {
		t.Fatalf("privileged stops = %d", d.PrivilegedStops)
	}
}

func TestSeedIgnoresNonPointerAddend(t *testing.T) {
	img := make([]byte, 0x100)
	img[0x40] = 0x55 // push rbp
	binary.LittleEndian.PutUint64(img[0x80:], 0x40)
	loaded := imageELF(img, []elfloader.AddrRange{{Start: 0x40, End: 0x41}})
	loaded.Relocations = []elfloader.Relocation{
		{Type: 2, Offset: 0x10, Addend: 0x40}, // R_X86_64_PC32, not a pointer
		{Type: elfloader.R_X86_64_RELATIVE, Offset: 0x80, Addend: 0x40},
	}
	entries := SeedEntryPoints(loaded, false)
	if len(entries) != 1 || entries[0] != 0x40 {
		t.Fatalf("seeds = %v", entries)
	}

	// Same pointer outside every FDE extent: unwind coverage elsewhere must not veto it (the
	// 0x40 target is code the FDEs do not cover). The FDE start is a seed too.
	loaded.FuncBounds = []elfloader.AddrRange{{Start: 0x50, End: 0x60}}
	entries = SeedEntryPoints(loaded, false)
	if len(entries) != 2 || entries[0] != 0x40 || entries[1] != 0x50 {
		t.Fatalf("seeds = %v, want the gap target 0x40 and the FDE start 0x50", entries)
	}

	// A pointer strictly inside an FDE extent is a label, not an entry; one at its start is.
	binary.LittleEndian.PutUint64(img[0x80:], 0x54)
	entries = SeedEntryPoints(loaded, false)
	if len(entries) != 1 || entries[0] != 0x50 {
		t.Fatalf("seeds = %v, want only the FDE start 0x50", entries)
	}
}

// A relocation target that is 16-aligned behind int3 padding is a function whatever its first
// instruction is (virtual methods often start with cmp/test/xor/movzx or a VEX load); an
// unaligned target, or one that does not decode, still needs the prologue filter.
func TestSeedAlignedEntryBehindPaddingAnyFirstInstruction(t *testing.T) {
	img := make([]byte, 0x200)
	for i := range img {
		img[i] = 0xcc
	}
	copy(img[0x40:], []byte{0x80, 0x7f, 0x48, 0x00, 0x74, 0x01, 0xc3, 0xc3}) // cmp byte [rdi+0x48],0 ; je ; ret ; ret
	copy(img[0x60:], []byte{0x85, 0xff, 0x0f, 0x94, 0xc0, 0xc3})             // test edi,edi ; sete al ; ret
	copy(img[0x80:], []byte{0xc5, 0xf8, 0x10, 0x06, 0xc3})                   // vmovups xmm0,[rsi] ; ret
	copy(img[0xa0:], []byte{0x31, 0xc0, 0xc3})                               // xor eax,eax ; ret
	copy(img[0xc3:], []byte{0x31, 0xc0, 0xc3})                               // same, unaligned
	copy(img[0xe0:], []byte{0x06, 0x06, 0x06})                               // invalid in 64-bit mode
	copy(img[0x100:], []byte{0x55, 0xc3})                                    // FDE-covered function
	targets := []uint64{0x40, 0x60, 0x80, 0xa0, 0xc3, 0xe0, 0x120 /* padding */}
	loaded := imageELF(img, []elfloader.AddrRange{{Start: 0x100, End: 0x102}})
	for i, a := range targets {
		binary.LittleEndian.PutUint64(img[0x180+i*8:], a) // the applied slot is the pointer
		loaded.Relocations = append(loaded.Relocations,
			elfloader.Relocation{Type: elfloader.R_X86_64_RELATIVE, Offset: 0x180 + uint64(i)*8, Addend: int64(a)})
	}
	got := map[uint64]bool{}
	for _, e := range SeedEntryPoints(loaded, false) {
		got[e] = true
	}
	for _, a := range []uint64{0x40, 0x60, 0x80, 0xa0, 0x100} {
		if !got[a] {
			t.Errorf("entry 0x%x was not seeded", a)
		}
	}
	for _, a := range []uint64{0xc3, 0xe0, 0x120} {
		if got[a] {
			t.Errorf("0x%x was seeded", a)
		}
	}
}

func TestNoReturnTerminatesBlock(t *testing.T) {
	img := make([]byte, 0x200)
	// Entry at 0x10: call abort (at 0x80)
	// Call instruction: E8 <rel32>
	// nextPC = 0x15, rel32 = 0x80 - 0x15 = 0x6b
	img[0x10] = 0xe8
	img[0x11] = 0x6b
	img[0x12] = 0x00
	img[0x13] = 0x00
	img[0x14] = 0x00
	// Bytes following the call: garbage/padding that would otherwise be decoded
	img[0x15] = 0x90 // nop
	img[0x16] = 0x90 // nop

	// Target 0x80: abort function
	img[0x80] = 0x0f // ud2
	img[0x81] = 0x0b

	loaded := imageELF(img, []elfloader.AddrRange{{Start: 0x10, End: 0x50}})
	loaded.Symbols = []elfloader.Symbol{
		{Name: "abort", Address: 0x80, Size: 2, Type: 2},
	}
	d, err := NewDisassembler(loaded)
	if err != nil {
		t.Fatal(err)
	}
	fn, _, err := d.DisasmFunction(0x10)
	if err != nil {
		t.Fatal(err)
	}
	// The function starting at 0x10 should only contain the CALL instruction (len 5, ending at 0x15).
	// It must NOT decode the trailing NOPs at 0x15.
	totalInsts := 0
	for _, b := range fn.Blocks {
		for _, inst := range b.Insts {
			totalInsts++
			if inst.Address >= 0x15 {
				t.Fatalf("decoded instruction 0x%x past noreturn call", inst.Address)
			}
		}
	}
	if totalInsts != 1 {
		t.Fatalf("expected 1 instruction, got %d", totalInsts)
	}
}

func TestBlockSplitsOnMaxBytes(t *testing.T) {
	const codeSize = 9000
	img := make([]byte, codeSize+0x100)
	for i := 0; i < codeSize-1; i++ {
		img[0x10+i] = 0x90 // nop
	}
	img[0x10+codeSize-1] = 0xc3 // ret

	loaded := imageELF(img, []elfloader.AddrRange{{Start: 0x10, End: uint64(0x10 + codeSize)}})
	d, err := NewDisassembler(loaded)
	if err != nil {
		t.Fatal(err)
	}
	fn, _, err := d.DisasmFunction(0x10)
	if err != nil {
		t.Fatal(err)
	}
	totalInsts := 0
	for _, b := range fn.Blocks {
		totalInsts += len(b.Insts)
	}
	if totalInsts != codeSize {
		t.Fatalf("expected %d instructions across split blocks, got %d", codeSize, totalInsts)
	}
	if len(fn.Blocks) <= 1 {
		t.Fatalf("expected function to split into multiple blocks (> 8192 bytes), got %d blocks", len(fn.Blocks))
	}
	if d.CapHits != 0 {
		t.Fatalf("block splitting should not record a cap hit, got %d cap hits", d.CapHits)
	}
}

// TestTLSSymbolSizeDoesNotBoundFunction: a merged companion module keeps its STT_TLS
// symbols unrelocated because their values are offsets in its TLS block, not addresses.
// libc.prx has one at 0x1d0 (size 8); taking that size as a function bound cut the eboot
// function at 0x1d0 after two instructions, and the lifter then dispatched to the
// unregistered fall-through 0x1db.
func TestTLSSymbolSizeDoesNotBoundFunction(t *testing.T) {
	img := make([]byte, 0x200)
	// 0x100: lea rax, [rip+0xf0]; 0x107: add rax, 0x10; the add ends at 0x10b.
	copy(img[0x100:], []byte{0x48, 0x8d, 0x05, 0xf0, 0x00, 0x00, 0x00})
	copy(img[0x107:], []byte{0x48, 0x83, 0xc0, 0x10})
	for i := 0x10b; i < 0x11f; i++ {
		img[i] = 0x90 // nop
	}
	img[0x11f] = 0xc3 // ret
	loaded := imageELF(img, []elfloader.AddrRange{{Start: 0x100, End: 0x120}})
	sym := elfloader.Symbol{Name: "J-oDqE1N8R4", Address: 0x100, Size: 8, Type: elf.STT_TLS}
	loaded.SymbolByAddr[0x100] = sym
	loaded.SymbolByName[sym.Name] = sym

	d, err := NewDisassembler(loaded)
	if err != nil {
		t.Fatal(err)
	}
	fn, _, err := d.DisasmFunction(0x100)
	if err != nil {
		t.Fatal(err)
	}
	lastEnd := uint64(0)
	continued := false
	for _, b := range fn.Blocks {
		for _, inst := range b.Insts {
			if inst.Address == 0x10b {
				continued = true
			}
			if end := inst.Address + uint64(inst.Inst.Len); end > lastEnd {
				lastEnd = end
			}
		}
	}
	if !continued {
		t.Fatalf("decode stopped at the TLS symbol's fake end 0x108; blocks=%v", fn.Blocks)
	}
	if lastEnd != 0x120 {
		t.Fatalf("function ends at 0x%x, want the ret's end 0x120", lastEnd)
	}
}

// TestFuncSymbolSizeStillBoundsDecode: a real STT_FUNC symbol size remains the extent of
// the function (the unwind ceiling does not override it).
func TestFuncSymbolSizeStillBoundsDecode(t *testing.T) {
	img := make([]byte, 0x100)
	for i := 0x40; i < 0x50; i++ {
		img[i] = 0x90 // nop
	}
	loaded := imageELF(img, []elfloader.AddrRange{{Start: 0x40, End: 0x50}})
	loaded.SymbolByAddr[0x40] = elfloader.Symbol{Name: "tiny", Address: 0x40, Size: 4, Type: elf.STT_FUNC}

	d, err := NewDisassembler(loaded)
	if err != nil {
		t.Fatal(err)
	}
	fn, _, err := d.DisasmFunction(0x40)
	if err != nil {
		t.Fatal(err)
	}
	total := 0
	lastEnd := uint64(0)
	for _, b := range fn.Blocks {
		total += len(b.Insts)
		for _, inst := range b.Insts {
			if end := inst.Address + uint64(inst.Inst.Len); end > lastEnd {
				lastEnd = end
			}
		}
	}
	if total != 4 || lastEnd != 0x44 {
		t.Fatalf("STT_FUNC size 4: %d instructions ending at 0x%x, want 4 ending at 0x44", total, lastEnd)
	}
}

func TestSeedFromFDEsAndFiniArray(t *testing.T) {
	img := make([]byte, 0x100)
	img[0x40] = 0x55
	img[0x60] = 0x55
	img[0x70] = 0x55
	binary.LittleEndian.PutUint64(img[0x90:], 0x70) // DT_FINI_ARRAY slot
	loaded := imageELF(img, []elfloader.AddrRange{{Start: 0x40, End: 0x50}, {Start: 0x60, End: 0x68}, {Start: 0x70, End: 0x78}})
	loaded.DynamicTags = map[int64]uint64{26: 0x90, 28: 8}
	got := map[uint64]bool{}
	for _, a := range SeedEntryPoints(loaded, false) {
		got[a] = true
	}
	for _, want := range []uint64{0x40, 0x60, 0x70} {
		if !got[want] {
			t.Errorf("seed 0x%x missing: %v", want, got)
		}
	}
}

// A direct branch from one function onto another known entry in code without unwind extents is
// a tail call: the target body is decoded once (as its own function), not again inside the
// branching function.
func TestTailBranchToKnownEntryNotDuplicated(t *testing.T) {
	img := make([]byte, 0x100)
	img[0x40], img[0x41] = 0xEB, 0x0E // jmp 0x50
	img[0x50], img[0x51], img[0x52] = 0x90, 0x90, 0xC3
	loaded := imageELF(img, nil)
	d, err := NewDisassembler(loaded)
	if err != nil {
		t.Fatal(err)
	}
	insts := 0
	funcs := 0
	err = d.WalkReachable([]uint64{0x40, 0x50}, func(fn *Function) error {
		funcs++
		for _, b := range fn.Blocks {
			insts += len(b.Insts)
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if funcs != 2 || insts != 4 {
		t.Fatalf("funcs=%d insts=%d, want 2 and 4 (jmp, nop, nop, ret)", funcs, insts)
	}
}
