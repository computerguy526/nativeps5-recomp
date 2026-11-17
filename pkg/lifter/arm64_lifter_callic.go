package lifter

import "ps4-recomp/pkg/disasm"

// Per-call-site inline caches for indirect and virtual calls (docs/m3/callic.md).
//
// Flat call model only. Every `call reg` / `call [mem]` / call to an address that is not a known
// function owns a 48-byte, 16-byte-aligned site record in __DATA,__nps5_ic:
//
//	+0  slot0 {guest target, host call entry}     16 bytes, read with one LDP, written with one STP
//	+16 slot1 {guest target, host call entry}
//	+32 miss counter (sampling of the slot1 replacement)
//
// An empty slot holds {~0, _recomp_indirect_call}: a call to the address ~0 "hits" it and takes
// the plain dispatch path (and its structured miss report) instead of jumping to nothing. The
// inline sequence
//
//	adrp/add x1, site ; ldp x2,x3,[x1] ; cmp x2,x0 ; b.eq hit
//	                    ldp x2,x3,[x1,#16] ; cmp x2,x0 ; b.eq hit
//	                    bl _recomp_indirect_call_ic ; b done
//	hit:  blr x3
//	done:                              <- the host continuation of the guest return address
//
// calls the cached call entry (the same code the dispatch table would have reached, so the
// callee's entry pushes the shadow entry as always). The miss path does the dispatch lookup,
// fills a slot and enters the target (asm_trampoline_flat.go).
var callIC = false

// SetCallIC turns the call-site inline caches on or off for all subsequent lifting
// (`-call-ic`). Ignored in the nested model.
func SetCallIC(on bool) { callIC = on }

// GetCallIC reports whether call-site inline caches are emitted (flat model only).
func GetCallIC() bool { return callIC && callModel == CallFlat }

// ICSiteSize is the size of a site record; the runtime walks __DATA,__nps5_ic in units of it.
const ICSiteSize = 48

// indirectCallLines is the tail of an indirect CALL lowering: x0 = guest target, x7 = guest
// return address (already stored on the guest stack). It ends with the label that marks the
// call continuation.
func indirectCallLines(lines []string) []string {
	if !GetCallIC() {
		return append(lines, "    bl _recomp_indirect_call")
	}
	return append(lines,
		"    .pushsection __DATA,__nps5_ic,regular,no_dead_strip",
		"    .p2align 4",
		"4:",
		"    .quad -1, _recomp_indirect_call, -1, _recomp_indirect_call, 0, 0",
		"    .popsection",
		"    adrp x1, 4b@PAGE",
		"    add x1, x1, 4b@PAGEOFF",
		"    ldp x2, x3, [x1]",
		"    cmp x2, x0",
		"    b.eq 3f",
		"    ldp x2, x3, [x1, #16]",
		"    cmp x2, x0",
		"    b.eq 3f",
		"    bl _recomp_indirect_call_ic",
		"    b 1f",
		"3:",
		"    blr x3",
		"1:",
	)
}

// Indirect jump caches (docs/m3/liftmisc.md item 3): `jmp reg` / `jmp [mem]` sites (vtable tail
// calls) own a site record of the same layout in the same section. A slot holds {guest target,
// jump entry}; a hit is `br x3`. Needs the call caches (flat model, -call-ic) and its own flag.
var jmpIC = true

// SetJmpIC turns the indirect-jump inline caches on or off (`-jmp-ic`).
func SetJmpIC(on bool) { jmpIC = on }

// GetJmpIC reports whether indirect jumps get an inline cache.
func GetJmpIC() bool { return jmpIC && GetCallIC() }

// indirectJumpCacheHit returns the inline check of an indirect jump (x0 = guest target): a hit
// on either slot branches to the cached jump entry; x1 is left holding the site record for the miss
// path. The record is defined in __DATA,__nps5_ic with the call sites'.
func indirectJumpCacheHit() []string {
	return []string{
		"    .pushsection __DATA,__nps5_ic,regular,no_dead_strip",
		"    .p2align 4",
		"4:",
		"    .quad -1, _recomp_indirect_call, -1, _recomp_indirect_call, 0, 0",
		"    .popsection",
		"    adrp x1, 4b@PAGE",
		"    add x1, x1, 4b@PAGEOFF",
		"    ldp x2, x3, [x1]",
		"    cmp x2, x0",
		"    b.ne 7f",
		"    br x3",
		"7:",
		"    ldp x2, x3, [x1, #16]",
		"    cmp x2, x0",
		"    b.ne 8f",
		"    br x3",
		"8:",
	}
}

// indirectJumpPrefix is the cache check at the start of `jmp reg` / `jmp [mem]` (x0 = guest target),
// before the local jump table: a cached target is never a block of this function, because only
// targets that fell through the local table reach the miss path that fills a slot. The local table
// uses x16/x17 only, so x1 (the site record) survives to the miss branch.
//
// A jump with a recovered jump table (a switch) is not cached: its targets are blocks of this
// function, so the cache could only miss.
func (l *Arm64Lifter) indirectJumpPrefix(fn *disasm.Function, inst disasm.Instruction) []string {
	if !jmpICSite(fn, inst) {
		return nil
	}
	return indirectJumpCacheHit()
}

// indirectJumpDispatch is the last line of the lowering: the plain dispatcher, or the cache miss path.
func (l *Arm64Lifter) indirectJumpDispatch(fn *disasm.Function, inst disasm.Instruction) string {
	if jmpICSite(fn, inst) {
		return "    b _recomp_indirect_jump_ic"
	}
	return "    b _recomp_indirect_jump"
}

func jmpICSite(fn *disasm.Function, inst disasm.Instruction) bool {
	return GetJmpIC() && !(fn != nil && len(fn.JumpTables[inst.Address]) > 0)
}
