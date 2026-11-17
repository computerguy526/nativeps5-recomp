// SPDX-License-Identifier: GPL-2.0-or-later

package elfloader

import (
	"debug/elf"
	"encoding/binary"
	"fmt"
	"sort"
)

// ImageRegion is one contiguous run of guest memory in an Image: Data[i] is the byte at guest
// address Addr+i. A region is one module's load span (first mapped byte to last, gaps between
// its segments included as zeros), so intra-module scans that cross segments keep working.
type ImageRegion struct {
	Addr uint64
	Data []byte
}

// End returns the first guest address past the region.
func (r ImageRegion) End() uint64 { return r.Addr + uint64(len(r.Data)) }

// Image is the sparse guest memory image of a title: sorted, non-overlapping regions addressed
// by final guest virtual address. It replaces the dense "byte i is guest address i" slice, which
// cannot represent an image placed at 0x70_0000_0000 (identity mode) or modules spread over the
// console layout. All accessors are nil-safe and bounds-checked; none of them allocates.
type Image struct {
	regions []ImageRegion
}

// NewImage returns an image with a single region at addr. data is not copied.
func NewImage(addr uint64, data []byte) *Image {
	return &Image{regions: []ImageRegion{{Addr: addr, Data: data}}}
}

// Regions returns the regions in ascending address order. The slice must not be modified.
func (m *Image) Regions() []ImageRegion {
	if m == nil {
		return nil
	}
	return m.regions
}

// find returns the region containing addr, or nil.
func (m *Image) find(addr uint64) *ImageRegion {
	if m == nil {
		return nil
	}
	rs := m.regions
	// Titles have a handful of regions; the single-region case is the hot one.
	if len(rs) == 1 {
		if addr >= rs[0].Addr && addr-rs[0].Addr < uint64(len(rs[0].Data)) {
			return &rs[0]
		}
		return nil
	}
	i := sort.Search(len(rs), func(i int) bool { return rs[i].End() > addr })
	if i < len(rs) && addr >= rs[i].Addr {
		return &rs[i]
	}
	return nil
}

// Has reports whether [addr, addr+n) lies inside one region.
func (m *Image) Has(addr, n uint64) bool {
	r := m.find(addr)
	return r != nil && n <= uint64(len(r.Data))-(addr-r.Addr)
}

// Contains reports whether addr is a mapped image byte.
func (m *Image) Contains(addr uint64) bool { return m.find(addr) != nil }

// Bytes returns the n bytes at addr (aliasing the image), or nil when the range is not wholly
// inside one region.
func (m *Image) Bytes(addr, n uint64) []byte {
	r := m.find(addr)
	if r == nil {
		return nil
	}
	off := addr - r.Addr
	if n > uint64(len(r.Data))-off {
		return nil
	}
	return r.Data[off : off+n : off+n]
}

// From returns the bytes from addr to the end of its region (aliasing the image), or nil when
// addr is not mapped. It is what instruction decoding and string scans use.
func (m *Image) From(addr uint64) []byte {
	r := m.find(addr)
	if r == nil {
		return nil
	}
	return r.Data[addr-r.Addr:]
}

// Region returns the region containing addr.
func (m *Image) Region(addr uint64) (ImageRegion, bool) {
	r := m.find(addr)
	if r == nil {
		return ImageRegion{}, false
	}
	return *r, true
}

// RegionEnd returns the end address of the region containing addr, or 0 when addr is unmapped.
func (m *Image) RegionEnd(addr uint64) uint64 {
	r := m.find(addr)
	if r == nil {
		return 0
	}
	return r.End()
}

// U64 reads a little-endian 64-bit value.
func (m *Image) U64(addr uint64) (uint64, bool) {
	b := m.Bytes(addr, 8)
	if b == nil {
		return 0, false
	}
	return binary.LittleEndian.Uint64(b), true
}

// U32 reads a little-endian 32-bit value.
func (m *Image) U32(addr uint64) (uint32, bool) {
	b := m.Bytes(addr, 4)
	if b == nil {
		return 0, false
	}
	return binary.LittleEndian.Uint32(b), true
}

// PutU64 writes a little-endian 64-bit value; false when the slot is not mapped.
func (m *Image) PutU64(addr, v uint64) bool {
	b := m.Bytes(addr, 8)
	if b == nil {
		return false
	}
	binary.LittleEndian.PutUint64(b, v)
	return true
}

// Lo returns the lowest mapped address (0 for an empty image).
func (m *Image) Lo() uint64 {
	if m == nil || len(m.regions) == 0 {
		return 0
	}
	return m.regions[0].Addr
}

// End returns the first address past the highest region (0 for an empty image).
func (m *Image) End() uint64 {
	if m == nil || len(m.regions) == 0 {
		return 0
	}
	return m.regions[len(m.regions)-1].End()
}

// Size returns the number of mapped bytes.
func (m *Image) Size() uint64 {
	var n uint64
	for _, r := range m.Regions() {
		n += uint64(len(r.Data))
	}
	return n
}

// Shift moves every region by delta (ApplyBias). No bytes are copied.
func (m *Image) Shift(delta uint64) {
	if m == nil {
		return
	}
	for i := range m.regions {
		m.regions[i].Addr += delta
	}
}

// Merge adds src's regions to m. Regions are shared, not copied. It fails on any overlap.
func (m *Image) Merge(src *Image) error {
	if m == nil {
		return fmt.Errorf("merge into a nil image")
	}
	for _, r := range src.Regions() {
		if len(r.Data) == 0 {
			continue
		}
		for _, o := range m.regions {
			if r.Addr < o.End() && o.Addr < r.End() {
				return fmt.Errorf("image region 0x%x-0x%x overlaps 0x%x-0x%x", r.Addr, r.End(), o.Addr, o.End())
			}
		}
		m.regions = append(m.regions, r)
	}
	sort.Slice(m.regions, func(i, j int) bool { return m.regions[i].Addr < m.regions[j].Addr })
	return nil
}

// CodeProtectionSegments returns what the runtime may make read-only on the host: every loaded
// segment (the runtime protects the executable, non-writable ones) plus, for a segment that is
// both executable and writable (the payload SDK links one RWE segment for text and headers), a
// synthetic read-execute segment for each executable non-writable section, widened to whole pages
// unless a writable section shares a page with it. Guest code is data to the host and nothing
// legitimate writes it; a store into it is a guest bug the crash report should name.
func (l *LoadedELF) CodeProtectionSegments() []*Segment {
	out := append([]*Segment(nil), l.Segments...)
	const page = 0x4000
	var writable []AddrRange
	for _, s := range l.Sections {
		if s != nil && s.Flags&elf.SHF_ALLOC != 0 && s.Flags&elf.SHF_WRITE != 0 && s.Size > 0 {
			writable = append(writable, AddrRange{Start: s.Addr, End: s.Addr + s.Size})
		}
	}
	for _, seg := range l.Segments {
		if seg.Flags&elf.PF_X == 0 || seg.Flags&elf.PF_W == 0 {
			continue
		}
		for _, s := range l.Sections {
			if s == nil || s.Flags&elf.SHF_EXECINSTR == 0 || s.Flags&elf.SHF_WRITE != 0 || s.Size == 0 ||
				s.Addr < seg.Vaddr || s.Addr+s.Size > seg.Vaddr+seg.Memsz {
				continue
			}
			lo, hi := s.Addr&^(page-1), (s.Addr+s.Size+page-1)&^(page-1)
			shared := false
			for _, w := range writable {
				if w.Start < hi && w.End > lo {
					shared = true
					break
				}
			}
			if shared {
				lo, hi = (s.Addr+page-1)&^(page-1), (s.Addr+s.Size)&^(page-1) // whole pages inside only
			}
			if hi > lo {
				out = append(out, &Segment{Vaddr: lo, Memsz: hi - lo, Flags: elf.PF_R | elf.PF_X})
			}
		}
	}
	return out
}
