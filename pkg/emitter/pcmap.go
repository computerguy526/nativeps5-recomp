package emitter

import (
	"fmt"
	"strings"
)

// Compact host-pc -> guest-pc map.
//
// The runtime crash handler and `nps5 addr2guest` need to turn a host pc inside translated
// code into the guest function and instruction. The map is a data section,
// __DATA,__nps5_pcmap, with one record per emitted function, 8-byte aligned:
//
//	u64 host        address of the function symbol (a relocation)
//	u64 guest       guest entry address of the function
//	u32 hostlen     size of the function's host code in bytes
//	u32 namelen     bytes of the NUL-terminated name that follows the header (0: unnamed)
//	u32 streamlen   bytes of the entry stream that follows the name
//	u32 nentries    guest instructions in the stream
//	name[namelen]
//	stream[streamlen]
//	zero padding to 8 bytes
//
// The stream has one entry per guest instruction, in host order, each giving the distance in
// 4-byte host words from the previous entry (from the function start for the first) and the
// guest address delta from the previous entry (from the function entry for the first): one
// byte hw<<4|gd when both fit 4 bits and are not both zero and the instruction has no flags;
// otherwise a zero byte followed by uleb128 flags, uleb128 hw and uleb128 zigzag(gd). The host
// distance is counted by the emitter from the lines it writes (asmSizer), so no label is
// needed per instruction; the assembler verifies the total (.if hostlen mismatch).
//
// Flags: 1 unsupported instruction stub, 2 unsupported indirect jump/call stub. The first entry
// of a function is also flagged 4 by the decoder (nps5_diag.c, pkg/diag).

func putULEB(b []byte, v uint64) []byte {
	for {
		c := byte(v & 0x7f)
		v >>= 7
		if v == 0 {
			return append(b, c)
		}
		b = append(b, c|0x80)
	}
}

func zigzag(v int64) uint64 { return uint64(v<<1) ^ uint64(v>>63) }

// encodePCStream encodes the entries of a function whose entry is at guest address fn.
func encodePCStream(fn uint64, ents []pcEntry) (stream []byte, ok bool) {
	prevOff, prevGuest := 0, fn
	for _, en := range ents {
		d := en.off - prevOff
		if d < 0 || d%4 != 0 {
			return nil, false
		}
		hw := d / 4
		gd := int64(en.guest) - int64(prevGuest)
		if en.flags == 0 && hw <= 15 && gd >= 0 && gd <= 15 && hw|int(gd) != 0 {
			stream = append(stream, byte(hw<<4|int(gd)))
		} else {
			stream = append(stream, 0)
			stream = putULEB(stream, uint64(en.flags))
			stream = putULEB(stream, uint64(hw))
			stream = putULEB(stream, zigzag(gd))
		}
		prevOff, prevGuest = en.off, en.guest
	}
	return stream, true
}

// pcmapName is the function name as it goes into the table: no characters an assembler string
// would need to escape.
func pcmapName(s string) string {
	var b strings.Builder
	for _, r := range s {
		if r == '"' || r == '\\' || r < 0x20 || r > 0x7e {
			b.WriteByte('_')
		} else {
			b.WriteRune(r)
		}
	}
	return b.String()
}

// endFunc finishes the function just written: the assembler-side size check and the table
// record. It leaves the function out of the table (with a warning) when its code has a shape
// the sizer cannot follow.
func (c *chunkRender) endFunc(addr uint64, name string) {
	e := c.e
	stream, ok := encodePCStream(addr, c.ents)
	if c.sz.irregular || !ok || len(c.ents) == 0 {
		if len(c.ents) > 0 {
			e.Warnings = append(e.Warnings, fmt.Sprintf("pc map: function 0x%x has code the pc-map sizer cannot follow; it is not in the map", addr))
		}
		return
	}
	check := ".warning"
	if e.PCMapStrict {
		check = ".error"
	}
	sym := fmt.Sprintf("_fn_0x%x", addr)
	if c.entrySym != "" {
		sym = c.entrySym // the kept body of a wrapped function (Replacement.Wrap)
	}
	fmt.Fprintf(&c.buf, "Le%x:\n.if (Le%x-%s)!=%d\n%s \"pcmap: size mismatch in %s\"\n.endif\n", addr, addr, sym, c.sz.off, check, sym)

	nm := pcmapName(name)
	nlen := 0
	if nm != "" {
		nlen = len(nm) + 1
	}
	fmt.Fprintf(&c.pcm, "\t.quad %s,0x%x\n\t.long %d,%d,%d,%d\n", sym, addr, c.sz.off, nlen, len(stream), len(c.ents))
	if nlen > 0 {
		fmt.Fprintf(&c.pcm, "\t.asciz \"%s\"\n", nm)
	}
	for i := 0; i < len(stream); i += 32 {
		end := min(i+32, len(stream))
		c.pcm.WriteString("\t.byte ")
		for j := i; j < end; j++ {
			if j > i {
				c.pcm.WriteByte(',')
			}
			fmt.Fprintf(&c.pcm, "%d", stream[j])
		}
		c.pcm.WriteByte('\n')
	}
	c.pcm.WriteString("\t.p2align 3\n")
	e.Facts.PCEntries += len(c.ents)
}
