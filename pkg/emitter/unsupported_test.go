package emitter

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"ps4-recomp/pkg/disasm"
	"ps4-recomp/pkg/elfloader"
	"ps4-recomp/pkg/lifter"
)

// fcos has no lowering: the translation must fail with its address and form unless
// unsupported instructions are allowed.
func TestUnsupportedPolicy(t *testing.T) {
	img := make([]byte, 0x100)
	copy(img[0x40:], []byte{0xd9, 0xff, 0xc3}) // fcos; ret
	for _, allow := range []bool{false, true} {
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
		l := lifter.NewArm64Lifter(map[uint64]bool{0x40: true})
		l.SetAllowUnsupported(allow)
		em := NewAsmEmitter(loaded, d, l)
		dir := t.TempDir()
		if err := em.emitSingleAsmChunk(filepath.Join(dir, "code_000.s"), 0, []uint64{0x40}); err != nil {
			t.Fatal(err)
		}
		err = em.checkUnsupported(dir)
		if allow {
			if err != nil {
				t.Fatalf("allow: %v", err)
			}
		} else {
			if err == nil || !strings.Contains(err.Error(), "0x40") || !strings.Contains(err.Error(), "FCOS") {
				t.Fatalf("want failure naming 0x40 and FCOS, got %v", err)
			}
		}
		if b, _ := os.ReadFile(filepath.Join(dir, "unsupported.txt")); !strings.Contains(string(b), "fn 0x40") {
			t.Errorf("allow=%v: unsupported.txt = %q", allow, b)
		}
	}
}
