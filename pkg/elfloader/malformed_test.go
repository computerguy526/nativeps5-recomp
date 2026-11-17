package elfloader

import (
	"encoding/binary"
	"errors"
	"os"
	"strings"
	"testing"
)

type tphdr struct {
	typ, flags           uint32
	off, vaddr, fsz, msz uint64
}

// buildELF assembles a minimal x86-64 ET_DYN image: ELF header, program
// headers, then body at file offset bodyOff. Tests poke fields to malform it.
func buildELF(phdrs []tphdr, body []byte, bodyOff int) []byte {
	out := make([]byte, bodyOff+len(body))
	copy(out, []byte{0x7f, 'E', 'L', 'F', 2, 1, 1})
	le := binary.LittleEndian
	le.PutUint16(out[16:], 3)  // ET_DYN
	le.PutUint16(out[18:], 62) // EM_X86_64
	le.PutUint32(out[20:], 1)  // version
	le.PutUint64(out[32:], 64) // phoff
	le.PutUint16(out[52:], 64) // ehsize
	le.PutUint16(out[54:], 56) // phentsize
	le.PutUint16(out[56:], uint16(len(phdrs)))
	for i, p := range phdrs {
		o := 64 + i*56
		le.PutUint32(out[o:], p.typ)
		le.PutUint32(out[o+4:], p.flags)
		le.PutUint64(out[o+8:], p.off)
		le.PutUint64(out[o+16:], p.vaddr)
		le.PutUint64(out[o+24:], p.vaddr)
		le.PutUint64(out[o+32:], p.fsz)
		le.PutUint64(out[o+40:], p.msz)
		le.PutUint64(out[o+48:], 0x1000)
	}
	copy(out[bodyOff:], body)
	return out
}

func dynEntries(kv ...uint64) []byte {
	b := make([]byte, 0, len(kv)*8+16)
	for _, v := range kv {
		b = binary.LittleEndian.AppendUint64(b, v)
	}
	return append(b, make([]byte, 16)...)
}

func rela(off uint64, typ uint32, sym uint32, addend int64) []byte {
	b := make([]byte, 24)
	binary.LittleEndian.PutUint64(b[0:], off)
	binary.LittleEndian.PutUint64(b[8:], uint64(sym)<<32|uint64(typ))
	binary.LittleEndian.PutUint64(b[16:], uint64(addend))
	return b
}

func mustLoad(t *testing.T, data []byte) *LoadedELF {
	t.Helper()
	l, err := LoadELFBytes(data)
	if err != nil {
		t.Fatalf("load: %v", err)
	}
	return l
}

func TestLoadMalformedRejected(t *testing.T) {
	good := buildELF([]tphdr{{typ: 1, flags: 5, off: 0, vaddr: 0, fsz: 0x200, msz: 0x200}}, make([]byte, 0x200-0x100), 0x100)
	if _, err := LoadELFBytes(good); err != nil {
		t.Fatalf("baseline image must load: %v", err)
	}
	le := binary.LittleEndian
	cases := []struct {
		name string
		data func() []byte
		want string
	}{
		{"empty", func() []byte { return nil }, "elf"},
		{"truncated ehdr", func() []byte { return good[:40] }, "elf"},
		{"truncated phdr table", func() []byte { return good[:100] }, "elf"},
		{"phoff beyond file", func() []byte {
			d := append([]byte(nil), good...)
			le.PutUint64(d[32:], 1<<40)
			return d
		}, "elf"},
		{"segment file range beyond file", func() []byte {
			d := append([]byte(nil), good...)
			le.PutUint64(d[64+8:], 0x1f0) // p_offset near EOF with filesz 0x200
			return d
		}, "file range"},
		{"segment filesz > memsz", func() []byte {
			d := append([]byte(nil), good...)
			le.PutUint64(d[64+40:], 0x10)
			return d
		}, "exceeds memsz"},
		{"segment vaddr overflow", func() []byte {
			d := append([]byte(nil), good...)
			le.PutUint64(d[64+16:], ^uint64(0)-8)
			return d
		}, "image limit"},
		{"huge memsz", func() []byte {
			d := append([]byte(nil), good...)
			le.PutUint64(d[64+40:], 1<<45)
			return d
		}, "image limit"},
		{"wrong machine", func() []byte {
			d := append([]byte(nil), good...)
			le.PutUint16(d[18:], 183)
			return d
		}, "x86-64"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			_, err := LoadELFBytes(tc.data())
			if err == nil {
				t.Fatal("expected error")
			}
			if !strings.Contains(err.Error(), tc.want) {
				t.Fatalf("error %q lacks %q", err, tc.want)
			}
		})
	}
}

func relocELF(relocs ...[]byte) []byte {
	var rb []byte
	for _, r := range relocs {
		rb = append(rb, r...)
	}
	body := make([]byte, 0x400-0x100)
	put := func(addr int, b []byte) { copy(body[addr-0x100:], b) }
	put(0x360, rb)
	dyn := dynEntries(6, 0x300, 5, 0x340, 10, 0x10, 7, 0x360, 8, uint64(len(rb)), 0x6100003f, 48)
	put(0x200, dyn)
	return buildELF([]tphdr{
		{typ: 1, flags: 5, off: 0, vaddr: 0, fsz: 0x400, msz: 0x400},
		{typ: 2, flags: 6, off: 0x200, vaddr: 0x200, fsz: uint64(len(dyn)), msz: uint64(len(dyn))},
	}, body, 0x100)
}

func TestUnknownRelocationRejected(t *testing.T) {
	_, err := LoadELFBytes(relocELF(rela(0x80, 8, 0, 0x10), rela(0x90, 0x1234, 0, 0)))
	var ue *UnsupportedRelocError
	if !errors.As(err, &ue) {
		t.Fatalf("want UnsupportedRelocError, got %v", err)
	}
	if ue.Type != 0x1234 || ue.Offset != 0x90 {
		t.Fatalf("got type %d offset 0x%x", ue.Type, ue.Offset)
	}
	if !strings.Contains(err.Error(), "type 4660") || !strings.Contains(err.Error(), "0x90") {
		t.Fatalf("message should list type and offset: %v", err)
	}
}

func TestStandardDynamicTagsYieldRelocations(t *testing.T) {
	l := mustLoad(t, relocELF(rela(0x80, 8, 0, 0x1234), rela(0x88, 6, 1, 0)))
	if len(l.Relocations) != 2 {
		t.Fatalf("relocations = %d, want 2", len(l.Relocations))
	}
	if got := imgU64(l.MemoryImage, 0x80); got != 0x1234 {
		t.Fatalf("RELATIVE not applied: 0x%x", got)
	}
}

func TestRelocationSymbolIndexOutOfRange(t *testing.T) {
	if _, err := LoadELFBytes(relocELF(rela(0x80, 6, 99, 0))); err == nil || !strings.Contains(err.Error(), "symbol index") {
		t.Fatalf("want symbol index error, got %v", err)
	}
}

func TestDynamicTablesOutsideImageRejected(t *testing.T) {
	d := relocELF(rela(0x80, 8, 0, 0))
	// Point DT_RELA (value of the 4th dynamic entry) outside the image.
	binary.LittleEndian.PutUint64(d[0x100+0x100+3*16+8:], 0x10_0000)
	if _, err := LoadELFBytes(d); err == nil || !strings.Contains(err.Error(), "DT_RELA") {
		t.Fatalf("want DT_RELA error, got %v", err)
	}
}

func TestSELFPS5MagicAndTruncation(t *testing.T) {
	if !IsSELF([]byte{0x54, 0x14, 0xF5, 0xEE, 0, 0}) || !IsSELF([]byte{0x4F, 0x15, 0x3D, 0x1D}) {
		t.Fatal("both SELF magics must be recognised")
	}
	if IsSELF([]byte{0x7f, 'E', 'L', 'F'}) {
		t.Fatal("plain ELF is not a SELF")
	}
	for name, d := range map[string][]byte{
		"magic only":   {0x54, 0x14, 0xF5, 0xEE},
		"short header": append([]byte{0x54, 0x14, 0xF5, 0xEE}, make([]byte, 0x10)...),
		"entries beyond": func() []byte {
			b := make([]byte, 0x20)
			copy(b, []byte{0x54, 0x14, 0xF5, 0xEE})
			binary.LittleEndian.PutUint16(b[0x18:], 100)
			return b
		}(),
		"too many entries": func() []byte {
			b := make([]byte, 0x20)
			copy(b, []byte{0x54, 0x14, 0xF5, 0xEE})
			binary.LittleEndian.PutUint16(b[0x18:], 60000)
			return b
		}(),
	} {
		if _, err := ExtractELFFromSELF(d); err == nil {
			t.Errorf("%s: expected error", name)
		}
	}
}

// Real PS5 artifacts from the local testbed; skipped when absent.
func TestRealPS5Binaries(t *testing.T) {
	base := os.Getenv("NPS5_ROOT") + "/testbed/bin/syobon/extracted/PPSA47402/"
	for _, tc := range []struct {
		file             string
		minRelocs, minDS int
	}{
		{"eboot.bin", 3000, 200},
		{"sce_module/libc.prx", 1000, 2000}, // PS5 magic 54 14 F5 EE
	} {
		if _, err := os.Stat(base + tc.file); err != nil {
			t.Skipf("testbed not present: %v", err)
		}
		l, err := LoadELF(base + tc.file)
		if err != nil {
			t.Fatalf("%s: %v", tc.file, err)
		}
		if len(l.Relocations) < tc.minRelocs || len(l.DynSymbols) < tc.minDS {
			t.Errorf("%s: relocations=%d dynsyms=%d, want >= %d / %d", tc.file, len(l.Relocations), len(l.DynSymbols), tc.minRelocs, tc.minDS)
		}
	}
}

// PS5 dumps keep e_shoff/e_shnum but drop the section headers: the image must still load.
func TestLoadSectionHeadersPastEOF(t *testing.T) {
	d := buildELF([]tphdr{{typ: 1, flags: 5, off: 0, vaddr: 0, fsz: 0x200, msz: 0x200}}, make([]byte, 0x200-0x100), 0x100)
	le := binary.LittleEndian
	le.PutUint64(d[40:], 0x8000000) // e_shoff past EOF
	le.PutUint16(d[58:], 64)        // e_shentsize
	le.PutUint16(d[60:], 30)        // e_shnum
	le.PutUint16(d[62:], 29)        // e_shstrndx
	orig := append([]byte(nil), d...)
	l := mustLoad(t, d)
	if len(l.Segments) != 1 || len(l.Sections) != 0 {
		t.Fatalf("segments=%d sections=%d", len(l.Segments), len(l.Sections))
	}
	if string(orig) != string(d) {
		t.Fatal("input was modified")
	}
}

func TestExcludedModuleMatch(t *testing.T) {
	ex := []string{"kitt_webkit-ps5-shipping"}
	for path, want := range map[string]bool{
		"/a/prx/kitt_webkit-ps5-shipping.prx": true,
		"KITT_WEBKIT-PS5-SHIPPING.PRX":        true,
		"/a/prx/kitt-ps5-shipping.prx":        false,
	} {
		if got := ExcludedModuleMatch(path, ex); got != want {
			t.Errorf("%s: %v", path, got)
		}
	}
	if !ExcludedModuleMatch("x/libfoo.prx", []string{"libfoo.prx"}) {
		t.Error("with extension")
	}
}
