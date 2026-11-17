package main

import (
	"bytes"
	"debug/elf"
	"encoding/binary"
	"fmt"
	"sort"
	"strings"

	"ps4-recomp/pkg/disasm"
	"ps4-recomp/pkg/elfloader"
	"ps4-recomp/pkg/lifter"

	"golang.org/x/arch/x86/x86asm"
)

// synthELF wraps blob in a minimal ET_EXEC x86-64 image with one R+X PT_LOAD at base.
func synthELF(blob []byte, base uint64) []byte {
	const off = 0x1000
	var b bytes.Buffer
	le := binary.LittleEndian
	eh := make([]byte, 64)
	copy(eh, []byte{0x7f, 'E', 'L', 'F', 2, 1, 1, 0})
	le.PutUint16(eh[16:], uint16(elf.ET_EXEC))
	le.PutUint16(eh[18:], uint16(elf.EM_X86_64))
	le.PutUint32(eh[20:], 1)
	le.PutUint64(eh[24:], base)
	le.PutUint64(eh[32:], 64) // phoff
	le.PutUint16(eh[52:], 64)
	le.PutUint16(eh[54:], 56)
	le.PutUint16(eh[56:], 1)
	b.Write(eh)
	ph := make([]byte, 56)
	le.PutUint32(ph[0:], uint32(elf.PT_LOAD))
	le.PutUint32(ph[4:], uint32(elf.PF_R|elf.PF_X))
	le.PutUint64(ph[8:], off)
	le.PutUint64(ph[16:], base)
	le.PutUint64(ph[24:], base)
	le.PutUint64(ph[32:], uint64(len(blob)))
	le.PutUint64(ph[40:], uint64(len(blob)))
	le.PutUint64(ph[48:], 0x1000)
	b.Write(ph)
	b.Write(make([]byte, off-b.Len()))
	b.Write(blob)
	return b.Bytes()
}

// Host registers of rax rcx rdx rbx rsp rbp rsi rdi r8-r15 (the lifter's pinned mapping).
var regOrder = []string{"x19", "x21", "x22", "x20", "x26", "x25", "x23", "x24", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15"}

// liftBlob lifts the test blob like ps4-recomp does for an ELF: it discovers the functions
// reachable from base, lifts every block, and adds the host entry/exit and the indirect-transfer
// dispatchers. It returns the assembly, the non-setup mnemonics, and any unsupported forms.
func liftBlob(blob []byte, base uint64, f80 bool) (asm string, mnems []string, forms []string, err error) {
	// The disassembler treats HLT as a privileged opcode, i.e. data on a wrong boundary, and
	// stops the block before it. FEX tests end with HLT, so each HLT at the end of a block is
	// patched to INT3 (the same one-byte, block-ending class) and lifted as the end marker.
	blob = append([]byte(nil), blob...)
	hlts := map[uint64]bool{}
	var d *disasm.Disassembler
	var funcs []disasm.ReachableFunc
	// seeds are code addresses taken by LEA rip / MOV imm in a test that transfers control
	// indirectly: a real ELF names such targets in its symbols or unwind data, this one does not.
	seeds := map[uint64]bool{}
	entries := func() []uint64 {
		e := []uint64{base}
		for a := range seeds {
			e = append(e, a)
		}
		sort.Slice(e, func(i, j int) bool { return e[i] < e[j] })
		return e
	}
	for iter := 0; ; iter++ {
		loaded, err := elfloader.LoadELFBytes(synthELF(blob, base))
		if err != nil {
			return "", nil, nil, err
		}
		d, err = disasm.NewDisassembler(loaded)
		if err != nil {
			return "", nil, nil, err
		}
		vecLive, okVec := disasm.ParseVecLiveMode(*flSimdLive)
		if !okVec {
			return "", nil, nil, fmt.Errorf("-simd-liveness %q: want strict, abi or off", *flSimdLive)
		}
		d.VecLive = vecLive
		funcs, err = d.DiscoverReachable(entries(), nil)
		if err != nil {
			return "", nil, nil, err
		}
		added := false
		indirect := false
		cands := map[uint64]bool{}
		for _, f := range funcs {
			fn, _, err := d.DisasmFunction(f.Addr)
			if err != nil || fn == nil {
				continue
			}
			for _, b := range fn.Blocks {
				for _, in := range b.Insts {
					// A branch to an HLT: the block is never formed (see above); patch the target.
					if rel, ok := in.Inst.Args[0].(x86asm.Rel); ok {
						t := uint64(int64(in.Address) + int64(in.Inst.Len) + int64(rel))
						if t >= base && t-base < uint64(len(blob)) && blob[t-base] == 0xF4 && !hlts[t] {
							hlts[t] = true
							blob[t-base] = 0xCC
							added = true
						}
					}
					switch in.Inst.Op {
					case x86asm.CALL, x86asm.JMP:
						if _, rel := in.Inst.Args[0].(x86asm.Rel); !rel {
							indirect = true
						}
					case x86asm.RET, x86asm.LRET, x86asm.IRETQ:
						// Flat call model: a RET goes where the guest stack says, so a pushed
						// code address is a branch target like any other (the nested model
						// returns to the host caller whatever was pushed).
						if lifter.GetCallModel() == lifter.CallFlat {
							indirect = true
						}
					case x86asm.LEA:
						if m, ok := in.Inst.Args[1].(x86asm.Mem); ok && m.Base == x86asm.RIP {
							cands[uint64(int64(in.Address)+int64(in.Inst.Len)+m.Disp)] = true
						}
					case x86asm.MOV:
						if im, ok := in.Inst.Args[1].(x86asm.Imm); ok {
							cands[uint64(im)] = true
						}
					}
				}
				if len(b.Insts) == 0 {
					continue
				}
				li := b.Insts[len(b.Insts)-1]
				switch li.Inst.Op {
				case x86asm.JMP, x86asm.RET, x86asm.UD2, x86asm.INT, x86asm.HLT:
					continue
				}
				e := li.Address + uint64(li.Inst.Len)
				if e-base < uint64(len(blob)) && blob[e-base] == 0xF4 && !hlts[e] {
					hlts[e] = true
					blob[e-base] = 0xCC
					added = true
				}
			}
		}
		if indirect {
			for a := range cands {
				if a > base && a < base+uint64(len(blob)) && !seeds[a] {
					seeds[a] = true
					added = true
					// A seed that is the test's final HLT: patch it like a branch target.
					if blob[a-base] == 0xF4 && !hlts[a] {
						hlts[a] = true
						blob[a-base] = 0xCC
					}
				}
			}
		}
		if !added || iter > 200 {
			break
		}
	}
	known := map[uint64]bool{}
	for _, f := range funcs {
		known[f.Addr] = true
	}
	if !known[base] {
		return "", nil, nil, fmt.Errorf("entry not discovered")
	}
	l := lifter.NewArm64Lifter(known)
	l.SetTSO(*flTSO)
	l.SetVecLiveness(*flSimdLive != "off")
	if m, err := lifter.ParseTSOElide(*flTSOElide); err != nil {
		return "", nil, nil, err
	} else {
		l.SetTSOElide(m)
	}
	if f80 {
		l.SetX87Mode("f80")
	}
	l.SetAllowUnsupported(true)

	var w, cold strings.Builder
	var blocks [][2]uint64 // (function entry, block address) for the indirect-jump dispatcher
	flat := lifter.GetCallModel() == lifter.CallFlat
	type cont struct {
		guest uint64
		label string
	}
	var conts []cont // flat model: return sites that are not block starts
	opSet := map[string]bool{}
	w.WriteString(".section __TEXT,__text,regular,pure_instructions\n.p2align 2\n")
	for _, f := range funcs {
		fn, _, err := d.DisasmFunction(f.Addr)
		if err != nil || fn == nil {
			return "", nil, nil, fmt.Errorf("function 0x%x: %v", f.Addr, err)
		}
		fmt.Fprintf(&w, ".globl _fn_0x%x\n_fn_0x%x:\n", f.Addr, f.Addr)
		if flat {
			// Call entry (shadow entry push), then the body: the emitter's layout.
			w.WriteString(strings.Join(lifter.FlatCallEntry(), "\n") + "\n")
			w.WriteString(strings.Join(lifter.FlatJumpEntryLabel(fmt.Sprintf("_fn_0x%x", f.Addr)), "\n") + "\n")
		} else {
			w.WriteString("    str x30, [sp, #-16]!\n")
		}
		raSeen := map[uint64]bool{}
		for bi, blockAddr := range fn.BlockOrder {
			block := fn.Blocks[blockAddr]
			if block == nil {
				continue
			}
			blocks = append(blocks, [2]uint64{f.Addr, blockAddr})
			fmt.Fprintf(&w, "\nL_fn_0x%x_loc_0x%x:\n", f.Addr, blockAddr)
			l.ResetFlags()
			l.SetBlock(block)
			var last disasm.Instruction
			for _, inst := range block.Insts {
				last = inst
				if !(inst.Inst.Op == x86asm.INT && hlts[inst.Address]) {
					opSet[mnemonic(inst.Inst)] = true
				}
				nextPC := inst.Address + uint64(inst.Inst.Len)
				if inst.Inst.Op == x86asm.HLT || (inst.Inst.Op == x86asm.INT && hlts[inst.Address]) {
					fmt.Fprintf(&w, "    // 0x%x: hlt\n    b _fex_exit\n", inst.Address)
					continue
				}
				lines, lerr := l.LiftInstructionToBuf(inst, nextPC, fn, nil)
				if lerr != nil {
					l.RecordUnsupported(inst, f.Addr, lerr.Error())
					fmt.Fprintf(&w, "    // 0x%x: [UNSUPPORTED: %v]\n    brk #0x777\n", inst.Address, lerr)
					continue
				}
				for _, ln := range lines {
					w.WriteString(ln)
					w.WriteByte('\n')
				}
				for _, ln := range l.TakeCold() { // outlined rare paths: written after the function
					cold.WriteString(ln)
					cold.WriteByte('\n')
				}
				// Flat model: a return address is a place the slow return path can continue at.
				if flat && inst.Inst.Op == x86asm.CALL && !raSeen[nextPC] && fn.Blocks[nextPC] == nil {
					raSeen[nextPC] = true
					lbl := lifter.ReturnSiteLabel(f.Addr, nextPC)
					fmt.Fprintf(&w, "%s:\n", lbl)
					conts = append(conts, cont{nextPC, lbl})
				}
			}
			if len(block.Insts) > 0 {
				for _, ln := range l.EndBlock(last) {
					w.WriteString(ln + "\n")
				}
				var next uint64
				hasNext := false
				for _, a := range fn.BlockOrder[bi+1:] {
					if fn.Blocks[a] != nil {
						next, hasNext = a, true
						break
					}
				}
				for _, ln := range l.FallThrough(fn, last, next, hasNext) {
					w.WriteString(ln + "\n")
				}
			}
		}
		if flat {
			fmt.Fprintf(&w, "    brk #0x3e7\n%s:\n    b _recomp_ret_slow\n\n", lifter.RetSlowLabel(f.Addr))
		} else {
			w.WriteString("    ldr x30, [sp], #16\n    ret\n\n")
		}
		w.WriteString(cold.String())
		cold.Reset()
	}
	fset := map[string]bool{}
	for _, s := range l.Unsupported() {
		if !seeds[s.Func] { // a guessed entry may be data: its holes do not count
			fset[s.Form] = true
		}
	}
	for f := range fset {
		forms = append(forms, f)
	}
	sort.Strings(forms)
	for m := range opSet {
		if !setupOps[m] {
			mnems = append(mnems, m)
		}
	}
	sort.Strings(mnems)

	// Indirect call: tail-branch to the function entry. Indirect jump (the caller already popped
	// the frame): re-push it and enter the block. x17 = target ^ addr, so NZCV is untouched.
	w.WriteString("// indirect transfers\n.globl _recomp_indirect_call\n_recomp_indirect_call:\n")
	for _, f := range funcs {
		w.WriteString(cmpLines(f.Addr))
		fmt.Fprintf(&w, "    cbnz x17, 1f\n    b _fn_0x%x\n1:\n", f.Addr)
	}
	w.WriteString("    brk #0x778\n")
	// Inline-cache miss path: the harness has no cache fill, the plain indirect call does the work.
	w.WriteString(".globl _recomp_indirect_call_ic\n_recomp_indirect_call_ic:\n    b _recomp_indirect_call\n")
	if flat {
		// A RET that missed the shadow top (x7 = the guest return address): continue by address,
		// like an indirect jump that may also land on a return site. The harness has no runtime,
		// so the shadow stack is not resynchronised: stale entries just miss again.
		w.WriteString(".globl _recomp_ret_slow\n_recomp_ret_slow:\n    mov x0, x7\n")
		for _, c := range conts {
			w.WriteString(cmpLines(c.guest))
			fmt.Fprintf(&w, "    cbnz x17, 1f\n    b %s\n1:\n", c.label)
		}
	}
	w.WriteString(".globl _recomp_indirect_jump\n_recomp_indirect_jump:\n")
	for _, b := range blocks {
		w.WriteString(cmpLines(b[1]))
		if flat {
			fmt.Fprintf(&w, "    cbnz x17, 1f\n    b L_fn_0x%x_loc_0x%x\n1:\n", b[0], b[1])
		} else {
			fmt.Fprintf(&w, "    cbnz x17, 1f\n    str x30, [sp, #-16]!\n    b L_fn_0x%x_loc_0x%x\n1:\n", b[0], b[1])
		}
	}
	w.WriteString("    brk #0x779\n.globl _recomp_unsupported_instruction\n_recomp_unsupported_instruction:\n    brk #0x777\n")

	// Host entry/exit. FexState layout (harness/fex_main.c): gpr @0, v @128, arena @704, fs @712.
	w.WriteString(`
.globl _fex_enter
_fex_enter:
    stp x29, x30, [sp, #-176]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    str x0, [sp, #160]
    mrs x2, fpcr
    str x2, [sp, #168]
    adrp x1, _fex_saved_sp@PAGE
    mov x2, sp
    str x2, [x1, _fex_saved_sp@PAGEOFF]
`)
	for i := 0; i < 32; i += 2 {
		fmt.Fprintf(&w, "    ldp q%d, q%d, [x0, #%d]\n", i, i+1, 128+16*i)
	}
	for i := 0; i < 16; i += 2 {
		fmt.Fprintf(&w, "    ldp %s, %s, [x0, #%d]\n", regOrder[i], regOrder[i+1], 8*i)
	}
	fmt.Fprintf(&w, "    ldr x28, [x0, #704]\n    ldr x27, [x0, #712]\n    msr fpcr, xzr\n")
	if flat {
		// x29 = shadow return stack (a static buffer, used from its middle so that unbalanced
		// tests cannot run off either end), x7 = the guest return address of the entry call:
		// whatever the guest stack holds, so that a top-level RET returns here.
		w.WriteString("    adrp x29, _fex_shadow@PAGE\n    add x29, x29, _fex_shadow@PAGEOFF\n    add x29, x29, #0x80000\n")
		w.WriteString("    ldr x7, [x28, x26]\n")
	}
	fmt.Fprintf(&w, "    bl _fn_0x%x\n", base)
	w.WriteString(`.globl _fex_exit
_fex_exit:
    adrp x16, _fex_saved_sp@PAGE
    ldr x16, [x16, _fex_saved_sp@PAGEOFF]
    ldr x17, [x16, #160]
`)
	for i := 0; i < 16; i += 2 {
		fmt.Fprintf(&w, "    stp %s, %s, [x17, #%d]\n", regOrder[i], regOrder[i+1], 8*i)
	}
	for i := 0; i < 32; i += 2 {
		fmt.Fprintf(&w, "    stp q%d, q%d, [x17, #%d]\n", i, i+1, 128+16*i)
	}
	w.WriteString(`    ldr x2, [x16, #168]
    msr fpcr, x2
    mov sp, x16
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x25, x26, [sp, #64]
    ldp x27, x28, [sp, #80]
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    ldp x29, x30, [sp], #176
    ret
.section __DATA,__data
.p2align 3
_fex_saved_sp: .quad 0
`)
	if flat {
		w.WriteString(".zerofill __DATA,__bss,_fex_shadow,0x100000,4\n")
	}
	return w.String(), mnems, forms, nil
}

// cmpLines sets x17 = x0 ^ addr (zero on a match) without touching NZCV.
func cmpLines(addr uint64) string {
	return fmt.Sprintf("    movz x16, #0x%x\n    movk x16, #0x%x, lsl #16\n    movk x16, #0x%x, lsl #32\n    eor x17, x0, x16\n",
		addr&0xffff, (addr>>16)&0xffff, (addr>>32)&0xffff)
}

// mnemonic names an instruction's operation; EXTRQ/INSERTQ are the repo's private Op values.
func mnemonic(in x86asm.Inst) string {
	if in.Op >= 65536 {
		if f := strings.Fields(disasm.InstString(in)); len(f) > 0 {
			return strings.ToUpper(f[0])
		}
	}
	return strings.ToUpper(strings.TrimSuffix(in.Op.String(), "_XMM"))
}
