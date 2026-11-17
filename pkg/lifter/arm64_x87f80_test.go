package lifter

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"strings"
	"testing"

	"ps4-recomp/pkg/disasm"

	"golang.org/x/arch/x86/x86asm"
)

// x87=f80: the instructions are lifted to calls of _recomp_x87 and run natively on the host against
// the real runtime (SoftFloat, nps5_x87.c). The expected values are 80-bit results derived by hand
// or with exact big-number arithmetic (see each case), and for the constants and special values the
// RegData of the FEX-Emu X87 unit tests (unittests/ASM/X87, MIT, see NOTICE) named in the case; the
// full FEX corpus runs in tools/fex_oracle.sh. No x86 code is executed: llvm-mc only assembles the
// instruction bytes that the lifter reads.

// TestX87KindsMatchHeader keeps the kind numbers of the lowering equal to nps5_x87.h.
func TestX87KindsMatchHeader(t *testing.T) {
	src, err := os.ReadFile(filepath.Join("..", "runtime", "core", "nps5_x87.h"))
	if err != nil {
		t.Fatal(err)
	}
	got := map[string]int{}
	for _, m := range regexp.MustCompile(`(?m)^#define (X87_(?:K|ARITH)_[A-Z0-9_]+) +(\d+)`).FindAllStringSubmatch(string(src), -1) {
		v, _ := strconv.Atoi(m[2])
		got[m[1]] = v
	}
	want := map[string]int{
		"X87_K_FLD_M": x87KFldM, "X87_K_FLD_ST": x87KFldSt, "X87_K_FLD_CONST": x87KFldConst, "X87_K_FILD": x87KFild,
		"X87_K_FST_M": x87KFstM, "X87_K_FST_ST": x87KFstSt, "X87_K_FIST": x87KFist, "X87_K_ARITH_ST": x87KArithSt,
		"X87_K_ARITH_M": x87KArithM, "X87_K_CHS": x87KChs, "X87_K_ABS": x87KAbs, "X87_K_SQRT": x87KSqrt,
		"X87_K_RNDINT": x87KRndint, "X87_K_SCALE": x87KScale, "X87_K_XTRACT": x87KXtract, "X87_K_PREM": x87KPrem,
		"X87_K_COM_ST": x87KComSt, "X87_K_COM_M": x87KComM, "X87_K_COMI": x87KComi, "X87_K_TST": x87KTst,
		"X87_K_XAM": x87KXam, "X87_K_XCH": x87KXch, "X87_K_LDCW": x87KLdcw, "X87_K_STCW": x87KStcw,
		"X87_K_STSW": x87KStsw, "X87_K_NINIT": x87KNinit, "X87_K_NCLEX": x87KNclex, "X87_K_CMOV": x87KCmov,
		"X87_K_FREE": x87KFree, "X87_K_DECSTP": x87KDecstp, "X87_K_INCSTP": x87KIncstp, "X87_K_STENV": x87KStenv,
		"X87_K_LDENV": x87KLdenv, "X87_K_SAVE": x87KSave, "X87_K_RSTOR": x87KRstor, "X87_K_FXSAVE": x87KFxsave,
		"X87_K_FXRSTOR": x87KFxrstor, "X87_K_BLD": x87KBld, "X87_K_BSTP": x87KBstp,
		"X87_ARITH_ADD": x87Add, "X87_ARITH_SUB": x87Sub, "X87_ARITH_SUBR": x87SubR, "X87_ARITH_MUL": x87Mul,
		"X87_ARITH_DIV": x87Div, "X87_ARITH_DIVR": x87DivR,
	}
	for k, v := range want {
		if g, ok := got[k]; !ok || g != v {
			t.Errorf("%s: header has %d (present %v), the lowering uses %d", k, g, ok, v)
		}
	}
	if len(got) != len(want) {
		t.Errorf("header defines %d kinds/operations, the lowering knows %d", len(got), len(want))
	}
	if w := (x87Word{kind: 7, a: 5, b: 6, sz: 2, aux: 9, pop: true, pop2: true}).value(); w != 7|5<<8|6<<11|1<<14|1<<15|2<<16|9<<18 {
		t.Errorf("op word layout: %#x", w)
	}
}

// 80-bit value as "significand:signExp".
func f80s(mant uint64, se uint16) string { return fmt.Sprintf("%016x:%04x", mant, se) }

func mem80(mant uint64, se uint16) []byte {
	b := make([]byte, 10)
	binary.LittleEndian.PutUint64(b, mant)
	binary.LittleEndian.PutUint16(b[8:], se)
	return b
}
func mem64(v float64) []byte { return binary.LittleEndian.AppendUint64(nil, math.Float64bits(v)) }
func mem32(v float32) []byte { return binary.LittleEndian.AppendUint32(nil, math.Float32bits(v)) }
func memI(sz int, v int64) []byte {
	switch sz {
	case 2:
		return binary.LittleEndian.AppendUint16(nil, uint16(v))
	case 4:
		return binary.LittleEndian.AppendUint32(nil, uint32(v))
	}
	return binary.LittleEndian.AppendUint64(nil, uint64(v))
}

// A case: asm (Intel syntax; rdx = the data window at guest address 0x100), the x87 stack that must
// result (ST(0) first; the rest of the registers must be empty), and optionally the status word,
// RAX and memory (offsets from rdx).
type x87Case struct {
	name string
	asm  string
	mem  map[int][]byte
	cw   uint16 // initial control word (0 = 0x037f)
	st   []string
	sw   int   // expected status word & swMask, when swMask != 0
	swMk int   // mask
	rax  int64 // expected RAX when raxSet
	raxS bool  // RAX is checked
	out  map[int][]byte
	src  string // the FEX test the case restates or the derivation of the expectation
}

const (
	x87One   = "8000000000000000:3fff"
	x87Zero  = "0000000000000000:0000"
	x87Indef = "c000000000000000:ffff"
)

var (
	third64 = f80s(0xAAAAAAAAAAAAAAAB, 0x3ffd) // 1/3 to 64 bits, round to nearest
	third53 = f80s(0xAAAAAAAAAAAAA800, 0x3ffd)
	third24 = f80s(0xAAAAAB0000000000, 0x3ffd)
)

var x87Cases = []x87Case{
	// ---- loads of constants (FEX X87/D9_E8..EE) ----
	{name: "fld1", asm: "fld1", st: []string{x87One}, src: "D9_E8"},
	{name: "fldz", asm: "fldz", st: []string{x87Zero}, src: "D9_EE"},
	{name: "fldl2t", asm: "fldl2t", st: []string{f80s(0xD49A784BCD1B8AFE, 0x4000)}, src: "D9_E9"},
	{name: "fldl2e", asm: "fldl2e", st: []string{f80s(0xB8AA3B295C17F0BC, 0x3fff)}, src: "D9_EA"},
	{name: "fldpi", asm: "fldpi", st: []string{f80s(0xC90FDAA22168C235, 0x4000)}, src: "D9_EB"},
	{name: "fldlg2", asm: "fldlg2", st: []string{f80s(0x9A209A84FBCFF799, 0x3ffd)}, src: "D9_EC"},
	{name: "fldln2", asm: "fldln2", st: []string{f80s(0xB17217F7D1CF79AC, 0x3ffe)}, src: "D9_ED"},
	// the constants are rounded by RC: down and toward zero take the lower significand
	{name: "fldpi rc=down", asm: "fldpi", cw: 0x077f, st: []string{f80s(0xC90FDAA22168C234, 0x4000)}, src: "SDM: FLDPI is rounded per RC"},
	{name: "fldpi rc=up", asm: "fldpi", cw: 0x0b7f, st: []string{f80s(0xC90FDAA22168C235, 0x4000)}, src: "SDM: FLDPI is rounded per RC"},
	{name: "fldl2t rc=up", asm: "fldl2t", cw: 0x0b7f, st: []string{f80s(0xD49A784BCD1B8AFF, 0x4000)}, src: "log2(10) = ...8AFE.28: up gives ...AFF"},

	// ---- loads and stores of every width ----
	{name: "fld m32", asm: "fld dword ptr [rdx]", mem: map[int][]byte{0: mem32(1.5)}, st: []string{f80s(0xC000000000000000, 0x3fff)}},
	{name: "fld m64 denormal", asm: "fld qword ptr [rdx]", mem: map[int][]byte{0: mem64(math.SmallestNonzeroFloat64)},
		st: []string{f80s(0x8000000000000000, 0x3fff-1074)}, src: "a double denormal is a normal 80-bit value"},
	{name: "fld m80 / fstp m80 raw", asm: "fld tbyte ptr [rdx]\nfstp tbyte ptr [rdx+16]", mem: map[int][]byte{0: mem80(0x8E1BC9BF04000400, 0x4034)},
		out: map[int][]byte{16: mem80(0x8E1BC9BF04000400, 0x4034)}},
	{name: "fild m16", asm: "fild word ptr [rdx]", mem: map[int][]byte{0: memI(2, -3)}, st: []string{f80s(0xC000000000000000, 0xc000)}},
	{name: "fild m32", asm: "fild dword ptr [rdx]", mem: map[int][]byte{0: memI(4, 100000)}, st: []string{f80s(0xC350000000000000, 0x400f)}},
	{name: "fild m64 2^63-1 is exact", asm: "fild qword ptr [rdx]", mem: map[int][]byte{0: memI(8, math.MaxInt64)}, st: []string{f80s(0xFFFFFFFFFFFFFFFE, 0x403d)}, src: "int64 -> long double (decision doc §3)"},
	{name: "fild m64 2^53+1 is exact", asm: "fild qword ptr [rdx]", mem: map[int][]byte{0: memI(8, 1<<53+1)}, st: []string{f80s(0x8000000000000400, 0x4034)}},
	{name: "fstp m32 rounds to nearest even", asm: "fld1\nfild dword ptr [rdx]\nfdivp st(1), st\nfstp dword ptr [rdx+16]", mem: map[int][]byte{0: memI(4, 3)},
		out: map[int][]byte{16: mem32(float32(1.0 / 3.0))}},
	{name: "fstp m64 rc=up", asm: "fldcw word ptr [rdx+32]\nfld1\nfild dword ptr [rdx]\nfdivp st(1), st\nfstp qword ptr [rdx+16]",
		mem: map[int][]byte{0: memI(4, 3), 32: {0x7f, 0x0b}}, out: map[int][]byte{16: memI(8, int64(math.Float64bits(1.0/3.0))+1)}, src: "1/3 is 0x3fd5555555555555 rounded down by the double: up gives ...56"},

	// ---- arithmetic with a 64-bit significand ----
	{name: "1/3 (FIDIV)", asm: "fld1\nfidiv dword ptr [rdx]", mem: map[int][]byte{0: memI(4, 3)}, st: []string{third64}, src: "t_x87 ext_mant_third = 0xaaaaaaaaaaaaaaab"},
	{name: "(1e16+1)-1e16 is 1", asm: "fld qword ptr [rdx]\nfld1\nfaddp st(1), st\nfld qword ptr [rdx]\nfsubp st(1), st", mem: map[int][]byte{0: mem64(1e16)},
		st: []string{x87One}, src: "t_x87 ext_add_sub"},
	{name: "fsqrt 2", asm: "fld1\nfadd st, st(0)\nfsqrt", st: []string{f80s(0xB504F333F9DE6484, 0x3fff)}, src: "isqrt(2<<126) with the remainder below half"},
	{name: "1e300*1e300/1e300 survives", asm: "fld qword ptr [rdx]\nfmul st, st(0)\nfdiv qword ptr [rdx]", mem: map[int][]byte{0: mem64(1e300)},
		st: []string{f80s(0xBF21E44003ACE001, 0x43e3)}, src: "round64(round64(x*x)/x) by exact rational arithmetic; the double model overflows to inf"},
	{name: "pc=53 1/3", asm: "fldcw word ptr [rdx+32]\nfld1\nfidiv dword ptr [rdx]", mem: map[int][]byte{0: memI(4, 3), 32: {0x7f, 0x02}}, st: []string{third53}, src: "FCW 0x027f: 53-bit precision"},
	{name: "pc=24 1/3", asm: "fldcw word ptr [rdx+32]\nfld1\nfidiv dword ptr [rdx]", mem: map[int][]byte{0: memI(4, 3), 32: {0x7f, 0x00}}, st: []string{third24}, src: "FCW 0x007f: 24-bit precision"},
	{name: "rc=down 1/3", asm: "fldcw word ptr [rdx+32]\nfld1\nfidiv dword ptr [rdx]", mem: map[int][]byte{0: memI(4, 3), 32: {0x7f, 0x07}}, st: []string{f80s(0xAAAAAAAAAAAAAAAA, 0x3ffd)}},
	{name: "rc=up 1/3", asm: "fldcw word ptr [rdx+32]\nfld1\nfidiv dword ptr [rdx]", mem: map[int][]byte{0: memI(4, 3), 32: {0x7f, 0x0b}}, st: []string{third64}},
	{name: "rc=zero 1/3", asm: "fldcw word ptr [rdx+32]\nfld1\nfidiv dword ptr [rdx]", mem: map[int][]byte{0: memI(4, 3), 32: {0x7f, 0x0f}}, st: []string{f80s(0xAAAAAAAAAAAAAAAA, 0x3ffd)}},
	{name: "fsub st(0), st(1) = st0 - st1", asm: "fld dword ptr [rdx]\nfld dword ptr [rdx+4]\nfsub st, st(1)", mem: map[int][]byte{0: mem32(5), 4: mem32(3)},
		st: []string{f80s(0x8000000000000000, 0xc000), f80s(0xA000000000000000, 0x4001)}},
	{name: "fsubr st(0), st(1) = st1 - st0", asm: "fld dword ptr [rdx]\nfld dword ptr [rdx+4]\nfsubr st, st(1)", mem: map[int][]byte{0: mem32(5), 4: mem32(3)},
		st: []string{f80s(0x8000000000000000, 0x4000), f80s(0xA000000000000000, 0x4001)}},
	{name: "fsubp st(1), st(0) pops", asm: "fld dword ptr [rdx]\nfld dword ptr [rdx+4]\nfsubp st(1), st", mem: map[int][]byte{0: mem32(5), 4: mem32(3)},
		st: []string{f80s(0x8000000000000000, 0x4000)}},
	{name: "fdivrp st(1), st(0) pops", asm: "fld dword ptr [rdx]\nfld dword ptr [rdx+4]\nfdivrp st(1), st", mem: map[int][]byte{0: mem32(2), 4: mem32(8)},
		st: []string{f80s(0x8000000000000000, 0x3fff+2)}, src: "st1 = st0 / st1 = 8/2"},
	{name: "fisubr m16", asm: "fld dword ptr [rdx]\nfisubr word ptr [rdx+4]", mem: map[int][]byte{0: mem32(3), 4: memI(2, 10)}, st: []string{f80s(0xE000000000000000, 0x4001)}, src: "X87/DE_E8-like: 10 - 3 = 7"},
	{name: "fidivr m32", asm: "fld dword ptr [rdx]\nfidivr dword ptr [rdx+4]", mem: map[int][]byte{0: mem32(4), 4: memI(4, 20)}, st: []string{f80s(0xA000000000000000, 0x4001)}, src: "20 / 4 = 5"},
	{name: "fchs fabs", asm: "fld1\nfchs\nfld1\nfchs\nfabs", st: []string{x87One, f80s(0x8000000000000000, 0xbfff)}},

	// ---- rounding, scaling, extraction, remainder ----
	{name: "frndint nearest-even 2.5", asm: "fld qword ptr [rdx]\nfrndint", mem: map[int][]byte{0: mem64(2.5)}, st: []string{f80s(0x8000000000000000, 0x4000)}},
	{name: "frndint rc=down -2.5", asm: "fldcw word ptr [rdx+32]\nfld qword ptr [rdx]\nfrndint", mem: map[int][]byte{0: mem64(-2.5), 32: {0x7f, 0x07}}, st: []string{f80s(0xC000000000000000, 0xc000)}, src: "-3.0"},
	{name: "frndint rc=up 2.5", asm: "fldcw word ptr [rdx+32]\nfld qword ptr [rdx]\nfrndint", mem: map[int][]byte{0: mem64(2.5), 32: {0x7f, 0x0b}}, st: []string{f80s(0xC000000000000000, 0x4000)}},
	{name: "frndint rc=zero -2.5", asm: "fldcw word ptr [rdx+32]\nfld qword ptr [rdx]\nfrndint", mem: map[int][]byte{0: mem64(-2.5), 32: {0x7f, 0x0f}}, st: []string{f80s(0x8000000000000000, 0xc000)}},
	{name: "fscale 3 * 2^2", asm: "fld dword ptr [rdx]\nfld dword ptr [rdx+4]\nfscale", mem: map[int][]byte{0: mem32(2.5), 4: mem32(3)},
		st: []string{f80s(0xC000000000000000, 0x4002), f80s(0xA000000000000000, 0x4000)}, src: "ST0 * 2^trunc(ST1): ST1 = 2.5 truncates toward zero to 2"},
	{name: "fscale beyond double range", asm: "fld dword ptr [rdx]\nfld1\nfscale\nfstp st(1)", mem: map[int][]byte{0: mem32(2000)}, st: []string{f80s(0x8000000000000000, 0x3fff+2000)}},
	{name: "fxtract 12", asm: "fld dword ptr [rdx]\nfxtract", mem: map[int][]byte{0: mem32(12)}, st: []string{f80s(0xC000000000000000, 0x3fff), f80s(0xC000000000000000, 0x4000)}, src: "ST0 = 1.5, ST1 = 3.0"},
	{name: "fxtract of an 80-bit denormal", asm: "fld tbyte ptr [rdx]\nfxtract", mem: map[int][]byte{0: mem80(0x0000000000000001, 0x0000)},
		st: []string{f80s(0x8000000000000000, 0x3fff), f80s(0x807A000000000000, 0xc00d)}, src: "2^-16445: exponent -16445 = -0x403d"},
	{name: "fprem 10 mod 3", asm: "fld dword ptr [rdx+4]\nfld dword ptr [rdx]\nfprem\nfnstsw ax", mem: map[int][]byte{0: mem32(10), 4: mem32(3)},
		st: []string{x87One, f80s(0xC000000000000000, 0x4000)}, swMk: 0x4700, sw: 0x4200, src: "q = 3: C3 = Q1, C1 = Q0, C0 = Q2 = 0, C2 = 0"},
	{name: "fprem1 10 rem 3 is -1", asm: "fld dword ptr [rdx+4]\nfld dword ptr [rdx]\nfprem1", mem: map[int][]byte{0: mem32(10), 4: mem32(3)},
		st: []string{x87One, f80s(0xC000000000000000, 0x4000)}, src: "10/3 = 3.33 -> q 3, 10 - 9 = 1"},
	{name: "fprem1 11 rem 3 is -1", asm: "fld dword ptr [rdx+4]\nfld dword ptr [rdx]\nfprem1", mem: map[int][]byte{0: mem32(11), 4: mem32(3)},
		st: []string{f80s(0x8000000000000000, 0xbfff), f80s(0xC000000000000000, 0x4000)}, src: "11/3 = 3.67 -> q 4, 11 - 12 = -1"},
	{name: "fprem partial (exponent difference 64+)", asm: "fld tbyte ptr [rdx+16]\nfld tbyte ptr [rdx]\nfprem\nfnstsw ax", mem: map[int][]byte{0: mem80(0x8000000000000000, 0x3fff+100), 16: mem80(0xC000000000000000, 0x3fff)},
		swMk: 0x0400, sw: 0x0400, src: "C2 = 1: the reduction is incomplete"},

	// ---- comparison and the status word ----
	{name: "fcom less sets C0", asm: "fld1\nfld dword ptr [rdx]\nfcom st(1)\nfnstsw ax", mem: map[int][]byte{0: mem32(0.5)}, swMk: 0x4500, sw: 0x0100},
	{name: "fcom equal sets C3", asm: "fld1\nfld1\nfcom st(1)\nfnstsw ax", swMk: 0x4500, sw: 0x4000},
	{name: "fcom greater clears", asm: "fld1\nfld dword ptr [rdx]\nfcom st(1)\nfnstsw ax", mem: map[int][]byte{0: mem32(2)}, swMk: 0x4500, sw: 0},
	{name: "fucom unordered quiet NaN: no IE", asm: "fld1\nfld qword ptr [rdx]\nfucom st(1)\nfnstsw ax", mem: map[int][]byte{0: binary.LittleEndian.AppendUint64(nil, 0x7ff8000000000000)}, swMk: 0x4501, sw: 0x4500},
	{name: "fcom quiet NaN raises IE", asm: "fld1\nfld qword ptr [rdx]\nfcom st(1)\nfnstsw ax", mem: map[int][]byte{0: binary.LittleEndian.AppendUint64(nil, 0x7ff8000000000000)}, swMk: 0x4501, sw: 0x4501},
	{name: "fcompp pops two", asm: "fld1\nfldz\nfcompp\nfnstsw ax", swMk: 0x4500 | 0x3800, sw: 0x0100},
	{name: "ftst of -0.5", asm: "fld dword ptr [rdx]\nftst\nfnstsw ax", mem: map[int][]byte{0: mem32(-0.5)}, st: []string{f80s(0x8000000000000000, 0xbffe)}, swMk: 0x4500, sw: 0x0100},
	{name: "fucomip equal: ZF set, CF and PF clear", asm: "fld1\nfld1\nfucomip st, st(1)\nsete al\nsetb cl\nsetp dl\nmovzx eax, al\nmovzx ecx, cl\nshl ecx, 8\nor eax, ecx\nmovzx edx, dl\nshl edx, 16\nor eax, edx\nfstp st(0)", rax: 1, raxS: true, src: "X87/DF_E8"},
	{name: "fcomi unordered: ZF CF PF", asm: "fld1\nfld qword ptr [rdx]\nfcomi st, st(1)\nsete al\nsetb cl\nsetp dl\nmovzx eax, al\nmovzx ecx, cl\nshl ecx, 8\nor eax, ecx\nmovzx edx, dl\nshl edx, 16\nor eax, edx\nfstp st(0)\nfstp st(0)",
		mem: map[int][]byte{0: binary.LittleEndian.AppendUint64(nil, 0x7ff8000000000000)}, rax: 0x10101, raxS: true},
	{name: "fcmove taken", asm: "fldpi\nfld1\nfldz\nfcomi st, st(0)\nfcmove st, st(2)", st: []string{f80s(0xC90FDAA22168C235, 0x4000), x87One, f80s(0xC90FDAA22168C235, 0x4000)}, src: "ZF = 1 after fcomi of a value with itself"},
	{name: "fcmovne not taken", asm: "fld1\nfldz\nfcomi st, st(0)\nfcmovne st, st(1)", st: []string{x87Zero, x87One}},
	{name: "fxam zero then normal", asm: "fldz\nfxam\nfnstsw ax\nand eax, 0x4700\nmov ecx, eax\nfstp st(0)\nfld1\nfxam\nfnstsw ax\nand eax, 0x4700\nshl eax, 16\nor eax, ecx\nfstp st(0)",
		rax: 0x04000000 | 0x4000, raxS: true, src: "zero = C3, normal = C2"},
	{name: "fxam -inf", asm: "fld qword ptr [rdx]\nfxam\nfnstsw ax\nand eax, 0x4700", mem: map[int][]byte{0: mem64(math.Inf(-1))}, rax: 0x0700, raxS: true, src: "inf = C2|C0, C1 = sign"},
	{name: "fxam NaN", asm: "fld qword ptr [rdx]\nfxam\nfnstsw ax\nand eax, 0x4700", mem: map[int][]byte{0: binary.LittleEndian.AppendUint64(nil, 0x7ff8000000000000)}, rax: 0x0100, raxS: true},
	{name: "fxam empty", asm: "fxam\nfnstsw ax\nand eax, 0x4700", rax: 0x4100, raxS: true, src: "X87/FXAM_Push: empty = C3|C0"},

	// ---- integer conversion ----
	{name: "fistp m32 nearest even 2.5 and 3.5", asm: "fld qword ptr [rdx]\nfistp dword ptr [rdx+16]\nfld qword ptr [rdx+8]\nfistp dword ptr [rdx+20]", mem: map[int][]byte{0: mem64(2.5), 8: mem64(3.5)},
		out: map[int][]byte{16: memI(4, 2), 20: memI(4, 4)}},
	{name: "fisttp truncates -2.7", asm: "fld qword ptr [rdx]\nfisttp dword ptr [rdx+16]", mem: map[int][]byte{0: mem64(-2.7)}, out: map[int][]byte{16: memI(4, -2)}},
	{name: "fist rc=down m16", asm: "fldcw word ptr [rdx+32]\nfld qword ptr [rdx]\nfistp word ptr [rdx+16]", mem: map[int][]byte{0: mem64(-2.1), 32: {0x7f, 0x07}}, out: map[int][]byte{16: memI(2, -3)}},
	{name: "fistp m16 overflow: indefinite and IE", asm: "fld dword ptr [rdx]\nfistp word ptr [rdx+16]\nfnstsw ax", mem: map[int][]byte{0: mem32(65536)}, out: map[int][]byte{16: memI(2, -0x8000)}, swMk: 0x0001, sw: 1},
	{name: "fistp m64 2^63 overflows", asm: "fld dword ptr [rdx]\nfistp qword ptr [rdx+16]", mem: map[int][]byte{0: mem32(9.223372036854775808e18)}, out: map[int][]byte{16: memI(8, math.MinInt64)}},
	{name: "fistp m64 of 2^53+1 is exact", asm: "fild qword ptr [rdx]\nfistp qword ptr [rdx+16]", mem: map[int][]byte{0: memI(8, 1<<53+1)}, out: map[int][]byte{16: memI(8, 1<<53+1)}},
	{name: "fistp m32 of NaN", asm: "fld qword ptr [rdx]\nfistp dword ptr [rdx+16]", mem: map[int][]byte{0: binary.LittleEndian.AppendUint64(nil, 0x7ff8000000000000)}, out: map[int][]byte{16: memI(4, math.MinInt32)}},

	// ---- special values and exception flags ----
	{name: "inf - inf is the indefinite, IE", asm: "fld qword ptr [rdx]\nfld st(0)\nfsubp st(1), st\nfnstsw ax", mem: map[int][]byte{0: mem64(math.Inf(1))}, st: []string{x87Indef}, swMk: 0x0001, sw: 1, src: "X87/invalid_infinity_sub_infinity"},
	{name: "1/0 is inf, ZE", asm: "fld1\nfldz\nfdivp st(1), st\nfnstsw ax", st: []string{f80s(0x8000000000000000, 0x7fff)}, swMk: 0x0004, sw: 4},
	{name: "sqrt(-1) is the indefinite, IE", asm: "fld1\nfchs\nfsqrt\nfnstsw ax", st: []string{x87Indef}, swMk: 1, sw: 1, src: "X87/invalid_sqrt_negative"},
	{name: "inexact result sets PE", asm: "fld1\nfidiv dword ptr [rdx]\nfnstsw ax", mem: map[int][]byte{0: memI(4, 3)}, st: []string{third64}, swMk: 0x20, sw: 0x20},
	{name: "overflow sets OE and PE", asm: "fld tbyte ptr [rdx]\nfadd st, st(0)\nfnstsw ax", mem: map[int][]byte{0: mem80(0xFFFFFFFFFFFFFFFF, 0x7ffe)}, st: []string{f80s(0x8000000000000000, 0x7fff)}, swMk: 0x28, sw: 0x28},

	// ---- stack discipline ----
	{name: "stack overflow: indefinite, IE SF C1", asm: "fld1\nfld1\nfld1\nfld1\nfld1\nfld1\nfld1\nfld1\nfld1\nfnstsw ax",
		st: []string{x87Indef, x87One, x87One, x87One, x87One, x87One, x87One, x87One}, swMk: 0x0241, sw: 0x0241},
	{name: "stack underflow: indefinite, IE SF", asm: "fldz\nfadd st, st(1)\nfnstsw ax", st: []string{x87Indef}, swMk: 0x0241, sw: 0x0041, src: "ST(1) is empty"},
	{name: "fxch", asm: "fld1\nfldz\nfxch st(1)", st: []string{x87One, x87Zero}},
	{name: "fdecstp moves TOP only", asm: "fld1\nfdecstp\nfnstsw ax", swMk: 0x3800, sw: 0x3000, src: "TOP 7 -> 6: bits 13:11 = 110"},
	{name: "ffree", asm: "fld1\nfldz\nffree st(1)\nfincstp", st: []string{x87Zero}, src: "ST1 (the 1.0) is freed; fincstp makes it ST0, empty; the 0.0 is ST7"},
	{name: "fninit", asm: "fld1\nfld1\nfninit\nfnstsw ax", st: []string{}, swMk: 0xffff, sw: 0},
	{name: "fnstcw / fldcw", asm: "fldcw word ptr [rdx]\nfnstcw word ptr [rdx+16]", mem: map[int][]byte{0: {0x7f, 0x0f}}, out: map[int][]byte{16: {0x7f, 0x0f}}},
	{name: "fnclex clears the flags", asm: "fld1\nfldz\nfdivp st(1), st\nfnclex\nfnstsw ax", st: []string{f80s(0x8000000000000000, 0x7fff)}, swMk: 0x80ff, sw: 0},

	// ---- environment, FNSAVE, FXSAVE, packed BCD ----
	{name: "fnstenv / fldenv keep the stack", asm: "fld1\nfldz\nfnstenv [rdx+64]\nfninit\nfldenv [rdx+64]\nfnstsw ax", st: []string{x87Zero, x87One}, swMk: 0x3800, sw: 0x3000},
	{name: "fnstenv tag word", asm: "fld1\nfldz\nfnstenv [rdx+64]\nmovzx eax, word ptr [rdx+72]", rax: 0x1fff, raxS: true, src: "R7 valid (00), R6 zero (01), R0-R5 empty (11)"},
	{name: "fnsave / frstor round trip", asm: "fld1\nfldpi\nfnsave [rdx+64]\nfrstor [rdx+64]", st: []string{f80s(0xC90FDAA22168C235, 0x4000), x87One}},
	{name: "o16 fnsave / frstor (14-byte environment)", asm: "fld1\nfldpi\nhex:66dd7240\nhex:66dd6240", st: []string{f80s(0xC90FDAA22168C235, 0x4000), x87One}, src: "X87/DD_04_2"},
	{name: "fxsave / fxrstor", asm: "fld1\nfldpi\nfxsave [rdx+64]\nfninit\nfxrstor [rdx+64]", st: []string{f80s(0xC90FDAA22168C235, 0x4000), x87One}},
	{name: "fbld 1234", asm: "fbld tbyte ptr [rdx]", mem: map[int][]byte{0: {0x34, 0x12, 0, 0, 0, 0, 0, 0, 0, 0}}, st: []string{f80s(0x9A40000000000000, 0x4009)}, src: "1234 = 0x4d2 = 1.205078125 * 2^10"},
	{name: "fbstp -4.5 (nearest even) = -4", asm: "fld qword ptr [rdx]\nfbstp tbyte ptr [rdx+16]", mem: map[int][]byte{0: mem64(-4.5)}, out: map[int][]byte{16: {0x04, 0, 0, 0, 0, 0, 0, 0, 0, 0x80}}},
	{name: "fbstp out of range gives the BCD indefinite", asm: "fld qword ptr [rdx]\nfbstp tbyte ptr [rdx+16]", mem: map[int][]byte{0: mem64(1e19)}, out: map[int][]byte{16: {0, 0, 0, 0, 0, 0, 0, 0xC0, 0xFF, 0xFF}}},

	// ---- MMX aliases the significand of the x87 register of the same number ----
	{name: "movq mm0 shares the significand of physical register 0 (= ST0 at TOP 0)", asm: "movq mm0, qword ptr [rdx]\nfxsave [rdx+64]", mem: map[int][]byte{0: memI(8, 0x1122334455667788)},
		out: map[int][]byte{96: memI(8, 0x1122334455667788)}, src: "X87/D9_F6 (MMn is x87 register n)"},
}

func llvmBin() string {
	if d := os.Getenv("LLVM_BIN"); d != "" {
		return d
	}
	return "/opt/homebrew/opt/llvm/bin"
}

var encRe = regexp.MustCompile(`encoding: \[([^\]]*)\]`)

// assemble returns the hex of each instruction of the Intel-syntax text (one per line).
func assemble(t *testing.T, text string) []string {
	t.Helper()
	var out []string
	for _, line := range strings.Split(text, "\n") {
		if h, ok := strings.CutPrefix(line, "hex:"); ok { // raw bytes (llvm-mc lists a prefix as its own instruction)
			out = append(out, h)
			continue
		}
		cmd := exec.Command(filepath.Join(llvmBin(), "llvm-mc"), "-triple=x86_64-unknown-linux", "-x86-asm-syntax=intel", "-show-encoding")
		cmd.Stdin = strings.NewReader(".intel_syntax noprefix\n" + line + "\n")
		b, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("llvm-mc %q: %v\n%s", line, err, b)
		}
		m := encRe.FindStringSubmatch(string(b))
		if m == nil {
			t.Fatalf("llvm-mc %q: no encoding in\n%s", line, b)
		}
		var h strings.Builder
		for _, x := range strings.Split(m[1], ",") {
			h.WriteString(strings.TrimPrefix(strings.TrimSpace(x), "0x"))
		}
		out = append(out, h.String())
	}
	return out
}

// buildX87Runtime compiles SoftFloat, nps5_x87.c and the bridge into a static library once.
func buildX87Runtime(t *testing.T, dir string) string { return buildX87RuntimeFlags(t, dir) }

// buildX87RuntimeFlags: the same with extra compiler flags (-DNPS5_X87_NOFAST: every operation on SoftFloat).
func buildX87RuntimeFlags(t *testing.T, dir string, flags ...string) string {
	t.Helper()
	core, _ := filepath.Abs(filepath.Join("..", "runtime", "core"))
	srcs, _ := filepath.Glob(filepath.Join(core, "softfloat", "*.c"))
	srcs = append(srcs, filepath.Join(core, "nps5_x87.c"))
	br, err := os.ReadFile(filepath.Join("..", "emitter", "asm_x87.go"))
	if err != nil {
		t.Fatal(err)
	}
	s := string(br)
	asm := s[strings.Index(s, "`")+1 : strings.LastIndex(s, "`")]
	bridge := filepath.Join(dir, "bridge.s")
	os.WriteFile(bridge, []byte(asm), 0o644)
	srcs = append(srcs, bridge)
	var objs []string
	for _, src := range srcs {
		obj := filepath.Join(dir, strings.TrimSuffix(filepath.Base(src), filepath.Ext(src))+".o")
		if o, err := exec.Command("clang", append([]string{"-arch", "arm64", "-O1", "-w", "-I" + core, "-I" + filepath.Join(core, "softfloat"), "-c", src, "-o", obj}, flags...)...).CombinedOutput(); err != nil {
			t.Fatalf("%s: %v\n%s", src, err, o)
		}
		objs = append(objs, obj)
	}
	lib := filepath.Join(dir, "x87rt.a")
	if o, err := exec.Command("ar", append([]string{"crs", lib}, objs...)...).CombinedOutput(); err != nil {
		t.Fatalf("ar: %v\n%s", err, o)
	}
	return lib
}

const x87Driver = `
#include "recomp_context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint16_t cw; uint32_t nmem; } hdr_t;
extern void lifted_case(uint8_t *window, uint64_t *rax);
extern int lifted_count;

static GuestContext ctx;

/* input: per case: u16 cw, u32 nbytes, (u32 off, u32 len, bytes)*  ; output: per case raw state */
int main(int argc, char **argv) {
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
    uint8_t *window = calloc(1, 1 << 20);
    typedef void (*fn_t)(uint8_t *, uint64_t *);
    extern fn_t lifted_table[];
    extern int lifted_n;
    for (int i = 0; i < lifted_n; i++) {
        uint16_t cw; uint32_t nseg;
        fread(&cw, 2, 1, in); fread(&nseg, 4, 1, in);
        memset(window, 0, 1 << 20);
        memset(&ctx, 0, sizeof ctx);
        ctx.fpu_cw = cw;
        ctx.mxcsr = 0x1f80;
        GuestContext *cp = &ctx;
        memcpy(window + 0x80000 + 0x70, &cp, 8);
        for (uint32_t s = 0; s < nseg; s++) {
            uint32_t off, len; fread(&off, 4, 1, in); fread(&len, 4, 1, in);
            fread(window + 0x100 + off, 1, len, in);
        }
        uint64_t rax = 0;
        lifted_table[i](window, &rax);
        uint32_t top = (uint32_t)ctx.fpu_top, sw = ctx.fpu_sw, c = ctx.fpu_cw, valid = ctx.fpu_valid;
        fwrite(&top, 4, 1, out); fwrite(&sw, 4, 1, out); fwrite(&c, 4, 1, out); fwrite(&valid, 4, 1, out);
        for (int r = 0; r < 8; r++) fwrite(&ctx.fpu80[r], 16, 1, out);
        fwrite(&rax, 8, 1, out);
        fwrite(window + 0x100, 1, 256, out);
    }
    return 0;
}
`

func TestX87F80Ops(t *testing.T) {
	if runtime.GOOS != "darwin" || runtime.GOARCH != "arm64" {
		t.Skip("needs an arm64 Mac (host lowering)")
	}
	for _, tool := range []string{"clang", "ar"} {
		if _, err := exec.LookPath(tool); err != nil {
			t.Skip(tool + " not found")
		}
	}
	if _, err := os.Stat(filepath.Join(llvmBin(), "llvm-mc")); err != nil {
		t.Skip("llvm-mc not found (set LLVM_BIN)")
	}
	dir := t.TempDir()
	lib := buildX87Runtime(t, dir)

	l := NewArm64Lifter(nil)
	if err := l.SetX87Mode("f80"); err != nil {
		t.Fatal(err)
	}
	l.SetTSO(false)
	fn := &disasm.Function{
		EntryAddr: 0x1000, EndAddr: 0x9000,
		Blocks: map[uint64]*disasm.BasicBlock{0x1000: {StartAddr: 0x1000}},
	}
	var asm strings.Builder
	asm.WriteString(".text\n")
	var table strings.Builder
	var in bytes.Buffer
	cases := x87Cases
	for i, c := range cases {
		pc := uint64(0x1000)
		fmt.Fprintf(&asm, ".p2align 2\n_case_%d:\n", i)
		// x0 = window (guest memory base, x28), x1 = &rax. Guest registers: rax = 0, rdx = 0x100,
		// rsp = 0x70000, FS base = 0x80000 (the TCB slot at +0x70 holds the GuestContext pointer).
		asm.WriteString("    stp x29, x30, [sp, #-112]!\n    stp x19, x20, [sp, #16]\n    stp x21, x22, [sp, #32]\n    stp x23, x24, [sp, #48]\n" +
			"    stp x25, x26, [sp, #64]\n    stp x27, x28, [sp, #80]\n    str x1, [sp, #96]\n")
		asm.WriteString("    mov x28, x0\n    mov x19, #0\n    mov x20, #0\n    mov x21, #0\n    mov x22, #0x100\n    mov x23, #0\n    mov x24, #0\n    mov x25, #0\n    mov x26, #0x70000\n    mov x27, #0x80000\n")
		for _, h := range assemble(t, c.asm) {
			b := mustHex(t, h)
			inst, err := x86asm.Decode(b, 64)
			if err != nil {
				t.Fatalf("%s: decode %s: %v", c.name, h, err)
			}
			next := pc + uint64(inst.Len)
			lines, err := l.LiftInstructionToBuf(disasm.Instruction{Address: pc, Inst: inst}, next, fn, nil)
			if err != nil {
				t.Fatalf("%s: lift %s: %v", c.name, inst, err)
			}
			for _, ln := range lines {
				if strings.Contains(ln, "_recomp_unsupported") {
					t.Fatalf("%s: %s not lifted:\n%s", c.name, inst, strings.Join(lines, "\n"))
				}
				asm.WriteString(ln + "\n")
			}
			pc = next
		}
		asm.WriteString("    ldr x1, [sp, #96]\n    str x19, [x1]\n    ldp x19, x20, [sp, #16]\n    ldp x21, x22, [sp, #32]\n    ldp x23, x24, [sp, #48]\n" +
			"    ldp x25, x26, [sp, #64]\n    ldp x27, x28, [sp, #80]\n    ldp x29, x30, [sp], #112\n    ret\n")
		fmt.Fprintf(&table, "    .quad _case_%d\n", i)

		cw := c.cw
		if cw == 0 {
			cw = 0x37f
		}
		binary.Write(&in, binary.LittleEndian, cw)
		binary.Write(&in, binary.LittleEndian, uint32(len(c.mem)))
		for off, b := range c.mem {
			binary.Write(&in, binary.LittleEndian, uint32(off))
			binary.Write(&in, binary.LittleEndian, uint32(len(b)))
			in.Write(b)
		}
	}
	asm.WriteString(".data\n.p2align 3\n.globl _lifted_table\n_lifted_table:\n" + table.String())
	fmt.Fprintf(&asm, ".globl _lifted_n\n_lifted_n:\n    .long %d\n", len(cases))
	asmPath := filepath.Join(dir, "cases.s")
	os.WriteFile(asmPath, []byte(asm.String()), 0o644)
	drv := filepath.Join(dir, "driver.c")
	os.WriteFile(drv, []byte(x87Driver), 0o644)
	exe := filepath.Join(dir, "x87test")
	core, _ := filepath.Abs(filepath.Join("..", "runtime", "core"))
	if o, err := exec.Command("clang", "-arch", "arm64", "-O1", "-w", "-I"+core, drv, asmPath, lib, "-o", exe).CombinedOutput(); err != nil {
		t.Fatalf("link: %v\n%s", err, o)
	}
	inPath, outPath := filepath.Join(dir, "in.bin"), filepath.Join(dir, "out.bin")
	os.WriteFile(inPath, in.Bytes(), 0o644)
	if o, err := exec.Command(exe, inPath, outPath).CombinedOutput(); err != nil {
		t.Fatalf("run: %v\n%s", err, o)
	}
	res, err := os.ReadFile(outPath)
	if err != nil {
		t.Fatal(err)
	}
	const rec = 16 + 128 + 8 + 256
	if len(res) != rec*len(cases) {
		t.Fatalf("output is %d bytes, want %d", len(res), rec*len(cases))
	}
	for i, c := range cases {
		r := res[i*rec : (i+1)*rec]
		top := int(binary.LittleEndian.Uint32(r[0:]))
		sw := int(binary.LittleEndian.Uint32(r[4:])) &^ 0x3800
		sw |= top << 11
		valid := binary.LittleEndian.Uint32(r[12:])
		rax := int64(binary.LittleEndian.Uint64(r[16+128:]))
		window := r[16+128+8:]
		t.Run(c.name, func(t *testing.T) {
			var got []string
			for k := 0; k < 8; k++ {
				p := (top + k) & 7
				if valid>>p&1 == 1 {
					got = append(got, f80s(binary.LittleEndian.Uint64(r[16+16*p:]), binary.LittleEndian.Uint16(r[16+16*p+8:])))
				}
			}
			if c.st != nil && !equalStrings(got, c.st) {
				t.Errorf("stack %v, want %v", got, c.st)
			}
			if c.swMk != 0 && sw&c.swMk != c.sw {
				t.Errorf("status word %#04x & %#04x = %#04x, want %#04x", sw, c.swMk, sw&c.swMk, c.sw)
			}
			if c.raxS && rax != c.rax {
				t.Errorf("rax %#x, want %#x", rax, c.rax)
			}
			for off, want := range c.out {
				if g := window[off : off+len(want)]; !bytes.Equal(g, want) {
					t.Errorf("memory +%d: % x, want % x", off, g, want)
				}
			}
		})
	}
}

func mustHex(t *testing.T, h string) []byte {
	t.Helper()
	b := make([]byte, len(h)/2)
	for i := range b {
		v, err := strconv.ParseUint(h[2*i:2*i+2], 16, 8)
		if err != nil {
			t.Fatal(err)
		}
		b[i] = byte(v)
	}
	return b
}

func equalStrings(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}
