package lifter

import (
	"encoding/binary"
	"fmt"
	"math"
	"math/bits"
	"testing"

	"golang.org/x/arch/x86/x86asm"
)

// Native tests of the lowerings written against FEX-Emu (https://github.com/FEX-Emu/FEX, MIT;
// see NOTICE): DF and backward string instructions, FCOMI/FCOMIP/FUCOMI/FUCOMIP, the x87
// constant loads, MMX PSHUFW, and lazy PF/AF with their readers (JP/JNP, SETP/SETNP,
// CMOVP/CMOVNP, FCMOVU/FCMOVNU, LAHF/SAHF, PUSHFQ/POPFQ).
//
// Only the lifted arm64 side runs (runExecExpect); no x86 code is executed. Expected values are
//   - the RegData of the FEX unit test named in each fexCase (arm64_fex_cases_test.go; x87 cases
//     use the reduced-precision X87_F64 variants where FEX has one, which is this lifter's
//     double-precision x87 model), or
//   - a Go model of the Intel SDM definition, for the sweeps over widths, operand forms and
//     directions that the FEX tests do not enumerate.
//
// Every case runs twice: lifted instruction by instruction with no liveness information (all
// flags live, PF always in the context bytes), and through the block and flag passes the
// emitter uses (execCase.cfg), where PF/AF are stored only when read and PF stays in host V
// after FP compares.

// bothModes returns the cases (those not marked cfg already) followed by cfg twins of all of
// them, with the expectations in the same order.
func bothModes(cases []execCase, want []map[int][]byte) ([]execCase, []map[int][]byte) {
	var out []execCase
	var outWant []map[int][]byte
	for i, c := range cases {
		if !c.cfg {
			out = append(out, c)
			outWant = append(outWant, want[i])
		}
	}
	for i, c := range cases {
		c.cfg = true
		c.name += " [cfg]"
		out = append(out, c)
		outWant = append(outWant, want[i])
	}
	return out, outWant
}

func TestFexCases(t *testing.T) {
	var cases []execCase
	var want []map[int][]byte
	for _, fc := range fexCases {
		c := fc.execCase
		c.name = fc.src + ": " + c.name
		if !fc.cfgOnly {
			cases = append(cases, c)
			want = append(want, fc.wantMem)
		}
		c.cfg = true
		c.name += " [cfg]"
		cases = append(cases, c)
		want = append(want, fc.wantMem)
	}
	runExecExpect(t, cases, want)
}

// x87 constants as doubles: FSTP m64 of each constant must give the nearest double of the exact
// value (the F64 FEX tests above check the float rounding of the same loads).
func TestX87ConstantsDouble(t *testing.T) {
	var cases []execCase
	var want []map[int][]byte
	for _, k := range []struct {
		name string
		op   string
		v    float64
	}{
		{"fld1", "d9e8", 1}, {"fldl2t", "d9e9", math.Log2(10)}, {"fldl2e", "d9ea", math.Log2E},
		{"fldpi", "d9eb", math.Pi}, {"fldlg2", "d9ec", math.Log10(2)}, {"fldln2", "d9ed", math.Ln2},
		{"fldz", "d9ee", 0},
	} {
		cases = append(cases, execCase{name: k.name, code: []string{k.op, "dd1b"}, // fstp qword [rbx]
			regs: map[x86asm.Reg]uint64{x86asm.RBX: execMemBase + 0x100}})
		want = append(want, map[int][]byte{0x100: le64(math.Float64bits(k.v))})
	}
	cases, want = bothModes(cases, want)
	runExecExpect(t, cases, want)
}

// ---- Direction flag: every string instruction, width, direction and repeat prefix ----

type subFlags struct{ cf, pf, af, zf, sf, of bool }

func (f subFlags) bits() uint64 {
	var v uint64
	for _, b := range []struct {
		on  bool
		bit uint64
	}{{f.cf, 1}, {f.pf, 4}, {f.af, 0x10}, {f.zf, 0x40}, {f.sf, 0x80}, {f.of, 0x800}} {
		if b.on {
			v |= b.bit
		}
	}
	return v
}

func parityEven(b uint64) bool { return bits.OnesCount8(uint8(b))%2 == 0 }

func widthMask(w int) uint64 {
	if w == 8 {
		return ^uint64(0)
	}
	return 1<<(8*w) - 1
}

// subModel is the SDM flag definition of a - b at w bytes.
func subModel(a, b uint64, w int) subFlags {
	m := widthMask(w)
	a, b = a&m, b&m
	r := (a - b) & m
	sign := uint64(1) << (8*w - 1)
	return subFlags{cf: a < b, pf: parityEven(r), af: (a^b^r)&0x10 != 0, zf: r == 0, sf: r&sign != 0,
		of: (a^b)&(a^r)&sign != 0}
}

func TestDirectionFlagStringOps(t *testing.T) {
	const srcOff, dstOff = 0x400, 0x500
	const acc = uint64(0x8877665544332211)
	type opDesc struct {
		name string
		opc  byte // 8-bit form; +1 for the wider ones
	}
	ops := []opDesc{{"movs", 0xa4}, {"cmps", 0xa6}, {"stos", 0xaa}, {"lods", 0xac}, {"scas", 0xae}}
	var cases []execCase
	var want []map[int][]byte
	for _, op := range ops {
		for _, w := range []int{1, 2, 4, 8} {
			for _, df := range []int{0, 1} {
				for _, rep := range []byte{0, 0xf3, 0xf2} {
					counts := []uint64{5}
					if rep != 0 {
						counts = []uint64{0, 2, 5}
					}
					for _, layout := range []int{0, 1} {
						for _, rcx := range counts {
							// Window: a filler pattern, then the first three elements each side
							// (in traversal order) arranged per layout.
							mem := make([]byte, 0x400)
							for i := range mem {
								mem[i] = byte(i*7 + 3)
							}
							const memBase = 0x300
							step := int64(w)
							if df == 1 {
								step = -step
							}
							put := func(off int, k int, v uint64) {
								var b [8]byte
								binary.LittleEndian.PutUint64(b[:], v)
								copy(mem[off-memBase+k*int(step):], b[:w])
							}
							a := acc & widthMask(w)
							x, y := (a^0x13)&widthMask(w), (a+0x25)&widthMask(w)
							if layout == 0 { // equal (and equal to the accumulator) twice, then different
								put(srcOff, 0, a)
								put(dstOff, 0, a)
								put(srcOff, 1, a)
								put(dstOff, 1, a)
								put(srcOff, 2, x)
								put(dstOff, 2, y)
							} else { // different twice, then equal (and equal to the accumulator)
								put(srcOff, 0, x)
								put(dstOff, 0, y)
								put(srcOff, 1, y)
								put(dstOff, 1, x)
								put(srcOff, 2, a)
								put(dstOff, 2, a)
							}
							init := append([]byte(nil), mem...)

							// Model (SDM): one element, or RCX of them with the REPE/REPNE exit.
							get := func(addr int64) uint64 {
								var b [8]byte
								copy(b[:], mem[addr-memBase:addr-memBase+int64(w)])
								return binary.LittleEndian.Uint64(b[:]) & widthMask(w)
							}
							set := func(addr int64, v uint64) {
								var b [8]byte
								binary.LittleEndian.PutUint64(b[:], v)
								copy(mem[addr-memBase:], b[:w])
							}
							rsi, rdi, rax, cnt := int64(srcOff), int64(dstOff), acc, rcx
							initFlags := uint64(fCF | fSF) // preserved by MOVS/STOS/LODS and by REP with RCX = 0
							flags := initFlags | 4         // PF of the fresh context byte (0: even parity), AF 0
							one := func() {
								switch op.name {
								case "movs":
									set(rdi, get(rsi))
									rsi += step
									rdi += step
								case "stos":
									set(rdi, rax)
									rdi += step
								case "lods":
									v := get(rsi)
									switch w {
									case 8, 4:
										rax = v
									default:
										rax = rax&^widthMask(w) | v
									}
									rsi += step
								case "scas":
									flags = subModel(rax, get(rdi), w).bits()
									rdi += step
								case "cmps":
									flags = subModel(get(rsi), get(rdi), w).bits()
									rsi += step
									rdi += step
								}
							}
							if rep == 0 {
								one()
							} else {
								for cnt != 0 {
									one()
									cnt--
									if op.name == "scas" || op.name == "cmps" {
										zf := flags&0x40 != 0
										if (rep == 0xf3 && !zf) || (rep == 0xf2 && zf) {
											break
										}
									}
								}
							}

							var enc []byte
							if w == 2 {
								enc = append(enc, 0x66)
							}
							if rep != 0 {
								enc = append(enc, rep)
							}
							if w == 8 {
								enc = append(enc, 0x48)
							}
							opc := op.opc
							if w > 1 {
								opc++
							}
							enc = append(enc, opc)
							dir := "fc" // cld
							if df == 1 {
								dir = "fd" // std
							}
							name := fmt.Sprintf("%s w%d df%d rep%02x layout%d rcx%d", op.name, w, df, rep, layout, rcx)
							cases = append(cases, execCase{name: name,
								code:  []string{dir, fmt.Sprintf("%x", enc), "9c", "4158"}, // op; pushfq; pop r8
								flags: initFlags,
								regs: map[x86asm.Reg]uint64{x86asm.RSI: execMemBase + srcOff, x86asm.RDI: execMemBase + dstOff,
									x86asm.RCX: rcx, x86asm.RAX: acc},
								mem: map[int][]byte{memBase: init},
								wantRegs: map[x86asm.Reg]uint64{
									x86asm.RSI: uint64(execMemBase + rsi), x86asm.RDI: uint64(execMemBase + rdi),
									x86asm.RCX: cnt, x86asm.RAX: rax,
									x86asm.R8: 0x202 | flags | uint64(df)<<10,
								}})
							want = append(want, map[int][]byte{memBase: append([]byte(nil), mem...)})
						}
					}
				}
			}
		}
	}
	cases, want = bothModes(cases, want)
	runExecExpect(t, cases, want)
}

// ---- PF and AF of the integer ALU, read through PUSHFQ, LAHF, SETP/SETNP and CMOVP ----

type aluOp struct {
	name string
	base byte // opcode of the r/m8, r8 form (binary ops)
	ext  byte // /n of the 80/83 immediate group, or of F6/F7 (neg) and FE/FF (inc/dec)
	kind int  // 0 binary, 1 compare-only, 2 neg, 3 inc/dec
}

var aluOps = []aluOp{
	{"add", 0x00, 0, 0}, {"or", 0x08, 1, 0}, {"adc", 0x10, 2, 0}, {"sbb", 0x18, 3, 0},
	{"and", 0x20, 4, 0}, {"sub", 0x28, 5, 0}, {"xor", 0x30, 6, 0}, {"cmp", 0x38, 7, 1},
	{"test", 0x84, 0, 1}, {"neg", 0xf6, 3, 2}, {"inc", 0xfe, 0, 3}, {"dec", 0xfe, 1, 3},
}

// aluModel returns the result and the six flags (SDM; AF of the logical operations, which the
// SDM leaves undefined, is 0 as on hardware and in FEX).
func aluModel(op string, a, b uint64, cin bool, w int) (uint64, uint64) {
	m := widthMask(w)
	sign := uint64(1) << (8*w - 1)
	a, b = a&m, b&m
	c := uint64(0)
	if cin {
		c = 1
	}
	var r uint64
	var cf, of, af bool
	switch op {
	case "add", "adc", "inc":
		if op == "inc" {
			b = 1
		}
		if op != "adc" {
			c = 0
		}
		r = (a + b + c) & m
		cf = r < a || (c == 1 && r == a)
		of = (a^r)&(b^r)&sign != 0
		af = (a^b^r)&0x10 != 0
	case "sub", "sbb", "cmp", "dec", "neg":
		if op == "dec" {
			b = 1
		}
		if op == "neg" {
			a, b = 0, a
		}
		if op != "sbb" {
			c = 0
		}
		r = (a - b - c) & m
		cf = a < b || (c == 1 && a == b)
		of = (a^b)&(a^r)&sign != 0
		af = (a^b^r)&0x10 != 0
	case "and", "test":
		r = a & b
	case "or":
		r = a | b
	case "xor":
		r = a ^ b
	}
	if op == "inc" || op == "dec" {
		cf = cin // preserved
	}
	f := subFlags{cf: cf, pf: parityEven(r), af: af, zf: r == 0, sf: r&sign != 0, of: of}
	return r, f.bits()
}

func TestParityAdjustALU(t *testing.T) {
	const memOff = 0x200
	values := [][2]uint64{{0, 0}, {0x0f, 1}, {0xff, 1}, {0x7f, 0x81}, {0x1234567f, 0x91},
		{0xffffffffffffffff, 1}, {0x8000000000000000, 0xffffffffffffffff}, {0x10, 0x23}}
	// Readers after the operation; each leaves what it read in registers.
	readers := []struct {
		name string
		code []string
		regs func(fl uint64) map[x86asm.Reg]uint64
	}{
		{"pushfq+lahf", []string{"9c", "4158", "9f", "0fb6ec"}, // pushfq; pop r8; lahf; movzx ebp, ah
			func(fl uint64) map[x86asm.Reg]uint64 {
				return map[x86asm.Reg]uint64{x86asm.R8: 0x202 | fl, x86asm.RBP: fl&0xd5 | 2}
			}},
		{"setp+cmovp", []string{"410f9ac1", "410f9bc2", "4d0f4ae3", "4d0f4bf5"}, // setp r9b; setnp r10b; cmovp r12, r11; cmovnp r14, r13
			func(fl uint64) map[x86asm.Reg]uint64 {
				p := fl >> 2 & 1
				m := map[x86asm.Reg]uint64{x86asm.R9: p, x86asm.R10: p ^ 1, x86asm.R12: 0x1200, x86asm.R14: 0x1400}
				if p == 1 {
					m[x86asm.R12] = 0x1100
				} else {
					m[x86asm.R14] = 0x1300
				}
				return m
			}},
	}
	var cases []execCase
	var want []map[int][]byte
	add := func(name string, enc []byte, op aluOp, w int, form string, a, b uint64, cin bool) {
		r, fl := aluModel(op.name, a, b, cin, w)
		m := widthMask(w)
		for _, rd := range readers {
			regs := map[x86asm.Reg]uint64{x86asm.RSI: execMemBase + memOff, x86asm.R11: 0x1100, x86asm.R12: 0x1200,
				x86asm.R13: 0x1300, x86asm.R14: 0x1400}
			wr := rd.regs(fl)
			mem := map[int][]byte{}
			wm := map[int][]byte{}
			merge := func(old uint64) uint64 { // what a w-byte write leaves in a 64-bit register
				switch w {
				case 8:
					return r
				case 4:
					return r
				}
				return old&^m | r
			}
			const fill = uint64(0xa5a5a5a5a5a5a5a5)
			writes := op.kind != 1
			switch form {
			case "rr", "imm", "unary": // destination RAX; source RBX or the immediate
				regs[x86asm.RAX], regs[x86asm.RBX] = a, b
				if writes && rd.name != "pushfq+lahf" { // LAHF then rewrites AH
					wr[x86asm.RAX] = merge(a)
				}
			case "ah": // destination AH, source BL
				regs[x86asm.RAX], regs[x86asm.RBX] = fill&^0xff00|(a&0xff)<<8, b
				if writes && rd.name != "pushfq+lahf" {
					wr[x86asm.RAX] = fill&^0xff00 | r<<8
				}
			case "mr", "munary": // destination [RSI]; source RBX
				regs[x86asm.RBX] = b
				mem[memOff] = le64(fill&^m | a&m)
				if writes {
					wm[memOff] = le64(fill&^m | r)
				} else {
					wm[memOff] = le64(fill&^m | a&m)
				}
			case "rm": // destination RBX; source [RSI]
				regs[x86asm.RBX] = a
				mem[memOff] = le64(b)
				if writes {
					wr[x86asm.RBX] = merge(a)
				}
			}
			fl0 := uint64(0)
			if cin {
				fl0 = fCF
			}
			cases = append(cases, execCase{name: fmt.Sprintf("%s %s a=%#x b=%#x cin=%v, %s", name, form, a, b, cin, rd.name),
				code: append([]string{fmt.Sprintf("%x", enc)}, rd.code...), regs: regs, flags: fl0, mem: mem, wantRegs: wr})
			want = append(want, wm)
		}
	}
	for _, op := range aluOps {
		for _, w := range []int{1, 2, 4, 8} {
			pre := []byte{}
			if w == 2 {
				pre = append(pre, 0x66)
			}
			if w == 8 {
				pre = append(pre, 0x48)
			}
			wide := byte(0)
			if w > 1 {
				wide = 1
			}
			cins := []bool{false}
			if op.name == "adc" || op.name == "sbb" || op.kind == 3 {
				cins = []bool{false, true}
			}
			for vi, v := range values {
				for _, cin := range cins {
					name := fmt.Sprintf("%s%d", op.name, 8*w)
					a, b := v[0], v[1]
					enc := func(bs ...byte) []byte { return append(append([]byte(nil), pre...), bs...) }
					switch op.kind {
					case 0, 1:
						add(name, enc(op.base+wide, 0xd8), op, w, "rr", a, b, cin) // op rax, rbx
						if vi%2 == 0 {
							add(name, enc(op.base+wide, 0x1e), op, w, "mr", a, b, cin) // op [rsi], rbx
							if op.name != "test" {
								add(name, enc(op.base+2+wide, 0x1e), op, w, "rm", a, b, cin) // op rbx, [rsi]
							}
						}
						if op.name != "test" && w > 1 { // op rax, imm8 (sign-extended)
							imm := uint64(int64(int8(b)))
							add(name, enc(0x83, 0xc0|op.ext<<3, byte(b)), op, w, "imm", a, imm, cin)
						}
						if w == 1 && op.name != "test" {
							add(name+"(ah)", []byte{op.base, 0xdc}, op, w, "ah", a, b, cin) // op ah, bl
						}
					case 2, 3:
						add(name, enc(op.base+wide, 0xc0|op.ext<<3), op, w, "unary", a, 0, cin) // op rax
						if vi%2 == 0 {
							add(name, enc(op.base+wide, op.ext<<3|6), op, w, "munary", a, 0, cin) // op [rsi]
						}
					}
				}
			}
		}
	}
	cases, want = bothModes(cases, want)
	runExecExpect(t, cases, want)
}

// Other PF/AF writers and the paths where PF moves between host V and the context byte.
// Expected values follow the SDM; the comments give the arithmetic.
func TestParityAdjustOther(t *testing.T) {
	b := uint64(execMemBase)
	f32 := func(v ...float32) []byte {
		var out []byte
		for _, x := range v {
			out = binary.LittleEndian.AppendUint32(out, math.Float32bits(x))
		}
		return out
	}
	nan := float32(math.NaN())
	cases := []execCase{
		// xadd: flags of the addition 0x0f + 0x01 = 0x10: AF, PF clear (one bit).
		{name: "xadd eax, ebx", code: []string{"0fc1d8", "9c", "4158"}, // xadd eax, ebx; pushfq; pop r8
			regs:     map[x86asm.Reg]uint64{x86asm.RAX: 0x0f, x86asm.RBX: 1},
			wantRegs: map[x86asm.Reg]uint64{x86asm.RAX: 0x10, x86asm.RBX: 0x0f, x86asm.R8: 0x212}},
		// lock xadd [rsi], ebx: 0xff + 1 = 0x100: PF (low byte 0), AF.
		{name: "lock xadd [rsi], ebx", code: []string{"f00fc11e", "9c", "4158"},
			regs:     map[x86asm.Reg]uint64{x86asm.RSI: b + 0x100, x86asm.RBX: 1},
			mem:      map[int][]byte{0x100: le64(0xff)},
			wantRegs: map[x86asm.Reg]uint64{x86asm.RBX: 0xff, x86asm.R8: 0x216}},
		// cmpxchg: flags of CMP EAX, dest. 0x10 - 0x01 = 0x0f: PF (four bits), AF (borrow from bit 4).
		{name: "cmpxchg ebx, ecx (miss)", code: []string{"0fb1cb", "9c", "4158"}, // cmpxchg ebx, ecx
			regs:     map[x86asm.Reg]uint64{x86asm.RAX: 0x10, x86asm.RBX: 1, x86asm.RCX: 7},
			wantRegs: map[x86asm.Reg]uint64{x86asm.RAX: 1, x86asm.RBX: 1, x86asm.R8: 0x216}},
		// lock cmpxchg hit: 5 - 5 = 0: ZF, PF.
		{name: "lock cmpxchg [rsi], ecx (hit)", code: []string{"f00fb10e", "9c", "4158"},
			regs:     map[x86asm.Reg]uint64{x86asm.RAX: 5, x86asm.RSI: b + 0x100, x86asm.RCX: 7},
			mem:      map[int][]byte{0x100: le64(5)},
			wantRegs: map[x86asm.Reg]uint64{x86asm.RAX: 5, x86asm.R8: 0x246}},
		// popcnt clears PF and AF (and everything but ZF), whatever the result's parity (3: even).
		{name: "popcnt after an even-parity add", code: []string{"01d8", "f30fb8cf", "9c", "4158"}, // add eax, ebx; popcnt ecx, edi
			regs:     map[x86asm.Reg]uint64{x86asm.RAX: 0x0f, x86asm.RBX: 0xf1, x86asm.RDI: 0x0b},
			wantRegs: map[x86asm.Reg]uint64{x86asm.RCX: 3, x86asm.R8: 0x202}},
		// ptest clears PF and AF; ZF = 1 here ((xmm0 & xmm0) == 0), CF = 1 ((~xmm0 & xmm0) == 0).
		{name: "ptest xmm0, xmm0", code: []string{"01d8", "660fefc0", "660f3817c0", "9c", "4158"}, // add; pxor xmm0, xmm0; ptest
			regs:     map[x86asm.Reg]uint64{x86asm.RAX: 0x0f, x86asm.RBX: 0xf1},
			wantRegs: map[x86asm.Reg]uint64{x86asm.R8: 0x243}},
		// Shifts: PF of the result. 3 << 1 = 6 (even parity); 6 >> 1 = 3 (even); 0x80 sar 7 = 0xff.
		{name: "shl eax, 1; setp", code: []string{"d1e0", "0f9ac1"}, regs: map[x86asm.Reg]uint64{x86asm.RAX: 3},
			wantRegs: map[x86asm.Reg]uint64{x86asm.RAX: 6, x86asm.RCX: 1}},
		{name: "shr byte [rsi], 2; setp", code: []string{"c02e02", "0f9ac1"}, regs: map[x86asm.Reg]uint64{x86asm.RSI: b + 0x100},
			mem: map[int][]byte{0x100: {0x1c}}, wantRegs: map[x86asm.Reg]uint64{x86asm.RCX: 0}}, // 0x1c >> 2 = 7: odd
		{name: "sar ah, 7; setnp", code: []string{"c0fc07", "0f9bc2"}, regs: map[x86asm.Reg]uint64{x86asm.RAX: 0x8000},
			wantRegs: map[x86asm.Reg]uint64{x86asm.RAX: 0xff00, x86asm.RDX: 0}},
		// Shift of RCX by CL: the count register is the destination. 0x103 << 3 = 0x818: low byte 0x18, even.
		{name: "shl ecx, cl; setp", code: []string{"83c001", "d3e1", "0f9ac2"}, // add eax, 1 (PF odd: 1); shl ecx, cl; setp dl
			regs:     map[x86asm.Reg]uint64{x86asm.RCX: 0x103},
			wantRegs: map[x86asm.Reg]uint64{x86asm.RCX: 0x818, x86asm.RDX: 1}},
		// Count 0 in CL (0x20 & 0x1f): PF of the add before survives (0x0f + 0xf1 = 0x100: even).
		{name: "shl edx, cl with count 0 keeps PF", code: []string{"01d8", "d3e2", "0f9ac0"}, // add eax, ebx; shl edx, cl; setp al
			regs:     map[x86asm.Reg]uint64{x86asm.RAX: 0x0f, x86asm.RBX: 0xf1, x86asm.RCX: 0x20, x86asm.RDX: 1},
			wantRegs: map[x86asm.Reg]uint64{x86asm.RDX: 1, x86asm.RAX: 0x101}},
		// shld eax, ebx, 4: (0x00000001 << 4) | (0xf0000000 >> 28) = 0x1f: five bits, odd.
		{name: "shld eax, ebx, 4; setp", code: []string{"0fa4d804", "0f9ac1"},
			regs:     map[x86asm.Reg]uint64{x86asm.RAX: 1, x86asm.RBX: 0xf0000000},
			wantRegs: map[x86asm.Reg]uint64{x86asm.RAX: 0x1f, x86asm.RCX: 0}},
		// shrd eax, ebx, cl with CL = 8: 0x00000300 >> 8 | 0x1 << 24 = 0x01000003: low byte 3, even.
		{name: "shrd eax, ebx, cl; setp", code: []string{"0fadd8", "0f9ac2"},
			regs:     map[x86asm.Reg]uint64{x86asm.RAX: 0x300, x86asm.RBX: 1, x86asm.RCX: 8},
			wantRegs: map[x86asm.Reg]uint64{x86asm.RAX: 0x01000003, x86asm.RDX: 1}},
		// FP compare, then the whole flags image: unordered gives ZF = PF = CF = 1; OF, SF, AF are 0
		// even though they were set before (sub 0x80000000 - 1 leaves OF and AF). Analysed code
		// only (cfg): PUSHFQ learns from the PF-location pass that V holds PF and OF is 0.
		{name: "ucomiss unordered; pushfq", cfg: true, code: []string{"83e801", "f30f1006", "0f2e4604", "9c", "4158"}, // sub eax,1; movss xmm0,[rsi]; ucomiss xmm0,[rsi+4]
			regs: map[x86asm.Reg]uint64{x86asm.RAX: 0x80000000, x86asm.RSI: b + 0x100}, mem: map[int][]byte{0x100: f32(1, nan)},
			wantRegs: map[x86asm.Reg]uint64{x86asm.R8: 0x247}},
		{name: "ucomiss less; pushfq", cfg: true, code: []string{"83e801", "f30f1006", "0f2e4604", "9c", "4158"},
			regs: map[x86asm.Reg]uint64{x86asm.RAX: 0x80000000, x86asm.RSI: b + 0x100}, mem: map[int][]byte{0x100: f32(1, 2)},
			wantRegs: map[x86asm.Reg]uint64{x86asm.R8: 0x203}},
		{name: "comisd equal; lahf", code: []string{"f20f1006", "660f2f06", "9f", "0fb6cc"}, // movsd xmm0,[rsi]; comisd xmm0,[rsi]; lahf; movzx ecx, ah
			regs: map[x86asm.Reg]uint64{x86asm.RSI: b + 0x100}, mem: map[int][]byte{0x100: le64(math.Float64bits(2.5))},
			wantRegs: map[x86asm.Reg]uint64{x86asm.RCX: 0x42}},
		// PF from an FP compare, then from an integer result: the reader must follow the last writer.
		{name: "ucomiss unordered; setp; add; setp", code: []string{"f30f1006", "0f2e4604", "0f9ac1", "83c001", "0f9ac2"},
			regs: map[x86asm.Reg]uint64{x86asm.RSI: b + 0x100}, mem: map[int][]byte{0x100: f32(nan, 1)},
			wantRegs: map[x86asm.Reg]uint64{x86asm.RCX: 1, x86asm.RDX: 0}}, // 0 + 1 = 1: odd
		// A rotate keeps PF but its lowering rewrites host V (OF of the rotate): the V-held PF is
		// stored first.
		{name: "ucomiss unordered; rol; setp", code: []string{"f30f1006", "0f2e4604", "d1c0", "0f9ac1", "0f90c2"}, // rol eax,1; setp cl; seto dl
			regs: map[x86asm.Reg]uint64{x86asm.RAX: 1, x86asm.RSI: b + 0x100}, mem: map[int][]byte{0x100: f32(nan, 1)},
			wantRegs: map[x86asm.Reg]uint64{x86asm.RAX: 2, x86asm.RCX: 1, x86asm.RDX: 0}},
		{name: "ucomiss ordered; ror; setp", code: []string{"f30f1006", "0f2e4604", "d1c8", "0f9ac1", "0f90c2"}, // ror eax,1: OF = msb ^ next = 1
			regs: map[x86asm.Reg]uint64{x86asm.RAX: 1, x86asm.RSI: b + 0x100}, mem: map[int][]byte{0x100: f32(3, 1)},
			wantRegs: map[x86asm.Reg]uint64{x86asm.RAX: 0x80000000, x86asm.RCX: 0, x86asm.RDX: 1}},
		// SAHF / POPFQ write PF and AF; the following readers see them.
		{name: "sahf PF only; setp; lahf", code: []string{"b404", "9e", "0f9ac1", "9f", "0fb6d4"}, // mov ah,4; sahf; setp cl; lahf; movzx edx, ah
			wantRegs: map[x86asm.Reg]uint64{x86asm.RCX: 1, x86asm.RDX: 0x06}},
		{name: "sahf AF only; setp; lahf", code: []string{"b410", "9e", "0f9ac1", "9f", "0fb6d4"},
			wantRegs: map[x86asm.Reg]uint64{x86asm.RCX: 0, x86asm.RDX: 0x12}},
		{name: "popfq all; pushfq", code: []string{"68d50c0000", "9d", "9c", "4158", "fc"}, // push 0xcd5; popfq; pushfq; pop r8; cld
			wantRegs: map[x86asm.Reg]uint64{x86asm.R8: 0xed7}},
		{name: "popfq sets DF: stosb goes down", code: []string{"6800040000", "9d", "aa", "fc", "aa"}, // push 0x400; popfq; stosb; cld; stosb
			regs: map[x86asm.Reg]uint64{x86asm.RAX: 0x5a, x86asm.RDI: b + 0x100}, wantRegs: map[x86asm.Reg]uint64{x86asm.RDI: b + 0x100}},
		// std; pushfq: DF is bit 10 (t_arith's df_flags).
		{name: "std; pushfq; cld; pushfq", code: []string{"fd", "9c", "4158", "fc", "9c", "4159"},
			wantRegs: map[x86asm.Reg]uint64{x86asm.R8: 0x606 | 0, x86asm.R9: 0x206}},
	}
	want := make([]map[int][]byte, len(cases))
	want[1] = map[int][]byte{0x100: le64(0x100)}
	want[3] = map[int][]byte{0x100: le64(7)}
	for i, c := range cases {
		if c.name == "shr byte [rsi], 2; setp" {
			want[i] = map[int][]byte{0x100: {7}}
		}
		if c.name == "popfq sets DF: stosb goes down" {
			want[i] = map[int][]byte{0xff: {0x5a, 0x5a}}
		}
	}
	cases, want = bothModes(cases, want)
	runExecExpect(t, cases, want)
}

// Branches on PF across blocks: the PF-location pass decides where each JP/JNP reads PF.
func TestParityAcrossBlocks(t *testing.T) {
	b := uint64(execMemBase)
	f32 := func(x, y float32) []byte {
		out := binary.LittleEndian.AppendUint32(nil, math.Float32bits(x))
		return binary.LittleEndian.AppendUint32(out, math.Float32bits(y))
	}
	nan := float32(math.NaN())
	// clang's x == y: ucomiss; jne out; jp out; (equal). The JP block is entered only from the
	// JNE block: PF stays in host V.
	eq := []string{
		"f30f1006",   // movss xmm0, [rsi]
		"0f2e4604",   // ucomiss xmm0, [rsi+4]
		"b800000000", // mov eax, 0
		"7507",       // jne out
		"7a05",       // jp out
		"b801000000", // mov eax, 1
	}
	// Two ways into the block that reads PF: an FP compare (PF in V, stored on the way out) and
	// an integer add (PF in the context byte).
	join := []string{
		"f30f1006",   // movss xmm0, [rsi]
		"85c9",       // test ecx, ecx
		"7406",       // je int
		"0f2e4604",   // ucomiss xmm0, [rsi+4]
		"eb02",       // jmp join
		"01da",       // int: add edx, ebx
		"0f9ac0",     // join: setp al
		"7a05",       // jp end
		"bf07000000", // mov edi, 7
	}
	var cases []execCase
	for _, d := range []struct {
		name string
		x, y float32
		want uint64
	}{{"equal", 2, 2, 1}, {"different", 2, 3, 0}, {"unordered", nan, 2, 0}} {
		cases = append(cases, execCase{name: "x == y: " + d.name, cfg: true, code: eq,
			regs: map[x86asm.Reg]uint64{x86asm.RSI: b + 0x100}, mem: map[int][]byte{0x100: f32(d.x, d.y)},
			wantRegs: map[x86asm.Reg]uint64{x86asm.RAX: d.want}})
	}
	for _, d := range []struct {
		name     string
		ecx      uint64
		x, y     float32
		edx, ebx uint64
		pf       uint64
	}{
		{"fp path, unordered", 1, nan, 1, 0, 0, 1}, {"fp path, ordered", 1, 1, 2, 0, 0, 0},
		{"int path, even", 0, nan, 1, 1, 2, 1}, {"int path, odd", 0, 1, 2, 1, 1, 0}, // 1+2 = 3 even; 1+1 = 2 odd
	} {
		edi := uint64(7)
		if d.pf == 1 {
			edi = 0x99
		}
		edx := d.edx
		if d.ecx == 0 {
			edx += d.ebx
		}
		cases = append(cases, execCase{name: "join: " + d.name, cfg: true, code: join,
			regs:     map[x86asm.Reg]uint64{x86asm.RSI: b + 0x100, x86asm.RCX: d.ecx, x86asm.RDX: d.edx, x86asm.RBX: d.ebx, x86asm.RDI: 0x99},
			mem:      map[int][]byte{0x100: f32(d.x, d.y)},
			wantRegs: map[x86asm.Reg]uint64{x86asm.RAX: d.pf, x86asm.RDI: edi, x86asm.RDX: edx}})
	}
	runExecExpect(t, cases, make([]map[int][]byte, len(cases)))

	// The lowerings themselves: V-held PF in the x == y shape, the context byte after the join.
	// With a RET at the end, as in a real function, no flag is live out of the sequence.
	eq, join = append(eq, "c3"), append(join, "c3")
	l := NewArm64Lifter(nil)
	l.SetPeephole(true)
	text := liftSeqCFG(t, l, execCase{name: "eq", code: eq}, 0x10000)
	requireInOrder(t, "x == y", text, "fcmp s0, s16", "b.ne ...", "b.vs ...")
	forbid(t, "x == y", text, "strb w16, [x17, #0x9c]", "ldrb w16, [x17, #0x9c]")
	text = liftSeqCFG(t, l, execCase{name: "join", code: join}, 0x20000)
	requireInOrder(t, "join", text, "fcmp s0, s16", "cset w16, vc", "strb w16, [x17, #0x9c]", "b L_fn_0x20000_loc_...",
		"strb w5, [x16, #0x9c]", "ldrb w16, [x17, #0x9c]", "rmif x16, #0, #1", "cset w16, vs",
		"ldrb w16, [x17, #0x9c]", "tbnz w16, #0, ...")
}

// MMX PSHUFW over all 256 immediates, register and memory sources, against the definition
// (word i of the result = word imm[2i+1:2i] of the source). FEX's MMX/pshufw.asm does the same
// sweep with a CRC; TwoByte/0F_70.asm is in fexCases.
func TestMmxPshufwAllImmediates(t *testing.T) {
	src := uint64(0x4142434445464748)
	shuf := func(imm int) uint64 {
		var r uint64
		for i := 0; i < 4; i++ {
			r |= (src >> (16 * (imm >> (2 * i) & 3)) & 0xffff) << (16 * i)
		}
		return r
	}
	var cases []execCase
	var want []map[int][]byte
	for _, memSrc := range []bool{false, true} {
		for lo := 0; lo < 256; lo += 64 {
			code := []string{"0f6f0e"} // movq mm1, [rsi]
			exp := map[int][]byte{}
			for imm := lo; imm < lo+64; imm++ {
				if memSrc {
					code = append(code, fmt.Sprintf("0f7006%02x", imm)) // pshufw mm0, [rsi], imm
				} else {
					code = append(code, fmt.Sprintf("0f70c1%02x", imm)) // pshufw mm0, mm1, imm
				}
				off := 8 * (imm - lo)
				code = append(code, fmt.Sprintf("0f7f87%08x", bits.ReverseBytes32(uint32(off)))) // movq [rdi+off], mm0
				exp[0x400+off] = le64(shuf(imm))
			}
			cases = append(cases, execCase{name: fmt.Sprintf("pshufw mem=%v imm %d..", memSrc, lo), code: code,
				regs: map[x86asm.Reg]uint64{x86asm.RSI: execMemBase + 0x100, x86asm.RDI: execMemBase + 0x400},
				mem:  map[int][]byte{0x100: le64(src)}})
			want = append(want, exp)
		}
	}
	runExecExpect(t, cases, want)
}
