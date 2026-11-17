package disasm

import (
	"encoding/binary"

	"ps4-recomp/pkg/elfloader"
)

// EHSites holds addresses recovered from C++ exception-handling tables.
type EHSites struct {
	LandingPads []uint64 // landing pads named by LSDA call-site records
	ReturnSites []uint64 // end of every call-site region that has a landing pad (return address of the throwing call)
	LSDAs       int      // distinct LSDAs parsed
	Malformed   int      // LSDAs or FDEs that did not parse
}

// ehReader reads EH data from one image region: img is the region's bytes, pos an offset into
// them, and base the guest address of img[0], so a pc-relative value resolves to a guest
// address wherever the module was placed.
type ehReader struct {
	img  []byte
	base uint64
	pos  uint64
	ok   bool
}

func (r *ehReader) u8() byte {
	if r.pos >= uint64(len(r.img)) {
		r.ok = false
		return 0
	}
	b := r.img[r.pos]
	r.pos++
	return b
}

func (r *ehReader) uleb() uint64 {
	var v uint64
	for shift := uint(0); shift < 64; shift += 7 {
		b := r.u8()
		v |= uint64(b&0x7f) << shift
		if b&0x80 == 0 || !r.ok {
			break
		}
	}
	return v
}

func (r *ehReader) sleb() int64 {
	var v int64
	var shift uint
	var b byte
	for shift < 64 {
		b = r.u8()
		v |= int64(b&0x7f) << shift
		shift += 7
		if b&0x80 == 0 || !r.ok {
			break
		}
	}
	if shift < 64 && b&0x40 != 0 {
		v |= -1 << shift
	}
	return v
}

// encoded reads a DW_EH_PE value. ok=false for an omitted or unsupported encoding.
func (r *ehReader) encoded(enc byte) (uint64, bool) {
	if enc == 0xff {
		return 0, false
	}
	at := r.pos
	var v uint64
	switch enc & 0x0f {
	case 0x00, 0x04, 0x0c: // absptr, udata8, sdata8
		if r.pos+8 > uint64(len(r.img)) {
			r.ok = false
			return 0, false
		}
		v = binary.LittleEndian.Uint64(r.img[r.pos:])
		r.pos += 8
	case 0x01:
		v = r.uleb()
	case 0x09:
		v = uint64(r.sleb())
	case 0x02, 0x03, 0x0a, 0x0b: // udata2/4, sdata2/4
		n := uint64(2)
		if enc&0x0f == 0x03 || enc&0x0f == 0x0b {
			n = 4
		}
		if r.pos+n > uint64(len(r.img)) {
			r.ok = false
			return 0, false
		}
		if n == 4 {
			v = uint64(binary.LittleEndian.Uint32(r.img[r.pos:]))
			if enc&0x0f == 0x0b {
				v = uint64(int64(int32(v)))
			}
		} else {
			v = uint64(binary.LittleEndian.Uint16(r.img[r.pos:]))
			if enc&0x0f == 0x0a {
				v = uint64(int64(int16(v)))
			}
		}
		r.pos += n
	default:
		r.ok = false
		return 0, false
	}
	switch enc & 0x70 {
	case 0x10: // pcrel
		v += r.base + at
	case 0x00:
	default:
		r.ok = false
		return 0, false
	}
	return v, true
}

type eheCIE struct {
	fdeEnc, lsdaEnc byte
	hasZ            bool
	ok              bool
}

// ParseLSDA walks .eh_frame and every referenced LSDA and returns the landing pads and
// throwing-call return sites. The tables are read from the image region that holds .eh_frame
// (a module's .eh_frame and its LSDAs are in the same region); positions below are offsets in
// that region and every decoded pointer is a guest address.
func ParseLSDA(l *elfloader.LoadedELF) *EHSites {
	out := &EHSites{}
	if l == nil {
		return out
	}
	if l.EHFrameSize == 0 {
		return out
	}
	region, mapped := l.MemoryImage.Region(l.EHFrameAddr)
	if !mapped {
		return out
	}
	img, base := region.Data, region.Addr
	start := l.EHFrameAddr - base
	end := start + l.EHFrameSize
	if end > uint64(len(img)) {
		end = uint64(len(img))
	}
	cies := map[uint64]eheCIE{}
	parseCIE := func(at uint64) eheCIE {
		if c, ok := cies[at]; ok {
			return c
		}
		c := eheCIE{fdeEnc: 0, lsdaEnc: 0xff}
		r := &ehReader{img: img[:end], base: base, pos: at, ok: true}
		length := uint64(binary.LittleEndian.Uint32(img[at:]))
		r.pos += 4
		if length == 0xffffffff {
			cies[at] = c
			return c
		}
		cend := at + 4 + length
		r.pos += 4 // CIE id
		ver := r.u8()
		var aug []byte
		for {
			b := r.u8()
			if b == 0 || !r.ok {
				break
			}
			aug = append(aug, b)
		}
		if ver == 4 {
			r.pos += 2
		}
		r.uleb()
		r.sleb()
		if ver == 1 {
			r.u8()
		} else {
			r.uleb()
		}
		if len(aug) > 0 && aug[0] == 'z' {
			c.hasZ = true
			alen := r.uleb()
			aend := r.pos + alen
			for _, ch := range aug[1:] {
				switch ch {
				case 'R':
					c.fdeEnc = r.u8()
				case 'L':
					c.lsdaEnc = r.u8()
				case 'P':
					pe := r.u8()
					r.encoded(pe)
				case 'S', 'B':
				}
			}
			r.pos = aend
		}
		c.ok = r.ok && cend <= end
		cies[at] = c
		return c
	}

	seenLSDA := map[uint64]bool{}
	seenLP := map[uint64]bool{}
	seenRS := map[uint64]bool{}
	pos := start
	for pos+8 <= end {
		length := uint64(binary.LittleEndian.Uint32(img[pos:]))
		if length == 0 {
			break
		}
		if length == 0xffffffff {
			break
		}
		next := pos + 4 + length
		if next > end {
			out.Malformed++
			break
		}
		id := uint64(binary.LittleEndian.Uint32(img[pos+4:]))
		if id != 0 { // FDE: id is the backward offset to its CIE
			cieAt := pos + 4 - id
			if cieAt >= end {
				out.Malformed++
				pos = next
				continue
			}
			c := parseCIE(cieAt)
			if c.ok && c.hasZ && c.lsdaEnc != 0xff {
				r := &ehReader{img: img[:end], base: base, pos: pos + 8, ok: true}
				fnStart, _ := r.encoded(c.fdeEnc)
				r.encoded(c.fdeEnc & 0x0f) // pc range: value form only
				alen := r.uleb()
				aend := r.pos + alen
				if r.ok && alen > 0 {
					if lsda, has := r.encoded(c.lsdaEnc); has && r.ok && lsda != 0 && !seenLSDA[lsda] {
						seenLSDA[lsda] = true
						if !parseLSDATable(img, base, lsda, fnStart, out, seenLP, seenRS) {
							out.Malformed++
						}
					}
				}
				_ = aend
			}
		}
		pos = next
	}
	out.LSDAs = len(seenLSDA)
	return out
}

// parseLSDATable parses the LSDA at guest address at; img is its region and base the region's
// guest address.
func parseLSDATable(img []byte, base, at, fnStart uint64, out *EHSites, seenLP, seenRS map[uint64]bool) bool {
	if at < base || at-base >= uint64(len(img)) {
		return false
	}
	r := &ehReader{img: img, base: base, pos: at - base, ok: true}
	lpEnc := r.u8()
	lpStart := fnStart
	if lpEnc != 0xff {
		v, ok := r.encoded(lpEnc)
		if !ok {
			return false
		}
		lpStart = v
	}
	if ttEnc := r.u8(); ttEnc != 0xff {
		r.uleb()
	}
	csEnc := r.u8()
	csLen := r.uleb()
	if !r.ok || csLen > 1<<24 {
		return false
	}
	tableEnd := r.pos + csLen
	for r.pos < tableEnd && r.ok {
		cs, ok1 := r.encoded(csEnc & 0x0f)
		ln, ok2 := r.encoded(csEnc & 0x0f)
		lp, ok3 := r.encoded(csEnc & 0x0f)
		r.uleb() // action
		if !ok1 || !ok2 || !ok3 || !r.ok {
			return false
		}
		if lp == 0 {
			continue
		}
		pad := lpStart + lp
		if !seenLP[pad] {
			seenLP[pad] = true
			out.LandingPads = append(out.LandingPads, pad)
		}
		ret := lpStart + cs + ln
		if !seenRS[ret] {
			seenRS[ret] = true
			out.ReturnSites = append(out.ReturnSites, ret)
		}
	}
	return r.ok
}
