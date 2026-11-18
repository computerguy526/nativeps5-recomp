package lifter

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"golang.org/x/arch/x86/x86asm"
	"math"
	"math/rand"
	"testing"
)

// Scalar (and a few merge-type) VEX three-operand forms keep lanes 1..n of src1 and write lane 0 from the
// operation on src2. A lowering that copies src1 into d BEFORE it has read src2 returns garbage when d is
// src2 and differs from src1 (vrcpss xmm1,xmm2,xmm1). Every form is run with d = xmm3 (no alias, reference),
// d = xmm2 (= src2 / rm) and d = xmm1 (= src1 / vvvv); the lifted results must be identical. FMA forms get
// the reference destination preloaded with the aliased register (their destination is also an input).

func vexRRR(pp, mm, w, op, d, v, rm int) string {
	return fmt.Sprintf("c4%02x%02x%02x%02x", 0xe0|mm, w<<7|((^v&15)<<3)|pp, op, 0xc0|d<<3|rm)
}

func vexRRRi(pp, mm, w, op, d, v, rm, imm int) string {
	return vexRRR(pp, mm, w, op, d, v, rm) + fmt.Sprintf("%02x", imm)
}

type aliasForm struct {
	name string
	enc  func(d, v, rm int) string
	fma  bool
}

func aliasForms() []aliasForm {
	var fs []aliasForm
	add := func(name string, f func(d, v, rm int) string) { fs = append(fs, aliasForm{name: name, enc: f}) }
	for _, p := range []struct {
		n  string
		op int
	}{{"add", 0x58}, {"mul", 0x59}, {"sub", 0x5c}, {"min", 0x5d}, {"div", 0x5e}, {"max", 0x5f}, {"sqrt", 0x51}, {"rsqrt", 0x52}, {"rcp", 0x53}} {
		p := p
		add("v"+p.n+"ss", func(d, v, rm int) string { return vexRRR(2, 1, 0, p.op, d, v, rm) })
		if p.n != "rsqrt" && p.n != "rcp" {
			add("v"+p.n+"sd", func(d, v, rm int) string { return vexRRR(3, 1, 0, p.op, d, v, rm) })
		}
	}
	add("vcvtss2sd", func(d, v, rm int) string { return vexRRR(2, 1, 0, 0x5a, d, v, rm) })
	add("vcvtsd2ss", func(d, v, rm int) string { return vexRRR(3, 1, 0, 0x5a, d, v, rm) })
	for _, imm := range []int{0, 1, 2, 3, 4, 7} {
		imm := imm
		add(fmt.Sprintf("vcmpss/%d", imm), func(d, v, rm int) string { return vexRRRi(2, 1, 0, 0xc2, d, v, rm, imm) })
		add(fmt.Sprintf("vcmpsd/%d", imm), func(d, v, rm int) string { return vexRRRi(3, 1, 0, 0xc2, d, v, rm, imm) })
	}
	for _, imm := range []int{0, 1, 2, 3, 4} {
		imm := imm
		add(fmt.Sprintf("vroundss/%d", imm), func(d, v, rm int) string { return vexRRRi(1, 3, 0, 0x0a, d, v, rm, imm) })
		add(fmt.Sprintf("vroundsd/%d", imm), func(d, v, rm int) string { return vexRRRi(1, 3, 0, 0x0b, d, v, rm, imm) })
	}
	add("vmovss", func(d, v, rm int) string { return vexRRR(2, 1, 0, 0x10, d, v, rm) })
	add("vmovsd", func(d, v, rm int) string { return vexRRR(3, 1, 0, 0x10, d, v, rm) })
	add("vmovlhps", func(d, v, rm int) string { return vexRRR(0, 1, 0, 0x16, d, v, rm) })
	add("vmovhlps", func(d, v, rm int) string { return vexRRR(0, 1, 0, 0x12, d, v, rm) })
	for _, imm := range []int{0x00, 0x1d, 0x4a, 0x90, 0xe5} {
		imm := imm
		add(fmt.Sprintf("vinsertps/%#x", imm), func(d, v, rm int) string { return vexRRRi(1, 3, 0, 0x21, d, v, rm, imm) })
	}
	for _, o := range []struct {
		n  string
		op int
	}{{"fmadd", 0x98}, {"fmsub", 0x9a}, {"fnmadd", 0x9c}, {"fnmsub", 0x9e}} {
		for _, ord := range []int{0x00, 0x10, 0x20} { // 132, 213, 231
			for _, w := range []int{0, 1} {
				o, ord, w := o, ord, w
				suf := map[int]string{0: "ss", 1: "sd"}[w]
				name := fmt.Sprintf("v%s%d%s", o.n, map[int]int{0: 132, 0x10: 213, 0x20: 231}[ord], suf)
				fs = append(fs, aliasForm{name: name, fma: true, enc: func(d, v, rm int) string { return vexRRR(1, 2, w, o.op+ord+1, d, v, rm) }})
			}
		}
	}
	return fs
}

func TestExecScalarDstAliasesSrc2(t *testing.T) {
	forms := aliasForms()
	rng := rand.New(rand.NewSource(7))
	vals := []float32{1, -1, 2.5, 0.375, -7.75, 100, 1e-3, 3e20, 0, float32(math.Inf(1))}
	var cases []execCase
	var meta []string
	for _, f := range forms {
		for rep := 0; rep < 3; rep++ {
			for _, alias := range []int{2, 1} { // d == src2 (rm), d == src1 (vvvv)
				mem := make([]byte, 32)
				for i := 0; i < 8; i++ {
					var u uint32
					if i%2 == 0 || rep == 0 {
						u = math.Float32bits(vals[rng.Intn(len(vals))] * float32(1+rng.Intn(4)))
					} else {
						u = rng.Uint32()
					}
					if rep == 2 && i == 1 { // a double-precision-like pair: keep exponent sane
						u = 0x3ff00000 + uint32(rng.Intn(0x1000))
					}
					binary.LittleEndian.PutUint32(mem[i*4:], u)
				}
				code := []string{
					"c5f8100f", "c5f8105710", // vmovups xmm1,[rdi]; xmm2,[rdi+16]
					fmt.Sprintf("c5f828%02x", 0xd8|alias), // vmovaps xmm3, xmm<alias>
					f.enc(3, 1, 2),                        // reference: d = xmm3
					"c5f8115f40",                          // vmovups [rdi+0x40], xmm3
					"c5f8100f", "c5f8105710",              // reload
					f.enc(alias, 1, 2),                         // aliased
					fmt.Sprintf("c5f811%02x50", 0x47|alias<<3), // vmovups [rdi+0x50], xmm<alias>
				}
				cases = append(cases, execCase{name: fmt.Sprintf("%s d=xmm%d rep%d", f.name, alias, rep), code: code, regs: map[x86asm.Reg]uint64{x86asm.RDI: execMemBase}, mem: map[int][]byte{0: mem}, ymm: true})
				meta = append(meta, f.name)
			}
		}
	}
	execSkip(t)
	var out []byte
	execCapture = &out
	defer func() { execCapture = nil }()
	runExecExpect(t, cases, make([]map[int][]byte, len(cases)))
	rec := stSize + execMemSize
	bad := map[string]bool{}
	for i, c := range cases {
		m := out[i*rec+stSize:]
		if !bytes.Equal(m[0x40:0x50], m[0x50:0x60]) {
			t.Errorf("%s: aliased % x, reference % x", c.name, m[0x50:0x60], m[0x40:0x50])
			bad[meta[i]] = true
		}
	}
	t.Logf("%d forms, %d cases, %d forms with a d-aliasing mismatch", len(forms), len(cases), len(bad))
}
