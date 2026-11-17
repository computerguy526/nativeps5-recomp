package disasm

import "testing"

// A weak seed that is 16-aligned and decodes cleanly but sits inside an instruction of the
// function before it must not cut that function.
func TestMidInstructionSeedDropped(t *testing.T) {
	img := make([]byte, 0x100)
	for i := 0x10; i < 0x1f; i++ {
		img[i] = 0x90 // nop
	}
	copy(img[0x1f:], []byte{0xb8, 0x90, 0x90, 0x90, 0xc3}) // mov eax, 0xc3909090 (spans 0x20)
	img[0x24] = 0xc3                                       // ret
	for i := 0x30; i < 0x3f; i++ {
		img[i] = 0x90
	}
	img[0x3f] = 0xc3 // second, legitimate function at 0x30
	loaded := imageELF(img, nil)
	d, err := NewDisassembler(loaded)
	if err != nil {
		t.Fatal(err)
	}
	got := map[uint64]*Function{}
	err = d.WalkReachable([]uint64{0x10, 0x20, 0x30}, func(fn *Function) error {
		got[fn.EntryAddr] = fn
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := got[0x20]; ok {
		t.Fatalf("mid-instruction seed 0x20 became a function")
	}
	if got[0x30] == nil {
		t.Fatalf("legitimate entry 0x30 was dropped")
	}
	f := got[0x10]
	if f == nil {
		t.Fatal("no function at 0x10")
	}
	seen := map[uint64]bool{}
	for _, b := range f.Blocks {
		for _, in := range b.Insts {
			seen[in.Address] = true
		}
	}
	if !seen[0x1f] || !seen[0x24] {
		t.Fatalf("function 0x10 was cut: insts at 0x1f/0x24 = %v/%v", seen[0x1f], seen[0x24])
	}
}

// Same, but an early ret stops the linear pre-pass: the block-following decoder must notice
// that an instruction runs over the next entry.
func TestMidInstructionSeedDroppedAfterRet(t *testing.T) {
	img := make([]byte, 0x100)
	copy(img[0x10:], []byte{0x74, 0x01, 0xc3}) // je +1 ; ret
	for i := 0x13; i < 0x1f; i++ {
		img[i] = 0x90
	}
	copy(img[0x1f:], []byte{0xb8, 0x90, 0x90, 0x90, 0xc3}) // mov eax, imm32 spanning 0x20
	img[0x24] = 0xc3
	d, err := NewDisassembler(imageELF(img, nil))
	if err != nil {
		t.Fatal(err)
	}
	got := map[uint64]*Function{}
	if err := d.WalkReachable([]uint64{0x10, 0x20}, func(fn *Function) error {
		got[fn.EntryAddr] = fn
		return nil
	}); err != nil {
		t.Fatal(err)
	}
	if got[0x20] != nil {
		t.Fatal("mid-instruction seed 0x20 became a function")
	}
	f := got[0x10]
	if f == nil {
		t.Fatal("no function at 0x10")
	}
	ok := false
	for _, b := range f.Blocks {
		for _, in := range b.Insts {
			if in.Address == 0x24 {
				ok = true
			}
		}
	}
	if !ok {
		t.Fatal("function 0x10 was cut at the mid-instruction entry")
	}
}
