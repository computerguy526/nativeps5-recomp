package lifter

import (
	"fmt"
	"math/bits"
	"math/rand"
	"strings"
	"testing"

	"golang.org/x/arch/x86/x86asm"
)

// Native tests (lifted arm64 side only; no x86 is executed) for the instruction-coverage
// additions of m2-gamelift: PADDUSW/PMULHUW (and the PSUBSW/PSUBUSW siblings), SHA-NI and the
// AVX2 gathers. Expected values are Go models of the Intel SDM pseudocode; the FEX unit tests
// (VEX/vgather_*, VEX/vpgather_*, H0F38/sha*, H0F3A/sha1rnds4, SSELanePreservation/sha*,
// TwoByte/0F_DD, 0F_E4, OpSize/66_DD, 66_E4) cover the same forms through tools/fex_oracle.sh.

const (
	sgIn1  = 0x100
	sgIn2  = 0x110
	sgKey  = 0x120
	sgOut  = 0x140
	sgBase = 0x100 // rbx = execMemBase + sgBase
)

func sgRegs() map[x86asm.Reg]uint64 { return map[x86asm.Reg]uint64{x86asm.RBX: execMemBase + sgBase} }

func lane16(b []byte, i int) uint16 { return uint16(b[2*i]) | uint16(b[2*i+1])<<8 }
func setLane16(b []byte, i int, v uint16) {
	b[2*i], b[2*i+1] = byte(v), byte(v>>8)
}

func satU16(v int) uint16 {
	if v < 0 {
		return 0
	}
	if v > 0xffff {
		return 0xffff
	}
	return uint16(v)
}
func satS16(v int) uint16 {
	if v < -32768 {
		v = -32768
	}
	if v > 32767 {
		v = 32767
	}
	return uint16(int16(v))
}

// TestSSEWordSaturatingAndMulhi covers PADDUSW, PSUBUSW, PSUBSW, PMULHUW and PMULHW in the SSE
// (register and memory) and MMX forms.
func TestSSEWordSaturatingAndMulhi(t *testing.T) {
	type op struct {
		name string
		opc  string // opcode bytes after 0f
		f    func(a, b uint16) uint16
	}
	ops := []op{
		{"paddusw", "dd", func(a, b uint16) uint16 { return satU16(int(a) + int(b)) }},
		{"psubusw", "d9", func(a, b uint16) uint16 { return satU16(int(a) - int(b)) }},
		{"psubsw", "e9", func(a, b uint16) uint16 { return satS16(int(int16(a)) - int(int16(b))) }},
		{"pmulhuw", "e4", func(a, b uint16) uint16 { return uint16(uint32(a) * uint32(b) >> 16) }},
		{"pmulhw", "e5", func(a, b uint16) uint16 { return uint16(uint32(int32(int16(a))*int32(int16(b))) >> 16) }},
	}
	r := rand.New(rand.NewSource(11))
	var cases []execCase
	var want []map[int][]byte
	for _, o := range ops {
		for _, form := range []string{"xmm", "xmm mem", "mm", "mm mem"} {
			a, b := randBytes(r, 16), randBytes(r, 16)
			// Edge lanes: saturation and sign boundaries.
			edge := []uint16{0, 1, 0x7fff, 0x8000, 0xffff, 0xfffe, 0x8001, 0x4000}
			for i := 0; i < 8; i++ {
				setLane16(a, i, edge[r.Intn(len(edge))])
				if i%2 == 0 {
					setLane16(b, i, edge[r.Intn(len(edge))])
				}
			}
			n := 16
			if form[0] == 'm' {
				n = 8
			}
			exp := make([]byte, 16)
			for i := 0; i < n/2; i++ {
				setLane16(exp, i, o.f(lane16(a, i), lane16(b, i)))
			}
			var code []string
			switch form {
			case "xmm":
				code = []string{"f30f6f0b", "f30f6f5310", "660f" + o.opc + "ca", "f30f7f4b40"}
			case "xmm mem":
				code = []string{"f30f6f0b", "660f" + o.opc + "4b10", "f30f7f4b40"}
			case "mm":
				code = []string{"0f6f0b", "0f6f5310", "0f" + o.opc + "ca", "0f7f4b40"}
			case "mm mem":
				code = []string{"0f6f0b", "0f" + o.opc + "4b10", "0f7f4b40"}
			}
			if form[0] == 'm' {
				exp = exp[:8]
			}
			// Without the final store of the unused half, the output window keeps zeros.
			cases = append(cases, execCase{name: o.name + " " + form, code: code, regs: sgRegs(),
				mem: map[int][]byte{sgIn1: a, sgIn2: b}})
			full := make([]byte, 16)
			copy(full, exp)
			want = append(want, map[int][]byte{sgOut: full[:len(exp)]})
		}
	}
	runExecExpect(t, cases, want)
}

// ---- SHA-NI reference models (Intel SDM pseudocode) ----

func rol(x uint32, n int) uint32 { return bits.RotateLeft32(x, n) }
func ror(x uint32, n int) uint32 { return bits.RotateLeft32(x, -n) }

func d32(b []byte, i int) uint32 { return u32(b, i) }

func refSha1Nexte(dst, src []byte) []byte {
	out := append([]byte(nil), src...)
	put32(out, 3, d32(src, 3)+rol(d32(dst, 3), 30))
	return out
}

func refSha1Msg1(dst, src []byte) []byte {
	w0, w1, w2, w3 := d32(dst, 3), d32(dst, 2), d32(dst, 1), d32(dst, 0)
	w4, w5 := d32(src, 3), d32(src, 2)
	out := make([]byte, 16)
	put32(out, 3, w2^w0)
	put32(out, 2, w3^w1)
	put32(out, 1, w4^w2)
	put32(out, 0, w5^w3)
	return out
}

func refSha1Msg2(dst, src []byte) []byte {
	w0, w1, w2, w3 := d32(dst, 3), d32(dst, 2), d32(dst, 1), d32(dst, 0)
	w13, w14, w15 := d32(src, 2), d32(src, 1), d32(src, 0)
	w16 := rol(w0^w13, 1)
	w17 := rol(w1^w14, 1)
	w18 := rol(w2^w15, 1)
	w19 := rol(w3^w16, 1)
	out := make([]byte, 16)
	put32(out, 3, w16)
	put32(out, 2, w17)
	put32(out, 1, w18)
	put32(out, 0, w19)
	return out
}

func refSha1Rnds4(dst, src []byte, imm int) []byte {
	a, b, c, d := d32(dst, 3), d32(dst, 2), d32(dst, 1), d32(dst, 0)
	w := [4]uint32{d32(src, 3), d32(src, 2), d32(src, 1), d32(src, 0)}
	k := [4]uint32{0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xCA62C1D6}[imm]
	e := uint32(0) // E is already folded into W0
	for i := 0; i < 4; i++ {
		var f uint32
		switch imm {
		case 0:
			f = (b & c) ^ (^b & d)
		case 2:
			f = (b & c) ^ (b & d) ^ (c & d)
		default:
			f = b ^ c ^ d
		}
		tmp := rol(a, 5) + f + e + w[i] + k
		e, d, c, b, a = d, c, rol(b, 30), a, tmp
	}
	out := make([]byte, 16)
	put32(out, 3, a)
	put32(out, 2, b)
	put32(out, 1, c)
	put32(out, 0, d)
	return out
}

func sig0(x uint32) uint32 { return ror(x, 7) ^ ror(x, 18) ^ x>>3 }
func sig1(x uint32) uint32 { return ror(x, 17) ^ ror(x, 19) ^ x>>10 }

func refSha256Msg1(dst, src []byte) []byte {
	out := make([]byte, 16)
	for i := 0; i < 3; i++ {
		put32(out, i, d32(dst, i)+sig0(d32(dst, i+1)))
	}
	put32(out, 3, d32(dst, 3)+sig0(d32(src, 0)))
	return out
}

func refSha256Msg2(dst, src []byte) []byte {
	w14, w15 := d32(src, 2), d32(src, 3)
	w16 := d32(dst, 0) + sig1(w14)
	w17 := d32(dst, 1) + sig1(w15)
	w18 := d32(dst, 2) + sig1(w16)
	w19 := d32(dst, 3) + sig1(w17)
	out := make([]byte, 16)
	put32(out, 0, w16)
	put32(out, 1, w17)
	put32(out, 2, w18)
	put32(out, 3, w19)
	return out
}

// refSha256Rnds2: dst = CDGH (dwords 3..0 = C,D,G,H), src = ABEF, xmm0 = WK0, WK1 in dwords 0,1.
func refSha256Rnds2(dst, src, key []byte) []byte {
	a, b, e, f := d32(src, 3), d32(src, 2), d32(src, 1), d32(src, 0)
	c, d, g, h := d32(dst, 3), d32(dst, 2), d32(dst, 1), d32(dst, 0)
	for i := 0; i < 2; i++ {
		ch := (e & f) ^ (^e & g)
		maj := (a & b) ^ (a & c) ^ (b & c)
		s1 := ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)
		s0 := ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)
		t1 := h + s1 + ch + d32(key, i)
		t2 := s0 + maj
		h, g, f, e, d, c, b, a = g, f, e, d+t1, c, b, a, t1+t2
	}
	out := make([]byte, 16)
	put32(out, 3, a)
	put32(out, 2, b)
	put32(out, 1, e)
	put32(out, 0, f)
	_ = c
	_ = d
	_ = g
	_ = h
	return out
}

func TestShaNI(t *testing.T) {
	r := rand.New(rand.NewSource(5))
	type sh struct {
		name string
		hex  string // opcode bytes with modrm omitted
		imm  int    // -1: none
		ref  func(d, s, k []byte) []byte
	}
	shs := []sh{
		{"sha1nexte", "0f38c8", -1, func(d, s, k []byte) []byte { return refSha1Nexte(d, s) }},
		{"sha1msg1", "0f38c9", -1, func(d, s, k []byte) []byte { return refSha1Msg1(d, s) }},
		{"sha1msg2", "0f38ca", -1, func(d, s, k []byte) []byte { return refSha1Msg2(d, s) }},
		{"sha256rnds2", "0f38cb", -1, refSha256Rnds2},
		{"sha256msg1", "0f38cc", -1, func(d, s, k []byte) []byte { return refSha256Msg1(d, s) }},
		{"sha256msg2", "0f38cd", -1, func(d, s, k []byte) []byte { return refSha256Msg2(d, s) }},
	}
	for imm := 0; imm < 4; imm++ {
		imm := imm
		shs = append(shs, sh{fmt.Sprintf("sha1rnds4 %d", imm), "0f3acc", imm,
			func(d, s, k []byte) []byte { return refSha1Rnds4(d, s, imm) }})
	}
	var cases []execCase
	var want []map[int][]byte
	for _, s := range shs {
		for _, mem := range []bool{false, true} {
			for rep := 0; rep < 3; rep++ {
				d, src, key := randBytes(r, 16), randBytes(r, 16), randBytes(r, 16)
				tail := ""
				if s.imm >= 0 {
					tail = fmt.Sprintf("%02x", s.imm)
				}
				op := "66" // none: legacy SHA ops have no mandatory prefix
				_ = op
				modrm := "ca"
				if mem {
					modrm = "4b10"
				}
				code := []string{"f30f6f0b", "f30f6f4320"} // xmm1 = dst, xmm0 = key
				if !mem {
					code = append(code, "f30f6f5310")
				}
				code = append(code, s.hex+modrm+tail, "f30f7f4b40")
				name := s.name
				if mem {
					name += " mem"
				}
				cases = append(cases, execCase{name: name, code: code, regs: sgRegs(),
					mem: map[int][]byte{sgIn1: d, sgIn2: src, sgKey: key}})
				want = append(want, map[int][]byte{sgOut: s.ref(d, src, key)})
			}
		}
	}
	runExecExpect(t, cases, want)
}

// ---- gathers ----

const (
	gaInit  = 0x100 // dst initial value   (rbx + 0x00)
	gaIdx   = 0x120 // index vector        (rbx + 0x20)
	gaMask  = 0x140 // mask vector         (rbx + 0x40)
	gaOutD  = 0x160 // stored dst          (rbx + 0x60)
	gaOutK  = 0x180 // stored mask         (rbx + 0x80)
	gaTable = 0x600 // rsi
)

func TestAVX2Gathers(t *testing.T) {
	r := rand.New(rand.NewSource(3))
	table := randBytes(r, 0xa00)
	forms := []struct {
		name        string
		opc         byte
		w           int
		idxB, dataB int
	}{
		{"vgatherdps", 0x92, 0, 4, 4}, {"vpgatherdd", 0x90, 0, 4, 4},
		{"vgatherdpd", 0x92, 1, 4, 8}, {"vpgatherdq", 0x90, 1, 4, 8},
		{"vgatherqps", 0x93, 0, 8, 4}, {"vpgatherqd", 0x91, 0, 8, 4},
		{"vgatherqpd", 0x93, 1, 8, 8}, {"vpgatherqq", 0x91, 1, 8, 8},
	}
	var cases []execCase
	var want []map[int][]byte
	for _, f := range forms {
		for l := 0; l < 2; l++ {
			vec := 16 << l
			maxB := f.idxB
			if f.dataB > maxB {
				maxB = f.dataB
			}
			n := vec / maxB
			for _, scale := range []int{1, 2, 4, 8} {
				for pat := 0; pat < 4; pat++ { // all set, none set, two random, alternating
					init, idx, mask := randBytes(r, 32), make([]byte, 32), randBytes(r, 32)
					disp := int8(r.Intn(64) - 8)
					lanes := 32 / f.dataB
					on := make([]bool, lanes)
					for i := range on {
						switch pat {
						case 0:
							on[i] = true
						case 1:
							on[i] = false
						case 2:
							on[i] = r.Intn(2) == 0
						default:
							on[i] = i%2 == 0
						}
						// MSB of each mask element; other bits random.
						if f.dataB == 4 {
							v := u32(mask, i)&0x7fffffff | uint32(b2i(on[i]))<<31
							put32(mask, i, v)
						} else {
							v := u64(mask, i)&0x7fffffffffffffff | uint64(b2i(on[i]))<<63
							put64(mask, i, v)
						}
					}
					exp := append([]byte(nil), init...)
					for i := 0; i < 32/f.dataB; i++ {
						if i >= n {
							for j := 0; j < f.dataB; j++ {
								exp[i*f.dataB+j] = 0
							}
							continue
						}
						// Index element i (sign-extended); masked-off lanes get an unmapped address.
						var ix int64
						if f.idxB == 4 {
							ix = int64(int32(r.Intn(100) - 16))
							if !on[i] {
								ix = 0x40000000
							}
							put32(idx, i, uint32(ix))
						} else {
							ix = int64(r.Intn(100) - 16)
							if !on[i] {
								ix = 0x7fff_0000_0000
							}
							put64(idx, i, uint64(ix))
						}
						if !on[i] {
							continue
						}
						a := int(ix)*scale + int(disp) + 0x200 // table offset; rsi = gaTable region start
						copy(exp[i*f.dataB:(i+1)*f.dataB], table[a:a+f.dataB])
					}
					for i := n * f.dataB; i < 32; i++ {
						exp[i] = 0
					}
					sc := map[int]byte{1: 0, 2: 1, 4: 2, 8: 3}[scale]
					b2 := byte(f.w<<7 | 0x60 | l<<2 | 1)
					gather := fmt.Sprintf("c4e2%02x%02x4c%02x%02x", b2, f.opc, sc<<6|2<<3|6, byte(disp))
					code := []string{"c5fe6f4b00", "c5fe6f5320", "c5fe6f5b40", gather, "c5fe7f4b60", "c5fe7f9b80000000"}
					// rsi = base of the table window + 0x200 (so negative scaled indices stay inside).
					cases = append(cases, execCase{
						name: fmt.Sprintf("%s l%d scale%d pat%d disp%d", f.name, l, scale, pat, disp),
						code: code, ymm: true,
						regs: map[x86asm.Reg]uint64{x86asm.RBX: execMemBase + 0x100, x86asm.RSI: execMemBase + gaTable + 0x200},
						mem:  map[int][]byte{gaInit: init, gaIdx: idx, gaMask: mask, gaTable: table},
					})
					want = append(want, map[int][]byte{gaOutD: exp, gaOutK: make([]byte, 32)})
				}
			}
		}
	}
	runExecExpect(t, cases, want)
}

// The scaled index wraps at 64 bits (FEX VEX/vgather_*_overflow): idx*scale overflows and the
// base brings the sum back into the window.
func TestAVX2GatherIndexOverflow(t *testing.T) {
	r := rand.New(rand.NewSource(9))
	table := randBytes(r, 0x400)
	idx := make([]byte, 32)
	put64(idx, 0, 0x8000000000000000+5) // *2 = +10 (mod 2^64)
	put64(idx, 1, 0x8000000000000000+9) // *2 = +18
	mask := make([]byte, 32)
	put64(mask, 0, 1<<63)
	put64(mask, 1, 1<<63)
	exp := make([]byte, 32)
	copy(exp[0:8], table[0x100+10:])
	copy(exp[8:16], table[0x100+18:])
	// vpgatherqq xmm1, [rsi + xmm2*2], xmm3
	gather := fmt.Sprintf("c4e2e1914c%02x00", 1<<6|2<<3|6)
	runExecExpect(t, []execCase{{
		name: "vpgatherqq index overflow",
		code: []string{"c5fe6f4b00", "c5fe6f5320", "c5fe6f5b40", gather, "c5fe7f4b60", "c5fe7f9b80000000"}, ymm: true,
		regs: map[x86asm.Reg]uint64{x86asm.RBX: execMemBase + 0x100, x86asm.RSI: execMemBase + gaTable + 0x100},
		mem:  map[int][]byte{gaIdx: idx, gaMask: mask, gaTable: table, gaInit: randBytes(r, 32)},
	}}, []map[int][]byte{{gaOutD: exp, gaOutK: make([]byte, 32)}})
}

// The VSIB index of a gather is a read of that register (PPSA30552 fn_0x331c760 intro crash):
// "vpermd" before "vpgatherqd xmm, [base+ymm15], xmm", the index not read afterwards. Lane
// liveness missed the index, so the upper half of ymm15 counted as dead after the vpermd and its
// host register (v31) became one of the vpermd lowering's scratch registers; index elements 2
// and 3 were then garbage (0x0101010101010101 in the title: a wild load).
func TestVecLiveGatherIndexNotScratch(t *testing.T) {
	code := []string{
		"c57e6f7b20",       // vmovdqu ymm15, [rbx+0x20]  index
		"c5fe6f5b00",       // vmovdqu ymm3, [rbx]        dst initial value
		"c5fe6f6340",       // vmovdqu ymm4, [rbx+0x40]   mask
		"c5fe6f8ba0000000", // vmovdqu ymm1, [rbx+0xa0]   vpermd indices
		"c5fe6f93e0000000", // vmovdqu ymm2, [rbx+0xe0]   vpermd source
		"c4e27536c2",       // vpermd ymm0, ymm1, ymm2
		"c4a25d911c3e",     // vpgatherqd xmm3, [rsi+ymm15*1], xmm4
		"c5fe7f5b60",       // vmovdqu [rbx+0x60], ymm3
		"c5fe7fa380000000", // vmovdqu [rbx+0x80], ymm4
		"c5fe7f83c0000000", // vmovdqu [rbx+0xc0], ymm0
		"c5f877",           // vzeroupper: no upper half is live after the stores
	}
	// The lowering of the vpermd must leave v31 (the upper half of ymm15) alone, or put it back.
	ls := liftVec(t, "abi", false, code...)
	uses, saved := false, false
	for _, ln := range ls[5] {
		c := vlCode(ln)
		uses = uses || vlRefs(c, 31)
		saved = saved || ((strings.HasPrefix(c, "stp q") || strings.HasPrefix(c, "str q")) && vlRefs(c, 31) && strings.Contains(c, "[sp, #-"))
	}
	if uses && !saved {
		t.Fatalf("vpermd lowering borrows v31, the live upper half of the gather index ymm15, without saving it:\n%s", strings.Join(ls[5], "\n"))
	}

	r := rand.New(rand.NewSource(11))
	table := randBytes(r, 0x400)
	idx, mask, exp := make([]byte, 32), make([]byte, 32), make([]byte, 32)
	for i := 0; i < 4; i++ {
		ix := r.Intn(0x200)
		put64(idx, i, uint64(ix))
		put32(mask, i, 1<<31)
		copy(exp[4*i:4*i+4], table[0x100+ix:])
	}
	pidx, psrc, pexp := randBytes(r, 32), randBytes(r, 32), make([]byte, 32)
	for i := 0; i < 8; i++ {
		put32(pexp, i, u32(psrc, int(u32(pidx, i)&7)))
	}
	for _, vec := range []string{"abi", "strict"} {
		runExecExpect(t, []execCase{{
			name: "vpermd then vpgatherqd ymm15 index, " + vec,
			code: code, ymm: true, vec: vec, noFlags: true,
			regs: map[x86asm.Reg]uint64{x86asm.RBX: execMemBase + 0x100, x86asm.RSI: execMemBase + gaTable + 0x100},
			mem:  map[int][]byte{gaInit: randBytes(r, 32), gaIdx: idx, gaMask: mask, gaTable: table, 0x1a0: pidx, 0x1e0: psrc},
		}}, []map[int][]byte{{gaOutD: exp, gaOutK: make([]byte, 32), 0x1c0: pexp}})
	}
}
