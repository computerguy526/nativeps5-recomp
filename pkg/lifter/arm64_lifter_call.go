package lifter

import (
	"fmt"
	"strings"

	"ps4-recomp/pkg/disasm"

	"golang.org/x/arch/x86/x86asm"
)

// Call/return model (SPEC 3.3, docs/m1/flatstack.md), selected at translate time
// (SetCallModel, `-call-model nested|flat`).
//
//   - nested: guest CALL is a host `bl`, every recompiled function keeps its host return
//     address in a 16-byte host frame [x30, entry guest rsp], guest RET is a host `ret` that
//     never looks at the guest return address. The host stack mirrors the guest call stack.
//
//   - flat: the guest stack is the only call stack. The host stack pointer does not move while
//     guest code runs. x29 is the shadow return stack pointer: 32-byte entries
//     {guest return address, guest rsp at entry, host continuation, aux}, growing down.
//     A function symbol has two entries: the call entry (symbol + 0) pushes the shadow entry
//     from x7 (guest return address), x26 and x30 (the caller used `bl`, which keeps the host
//     return-address predictor paired with the `ret` of the guest RET); the jump entry
//     (symbol + FlatJumpEntryOffset) is the body, used by tail calls and the dispatcher.
//     RET pops the guest return address, compares {address, rsp} with the shadow top and
//     returns to its continuation; a mismatch goes to _recomp_ret_slow, which resynchronises
//     the shadow stack and continues at the guest address through the dispatch API
//     (pkg/runtime/core/recomp_dispatch_api.h). x30 is a scratch register.
//
// Why x29: the register map leaves no free callee-saved register (x19-x26 guest GPRs, x27 FS
// base, x28 memory base, which base addressing needs), x18 is reserved by the platform, x30 is
// what `bl` writes, and x0-x7/x16/x17 are the scratch registers of every lowering and do not
// survive the C helpers that lowerings call. x29 is callee-saved under AAPCS64, so helper and
// libm calls made in the middle of an instruction keep it, and no lowering ever used it (guest
// code builds no frame records; the nested model left it pointing at the recomp_enter_guest
// frame). The cost is that x29 is not a frame pointer while guest code runs: the runtime's
// walkers start from GuestContext.host_boundary instead (the shim bridge puts a valid frame
// record chain back before it calls C).
type CallModel int

const (
	CallNested CallModel = iota
	CallFlat
)

// FlatJumpEntryOffset is the size of the call entry of a function in the flat model
// (RECOMP_FLAT_JUMP_ENTRY_OFFSET in recomp_dispatch_api.h).
const FlatJumpEntryOffset = 8

// ShadowEntrySize is sizeof(RecompShadowEntry).
const ShadowEntrySize = 32

func (m CallModel) String() string {
	if m == CallFlat {
		return "flat"
	}
	return "nested"
}

// ParseCallModel parses "nested" or "flat".
func ParseCallModel(s string) (CallModel, error) {
	switch s {
	case "nested":
		return CallNested, nil
	case "flat", "":
		return CallFlat, nil
	}
	return CallNested, fmt.Errorf("unknown call model %q (want nested or flat)", s)
}

// callModel is process-wide like addrMode: set once before lifting starts.
var callModel = CallNested

// SetCallModel selects the call/return model for all subsequent lifting and emitting.
func SetCallModel(m CallModel) { callModel = m }

// GetCallModel returns the current call/return model.
func GetCallModel() CallModel { return callModel }

// FlatRAReg holds the guest return address at a flat call entry and at _recomp_ret_slow. Not
// x16/x17: those are the linker's veneer scratch registers (AAPCS64 IP0/IP1) and may be
// clobbered between a far `bl`/`b` and its target, which would turn every far call into a
// shadow mismatch. The nested model never reads the register again and keeps x16.
const FlatRAReg = "x7"

// raReg is the register a CALL lowering materialises the guest return address in.
func raReg() string {
	if callModel == CallFlat {
		return FlatRAReg
	}
	return "x16"
}

// FlatCallEntry returns the instructions of the call entry of a function or shim stub in the
// flat model: push the shadow entry {x7 guest return address, x26 guest rsp, x30 host
// continuation}. A push into the guard page below the shadow stack faults and the runtime
// compacts the stack (nps5_shadow.c); the handler recognises exactly this stp.
func FlatCallEntry() []string {
	return []string{
		fmt.Sprintf("    stp %s, x26, [x29, #-%d]!", FlatRAReg, ShadowEntrySize),
		"    str x30, [x29, #16]",
	}
}

// leaveFrame returns the instructions that drop the function's host frame before control
// leaves the function for good (tail call, indirect jump): the nested model reloads the host
// return address, the flat model has no host frame.
func leaveFrame() []string {
	if callModel == CallFlat {
		return nil
	}
	return []string{"    ldr x30, [sp], #16"}
}

// FlatJumpEntrySymbol returns the global symbol of the jump entry of the function or shim stub
// whose call-entry symbol is sym ("_fn_0x<addr>" -> "_fnj_0x<addr>"). Tail calls branch to this
// symbol, not to "sym+8": Apple's linker routes a far `b sym+8` (more than 128 MB away, which a
// large title's text reaches) through a branch island of sym and then applies the addend to the
// ISLAND's address, i.e. it lands two islands further, in an unrelated function's island
// (ASTRO BOT: the __cxa_atexit PLT stub's tail jump entered the __cxa_guard_release stub).
func FlatJumpEntrySymbol(sym string) string {
	if strings.HasPrefix(sym, "_fn_0x") {
		return "_fnj_0x" + sym[len("_fn_0x"):]
	}
	return sym + "$j"
}

// FlatJumpEntryLabel returns the lines that define the jump-entry symbol of sym; they go right
// after FlatCallEntry().
func FlatJumpEntryLabel(sym string) []string {
	j := FlatJumpEntrySymbol(sym)
	return []string{".globl " + j, j + ":"}
}

// tailCallTo returns the branch to the body of the known function at target.
func tailCallTo(target uint64) string {
	if callModel == CallFlat {
		return "    b " + FlatJumpEntrySymbol(fmt.Sprintf("_fn_0x%x", target))
	}
	return fmt.Sprintf("    b _fn_0x%x", target)
}

// RetSlowLabel is the per-function label the flat RET branches to on a shadow mismatch; the
// emitter defines it at the end of the function as a branch to _recomp_ret_slow (a conditional
// branch cannot reach or relocate against a global symbol).
func RetSlowLabel(fnEntry uint64) string {
	return fmt.Sprintf("L_fn_0x%x_retslow", fnEntry)
}

// ReturnSiteLabel names the host continuation of the guest return address ra inside the
// function at fnEntry. The emitter defines it right after the lowering of the CALL that ends at
// ra and lists it in the continuation table, so the slow paths can resume at a return address.
func ReturnSiteLabel(fnEntry, ra uint64) string {
	return fmt.Sprintf("L_fn_0x%x_ra_0x%x", fnEntry, ra)
}

func addImm(reg string, v uint64) []string {
	var out []string
	if lo := v & 0xfff; lo != 0 || v == 0 {
		out = append(out, fmt.Sprintf("    add %s, %s, #%d", reg, reg, lo))
	}
	if hi := (v >> 12) & 0xfff; hi != 0 {
		out = append(out, fmt.Sprintf("    add %s, %s, #%d, lsl #12", reg, reg, hi))
	}
	return out
}

// liftRet lowers RET / RET imm16 (and LRET with extra = 8 for the code segment selector).
func (l *Arm64Lifter) liftRet(inst disasm.Instruction, fn *disasm.Function, extra uint64, lines []string) []string {
	pop := 8 + extra
	if len(inst.Inst.Args) > 0 {
		if imm, ok := inst.Inst.Args[0].(x86asm.Imm); ok {
			pop += uint64(uint16(imm))
		}
	}
	if callModel != CallFlat {
		// The host frame holds the return path; the guest return address is only popped.
		lines = append(lines, "    ldr x16, "+l.gm("x26"))
		lines = append(lines, addImm("x26", pop)...)
		return append(lines, "    ldr x30, [sp], #16", "    ret")
	}
	lines = append(lines,
		"    ldr "+FlatRAReg+", "+l.gm("x26"),                         // guest return address
		fmt.Sprintf("    ldp x17, x0, [x29], #%d", ShadowEntrySize), // shadow top {return address, rsp}, popped
		"    cmp "+FlatRAReg+", x17",
		"    ccmp x26, x0, #0, eq",
	)
	lines = append(lines, addImm("x26", pop)...)
	if fn != nil {
		lines = append(lines, "    b.ne "+RetSlowLabel(fn.EntryAddr))
	} else {
		lines = append(lines, "    b.eq 1f", "    b _recomp_ret_slow", "1:")
	}
	return append(lines,
		fmt.Sprintf("    ldur x30, [x29, #-%d]", ShadowEntrySize-16), // host continuation
		"    ret")
}

// flatLocalCall is CALL rel32 to a block of the calling function (`call 1f; 1: pop reg`): the
// guest return address is pushed by the caller already (FlatRAReg); push the shadow entry here, as a
// function's call entry would, and branch to the block.
func flatLocalCall(label string) []string {
	out := []string{"    adr x30, 1f"}
	out = append(out, FlatCallEntry()...)
	return append(out, "    b "+label, "1:")
}

// helperFramePointer returns the instruction that points x29 at the frame record a lowering
// builds around a helper call. In the flat model x29 is the shadow stack pointer and must stay
// so for as long as the host pc is in guest code (a signal handler that enters the guest reads
// it), so the record is stored but not linked; the line becomes a comment.
func helperFramePointer(off int) string {
	if callModel == CallFlat {
		return "    // x29 stays the shadow stack pointer"
	}
	return fmt.Sprintf("    add  x29, sp, #%d", off)
}
