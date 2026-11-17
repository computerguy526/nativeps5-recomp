package lifter

import (
	"encoding/binary"
	"fmt"
	"math"
	"math/rand"
	"testing"

	"golang.org/x/arch/x86/x86asm"
)

// Semantic tests for the AVX2/FMA lowerings. Each case loads ymm1 (in1), ymm2 (in2) and ymm0
// (garbage) from memory, runs one VEX instruction (register and memory form of the same
// operand), stores ymm0, and compares the result with a Go reference of the Intel semantics.
// The lifted side runs natively on the arm64 host (runExecExpect); no x86 code is executed.

const (
	avIn1  = 0x100
	avIn2  = 0x120
	avInit = 0x140
	avOut  = 0x160
)

// vexHex encodes a VEX instruction: m = map (1 0F, 2 0F38, 3 0F3A), pp (1 = 66), opc, with
// ymm0 as reg, vvvv register number, modrm tail (register ymm2 or [rbx+0x20]).
func vexHex(m, w, vvvv, l, pp int, opc byte, mem bool, imm ...byte) string {
	b1 := byte(0xe0 | m)
	b2 := byte(w<<7 | (^vvvv&0xf)<<3 | l<<2 | pp)
	modrm := []byte{0xc2}
	if mem {
		modrm = []byte{0x43, 0x20}
	}
	out := append([]byte{0xc4, b1, b2, opc}, modrm...)
	out = append(out, imm...)
	return fmt.Sprintf("%x", out)
}

type avCase struct {
	name       string
	code       func(mem bool) string
	ref        func(in1, in2, init []byte) []byte
	in1, in2   []byte // nil: random bytes
	float      int    // 0 integer data, 4 / 8: float lanes of that size
	noMemForm  bool
	memOnlyArg bool
}

func randBytes(r *rand.Rand, n int) []byte {
	b := make([]byte, n)
	r.Read(b)
	return b
}

func floatLanes(r *rand.Rand, size int) []byte {
	b := make([]byte, 32)
	for i := 0; i < 32/size; i++ {
		v := float64(r.Intn(33)-16) / 4
		if size == 4 {
			binary.LittleEndian.PutUint32(b[i*4:], math.Float32bits(float32(v)))
		} else {
			binary.LittleEndian.PutUint64(b[i*8:], math.Float64bits(v))
		}
	}
	return b
}

func u32(b []byte, i int) uint32      { return binary.LittleEndian.Uint32(b[i*4:]) }
func u64(b []byte, i int) uint64      { return binary.LittleEndian.Uint64(b[i*8:]) }
func put32(b []byte, i int, v uint32) { binary.LittleEndian.PutUint32(b[i*4:], v) }
func put64(b []byte, i int, v uint64) { binary.LittleEndian.PutUint64(b[i*8:], v) }

func fma32(a, b, c float32) float32 { return float32(math.FMA(float64(a), float64(b), float64(c))) }

func avCases() []avCase {
	var cs []avCase
	// Broadcasts.
	for _, bc := range []struct {
		name string
		opc  byte
		sz   int
	}{{"B", 0x78, 1}, {"W", 0x79, 2}, {"D", 0x58, 4}, {"Q", 0x59, 8}} {
		bc := bc
		for _, l := range []int{0, 1} {
			l := l
			cs = append(cs, avCase{name: fmt.Sprintf("vpbroadcast%s l%d", bc.name, l),
				code: func(mem bool) string { return vexHex(2, 0, 0xf, l, 1, bc.opc, mem) },
				ref: func(in1, in2, init []byte) []byte {
					out := make([]byte, 32)
					n := 16
					if l == 1 {
						n = 32
					}
					for i := 0; i < n; i += bc.sz {
						copy(out[i:i+bc.sz], in2[:bc.sz])
					}
					return out
				}})
		}
	}
	// Permutes with immediate.
	for _, imm := range []byte{0x00, 0x1b, 0xe4, 0x4e, 0xb1, 0xff, 0x39, 0x93} {
		imm := imm
		for _, p := range []struct {
			n   string
			opc byte
		}{{"vpermq", 0}, {"vpermpd", 1}} {
			p := p
			cs = append(cs, avCase{name: fmt.Sprintf("%s %#x", p.n, imm),
				code: func(mem bool) string { return vexHex(3, 1, 0xf, 1, 1, p.opc, mem, imm) },
				ref: func(in1, in2, init []byte) []byte {
					out := make([]byte, 32)
					for k := 0; k < 4; k++ {
						s := int(imm>>(2*uint(k))) & 3
						put64(out, k, u64(in2, s))
					}
					return out
				}})
		}
		cs = append(cs, avCase{name: fmt.Sprintf("vperm2i128 %#x", imm),
			code: func(mem bool) string { return vexHex(3, 0, 1, 1, 1, 0x46, mem, imm) },
			ref: func(in1, in2, init []byte) []byte {
				out := make([]byte, 32)
				sel := func(s byte) []byte {
					switch s & 3 {
					case 0:
						return in1[:16]
					case 1:
						return in1[16:]
					case 2:
						return in2[:16]
					}
					return in2[16:]
				}
				if imm&8 == 0 {
					copy(out[:16], sel(imm))
				}
				if imm&0x80 == 0 {
					copy(out[16:], sel(imm>>4))
				}
				return out
			}})
	}
	// VPERMD / VPERMPS (in1 = indexes, in2 = table).
	for _, p := range []struct {
		n   string
		opc byte
	}{{"vpermd", 0x36}, {"vpermps", 0x16}} {
		p := p
		cs = append(cs, avCase{name: p.n,
			code: func(mem bool) string { return vexHex(2, 0, 1, 1, 1, p.opc, mem) },
			ref: func(in1, in2, init []byte) []byte {
				out := make([]byte, 32)
				for i := 0; i < 8; i++ {
					put32(out, i, u32(in2, int(u32(in1, i)&7)))
				}
				return out
			}})
	}
	// Variable shifts.
	shifts := []struct {
		n    string
		opc  byte
		w    int
		fn   func(v uint64, c uint64) uint64
		bits uint
	}{
		{"vpsllvd", 0x47, 0, func(v, c uint64) uint64 { return uint64(uint32(v) << c) }, 32},
		{"vpsrlvd", 0x45, 0, func(v, c uint64) uint64 { return uint64(uint32(v) >> c) }, 32},
		{"vpsravd", 0x46, 0, func(v, c uint64) uint64 { return uint64(uint32(int32(v) >> c)) }, 32},
		{"vpsllvq", 0x47, 1, func(v, c uint64) uint64 { return v << c }, 64},
		{"vpsrlvq", 0x45, 1, func(v, c uint64) uint64 { return v >> c }, 64},
	}
	for _, s := range shifts {
		s := s
		for _, l := range []int{0, 1} {
			l := l
			cs = append(cs, avCase{name: fmt.Sprintf("%s l%d", s.n, l),
				code: func(mem bool) string { return vexHex(2, s.w, 1, l, 1, s.opc, mem) },
				ref: func(in1, in2, init []byte) []byte {
					out := make([]byte, 32)
					esz := int(s.bits / 8)
					n := 16 / esz
					if l == 1 {
						n *= 2
					}
					for i := 0; i < n; i++ {
						var v, c uint64
						if esz == 4 {
							v, c = uint64(u32(in1, i)), uint64(u32(in2, i))
						} else {
							v, c = u64(in1, i), u64(in2, i)
						}
						var r uint64
						if c >= uint64(s.bits) {
							if s.n == "vpsravd" && int32(v) < 0 {
								r = 0xffffffff
							}
						} else {
							r = s.fn(v, c)
						}
						if esz == 4 {
							put32(out, i, uint32(r))
						} else {
							put64(out, i, r)
						}
					}
					return out
				}})
		}
	}
	// VPMULHUW.
	for _, l := range []int{0, 1} {
		l := l
		cs = append(cs, avCase{name: fmt.Sprintf("vpmulhuw l%d", l),
			code: func(mem bool) string { return vexHex(1, 0, 1, l, 1, 0xe4, mem) },
			ref: func(in1, in2, init []byte) []byte {
				out := make([]byte, 32)
				n := 8
				if l == 1 {
					n = 16
				}
				for i := 0; i < n; i++ {
					a := uint32(binary.LittleEndian.Uint16(in1[2*i:]))
					b := uint32(binary.LittleEndian.Uint16(in2[2*i:]))
					binary.LittleEndian.PutUint16(out[2*i:], uint16(a*b>>16))
				}
				return out
			}})
	}
	// FMA families.
	type fk struct {
		name     string
		neg, sub bool
		base     byte // opcode of 132 packed; +0x10 for 213, +0x20 for 231; scalar = +1
	}
	for _, f := range []fk{{"vfmadd", false, false, 0x98}, {"vfmsub", false, true, 0x9a}, {"vfnmadd", true, false, 0x9c}, {"vfnmsub", true, true, 0x9e}} {
		f := f
		for oi, order := range []int{132, 213, 231} {
			order := order
			for _, t := range []struct {
				n      string
				scalar bool
				dbl    bool
				l      int
			}{{"ps", false, false, 1}, {"ps", false, false, 0}, {"pd", false, true, 1}, {"pd", false, true, 0}, {"ss", true, false, 0}, {"sd", true, true, 0}} {
				t := t
				opc := f.base + byte(oi)*0x10
				if t.scalar {
					opc++
				}
				w := 0
				sz := 4
				if t.dbl {
					w, sz = 1, 8
				}
				cs = append(cs, avCase{name: fmt.Sprintf("%s%d%s l%d", f.name, order, t.n, t.l), float: sz,
					code: func(mem bool) string { return vexHex(2, w, 1, t.l, 1, opc, mem) },
					ref: func(in1, in2, init []byte) []byte {
						out := make([]byte, 32)
						n := 32 / sz
						switch {
						case t.scalar:
							n = 1
						case t.l == 0:
							n = 16 / sz
						}
						if t.scalar {
							copy(out[:16], init[:16])
						}
						for i := 0; i < n; i++ {
							var a, b, c float64
							get := func(buf []byte) float64 {
								if sz == 4 {
									return float64(math.Float32frombits(u32(buf, i)))
								}
								return math.Float64frombits(u64(buf, i))
							}
							op1, op2, op3 := get(init), get(in1), get(in2)
							switch order {
							case 132:
								a, b, c = op1, op3, op2
							case 213:
								a, b, c = op2, op1, op3
							default:
								a, b, c = op2, op3, op1
							}
							p := a * b // exact for these inputs
							if f.neg {
								p = -p
							}
							if f.sub {
								c = -c
							}
							r := p + c
							if sz == 4 {
								put32(out, i, math.Float32bits(float32(r)))
							} else {
								put64(out, i, math.Float64bits(r))
							}
						}
						return out
					}})
			}
		}
	}
	return cs
}

func TestAVX2AndFMA(t *testing.T) {
	r := rand.New(rand.NewSource(1))
	var cases []execCase
	var want []map[int][]byte
	rb := uint64(execMemBase)
	for _, c := range avCases() {
		for _, mem := range []bool{false, true} {
			var in1, in2 []byte
			switch c.float {
			case 0:
				in1, in2 = randBytes(r, 32), randBytes(r, 32)
				// Shift counts: mix in-range, boundary and huge values.
				if len(c.name) > 5 && c.name[:5] == "vpsll" || len(c.name) > 5 && (c.name[:5] == "vpsrl" || c.name[:5] == "vpsra") {
					counts := []uint32{0, 1, 31, 32, 33, 63, 64, 0x80000000, 0x100, 5, 17, 0xffffffff}
					for i := 0; i < 8; i++ {
						put32(in2, i, counts[r.Intn(len(counts))])
					}
				}
				if c.name == "vpermd" || c.name == "vpermps" {
					for i := 0; i < 8; i++ {
						put32(in1, i, uint32(r.Intn(16)))
					}
				}
			default:
				in1, in2 = floatLanes(r, c.float), floatLanes(r, c.float)
			}
			init := floatLanes(r, 4)
			if c.float == 0 {
				init = randBytes(r, 32)
			}
			name := c.name
			if mem {
				name += " mem"
			}
			code := []string{"c5fe6f0b", "c5fe6f5320", "c5fe6f4340", c.code(mem), "c5fe7f4360"}
			cases = append(cases, execCase{name: name, code: code, ymm: true,
				regs: map[x86asm.Reg]uint64{x86asm.RBX: rb + avIn1}, mem: map[int][]byte{avIn1: in1, avIn2: in2, avInit: init}})
			want = append(want, map[int][]byte{avOut: c.ref(in1, in2, init)})
		}
	}
	runExecExpect(t, cases, want)
}

// dst aliasing a source: VPERMQ ymm0, ymm0, imm and VPERMD ymm0, ymm0(idx), ymm2, and a variable
// shift whose count register is the destination.
func TestAVX2DestAliasesSource(t *testing.T) {
	r := rand.New(rand.NewSource(7))
	init, in2 := randBytes(r, 32), randBytes(r, 32)
	for i := 0; i < 8; i++ {
		put32(init, i, uint32(r.Intn(8)))
	}
	pre := []string{"c5fe6f5320", "c5fe6f4340"}
	post := "c5fe7f4360"
	perm := make([]byte, 32)
	for k, s := range []int{3, 2, 1, 0} {
		put64(perm, k, u64(init, s))
	}
	pd := make([]byte, 32)
	for i := 0; i < 8; i++ {
		put32(pd, i, u32(in2, int(u32(init, i))))
	}
	sh := make([]byte, 32)
	for i := 0; i < 8; i++ {
		c := u32(init, i)
		put32(sh, i, u32(in2, i)<<c)
	}
	cases := []execCase{
		{name: "vpermq ymm0,ymm0", code: append(append([]string{}, pre...), "c4e3fd00c01b", post)},
		{name: "vpermd ymm0,ymm0,ymm2", code: append(append([]string{}, pre...), "c4e27d36c2", post)},
		{name: "vpsllvd ymm0,ymm2,ymm0", code: append(append([]string{}, pre...), "c4e26d47c0", post)},
	}
	want := []map[int][]byte{{avOut: perm}, {avOut: pd}, {avOut: sh}}
	for i := range cases {
		cases[i].ymm = true
		cases[i].regs = map[x86asm.Reg]uint64{x86asm.RBX: uint64(execMemBase) + avIn1}
		cases[i].mem = map[int][]byte{avIn2: in2, avInit: init}
	}
	runExecExpect(t, cases, want)
}
