package lifter

import (
	"encoding/binary"
	"fmt"
	"testing"

	"golang.org/x/arch/x86/x86asm"
)

// Regression tests for lowering bugs the differential fuzzer (arm64_liftfuzz_test.go) found,
// each against the x86 reference under Rosetta 2. The inputs live at [rdi], the results are
// stored as whole YMM registers at [rdi+0x60] (VEX.128 must zero bits 255:128).

const lfxBase = execMemBase + 0x100

// VPHMINPOSUW with the destination equal to the source wrote the minimum into word 0 before
// searching for its index, so the search always found index 0 (the title the fuzzer ran on has
// 12 sites, all of them aliased: vphminposuw xmmN, xmmN).
func TestExecVphminposuwAlias(t *testing.T) {
	inputs := [][8]uint16{
		{0xffff, 0xffff, 0, 0x8000, 7, 7, 7, 7},                // minimum at index 2
		{9, 8, 7, 6, 5, 4, 3, 2},                               // at the last index
		{5, 1, 1, 1, 1, 1, 1, 1},                               // ties: the lowest index wins
		{0, 0, 0, 0, 0, 0, 0, 0},                               // all equal
		{0x8000, 0x7fff, 0xffff, 0x8001, 0x7fff, 3, 2, 0x1234}, // unsigned compare
	}
	var cases []execCase
	for i, in := range inputs {
		mem := make([]byte, 0x40)
		for k, w := range in {
			binary.LittleEndian.PutUint16(mem[2*k:], w)
			binary.LittleEndian.PutUint16(mem[0x20+2*k:], w^0x5555)
		}
		for j := 16; j < 32; j++ {
			mem[j] = 0xa5 // upper half of ymm0: VEX.128 zeroes it
		}
		regs := map[x86asm.Reg]uint64{x86asm.RDI: lfxBase}
		cases = append(cases,
			execCase{name: fmt.Sprintf("vphminposuw xmm0,xmm0 #%d", i), ymm: true, regs: regs, mem: map[int][]byte{0x100: mem},
				code: []string{"c5fe6f07", "c4e27941c0", "c5fe7f4760"}},
			execCase{name: fmt.Sprintf("vphminposuw xmm1,xmm2 #%d", i), ymm: true, regs: regs, mem: map[int][]byte{0x100: mem},
				code: []string{"c5fe6f4f20", "c5fe6f17", "c4e27941ca", "c5fe7f4f60"}},
			execCase{name: fmt.Sprintf("vphminposuw xmm1,[rdi] #%d", i), ymm: true, regs: regs, mem: map[int][]byte{0x100: mem},
				code: []string{"c5fe6f4f20", "c4e279410f", "c5fe7f4f60"}},
		)
	}
	runExecCases(t, cases)
}

// VPERMILPD ymm, ymm/m256, imm8 selected the upper 128-bit lane's elements with imm8 bits 0-1
// instead of bits 2-3.
func TestExecVpermilpdYmmImm(t *testing.T) {
	mem := make([]byte, 0x60)
	for k := 0; k < 4; k++ {
		binary.LittleEndian.PutUint64(mem[0x20+8*k:], 0x1111111111111111*uint64(k+1))
		binary.LittleEndian.PutUint64(mem[0x40+8*k:], 0x0101010101010101*uint64(k+0xa))
	}
	regs := map[x86asm.Reg]uint64{x86asm.RDI: lfxBase}
	var cases []execCase
	for imm := 0; imm < 16; imm++ {
		cases = append(cases,
			execCase{name: fmt.Sprintf("vpermilpd ymm1,ymm2,%#x", imm), ymm: true, regs: regs, mem: map[int][]byte{0x100: mem},
				code: []string{"c5fe6f5720", fmt.Sprintf("c4e37d05ca%02x", imm), "c5fe7f4f60"}},
			execCase{name: fmt.Sprintf("vpermilpd ymm0,[rdi+0x40],%#x", imm), ymm: true, regs: regs, mem: map[int][]byte{0x100: mem},
				code: []string{fmt.Sprintf("c4e37d054740%02x", imm), "c5fe7f4760"}},
			execCase{name: fmt.Sprintf("vpermilpd ymm2,ymm2,%#x", imm), ymm: true, regs: regs, mem: map[int][]byte{0x100: mem},
				code: []string{"c5fe6f5720", fmt.Sprintf("c4e37d05d2%02x", imm), "c5fe7f5760"}},
		)
	}
	runExecCases(t, cases)
}
