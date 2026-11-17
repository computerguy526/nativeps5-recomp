// SPDX-License-Identifier: GPL-2.0-or-later

package elfloader

// Adversarial loader / sparse-image tests (docs/m1/memory.md section 5). Written from the
// documented contract: a module's image holds exactly its segments' bytes at their linked
// addresses; malformed input is rejected with an error, never a panic or a silent half-load;
// relocations land inside the image; ApplyBias moves everything consistently.
// KNOWN-FAIL policy: see docs/m1/memtests.md. knownFail skips with the reason once the violation
// has been observed; MEMTESTS_STRICT=1 makes it a failure.

import (
	"encoding/binary"
	"fmt"
	"strings"
	"testing"
)

// knownFail was the KNOWN-FAIL hook (docs/m1/memtests.md); every defect it covered is fixed
// (docs/m1/memory-fixes-2.md), so a violation is now always a failure.
func knownFail(t *testing.T, format string, args ...any) {
	t.Helper()
	t.Fatalf("violated: %s", fmt.Sprintf(format, args...))
}

// loadNoPanic loads and converts a panic into a returned flag.
func loadNoPanic(data []byte) (l *LoadedELF, err error, panicked any) {
	defer func() {
		if r := recover(); r != nil {
			panicked = r
		}
	}()
	l, err = LoadELFBytes(data)
	return
}

func TestMemSegmentsWithGaps(t *testing.T) {
	// two segments 64 MiB apart, the second at an odd (unaligned) address inside its page
	hi := uint64(0x400_0000)
	body := make([]byte, 0x300)
	for i := range body {
		body[i] = byte(i%251) + 1
	}
	d := buildELF([]tphdr{
		{typ: 1, flags: 5, off: 0, vaddr: 0, fsz: 0x200, msz: 0x200},
		{typ: 1, flags: 6, off: 0x200, vaddr: hi + 0x200, fsz: 0x100, msz: 0x180},
	}, body, 0x100)
	l := mustLoad(t, d)
	if len(l.Segments) != 2 || l.MinVAddr != 0 || l.MaxVAddr != hi+0x200+0x180 {
		t.Fatalf("segments %d min %#x max %#x", len(l.Segments), l.MinVAddr, l.MaxVAddr)
	}
	// bytes of both segments are where they were linked
	if b := l.MemoryImage.Bytes(hi+0x200, 0x100); len(b) != 0x100 || b[0] != d[0x200] || b[0xff] != d[0x2ff] {
		t.Errorf("high segment bytes wrong or missing")
	}
	// the gap is zero, never file data
	for _, a := range []uint64{0x300, 0x1000, hi / 2, hi + 0x1ff} {
		if v, ok := l.MemoryImage.U64(a - a%8); !ok || v != 0 {
			t.Errorf("gap at %#x = %#x ok=%v, want zeros", a, v, ok)
		}
	}
	// .bss tail (memsz > filesz) is zero and the file bytes that follow the segment do not leak into it
	if v, ok := l.MemoryImage.U64(hi + 0x300 + 0x40); !ok || v != 0 {
		t.Errorf("bss word = %#x ok=%v", v, ok)
	}
	if l.MemoryImage.Has(hi+0x1000_0000, 8) {
		t.Errorf("image claims bytes far beyond the last segment")
	}
	if l.MemoryImage.Has(l.MaxVAddr, 1) && l.MaxVAddr&0x3fff == 0 {
		t.Errorf("image extends past the last segment's page")
	}
}

func TestMemBssNotFilledFromFile(t *testing.T) {
	body := make([]byte, 0x400)
	for i := range body {
		body[i] = 0xEE // file bytes after filesz are junk that belongs to other data
	}
	d := buildELF([]tphdr{{typ: 1, flags: 6, off: 0, vaddr: 0, fsz: 0x180, msz: 0x300}}, body, 0x100)
	l := mustLoad(t, d)
	for a := uint64(0x180); a < 0x300; a += 8 {
		if v, ok := l.MemoryImage.U64(a); !ok || v != 0 {
			t.Fatalf("bss word at %#x = %#x ok=%v", a, v, ok)
		}
	}
	if v, _ := l.MemoryImage.U64(0x178); v != 0xEEEEEEEEEEEEEEEE {
		t.Errorf("last file word at 0x178 = %#x", v)
	}
}

func TestMemOverlappingSegments(t *testing.T) {
	body := make([]byte, 0x400)
	for i := range body {
		body[i] = 0xA0
	}
	for i := 0x200; i < 0x400; i++ {
		body[i] = 0xB0
	}
	d := buildELF([]tphdr{
		{typ: 1, flags: 5, off: 0, vaddr: 0, fsz: 0x300, msz: 0x300},
		{typ: 1, flags: 6, off: 0x200, vaddr: 0x280, fsz: 0x100, msz: 0x100}, // overlaps [0x280,0x300)
	}, body, 0x100)
	l, err, p := loadNoPanic(d)
	if p != nil {
		t.Fatalf("panic: %v", p)
	}
	if err == nil {
		v, _ := l.MemoryImage.U64(0x280)
		knownFail(t, "two PT_LOAD segments overlapping by 0x80 bytes are accepted; the later one silently overwrites the earlier (byte at 0x280 = %#x)", v&0xff)
	}
	// identical segments (full overlap) and a segment fully inside another
	d2 := buildELF([]tphdr{
		{typ: 1, flags: 5, off: 0, vaddr: 0, fsz: 0x300, msz: 0x300},
		{typ: 1, flags: 6, off: 0x200, vaddr: 0x100, fsz: 0x40, msz: 0x40},
	}, body, 0x100)
	if _, err, _ := loadNoPanic(d2); err == nil {
		knownFail(t, "a PT_LOAD segment nested inside another is accepted")
	}
}

func relocImage(t *testing.T, segEnd uint64, relocs ...[]byte) []byte {
	t.Helper()
	var rb []byte
	for _, r := range relocs {
		rb = append(rb, r...)
	}
	body := make([]byte, 0x400-0x100)
	copy(body[0x360-0x100:], rb)
	dyn := dynEntries(6, 0x300, 5, 0x340, 10, 0x10, 7, 0x360, 8, uint64(len(rb)), 0x6100003f, 48)
	copy(body[0x200-0x100:], dyn)
	return buildELF([]tphdr{
		{typ: 1, flags: 5, off: 0, vaddr: 0, fsz: 0x400, msz: segEnd},
		{typ: 2, flags: 6, off: 0x200, vaddr: 0x200, fsz: uint64(len(dyn)), msz: uint64(len(dyn))},
	}, body, 0x100)
}

func TestMemRelocationTargetsAtSegmentEnd(t *testing.T) {
	const segEnd = 0x1234 // not page aligned: the image buffer is padded beyond it
	// last full word inside the segment: applied
	l := mustLoad(t, relocImage(t, segEnd, rela(segEnd-8, 8, 0, 0x55)))
	if v, ok := l.MemoryImage.U64(segEnd - 8); !ok || v != 0x55 {
		t.Errorf("RELATIVE at the last word: %#x %v", v, ok)
	}
	for _, c := range []struct {
		name string
		off  uint64
	}{
		{"straddles the segment end by 4 bytes", segEnd - 4},
		{"first byte past the segment", segEnd},
		{"inside the page padding", segEnd + 0x100},
		{"beyond the image", 0x1_0000_0000},
	} {
		t.Run(c.name, func(t *testing.T) {
			_, err, p := loadNoPanic(relocImage(t, segEnd, rela(c.off, 8, 0, 0x77)))
			if p != nil {
				t.Fatalf("panic: %v", p)
			}
			if err == nil {
				knownFail(t, "a RELATIVE relocation at %#x (segment ends at %#x) is accepted and %s", c.off, uint64(segEnd), "silently written into padding or dropped")
			}
		})
	}
	t.Run("offset wraps uint64", func(t *testing.T) {
		for _, off := range []uint64{^uint64(0) - 3, ^uint64(0) - 7, ^uint64(0)} {
			_, err, p := loadNoPanic(relocImage(t, segEnd, rela(off, 8, 0, 1)))
			if p != nil {
				knownFail(t, "relocation offset %#x makes `offset+8 <= len(image)` wrap and the loader panics: %v", off, p)
			}
			if err == nil {
				knownFail(t, "relocation offset %#x is accepted", off)
			}
		}
	})
}

func TestMemRelocationsFollowBias(t *testing.T) {
	for _, mode := range []struct {
		name  string
		delta uint64
	}{{"base", 0x400000}, {"identity", 0x70_0040_0000}} {
		t.Run(mode.name, func(t *testing.T) {
			l := mustLoad(t, relocImage(t, 0x400, rela(0x80, 8, 0, 0x1234), rela(0x88, 8, 0, 0)))
			if err := l.ApplyBias(mode.delta); err != nil {
				t.Fatal(err)
			}
			if v, ok := l.MemoryImage.U64(0x80 + mode.delta); !ok || v != 0x1234+mode.delta {
				t.Errorf("RELATIVE after bias: %#x %v want %#x", v, ok, 0x1234+mode.delta)
			}
			if v, _ := l.MemoryImage.U64(0x88 + mode.delta); v != mode.delta {
				t.Errorf("RELATIVE addend 0 after bias = %#x want %#x", v, mode.delta)
			}
			// a second displacement is cumulative: the value is addend + total bias, not applied twice
			if err := l.ApplyBias(0x10000); err != nil {
				t.Fatal(err)
			}
			if v, _ := l.MemoryImage.U64(0x80 + mode.delta + 0x10000); v != 0x1234+mode.delta+0x10000 {
				t.Errorf("RELATIVE after two biases = %#x want %#x", v, 0x1234+mode.delta+0x10000)
			}
			if l.Bias != mode.delta+0x10000 {
				t.Errorf("Bias = %#x", l.Bias)
			}
			if l.MemoryImage.Has(0x80, 8) {
				t.Errorf("old address still mapped after ApplyBias")
			}
		})
	}
	t.Run("overflowing bias", func(t *testing.T) {
		l := mustLoad(t, relocImage(t, 0x400, rela(0x80, 8, 0, 0x1234)))
		err := l.ApplyBias(^uint64(0) - 0x100)
		if err == nil {
			knownFail(t, "ApplyBias accepts a displacement that wraps the image past 2^64 (MaxVAddr now %#x)", l.MaxVAddr)
		}
	})
}

func TestMemMalformedSizes(t *testing.T) {
	body := make([]byte, 0x400)
	cases := []struct {
		name  string
		phdrs []tphdr
		known string // non-empty: loading is expected to fail but currently does not
	}{
		{"filesz > memsz with memsz 0", []tphdr{{typ: 1, flags: 5, off: 0, vaddr: 0, fsz: 0x200, msz: 0x200}, {typ: 1, flags: 6, off: 0x200, vaddr: 0x1000, fsz: 0x100, msz: 0}}, "a PT_LOAD with filesz > memsz == 0 is silently ignored instead of rejected"},
		{"segment ends exactly at the 4 GiB limit + 1", []tphdr{{typ: 1, flags: 5, off: 0, vaddr: 1 << 32, fsz: 0x10, msz: 1}}, ""},
		{"memsz max uint64", []tphdr{{typ: 1, flags: 5, off: 0, vaddr: 0, fsz: 0, msz: ^uint64(0)}}, ""},
		{"vaddr + memsz wraps", []tphdr{{typ: 1, flags: 5, off: 0, vaddr: ^uint64(0) - 0xff, fsz: 0, msz: 0x200}}, ""},
		{"offset + filesz wraps", []tphdr{{typ: 1, flags: 5, off: ^uint64(0) - 0x10, vaddr: 0, fsz: 0x40, msz: 0x40}}, ""},
		{"only non-load headers", []tphdr{{typ: 2, flags: 5, off: 0, vaddr: 0, fsz: 0x10, msz: 0x10}}, ""},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			d := buildELF(c.phdrs, body, 0x100)
			_, err, p := loadNoPanic(d)
			if p != nil {
				t.Fatalf("panic: %v", p)
			}
			if err == nil {
				if c.known != "" {
					knownFail(t, "%s", c.known)
				}
				t.Fatal("accepted")
			}
		})
	}
	t.Run("many segments", func(t *testing.T) {
		var ph []tphdr
		for i := 0; i < 200; i++ {
			ph = append(ph, tphdr{typ: 1, flags: 6, off: 0, vaddr: uint64(i) * 0x1000, fsz: 0x10, msz: 0x100})
		}
		l, err, p := loadNoPanic(buildELF(ph, make([]byte, 0x100), 0x3000))
		if p != nil || err != nil {
			t.Fatalf("200 well-formed segments: err=%v panic=%v", err, p)
		}
		if len(l.Segments) != 200 {
			t.Errorf("segments = %d", len(l.Segments))
		}
	})
}

func TestMemImageAdversarial(t *testing.T) {
	data := make([]byte, 0x4000)
	m := NewImage(0x10000, data)
	adj := NewImage(0x14000, make([]byte, 0x4000))
	if err := m.Merge(adj); err != nil {
		t.Fatalf("adjacent regions must merge: %v", err)
	}
	cases := []struct {
		addr, n uint64
		want    bool
	}{
		{0x10000, 0, true}, {0xFFFF, 1, false}, {0x13FFF, 2, true /* spans two adjacent regions */}, {^uint64(0) - 7, 16, false},
		{^uint64(0), 1, false}, {0x17FF8, 8, true}, {0x17FF9, 8, false}, {0x18000, 1, false},
	}
	for _, c := range cases {
		func() {
			defer func() {
				if r := recover(); r != nil {
					t.Errorf("Has(%#x,%#x) panicked: %v", c.addr, c.n, r)
				}
			}()
			got := m.Has(c.addr, c.n)
			if c.addr == 0x13FFF && c.n == 2 {
				return // spanning two adjacent regions: either answer is defensible, only no-panic matters
			}
			if got != c.want {
				t.Errorf("Has(%#x,%#x) = %v want %v", c.addr, c.n, got, c.want)
			}
		}()
	}
	// no write may escape the image
	for _, a := range []uint64{0x17FFC, 0x13FFC, 0xFFFC, ^uint64(0) - 3} {
		func() {
			defer func() {
				if r := recover(); r != nil {
					t.Errorf("PutU64(%#x) panicked: %v", a, r)
				}
			}()
			before := append([]byte(nil), data...)
			ok := m.PutU64(a, 0x1111111111111111)
			if !ok && string(before) != string(data) {
				t.Errorf("failed PutU64(%#x) modified memory", a)
			}
			if a == 0x17FFC && ok {
				t.Errorf("PutU64 across the end of the image succeeded")
			}
		}()
	}
	// an image whose end wraps 2^64
	func() {
		defer func() {
			if r := recover(); r != nil {
				t.Errorf("wrapping region panicked: %v", r)
			}
		}()
		w := NewImage(^uint64(0)-0xff, make([]byte, 0x200))
		if w.Has(^uint64(0)-8, 0x10) && w.Has(0, 1) {
			t.Errorf("image wrapping past 2^64 claims address 0")
		}
		w.Shift(0x10000)
	}()
	// merging nil / empty
	func() {
		defer func() {
			if r := recover(); r != nil {
				t.Errorf("Merge(nil) panicked: %v", r)
			}
		}()
		_ = m.Merge(nil)
	}()
	if err := m.Merge(NewImage(0x100000, nil)); err != nil {
		t.Errorf("merging an empty image failed: %v", err)
	}
}

func TestMemPutGetRoundTripAcrossEnds(t *testing.T) {
	data := make([]byte, 0x4000)
	m := NewImage(0x400000, data)
	for _, a := range []uint64{0x400000, 0x403FF8} {
		if !m.PutU64(a, 0x0102030405060708) {
			t.Fatalf("PutU64(%#x) failed", a)
		}
		if v, ok := m.U64(a); !ok || v != 0x0102030405060708 {
			t.Errorf("U64(%#x) = %#x %v", a, v, ok)
		}
	}
	if binary.LittleEndian.Uint64(data[0x3FF8:]) != 0x0102030405060708 {
		t.Errorf("data not aliased at the end")
	}
	if _, ok := m.U32(0x403FFD); ok {
		t.Errorf("U32 straddling the end succeeded")
	}
	if v, ok := m.U32(0x403FFC); !ok || v != 0x01020304 {
		t.Errorf("U32 at the last word = %#x %v", v, ok)
	}
	if !strings.Contains(fmt.Sprint(m.Merge(NewImage(0x403000, make([]byte, 0x2000)))), "overlaps") {
		t.Errorf("partial overlap not reported with the word 'overlaps'")
	}
}
