package elfloader

import "testing"

// imgU64 reads a 64-bit slot of a test image; an unmapped slot reads as all-ones so a test
// comparing against a real value fails.
func imgU64(m *Image, addr uint64) uint64 {
	v, ok := m.U64(addr)
	if !ok {
		return ^uint64(0)
	}
	return v
}

func TestImageSparseAccess(t *testing.T) {
	lo := make([]byte, 0x4000)
	hi := make([]byte, 0x8000)
	m := NewImage(0x400000, lo)
	if err := m.Merge(NewImage(0x7000400000, hi)); err != nil {
		t.Fatal(err)
	}
	if !m.PutU64(0x7000400ff8, 0x1122334455667788) {
		t.Fatal("PutU64 in the high region failed")
	}
	if v, ok := m.U64(0x7000400ff8); !ok || v != 0x1122334455667788 {
		t.Fatalf("U64 = %#x, %v", v, ok)
	}
	if hi[0xff8] != 0x88 {
		t.Fatal("region data is not aliased")
	}
	for _, c := range []struct {
		addr, n uint64
		want    bool
	}{
		{0, 1, false}, {0x3fffff, 1, false}, {0x400000, 0x4000, true}, {0x400000, 0x4001, false},
		{0x403fff, 1, true}, {0x404000, 1, false}, {0x7000400000, 0x8000, true}, {0x7000407ff9, 8, false},
	} {
		if got := m.Has(c.addr, c.n); got != c.want {
			t.Errorf("Has(%#x, %#x) = %v, want %v", c.addr, c.n, got, c.want)
		}
	}
	if got := len(m.From(0x403ff0)); got != 0x10 {
		t.Errorf("From stops at the region end: len %#x", got)
	}
	if m.Lo() != 0x400000 || m.End() != 0x7000408000 || m.Size() != 0xc000 {
		t.Errorf("Lo/End/Size = %#x %#x %#x", m.Lo(), m.End(), m.Size())
	}
	if err := m.Merge(NewImage(0x402000, make([]byte, 0x4000))); err == nil {
		t.Error("overlapping merge accepted")
	}
	m.Shift(0x10000)
	if !m.Has(0x410000, 8) || m.Has(0x400000, 8) {
		t.Error("Shift did not move the regions")
	}
	var nilImg *Image
	if nilImg.Has(0, 1) || nilImg.From(0) != nil || nilImg.End() != 0 {
		t.Error("nil image is not empty")
	}
}
