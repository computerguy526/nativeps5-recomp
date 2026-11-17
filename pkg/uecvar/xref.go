package uecvar

import (
	"encoding/binary"
	"math"
	"sort"
	"strconv"
	"strings"

	"golang.org/x/arch/x86/x86asm"
)

// Memory is read access to a linked x86-64 image by virtual address (elfloader.Image fits).
type Memory interface {
	Bytes(addr, n uint64) []byte // nil when the range is not mapped
}

// Range is a half-open [Start, End) virtual address interval.
type Range struct{ Start, End uint64 }

// Kind says how a console variable is stored.
type Kind string

const (
	KindTACV   Kind = "tacv" // TAutoConsoleVariable<T>: Storage is the FAutoConsoleObject (+8 = IConsoleVariable*)
	KindRef    Kind = "ref"  // FAutoConsoleVariableRef: Storage is the plain global the variable points at
	KindUnused Kind = "unknown"
)

// Cvar is one registration found in the binary.
type Cvar struct {
	Name     string `json:"name"`
	Type     string `json:"type"` // bool, int, float, string, or "?" (a Ref slot whose type is not established)
	Kind     Kind   `json:"kind"`
	Default  string `json:"default,omitempty"` // text; for Ref variables the global's initial value (before the engine runs)
	Storage  uint64 `json:"storage,omitempty"` // linked VA (guest address = VA + load bias)
	Flags    uint32 `json:"flags"`
	Slot     int    `json:"slot"` // IConsoleManager vtable byte offset of the registration call
	Site     uint64 `json:"site"` // VA of the lea that loads the name
	DefaultV uint64 `json:"default_bits,omitempty"`
}

// slotInfo maps the IConsoleManager vtable offset of the registration call to (type, kind).
// Offsets 0..0x18 register a variable the manager owns (TAutoConsoleVariable<T>); the others
// bind a global (FAutoConsoleVariableRef). The Ref slot types come from the allocation sizes of
// the FConsoleManager methods and the initial values of the globals: slot 0x28 holds packed bytes
// (257, 0x01010101: bool&), 0x30 small ints and -1 (int&), 0x38 float bit patterns (float&), 0x40
// string-array names (*DenyList), 0x20 FString& (always zero-initialised).
var slotInfo = map[int]struct {
	Type string
	Kind Kind
}{
	0x00: {"bool", KindTACV}, 0x08: {"int", KindTACV}, 0x10: {"float", KindTACV}, 0x18: {"string", KindTACV},
	0x20: {"string", KindRef}, 0x28: {"bool", KindRef}, 0x30: {"int", KindRef}, 0x38: {"float", KindRef}, 0x40: {"strarray", KindRef},
}

// SetRefSlotType overrides the reference slot's type (used by tests and after evidence gathering).
func SetRefSlotType(slot int, typ string) {
	s := slotInfo[slot]
	s.Type = typ
	slotInfo[slot] = s
}

// FillRefDefaults sets Default and DefaultV of the Ref variables from the initial image value of
// their global (the value before any static constructor or ini file changes it).
func FillRefDefaults(m Memory, cv []Cvar) {
	for i := range cv {
		c := &cv[i]
		if c.Kind != KindRef || c.Storage == 0 {
			continue
		}
		b := m.Bytes(c.Storage, 4)
		if b == nil {
			continue
		}
		u := binary.LittleEndian.Uint32(b)
		switch c.Type {
		case "bool":
			c.DefaultV, c.Default = uint64(b[0]), strconv.Itoa(int(b[0]))
		case "int":
			c.DefaultV, c.Default = uint64(u), itoa(int32(u))
		case "float":
			c.DefaultV, c.Default = uint64(u), fmtFloat(u)
		}
	}
}

type val struct {
	k int // 0 unknown, 1 immediate, 2 address, 3 float bits
	v uint64
}

// UTF16Names returns every cvar-looking UTF-16LE string that starts at an aligned address inside
// the ranges (a TEXT("...") literal), by address.
func UTF16Names(m Memory, ranges []Range) map[uint64]string {
	out := map[uint64]string{}
	for _, r := range ranges {
		b := m.Bytes(r.Start, r.End-r.Start)
		start := -1
		for i := 0; i+2 <= len(b); i += 2 {
			c := b[i]
			if b[i+1] == 0 && c >= 0x20 && c < 0x7f {
				if start < 0 {
					start = i
				}
				continue
			}
			if start >= 0 && b[i+1] == 0 && c == 0 && (start == 0 || b[start-1] == 0 || start >= 2 && b[start-2] == 0 && b[start-1] == 0) {
				s := make([]byte, 0, (i-start)/2)
				for j := start; j < i; j += 2 {
					s = append(s, b[j])
				}
				if LooksLikeCvar(string(s)) {
					out[r.Start+uint64(start)] = string(s)
				}
			}
			start = -1
		}
	}
	return out
}

// LooksLikeCvar reports whether s is a dotted identifier with a known cvar prefix.
func LooksLikeCvar(s string) bool {
	if len(s) < 4 || len(s) > 160 || !nameRE.MatchString(s) {
		return false
	}
	for _, p := range prefixes {
		if strings.HasPrefix(s, p) {
			return true
		}
	}
	return false
}

// Xref finds the registrations of the named strings: it looks for RIP-relative lea instructions in
// code that load a name, then reads the registration call that follows (an IConsoleManager
// virtual call: name in rsi, default in edx/xmm0/lea, flags in r8d/ecx) and the
// FAutoConsoleObject constructor call that wraps the result.
func Xref(m Memory, code Range, names map[uint64]string) []Cvar {
	b := m.Bytes(code.Start, code.End-code.Start)
	var out []Cvar
	for i := 0; i+7 <= len(b); i++ {
		if (b[i] == 0x48 || b[i] == 0x4c) && b[i+1] == 0x8d && b[i+2]&0xc7 == 5 {
			d := int32(binary.LittleEndian.Uint32(b[i+3:]))
			t := uint64(int64(code.Start) + int64(i) + 7 + int64(d))
			if nm, ok := names[t]; ok {
				if c, ok := analyze(m, code, code.Start+uint64(i), t, nm); ok {
					out = append(out, c)
				}
			}
		}
	}
	sort.SliceStable(out, func(a, c int) bool { return out[a].Name < out[c].Name })
	return out
}

func regIdx(r x86asm.Reg) int {
	switch {
	case r >= x86asm.RAX && r <= x86asm.R15:
		return int(r - x86asm.RAX)
	case r >= x86asm.EAX && r <= x86asm.R15L:
		return int(r - x86asm.EAX)
	case r >= x86asm.AX && r <= x86asm.R15W:
		return int(r - x86asm.AX)
	}
	return -1
}

func is64(r x86asm.Reg) bool { return r >= x86asm.RAX && r <= x86asm.R15 }

const (
	rax = 0
	rcx = 1
	rdx = 2
	rsi = 6
	rdi = 7
	r8  = 8
)

// window: decode from a start offset before the site that lands exactly on the site.
func decodeFrom(m Memory, code Range, site uint64) (start uint64, ok bool) {
	for back := uint64(56); back >= 1; back-- {
		if site < code.Start+back {
			continue
		}
		p := site - back
		buf := m.Bytes(p, back+16)
		if buf == nil {
			continue
		}
		off := uint64(0)
		for off < back {
			in, err := x86asm.Decode(buf[off:], 64)
			if err != nil {
				break
			}
			off += uint64(in.Len)
		}
		if off == back {
			return p, true
		}
	}
	return site, true
}

func analyze(m Memory, code Range, site, nameAddr uint64, name string) (Cvar, bool) {
	p, _ := decodeFrom(m, code, site)
	var regs [16]val
	var xmm0 val
	c := Cvar{Name: name, Site: site, Slot: -1}
	registered := false
	var retInRsi bool
	steps := 0
	for ; steps < 60; steps++ {
		buf := m.Bytes(p, 16)
		if buf == nil {
			return c, false
		}
		in, err := x86asm.Decode(buf, 64)
		if err != nil {
			return c, false
		}
		next := p + uint64(in.Len)
		switch in.Op {
		case x86asm.RET, x86asm.JMP, x86asm.INT:
			if p < site {
				regs = [16]val{}
				xmm0 = val{}
				p = next
				continue
			}
			return c, registered
		case x86asm.CALL:
			if p < site {
				regs = [16]val{}
				xmm0 = val{}
				p = next
				continue
			}
			if !registered {
				if regs[rsi].k != 2 || regs[rsi].v != nameAddr {
					return c, false
				}
				mem, isMem := in.Args[0].(x86asm.Mem)
				if !isMem {
					// direct call: a TAutoConsoleVariable constructor taking (this, name, default,...) is not modelled
					return c, false
				}
				slot := int(mem.Disp)
				info, known := slotInfo[slot]
				if !known {
					return c, false
				}
				c.Slot, c.Type, c.Kind = slot, info.Type, info.Kind
				if info.Kind == KindTACV {
					switch info.Type {
					case "float":
						c.DefaultV = xmm0.v
						c.Default = fmtFloat(uint32(xmm0.v))
						if xmm0.k == 0 {
							c.Default = "?"
						}
						c.Flags = uint32(regs[rcx].v)
					case "string":
						c.Flags = uint32(regs[r8].v)
					default:
						c.DefaultV = regs[rdx].v
						c.Default = itoa(int32(regs[rdx].v))
						if regs[rdx].k == 0 {
							c.Default = "?"
						}
						c.Flags = uint32(regs[r8].v)
					}
				} else {
					c.Flags = uint32(regs[r8].v)
					if regs[rdx].k == 2 {
						c.Storage = regs[rdx].v
					}
				}
				registered = true
				regs = [16]val{}
				xmm0 = val{}
				regs[rax] = val{k: 4}
				p = next
				continue
			}
			// the FAutoConsoleObject constructor: (this=rdi, IConsoleVariable*=rsi)
			if c.Kind == KindTACV && retInRsi && regs[rdi].k == 2 {
				c.Storage = regs[rdi].v
			}
			if c.Kind == KindTACV && c.Storage == 0 {
				return c, false
			}
			return c, true
		}
		// register tracking
		switch in.Op {
		case x86asm.LEA:
			dst, _ := in.Args[0].(x86asm.Reg)
			if mem, ok := in.Args[1].(x86asm.Mem); ok && mem.Base == x86asm.RIP && regIdx(dst) >= 0 {
				regs[regIdx(dst)] = val{2, uint64(int64(next) + mem.Disp)}
			} else if regIdx(dst) >= 0 {
				regs[regIdx(dst)] = val{}
			}
		case x86asm.MOV:
			dst, isReg := in.Args[0].(x86asm.Reg)
			if !isReg || regIdx(dst) < 0 {
				break
			}
			switch s := in.Args[1].(type) {
			case x86asm.Imm:
				v := uint64(int64(s))
				if !is64(dst) {
					v &= 0xffffffff
				}
				regs[regIdx(dst)] = val{1, v}
			case x86asm.Reg:
				if regIdx(s) >= 0 {
					regs[regIdx(dst)] = regs[regIdx(s)]
					if regIdx(s) == rax && regs[rax].k == 4 && regIdx(dst) == rsi {
						retInRsi = true
					}
				} else {
					regs[regIdx(dst)] = val{}
				}
			default:
				regs[regIdx(dst)] = val{}
			}
		case x86asm.XOR:
			a, ok1 := in.Args[0].(x86asm.Reg)
			bb, ok2 := in.Args[1].(x86asm.Reg)
			if ok1 && ok2 && a == bb && regIdx(a) >= 0 {
				regs[regIdx(a)] = val{1, 0}
			} else if ok1 && regIdx(a) >= 0 {
				regs[regIdx(a)] = val{}
			}
		case x86asm.XORPS, x86asm.PXOR, x86asm.VXORPS, x86asm.VPXOR:
			if a, ok := in.Args[0].(x86asm.Reg); ok && a == x86asm.X0 {
				if bb, ok := in.Args[1].(x86asm.Reg); ok && bb == a {
					xmm0 = val{3, 0}
				} else {
					xmm0 = val{}
				}
			}
		case x86asm.MOVSS, x86asm.VMOVSS:
			if a, ok := in.Args[0].(x86asm.Reg); ok && a == x86asm.X0 {
				xmm0 = val{}
				if mem, ok := in.Args[1].(x86asm.Mem); ok && (mem.Base == x86asm.RIP || in.Op == x86asm.VMOVSS && mem.Base == 0 && mem.Index == 0) { // x86asm drops RIP for VEX forms
					if bb := m.Bytes(uint64(int64(next)+mem.Disp), 4); bb != nil {
						xmm0 = val{3, uint64(binary.LittleEndian.Uint32(bb))}
					}
				}
			}
		case x86asm.MOVD, x86asm.VMOVD:
			if a, ok := in.Args[0].(x86asm.Reg); ok && a == x86asm.X0 {
				xmm0 = val{}
				if s, ok := in.Args[1].(x86asm.Reg); ok && regIdx(s) >= 0 && regs[regIdx(s)].k == 1 {
					xmm0 = val{3, regs[regIdx(s)].v & 0xffffffff}
				}
			}
		default:
			// any other write to a tracked register: forget the common destinations
			if len(in.Args) > 0 {
				if d, ok := in.Args[0].(x86asm.Reg); ok && regIdx(d) >= 0 && in.Op != x86asm.CMP && in.Op != x86asm.TEST && in.Op != x86asm.PUSH {
					regs[regIdx(d)] = val{}
				}
			}
		}
		p = next
	}
	return c, false
}

func itoa(v int32) string { return strconv.Itoa(int(v)) }

func fmtFloat(b uint32) string {
	return strconv.FormatFloat(float64(math.Float32frombits(b)), 'g', 7, 32)
}

// Ref is an instruction that addresses a virtual address RIP-relatively.
type Ref struct{ Addr uint64 }

// FindRIPRefs returns the instructions in code (which starts at base) with a rip+disp32 memory
// operand whose target is va. The disp32 is followed by 0, 1, 2 or 4 immediate bytes, so each
// candidate position is confirmed by decoding an instruction that ends exactly there and whose
// memory operand has that displacement.
func FindRIPRefs(code []byte, base, va uint64) []Ref {
	var out []Ref
	for i := 0; i+4 <= len(code); i++ {
		d := int64(int32(binary.LittleEndian.Uint32(code[i:])))
		for _, imm := range []int{0, 1, 2, 4} {
			end := i + 4 + imm
			if base+uint64(end)+uint64(d) != va {
				continue
			}
			for s := i - 1; s >= 0 && s >= end-15; s-- {
				in, err := x86asm.Decode(code[s:], 64)
				if err != nil || s+in.Len != end {
					continue
				}
				if hasRIPDisp(in, d) {
					out = append(out, Ref{base + uint64(s)})
					break
				}
			}
		}
	}
	return out
}

// hasRIPDisp: a memory operand with displacement d that is RIP-based (x86asm reports the RIP base
// of VEX forms as no base at all).
func hasRIPDisp(in x86asm.Inst, d int64) bool {
	for _, a := range in.Args {
		if m, ok := a.(x86asm.Mem); ok && m.Disp == d && (m.Base == x86asm.RIP || m.Base == 0 && m.Index == 0 && strings.HasPrefix(in.Op.String(), "V")) {
			return true
		}
	}
	return false
}
