package lifter

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"strings"
	"testing"

	"golang.org/x/arch/x86/x86asm"

	"ps4-recomp/pkg/disasm"
)

// Differential test of the BMI and narrow-flag peepholes (-bmi-peep): every sequence is lifted with the
// peepholes off (the general lowering, the reference) and on, run on the host from the same registers and
// memory, and the GPRs and the memory window must be identical. The sequences end in SETcc readers of
// exactly the flags the case declares live, so the liveness pass leaves those flags live and the readers
// copy their values into registers that are compared: a flag that is live is compared exactly, a flag that
// is dead is not (it is not computed).

var bmiVals = []uint64{0, 1, 2, 3, 0x80, 0xff, 0x100, 0x7fffffff, 0x80000000, 0x80000001, 0xffffffff,
	0x100000000, 0x8000000000000000, 0xffffffffffffffff, 0x00f0f0f000f0f000, 0x123456789abcdef0, 0xfedcba9876543210,
	0x0000000100000000 | 0x40, 0x7fffffffffffffff}

// reader sets: the flags left live after the instruction under test.
var bmiReaders = []struct {
	name string
	live string // c z s o: the flags the readers read
	code []string
}{
	{"all", "czso", []string{"410f92c0", "410f94c1", "410f98c2", "410f90c3"}}, // setb r8b, sete r9b, sets r10b, seto r11b
	{"zf", "z", []string{"410f94c1"}},
	{"zf+sf", "zs", []string{"410f94c1", "410f98c2"}},
	{"cf", "c", []string{"410f92c0"}},
	{"sf", "s", []string{"410f98c2"}},
	{"zf+cf", "zc", []string{"410f94c1", "410f92c0"}},
	{"none", "", nil},
}

func vexBmi(n int, w bool, dst, src int) string {
	b3 := byte((^dst & 0xf) << 3)
	if w {
		b3 |= 0x80
	}
	return fmt.Sprintf("c4e2%02xf3%02x", b3, 0xc0|n<<3|src)
}

func vexBextr(w bool, dst, src, ctl int) string {
	b3 := byte((^ctl & 0xf) << 3)
	if w {
		b3 |= 0x80
	}
	return fmt.Sprintf("c4e2%02xf7%02x", b3, 0xc0|dst<<3|src)
}

// bmiCase: the instruction at index idx of code leaves the flags in live live (letters c, z, s, o);
// the liveness bits the pass computed (everything live at the end of the sequence) are replaced by it.
func bmiCase(name string, code []string, regs map[x86asm.Reg]uint64, idx int, live string, more ...any) execCase {
	extra := map[int]string{}
	for k := 0; k+1 < len(more); k += 2 {
		extra[more[k].(int)] = more[k+1].(string)
	}
	return execCase{name: name, code: code, regs: regs, cfg: true, block: true, noFlags: true,
		prep: func(i int, in *disasm.Instruction) {
			live := live
			if e, ok := extra[i]; ok {
				live = e
			} else if i != idx {
				return
			}
			in.SkipFlags = live == ""
			in.CarryDead = !strings.Contains(live, "c")
			in.ZeroDead = !strings.Contains(live, "z")
			in.SignDead = !strings.Contains(live, "s")
			in.OverflowDead = !strings.Contains(live, "o")
			in.ParityDead, in.AdjustDead = true, true
		}}
}

func bmiDiffCases() []execCase {
	var cases []execCase
	rb := uint64(execMemBase)
	// regs: eax=0 ecx=1 edx=2 ebx=3 esp=4 ebp=5 esi=6 edi=7
	for _, rd := range bmiReaders {
		for _, v := range bmiVals {
			regs := func(extra ...uint64) map[x86asm.Reg]uint64 {
				m := map[x86asm.Reg]uint64{x86asm.RDX: v, x86asm.RSI: v ^ 0x5555, x86asm.RAX: 0x1111111122222222,
					x86asm.RCX: 0x3333333344444444, x86asm.RDI: rb + 0x100}
				return m
			}
			for _, w := range []bool{false, true} {
				wn := map[bool]string{false: "32", true: "64"}[w]
				for _, c := range []struct {
					n    int
					name string
				}{{3, "blsi"}, {1, "blsr"}, {2, "blsmsk"}} {
					// dst != src, dst == src
					for _, pair := range [][2]int{{1, 2}, {2, 2}} {
						code := append([]string{vexBmi(c.n, w, pair[0], pair[1])}, rd.code...)
						cases = append(cases, bmiCase(fmt.Sprintf("%s%s r%d,r%d %s v=%#x", c.name, wn, pair[0], pair[1], rd.name, v), code, regs(), 0, rd.live))
					}
					// memory source
					memc := fmt.Sprintf("c4e2%02xf3%02x", byte((^1&0xf)<<3)|map[bool]byte{true: 0x80}[w], 0x07|c.n<<3)
					code := append([]string{memc}, rd.code...) // blsX ecx, [rdi]
					m := bmiCase(fmt.Sprintf("%s%s [mem] %s v=%#x", c.name, wn, rd.name, v), code, regs(), 0, rd.live)
					m.mem = map[int][]byte{0x100: binary.LittleEndian.AppendUint64(nil, v)}
					cases = append(cases, m)
				}
				// lzcnt / tzcnt: f3 [48] 0f bd/bc /r  (eax <- ecx; ecx <- ecx; eax <- [rdi])
				rex := ""
				if w {
					rex = "48"
				}
				for _, o := range []struct{ n, op string }{{"lzcnt", "bd"}, {"tzcnt", "bc"}} {
					for _, h := range []struct{ name, h string }{{"eax,edx", "f3" + rex + "0f" + o.op + "c2"}, {"edx,edx", "f3" + rex + "0f" + o.op + "d2"}, {"eax,[rdi]", "f3" + rex + "0f" + o.op + "07"}} {
						code := append([]string{h.h}, rd.code...)
						m := bmiCase(fmt.Sprintf("%s%s %s %s v=%#x", o.n, wn, h.name, rd.name, v), code, regs(), 0, rd.live)
						m.mem = map[int][]byte{0x100: binary.LittleEndian.AppendUint64(nil, v)}
						cases = append(cases, m)
					}
				}
			}
			// test r8,r8 / r16,r16 on assorted registers.
			for _, h := range []struct{ name, h string }{{"test al,al", "84c0"}, {"test cl,cl", "84c9"}, {"test dil,dil", "4084ff"}, {"test r9b,r9b", "4584c9"}, {"test ax,ax", "6685c0"}, {"test si,si", "6685f6"}, {"test ah,ah", "84e4"}} {
				code := append([]string{h.h}, rd.code...)
				m := bmiCase(fmt.Sprintf("%s %s v=%#x", h.name, rd.name, v), code, regs(), 0, rd.live)
				m.regs[x86asm.RAX], m.regs[x86asm.RDI], m.regs[x86asm.R9] = v, v|0x80, v
				m.regs[x86asm.RSI] = v >> 3
				cases = append(cases, m)
			}
			// xor r,r
			for _, h := range []struct{ name, h string }{{"xor eax,eax", "31c0"}, {"xor rcx,rcx", "4831c9"}, {"xor r8d,r8d", "4531c0"}, {"xor al,al", "30c0"}} {
				code := append([]string{h.h}, rd.code...)
				cases = append(cases, bmiCase(fmt.Sprintf("%s %s v=%#x", h.name, rd.name, v), code, regs(), 0, rd.live))
			}
		}
	}
	// narrow xor/or/and with an immediate, flags dead, with and without a folded movzx.
	for _, v := range bmiVals {
		for _, imm := range []byte{0x01, 0x80, 0xff, 0x05, 0x0f, 0x55, 0xaa, 0x03, 0x00, 0x7e, 0x24} {
			for _, o := range []struct{ n, op string }{{"xor", "34"}, {"or", "0c"}, {"and", "24"}} {
				for _, tail := range []struct {
					name string
					code []string
				}{{"", nil}, {"; movzx eax,al", []string{"0fb6c0"}}, {"; movzx ecx,al", []string{"0fb6c8"}}, {"; movzx rax,al", []string{"480fb6c0"}}, {"; movzx edx,al", []string{"0fb6d0"}}} {
					code := append([]string{o.op + fmt.Sprintf("%02x", imm)}, tail.code...)
					c := bmiCase(fmt.Sprintf("%s al,%#x%s v=%#x", o.n, imm, tail.name, v), code, map[x86asm.Reg]uint64{x86asm.RAX: v, x86asm.RCX: 0xdead00beef, x86asm.RDX: 0x7777}, 0, "")
					cases = append(cases, c)
				}
				// xor cl,imm (80 /6 ib on r/m8) — a register other than al.
			}
		}
	}
	// bextr with a constant control built by a mov in the block (and without: the general path).
	for _, rd := range bmiReaders {
		for _, w := range []bool{false, true} {
			for _, ctl := range []uint32{0x0408, 0x0800, 0x0000, 0x2000, 0x2005, 0x1f01, 0x2001, 0x0120, 0x01ff, 0xffff, 0x2040, 0x4004, 0x0020, 0x0a1f, 0x400a, 0x3f01, 0x0101} {
				for _, v := range []uint64{0, 1, 0xffffffff, 0x80000000, 0x123456789abcdef0, 0xffffffffffffffff, 0x00000000deadbeef} {
					for _, h := range []struct {
						name string
						code []string
					}{
						{"edx", []string{vexBextr(w, 0, 2, 1)}}, // bextr eax, edx, ecx
						{"alias", []string{vexBextr(w, 2, 2, 1)}},
						{"mem", []string{fmt.Sprintf("c4e2%02xf7%02x", byte((^1&0xf)<<3)|map[bool]byte{true: 0x80}[w], 0x07)}},
					} {
						code := append([]string{fmt.Sprintf("b9%02x%02x%02x%02x", byte(ctl), byte(ctl>>8), byte(ctl>>16), byte(ctl>>24))}, h.code...)
						code = append(code, rd.code...)
						c := bmiCase(fmt.Sprintf("bextr w=%v ctl=%#x %s %s v=%#x", w, ctl, h.name, rd.name, v), code,
							map[x86asm.Reg]uint64{x86asm.RDX: v, x86asm.RAX: 0x1111111122222222, x86asm.RDI: rb + 0x100}, 1, rd.live)
						c.mem = map[int][]byte{0x100: binary.LittleEndian.AppendUint64(nil, v)}
						cases = append(cases, c)
					}
				}
			}
		}
	}
	// cmp [mem], 0 ; je/jne +5 ; mov eax, 1
	for _, size := range []struct{ name, cmp string }{{"byte", "803f00"}, {"word", "66833f00"}, {"dword", "833f00"}, {"qword", "48833f00"}} {
		for _, jcc := range []string{"74", "75"} {
			for _, b := range [][]byte{{0, 0, 0, 0, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 0, 0, 0}, {0, 1, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0x80}, {0, 0, 0, 1, 0, 0, 0, 0}, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}} {
				c := bmiCase(fmt.Sprintf("cmp %s [rdi],0; j%s mem=% x", size.name, jcc, b), []string{size.cmp, jcc + "05", "b801000000"},
					map[x86asm.Reg]uint64{x86asm.RAX: 7, x86asm.RDI: rb + 0x100}, 0, "z", 1, "")
				c.mem = map[int][]byte{0x100: b}
				cases = append(cases, c)
			}
		}
	}
	return cases
}

func TestBmiPeepMatchesGeneralLowering(t *testing.T) {
	defer SetBMIPeep(GetBMIPeep())
	cases := bmiDiffCases()
	run := func(on bool) []byte {
		SetBMIPeep(on)
		var out []byte
		execCapture = &out
		defer func() { execCapture = nil }()
		runExecExpectTSO(t, cases, true)
		return out
	}
	off, on := run(false), run(true)
	if len(on) == 0 || len(on) != len(off) {
		t.Fatalf("output sizes %d vs %d", len(on), len(off))
	}
	rec := stSize + execMemSize
	bad := 0
	for i, c := range cases {
		a, b := on[i*rec:(i+1)*rec], off[i*rec:(i+1)*rec]
		if !bytes.Equal(a[:stFlags], b[:stFlags]) || !bytes.Equal(a[stSize:], b[stSize:]) {
			bad++
			if bad <= 20 {
				t.Errorf("%s: result with the peepholes differs from the general lowering\n on:  % x\n off: % x", c.name, a[:stFlags], b[:stFlags])
			}
		}
	}
	t.Logf("%d cases compared (%d differ)", len(cases), bad)
}

// The shapes: the host instructions of the lowerings the peepholes shorten, with the flags they leave live.
func TestBmiPeepShapes(t *testing.T) {
	defer SetBMIPeep(GetBMIPeep())
	SetBMIPeep(true)
	lower := func(c execCase) string {
		l := NewArm64Lifter(nil)
		l.SetComments(false)
		lines, _ := liftSeq(t, l, c, 0x10000)
		var out []string
		for _, ln := range lines {
			if s := strings.TrimSpace(ln); s != "" && !strings.HasSuffix(s, ":") && !strings.HasPrefix(s, "//") && !strings.HasPrefix(s, "cfinv") {
				out = append(out, s)
			}
		}
		return strings.Join(out, "; ")
	}
	r := map[x86asm.Reg]uint64{}
	for _, c := range []struct {
		name string
		c    execCase
		want string // substring that must appear
		n    int    // instructions of the lowering
	}{
		{"blsi, flags dead", bmiCase("", []string{vexBmi(3, false, 1, 2)}, r, 0, ""), "neg w1, w22; and w21, w22, w1", 2},
		{"blsi, zf live", bmiCase("", []string{vexBmi(3, false, 1, 2)}, r, 0, "z"), "neg w1, w22; ands w21, w22, w1", 2},
		{"blsi, cf live", bmiCase("", []string{vexBmi(3, false, 1, 2)}, r, 0, "c"), "ands w21, w22, w1; cset w2, ne; rmif x2, #63, #2", 4},
		{"blsr, flags dead", bmiCase("", []string{vexBmi(1, true, 1, 2)}, r, 0, ""), "sub x1, x22, #1; and x21, x22, x1", 2},
		{"lzcnt, flags dead", bmiCase("", []string{"f30fbdc2"}, r, 0, ""), "clz w19, w22", 1},
		{"tzcnt, flags dead", bmiCase("", []string{"f30fbcc2"}, r, 0, ""), "rbit w1, w22; clz w19, w1", 2},
		{"test al,al, zf only", bmiCase("", []string{"84c0"}, r, 0, "z"), "tst w19, #0xff", 1},
		{"test al,al, sf live", bmiCase("", []string{"84c0"}, r, 0, "zs"), "lsl w17, w19, #24; tst w17, w17", 2},
		{"xor al,1; movzx eax,al", bmiCase("", []string{"3401", "0fb6c0"}, r, 0, ""), "eor w16, w19, #0x1; uxtb w19, w16", 2},
		{"xor al,1", bmiCase("", []string{"3401"}, r, 0, ""), "eor w16, w19, #0x1; bfi x19, x16, #0, #8", 2},
		{"cmp byte [rdi],0; je", bmiCase("", []string{"803f00", "7405", "b801000000"}, r, 0, "z", 1, ""), "ldaprb w17, [x7]; cbz w17, ", 5},
		{"cmp qword [rdi],0; jne", bmiCase("", []string{"48833f00", "7505", "b801000000"}, r, 0, "z", 1, ""), "cbnz x17, ", 8},
		{"xor eax,eax, zf live", bmiCase("", []string{"31c0"}, r, 0, "z"), "ands w19, wzr, wzr", 1},
		{"xor eax,eax, flags dead", bmiCase("", []string{"31c0"}, r, 0, ""), "mov w19, wzr", 1},
		{"bextr, constant control", bmiCase("", []string{"b908040000", vexBextr(false, 0, 2, 1)}, r, 1, ""), "ubfx w19, w22, #8, #4", 2},
	} {
		got := lower(c.c)
		if !strings.Contains(got, c.want) {
			t.Errorf("%s: %q not in\n%s", c.name, c.want, got)
		}
		if n := strings.Count(got, ";") + 1; n > c.n+1 && c.n < 5 {
			t.Errorf("%s: %d instructions, want %d\n%s", c.name, n, c.n, got)
		}
	}
}
