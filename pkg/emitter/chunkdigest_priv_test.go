package emitter

import (
	"testing"

	"ps4-recomp/pkg/disasm"
	"ps4-recomp/pkg/elfloader"
	"ps4-recomp/pkg/lifter"
)

// The stack privacy bits are an input of the lowering in the stack modes, so they are in the chunk
// digest there (a changed analysis or import table cannot reuse a cached chunk) and not in off.
func TestChunkDigestCoversStackPriv(t *testing.T) {
	img := make([]byte, 0x100)
	copy(img[0x40:], []byte{0x8b, 0x44, 0x24, 0x08, 0xc3}) // mov eax,[rsp+8]; ret
	loaded := &elfloader.LoadedELF{
		MemoryImage:  elfloader.NewImage(0, img),
		ExecRanges:   []elfloader.AddrRange{{Start: 0, End: uint64(len(img))}},
		SymbolByAddr: map[uint64]elfloader.Symbol{},
		SymbolByName: map[string]elfloader.Symbol{},
	}
	d, err := disasm.NewDisassembler(loaded)
	if err != nil {
		t.Fatal(err)
	}
	fn, _, err := d.DisasmFunction(0x40)
	if err != nil || fn == nil {
		t.Fatalf("disasm: %v", err)
	}
	digest := func(mode lifter.TSOElideMode) string {
		l := lifter.NewArm64Lifter(map[uint64]bool{0x40: true})
		l.SetTSOElide(mode)
		e := NewAsmEmitter(loaded, d, l)
		g, _ := e.chunkDigests([]uint64{0x40}, []*disasm.Function{fn})
		return g
	}
	flip := func() {
		for _, b := range fn.Blocks {
			for i := range b.Insts {
				b.Insts[i].StackPriv ^= disasm.StackPrivMem
			}
		}
	}
	offA, stA := digest(lifter.TSOElideOff), digest(lifter.TSOElideStack)
	flip()
	offB, stB := digest(lifter.TSOElideOff), digest(lifter.TSOElideStack)
	if offA != offB {
		t.Error("off digest must not depend on the privacy bits")
	}
	if stA == stB {
		t.Error("stack digest must depend on the privacy bits")
	}
}
