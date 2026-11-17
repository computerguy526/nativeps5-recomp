package lifter

import (
	"fmt"
	"math/bits"
	"testing"

	"golang.org/x/arch/x86/x86asm"
)

// mulxHex encodes VEX.LZ.F2.0F38.W{0,1} F6 /r: MULX hi(modrm.reg), lo(vvvv), src(modrm.rm).
// Register numbers 0-7 only (rax rcx rdx rbx rsp rbp rsi rdi); mem means [src] (mod 00).
func mulxHex(w bool, hi, lo, src int, mem bool) string {
	b2 := byte(^lo&0xf)<<3 | 0x03 // vvvv (inverted), L=0, pp=F2
	if w {
		b2 |= 0x80
	}
	mod := byte(3)
	if mem {
		mod = 0
	}
	return fmt.Sprintf("c4e2%02xf6%02x", b2, mod<<6|byte(hi)<<3|byte(src))
}

// TestMulxAliasing: MULX must read RDX and the source before writing either destination,
// write lo then hi (hi wins when hi == lo; SDM: "If the first and second operands are
// identical, it will contain the high half"). Restates FEX unittests/ASM/VEX/mulx.asm
// ("mulx rax, rbx, rbx", "mulx edx, ecx, ecx", ...) plus the aliasing forms it omits.
func TestMulxAliasing(t *testing.T) {
	const (
		rax, rcx, rdx, rbx, rsi, rdi = 0, 1, 2, 3, 6, 7
	)
	R := map[int]x86asm.Reg{rax: x86asm.RAX, rcx: x86asm.RCX, rdx: x86asm.RDX, rbx: x86asm.RBX, rsi: x86asm.RSI, rdi: x86asm.RDI}
	const a64, s64 = 0xfedcba9876543215, 0x123456789abcdef1
	const a32, s32 = 0xfedcba98, 0x89abcdef // upper halves of the 64-bit regs hold junk
	const junk = 0xdeadbeef00000000
	hi64, lo64 := bits.Mul64(a64, s64)
	p32 := uint64(a32) * uint64(s32)
	hi32, lo32 := p32>>32, p32&0xffffffff

	type tc struct {
		name        string
		w           bool
		hi, lo, src int
		mem         bool
	}
	var cases []execCase
	var tcs []tc
	add := func(c tc) { tcs = append(tcs, c) }
	// 64-bit
	add(tc{name: "64 distinct", w: true, hi: rax, lo: rbx, src: rcx})
	add(tc{name: "64 hi==lo==rdx", w: true, hi: rdx, lo: rdx, src: rdi})
	add(tc{name: "64 lo==rdx", w: true, hi: rax, lo: rdx, src: rdi})
	add(tc{name: "64 hi==rdx", w: true, hi: rdx, lo: rax, src: rdi})
	add(tc{name: "64 hi==src", w: true, hi: rdi, lo: rax, src: rdi})
	add(tc{name: "64 lo==src", w: true, hi: rax, lo: rdi, src: rdi})
	add(tc{name: "64 hi==lo==src", w: true, hi: rdi, lo: rdi, src: rdi})
	add(tc{name: "64 mem", w: true, hi: rax, lo: rbx, src: rsi, mem: true})
	add(tc{name: "64 mem lo==rdx", w: true, hi: rax, lo: rdx, src: rsi, mem: true})
	// 32-bit
	add(tc{name: "32 distinct", hi: rax, lo: rbx, src: rcx})
	add(tc{name: "32 hi==lo==rdx", hi: rdx, lo: rdx, src: rdi})
	add(tc{name: "32 lo==rdx", hi: rax, lo: rdx, src: rdi})
	add(tc{name: "32 hi==src", hi: rdi, lo: rax, src: rdi})
	add(tc{name: "32 lo==src", hi: rax, lo: rdi, src: rdi})
	add(tc{name: "32 mem", hi: rax, lo: rbx, src: rsi, mem: true})

	for i := range tcs {
		c := &tcs[i]
		h, l := hi64, lo64
		a, s := uint64(a64), uint64(s64)
		if !c.w {
			h, l = hi32, lo32
			a, s = junk|a32, junk|s32
		}
		regs := map[x86asm.Reg]uint64{x86asm.RDX: a, x86asm.RAX: 0x1111, x86asm.RBX: 0x2222}
		var mem map[int][]byte
		if c.mem {
			regs[x86asm.RSI] = execMemBase + 0x200
			mem = map[int][]byte{0x200: le64(s)}
		} else {
			regs[R[c.src]] = s
		}
		// Expected registers: everything unchanged except lo then hi.
		want := map[x86asm.Reg]uint64{}
		for r, v := range regs {
			want[r] = v
		}
		set := func(reg int, v uint64) {
			if c.w {
				want[R[reg]] = v
			} else {
				want[R[reg]] = v & 0xffffffff // 32-bit write zero-extends
			}
		}
		set(c.lo, l)
		set(c.hi, h)
		cases = append(cases, execCase{name: "mulx " + c.name, code: []string{mulxHex(c.w, c.hi, c.lo, c.src, c.mem)},
			regs: regs, mem: mem, noFlags: true, wantRegs: want})
	}
	runExecExpect(t, cases, make([]map[int][]byte, len(cases)))
}
