// SPDX-License-Identifier: GPL-2.0-or-later

package elfloader

import (
	"debug/elf"
	"testing"
)

// The payload SDK links text and headers into one RWE segment; guest code must still become
// read-only on the host, without taking a writable section's page with it.
func TestCodeProtectionSegmentsForRWESegment(t *testing.T) {
	l := &LoadedELF{
		Segments: []*Segment{
			{Vaddr: 0x400000, Memsz: 0xa000, Flags: elf.PF_R | elf.PF_W | elf.PF_X},
			{Vaddr: 0x40c000, Memsz: 0x5000, Flags: elf.PF_R | elf.PF_W},
		},
		Sections: map[string]*elf.Section{
			".text": {SectionHeader: elf.SectionHeader{Addr: 0x400000, Size: 0x9a99, Flags: elf.SHF_ALLOC | elf.SHF_EXECINSTR}},
			".data": {SectionHeader: elf.SectionHeader{Addr: 0x40c000, Size: 0x100, Flags: elf.SHF_ALLOC | elf.SHF_WRITE}},
		},
	}
	segs := l.CodeProtectionSegments()
	if len(segs) != 3 {
		t.Fatalf("segments: %d", len(segs))
	}
	if s := segs[2]; s.Vaddr != 0x400000 || s.Memsz != 0xc000 || s.Flags&elf.PF_W != 0 || s.Flags&elf.PF_X == 0 {
		t.Errorf("synthetic code segment %+v: want [0x400000, +0xc000) R+X (text widened to whole pages)", s)
	}
	// a writable section in the text's last page: only whole pages inside the text are protected
	l.Sections[".got"] = &elf.Section{SectionHeader: elf.SectionHeader{Addr: 0x409f00, Size: 0x40, Flags: elf.SHF_ALLOC | elf.SHF_WRITE}}
	segs = l.CodeProtectionSegments()
	if s := segs[len(segs)-1]; s.Vaddr != 0x400000 || s.Memsz != 0x8000 {
		t.Errorf("text sharing a page with .got: %+v want [0x400000, +0x8000)", s)
	}
}
