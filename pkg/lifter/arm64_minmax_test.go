package lifter

import (
	"encoding/binary"
	"fmt"
	"math"
	"testing"

	"golang.org/x/arch/x86/x86asm"
)

// x86 MINPS/MAXPS/MINSS/MAXSS (and the PD/SD and VEX forms): the result lane is
//
//	min(a, b) = a < b ? a : b        max(a, b) = a > b ? a : b
//
// with a the destination (first source) and b the second source, so the SECOND operand comes
// back, bit for bit, when either input is a NaN (quiet or signalling) and when both are zero
// (+0/-0 in either order). ARM FMIN/FMAX return the NaN (quieted) and order -0 < +0; FMINNM/FMAXNM
// return the number when one input is a quiet NaN. This test runs the lifted code on the host
// over every pair of a set of boundary values in every lane and compares with that rule
// computed here (nothing x86 is executed; tools/fex_oracle.sh covers the FEX tests of the same
// instructions: FEX_bugs/MAXPS_NaN, TwoByte/0F_5D, 0F_5F, REP/F3_5D, F3_5F, VEX/v{min,max}{ps,ss}).

var (
	mmVals32 = []uint32{
		0x00000000, 0x80000000, // +0, -0
		0x3f800000, 0xbf800000, 0x40000000, 0xc0000000, 0x3f000000, // 1, -1, 2, -2, 0.5
		0x7f800000, 0xff800000, // +inf, -inf
		0x7fc00000, 0xffc00000, 0x7fa00001, 0xffa00001, 0x7fc12345, // qNaN, -qNaN, sNaN, -sNaN, qNaN payload
		0x00000001, 0x80000001, 0x007fffff, 0x807fffff, 0x00800000, // denormals, smallest normal
		0x7f7fffff, 0xff7fffff, // +-FLT_MAX
		0x40490fdb,
	}
	mmVals64 = []uint64{
		0x0000000000000000, 0x8000000000000000,
		0x3ff0000000000000, 0xbff0000000000000, 0x4000000000000000, 0xc000000000000000, 0x3fe0000000000000,
		0x7ff0000000000000, 0xfff0000000000000,
		0x7ff8000000000000, 0xfff8000000000000, 0x7ff4000000000001, 0xfff4000000000001, 0x7ff8123456789abc,
		0x0000000000000001, 0x8000000000000001, 0x000fffffffffffff, 0x800fffffffffffff, 0x0010000000000000,
		0x7fefffffffffffff, 0xffefffffffffffff,
		0x400921fb54442d18,
	}
)

// x86 rule on raw bits; the comparison is IEEE (NaN compares false, +0 == -0).
func mmRule32(isMax bool, a, b uint32) uint32 {
	fa, fb := math.Float32frombits(a), math.Float32frombits(b)
	if isMax && fa > fb || !isMax && fa < fb {
		return a
	}
	return b
}

func mmRule64(isMax bool, a, b uint64) uint64 {
	fa, fb := math.Float64frombits(a), math.Float64frombits(b)
	if isMax && fa > fb || !isMax && fa < fb {
		return a
	}
	return b
}

// mmForm is one encoding of a min/max instruction family.
type mmForm struct {
	name   string
	isMax  bool
	double bool // PD/SD element
	scalar bool
	vex    int  // 0: legacy SSE (dest = src1), 128 or 256: VEX with a third operand
	mem    bool // the second source is a memory operand
}

// code returns the hex of the instructions of test i: load a, load b (or use memory), the
// operation, store the result. Registers: xmm0 = a (dest), xmm1 = b (SSE) or src1 = xmm1, src2 = xmm2 (VEX).
func (f mmForm) code(aOff, bOff, outOff int) []string {
	d := func(o int) string { // disp32 of [rdi+o]
		var b [4]byte
		binary.LittleEndian.PutUint32(b[:], uint32(o))
		return fmt.Sprintf("%02x%02x%02x%02x", b[0], b[1], b[2], b[3])
	}
	op := "5d"
	if f.isMax {
		op = "5f"
	}
	pfx := ""
	switch {
	case f.scalar && !f.double:
		pfx = "f3"
	case f.scalar && f.double:
		pfx = "f2"
	case f.double:
		pfx = "66"
	}
	if f.vex == 0 {
		out := []string{"0f1087" + d(aOff)} // movups xmm0, [rdi+a]: the whole destination
		if f.mem {
			out = append(out, pfx+"0f"+op+"87"+d(bOff)) // op xmm0, [rdi+bOff]
		} else {
			out = append(out, "0f108f"+d(bOff), pfx+"0f"+op+"c1") // xmm1 = b; op xmm0, xmm1
		}
		return append(out, "0f1187"+d(outOff))
	}
	// VEX: vmovups load of a into xmm1 (ymm1), op xmm0, xmm1, xmm2/mem, store xmm0 (ymm0).
	var pp byte
	switch {
	case f.scalar && !f.double:
		pp = 2
	case f.scalar && f.double:
		pp = 3
	case f.double:
		pp = 1
	}
	l := byte(0)
	if f.vex == 256 {
		l = 1
	}
	// C5: R=1, vvvv = ~1 (xmm1) = 1110, L, pp
	b1 := 0x80 | (^byte(1)&0xf)<<3 | l<<2 | pp
	var out []string
	if f.vex == 256 {
		out = append(out, "c5fc10"+"8f"+d(aOff)) // vmovups ymm1, [rdi+a]
		out = append(out, "c5fc10"+"97"+d(bOff)) // vmovups ymm2, [rdi+b]
	} else {
		out = append(out, "c5f810"+"8f"+d(aOff))
		out = append(out, "c5f810"+"97"+d(bOff))
	}
	if f.mem {
		out = append(out, fmt.Sprintf("c5%02x%s87%s", b1, op, d(bOff))) // op xmm0, xmm1, [rdi+b]
	} else {
		out = append(out, fmt.Sprintf("c5%02x%sc2", b1, op)) // op xmm0, xmm1, xmm2
	}
	if f.vex == 256 {
		return append(out, "c5fc1187"+d(outOff))
	}
	return append(out, "c5f81187"+d(outOff))
}

// expect computes the stored bytes for inputs a, b (32 bytes each; only 16 or 32 used by the
// form) and the destination's initial content (only the SSE scalar forms keep it: here xmm0 = a).
func (f mmForm) expect(a, b []byte) []byte {
	n := 16
	if f.vex == 256 {
		n = 32
	}
	out := make([]byte, n)
	copy(out, a[:n]) // dest = a; the unchanged lanes (scalar) keep a's upper elements
	if f.vex != 0 && f.scalar {
		copy(out, a[:16]) // VEX scalar: bits 127:32 (63:0 for SD) come from src1 = a
		for i := 16; i < n; i++ {
			out[i] = 0
		}
	}
	elem := 4
	if f.double {
		elem = 8
	}
	lanes := n / elem
	if f.scalar {
		lanes = 1
	}
	for k := 0; k < lanes; k++ {
		if f.double {
			r := mmRule64(f.isMax, binary.LittleEndian.Uint64(a[8*k:]), binary.LittleEndian.Uint64(b[8*k:]))
			binary.LittleEndian.PutUint64(out[8*k:], r)
		} else {
			r := mmRule32(f.isMax, binary.LittleEndian.Uint32(a[4*k:]), binary.LittleEndian.Uint32(b[4*k:]))
			binary.LittleEndian.PutUint32(out[4*k:], r)
		}
	}
	return out
}

func mmForms() []mmForm {
	var fs []mmForm
	for _, isMax := range []bool{false, true} {
		for _, double := range []bool{false, true} {
			for _, scalar := range []bool{false, true} {
				for _, vex := range []int{0, 128, 256} {
					if vex == 256 && scalar {
						continue
					}
					for _, mem := range []bool{false, true} {
						n := map[bool]string{false: "min", true: "max"}[isMax]
						n += map[bool]string{false: "p", true: "s"}[scalar] + map[bool]string{false: "s", true: "d"}[double]
						n = map[int]string{0: "", 128: "v", 256: "v"}[vex] + n
						if vex == 256 {
							n += "_ymm"
						}
						if mem {
							n += "_mem"
						}
						fs = append(fs, mmForm{name: n, isMax: isMax, double: double, scalar: scalar, vex: vex, mem: mem})
					}
				}
			}
		}
	}
	return fs
}

// mmCases builds the execution cases of one form: every ordered pair of the value set appears in
// every lane (lane l of test (i, j) holds values (i+l, j+2l) mod n, a bijection of the pairs), in
// batches of 64 tests (a: 0..0x800, b: 0x800..0x1000 area is used as a/b of 32 bytes each).
func mmCases(f mmForm) ([]execCase, []map[int][]byte) {
	w := 16
	if f.vex == 256 {
		w = 32
	}
	const batch = 64
	vals := len(mmVals32)
	if f.double {
		vals = len(mmVals64)
	}
	type pair struct{ i, j int }
	var pairs []pair
	for i := 0; i < vals; i++ {
		for j := 0; j < vals; j++ {
			pairs = append(pairs, pair{i, j})
		}
	}
	var cases []execCase
	var want []map[int][]byte
	for s := 0; s < len(pairs); s += batch {
		e := min(s+batch, len(pairs))
		var code []string
		mem := map[int][]byte{}
		wmem := map[int][]byte{}
		for t, p := range pairs[s:e] {
			aOff, bOff, outOff := t*w, 0x800+t*w, 0x1000+t*w
			a, b := make([]byte, w), make([]byte, w)
			if f.double {
				for l := 0; l < w/8; l++ {
					binary.LittleEndian.PutUint64(a[8*l:], mmVals64[(p.i+l)%vals])
					binary.LittleEndian.PutUint64(b[8*l:], mmVals64[(p.j+2*l)%vals])
				}
			} else {
				for l := 0; l < w/4; l++ {
					binary.LittleEndian.PutUint32(a[4*l:], mmVals32[(p.i+l)%vals])
					binary.LittleEndian.PutUint32(b[4*l:], mmVals32[(p.j+2*l)%vals])
				}
			}
			mem[aOff], mem[bOff] = a, b
			wmem[outOff] = f.expect(a, b)
			code = append(code, f.code(aOff, bOff, outOff)...)
		}
		cases = append(cases, execCase{name: fmt.Sprintf("%s[%d:%d]", f.name, s, e), code: code,
			regs: map[x86asm.Reg]uint64{x86asm.RDI: execMemBase}, mem: mem, noFlags: true, ymm: f.vex == 256})
		want = append(want, wmem)
	}
	return cases, want
}

func TestMinMaxX86Semantics(t *testing.T) {
	for _, f := range mmForms() {
		cases, want := mmCases(f)
		t.Run(f.name, func(t *testing.T) { runExecExpect(t, cases, want) })
	}
}

// The reference itself: the four rules the instruction manual states.
func TestMinMaxReferenceRule(t *testing.T) {
	nan, one, pz, nz := uint32(0x7fc00000), uint32(0x3f800000), uint32(0), uint32(0x80000000)
	for _, c := range []struct {
		isMax   bool
		a, b, r uint32
	}{
		{false, nan, one, one}, {false, one, nan, nan}, {true, nan, one, one}, {true, one, nan, nan},
		{false, pz, nz, nz}, {false, nz, pz, pz}, {true, pz, nz, nz}, {true, nz, pz, pz},
		{false, one, 0x40000000, one}, {true, one, 0x40000000, 0x40000000},
	} {
		if got := mmRule32(c.isMax, c.a, c.b); got != c.r {
			t.Errorf("rule(max=%v, %#x, %#x) = %#x, want %#x", c.isMax, c.a, c.b, got, c.r)
		}
	}
}
