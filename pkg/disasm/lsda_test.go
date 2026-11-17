package disasm

import (
	"debug/elf"
	"encoding/binary"
	"testing"

	"ps4-recomp/pkg/elfloader"
)

// synthetic image: code at 0x100, .eh_frame at 0x200 (CIE "zPLR" + one FDE), LSDA at 0x300.
func lsdaImage() *elfloader.LoadedELF {
	img := make([]byte, 0x1000)
	// function at 0x100: push rbp; mov rbp,rsp; call; pad: ...; ret
	copy(img[0x100:], []byte{0x55, 0x48, 0x89, 0xe5, 0xe8, 0x00, 0x00, 0x00, 0x00, 0x5d, 0xc3, 0x50, 0x58, 0xc3})
	p := 0x200
	put32 := func(off int, v uint32) { binary.LittleEndian.PutUint32(img[off:], v) }
	// CIE
	cie := []byte{0, 0, 0, 0, // id
		1,                     // version
		'z', 'P', 'L', 'R', 0, // augmentation
		1, 0x78, 16, // code align, data align (-8), RA reg
		7,                // aug length: P(1+4 pcrel sdata4... use absptr-less omit) -> P enc + 4B, L enc, R enc
		0x1b, 0, 0, 0, 0, // personality pcrel|sdata4 + value
		0x1b, // LSDA encoding
		0x1b, // FDE encoding
	}
	// aug length counts bytes after it: P(1+4) + L(1) + R(1) = 7
	put32(p, uint32(len(cie)))
	copy(img[p+4:], cie)
	cieEnd := p + 4 + len(cie)
	// FDE
	fdeStart := cieEnd
	put32(fdeStart+4, uint32(fdeStart+4-p)) // CIE pointer (back offset)
	q := fdeStart + 8
	put32(q, uint32(int32(0x100-q))) // pc_begin pcrel sdata4
	put32(q+4, 14)                   // pc range
	img[q+8] = 4                     // aug length
	put32(q+9, uint32(int32(0x300-(q+9))))
	fdeLen := (q + 13) - (fdeStart + 4)
	put32(fdeStart, uint32(fdeLen))
	put32(q+13, 0) // terminator
	// LSDA at 0x300: lpstart omit, ttype omit, call-site uleb, len, then one record
	copy(img[0x300:], []byte{0xff, 0xff, 0x01, 0x07,
		0x04, 0x05, 0x0b, 0x01, // start=4 len=5 landing pad=0xb action=1
		0, 0, 0})
	return &elfloader.LoadedELF{
		MemoryImage: elfloader.NewImage(0, img), EHFrameAddr: 0x200, EHFrameSize: uint64(q + 17 - 0x200),
		ExecRanges: []elfloader.AddrRange{{Start: 0x100, End: 0x200}},
		Segments:   []*elfloader.Segment{{Vaddr: 0, Memsz: 0x1000, Filesz: 0x1000, Flags: elf.PF_R | elf.PF_W}},
	}
}

func TestParseLSDALandingPads(t *testing.T) {
	l := lsdaImage()
	eh := ParseLSDA(l)
	if eh.Malformed != 0 || eh.LSDAs != 1 {
		t.Fatalf("lsdas=%d malformed=%d", eh.LSDAs, eh.Malformed)
	}
	if len(eh.LandingPads) != 1 || eh.LandingPads[0] != 0x10b {
		t.Fatalf("landing pads = %#x", eh.LandingPads)
	}
	if len(eh.ReturnSites) != 1 || eh.ReturnSites[0] != 0x109 {
		t.Fatalf("return sites = %#x", eh.ReturnSites)
	}
}

func TestSeedLandingPadOption(t *testing.T) {
	l := lsdaImage()
	got, _ := SeedEntryPointsOpts(l, SeedOptions{LandingPads: true})
	if len(got) != 1 || got[0] != 0x10b {
		t.Fatalf("seeds = %#x", got)
	}
	got, _ = SeedEntryPointsOpts(l, SeedOptions{ReturnSites: true})
	if len(got) != 1 || got[0] != 0x109 {
		t.Fatalf("seeds = %#x", got)
	}
}

func TestWritableDataScanValidatesDecode(t *testing.T) {
	l := lsdaImage()
	l.EHFrameSize = 0
	// two words in a writable segment: a 16-aligned prologue (0x100) and a mid-instruction (0x101).
	l.MemoryImage.PutU64(0x400, 0x100)
	l.MemoryImage.PutU64(0x408, 0x101)
	l.MemoryImage.PutU64(0x410, 0x0)
	got, rep := SeedEntryPointsOpts(l, SeedOptions{WritableData: true})
	if len(got) != 1 || got[0] != 0x100 {
		t.Fatalf("seeds = %#x (rejected %v)", got, rep.Rejected)
	}
	if rep.Rejected["data_scan"] != 1 {
		t.Fatalf("rejected = %v", rep.Rejected)
	}
}

// TestParseLSDAPlacedImage: the same tables in a module placed at the identity-mode executable
// base. The FDE's pc-relative function start must resolve to guest addresses there, not to
// offsets in the image.
func TestParseLSDAPlacedImage(t *testing.T) {
	const base = 0x7000400000
	l := lsdaImage()
	if err := l.ApplyBias(base); err != nil {
		t.Fatal(err)
	}
	if l.EHFrameAddr != base+0x200 {
		t.Fatalf("EHFrameAddr = %#x", l.EHFrameAddr)
	}
	eh := ParseLSDA(l)
	if eh.Malformed != 0 || eh.LSDAs != 1 {
		t.Fatalf("lsdas=%d malformed=%d", eh.LSDAs, eh.Malformed)
	}
	if len(eh.LandingPads) != 1 || eh.LandingPads[0] != base+0x10b {
		t.Fatalf("landing pads = %#x", eh.LandingPads)
	}
	if len(eh.ReturnSites) != 1 || eh.ReturnSites[0] != base+0x109 {
		t.Fatalf("return sites = %#x", eh.ReturnSites)
	}
}
