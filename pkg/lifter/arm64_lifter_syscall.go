package lifter

import "fmt"

// liftSyscall lowers the SYSCALL instruction to a call of the runtime's raw syscall entry
// (shim_raw_syscall, pkg/runtime/kernel/syscalls) through _recomp_call_shim, which stores every
// guest GPR to the context and reloads them afterwards. Convention (FreeBSD/Orbis): rax =
// number, rdi, rsi, rdx, r10, r8, r9 = arguments, result in rax, CF set and rax = errno on
// error; rcx and r11 are clobbered. The shim returns CF in bit 0 of r11 (x11); it is loaded into
// the C flag here (the bridge does not sync flags).
//
// The mid-function bl clobbers x30, so the host frame record is saved around it.
func (l *Arm64Lifter) liftSyscall(addr, nextPC uint64, lines []string) []string {
	if callModel == CallFlat {
		return l.liftSyscallFlat(addr, nextPC, lines)
	}
	lines = append(lines,
		"    // syscall",
		"    stp  x29, x30, [sp, #-16]!",
		"    adrp x0, _shim_raw_syscall@PAGE",
		"    add  x0, x0, _shim_raw_syscall@PAGEOFF",
		fmt.Sprintf("    movz x1, #0x%x", addr&0xffff),
	)
	for shift := 16; shift < 64; shift += 16 {
		if chunk := (addr >> shift) & 0xffff; chunk != 0 {
			lines = append(lines, fmt.Sprintf("    movk x1, #0x%x, lsl #%d", chunk, shift))
		}
	}
	lines = append(lines,
		"    bl   _recomp_call_shim",
		"    ldp  x29, x30, [sp], #16",
		"    mrs  x16, nzcv",
		"    bfi  x16, x11, #29, #1 // C = CF (error)",
		"    msr  nzcv, x16",
	)
	l.cpol = carryAdd
	return lines
}

// liftSyscallFlat is the flat-model SYSCALL: the bridge continues like a guest RET (the shadow
// top must be the entry of the call it returns from), so the lowering is a call of a shim stub:
// push the guest return address (the next instruction), push the shadow entry with the local
// continuation, enter the bridge. shim_raw_syscall_flat pops the return address.
func (l *Arm64Lifter) liftSyscallFlat(addr, nextPC uint64, lines []string) []string {
	lines = append(lines, "    // syscall", "    sub x26, x26, #8")
	lines = append(lines, loadImm64(FlatRAReg, nextPC)...)
	lines = append(lines, "    str "+FlatRAReg+", "+l.gm("x26"), "    adr x30, 1f")
	lines = append(lines, FlatCallEntry()...)
	lines = append(lines,
		"    adrp x0, _shim_raw_syscall_flat@PAGE",
		"    add  x0, x0, _shim_raw_syscall_flat@PAGEOFF",
		fmt.Sprintf("    movz x1, #0x%x", addr&0xffff),
	)
	for shift := 16; shift < 64; shift += 16 {
		if chunk := (addr >> shift) & 0xffff; chunk != 0 {
			lines = append(lines, fmt.Sprintf("    movk x1, #0x%x, lsl #%d", chunk, shift))
		}
	}
	lines = append(lines,
		"    b    _recomp_call_shim",
		"1:",
		"    mrs  x16, nzcv",
		"    bfi  x16, x11, #29, #1 // C = CF (error)",
		"    msr  nzcv, x16",
	)
	l.cpol = carryAdd
	return lines
}
