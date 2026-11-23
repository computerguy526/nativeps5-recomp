package lifter

import (
	"fmt"
	"regexp"
	"sort"
	"strconv"
	"strings"

	"ps4-recomp/pkg/disasm"

	"golang.org/x/arch/x86/x86asm"
)

// x86 memory ordering (TSO) on ARM.
//
// x86 keeps loads ordered with loads, stores with stores and loads before later stores; ARM
// orders none of them. As in the C backend (MEM_LD*/MEM_ST* in recomp_context.h), guest memory
// accesses whose address is not RSP- or RBP-relative use load-acquire (LDAPR, RCpc) and
// store-release (STLR): lock-free guest code that publishes data with plain stores and reads it
// with plain loads keeps its ordering. RSP/RBP-relative accesses are thread-private stack and
// frame accesses and stay plain, as do vector, FP and x87 accesses (the C backend did not order
// them either, and TLOU was correct without it).
//
// The ordering is applied to the lowering of one instruction after it is generated: every scalar
// GPR load or store of the form [x28, xN] (guest memory base + guest address; xN != x26, the
// guest RSP) is rewritten to base-only addressing, which LDAPR/STLR require, with a scratch
// register from x0-x7 that the lowering does not use:
//
//	ldr  w17, [x28, x16]   ->  add x7, x28, x16; ldapr w17, [x7]
//	ldrsh x0, [x28, x16]   ->  add x7, x28, x16; ldaprh w0, [x7]; sxth x0, w0
//	str  w17, [x28, x16]   ->  add x7, x28, x16; stlr w17, [x7]
//
// 2-, 4- and 8-byte accesses first check for a 16-byte boundary crossing (orderedAccess).
//
// Why LDAPR (RCpc) and not LDAR (RCsc): the two differ in one case only. An LDAR may not be
// satisfied before an earlier STLR to another address has become visible; an LDAPR may. x86-TSO
// allows exactly that reordering (a store followed by a load from another address may be
// observed in the other order: the store-buffer relaxation). x86 code that needs it closed uses
// MFENCE, lowered to dmb ish, or a locked instruction, lowered to an acquire-release atomic
// (casal, ldaddal, swpal, ldaxr/stlxr) that no later load passes. The orderings TSO does
// guarantee, load->load, load->store and store->store, LDAPR + STLR guarantee as well, so LDAR
// would only add ordering x86 never promised. The spec left "LDAPR or LDAR" to measurement; it
// was settled by this argument, and Task 9's TSO on/off A/B measured the cost of the ordering.

// Cold-section outlining (docs/m3/coldpath.md). With it on, the rare paths of the TSO lowering (the
// access whose bytes cross a 16-byte boundary, the access with an unaligned base) are not emitted
// inline: the hot path keeps a forward skip over one `b` and the body goes to the lifter's cold
// buffer (TakeCold), which the emitter writes after the last function of the chunk. Off, the
// output is byte-identical to a build without the feature. The consumer of the lifted lines must
// drain the buffer; the default is off so that a caller that does not (tests, tools) is unchanged.
var coldOutline = false

// SetColdOutline turns cold-section outlining on or off (`-cold-outline`).
func SetColdOutline(on bool) { coldOutline = on }

// GetColdOutline reports whether rare paths are outlined.
func GetColdOutline() bool { return coldOutline }

// TakeCold returns the cold lines the lowering of the instruction(s) lifted since the last call
// produced (labels and instructions, ready to be written to the chunk's cold area) and empties
// the buffer.
func (l *Arm64Lifter) TakeCold() []string {
	c := l.cold
	l.cold = nil
	return c
}

// coldLabel names the entry of an outlined body: Lc<n> after the shortener (L_tso_cold_<n>).
func (l *Arm64Lifter) coldLabel() string {
	return fmt.Sprintf("L_tso_cold_%d", l.labelCounter)
}

// SetTSO turns x86 memory ordering for non-stack guest memory on (the default) or off.
func (l *Arm64Lifter) SetTSO(on bool) {
	l.noTSO = !on
}

// SetTSORelax names functions (guest entry addresses) that are lifted with plain loads and stores
// while the rest of the title keeps x86 ordering. It is a per-title, translate-time opt-in for hot
// leaf kernels whose loads cannot take part in any lock-free protocol (a software blitter reading
// pixels, format fields and static tables): every ordered load costs about a cycle of throughput on
// Apple cores whatever surrounds it (LDAPR, LDAR and LDR + DMB ISHLD alike), which is half of
// Syobon's frame. The default is empty; NPS5_TSO_RELAX_FUNCS=0x61d9e0,0x... sets it from the
// environment. The lifter manifest lists the functions. See docs/m3/perf.md for the evidence a
// function needs.
func (l *Arm64Lifter) SetTSORelax(entries []uint64) {
	l.tsoRelax = map[uint64]bool{}
	for _, e := range entries {
		l.tsoRelax[e] = true
	}
}

// TSORelaxed returns the functions lifted without ordering, sorted.
func (l *Arm64Lifter) TSORelaxed() []uint64 {
	var out []uint64
	for e := range l.tsoRelax {
		out = append(out, e)
	}
	sort.Slice(out, func(i, j int) bool { return out[i] < out[j] })
	return out
}

// SetTSOStrict names functions (guest entry addresses) whose vector, FP/x87 and string
// (rep movs/stos/...) guest accesses are ordered like the scalar ones: a `dmb ish` before every
// instruction that stores and a `dmb ishld` after every instruction that loads. The scalar TSO
// lowering (applyTSO) leaves those accesses plain, so a scalar STLR that publishes a block filled
// by vector stores or rep movs can be observed before the block, and a plain vector load can pass
// the acquire that guards it. The default is empty (the output is then byte-identical);
// NPS5_TSO_STRICT_FUNCS=0x...,0x... sets it from the environment. It is a per-title, per-function
// opt-in for a producer or consumer shown to need it (SAROS PPSA07631 record array).
func (l *Arm64Lifter) SetTSOStrict(entries []uint64) {
	l.tsoStrict = map[uint64]bool{}
	for _, e := range entries {
		l.tsoStrict[e] = true
	}
}

// TSOStrict returns the functions with strict vector/string ordering, sorted.
func (l *Arm64Lifter) TSOStrict() []uint64 {
	var out []uint64
	for e := range l.tsoStrict {
		out = append(out, e)
	}
	sort.Slice(out, func(i, j int) bool { return out[i] < out[j] })
	return out
}

// EnvTSOStrict is the strict list a new lifter takes from NPS5_TSO_STRICT_FUNCS (sorted).
func EnvTSOStrict() []uint64 { return NewArm64Lifter(nil).TSOStrict() }

var strictAccessRe = regexp.MustCompile(`^\s*(ld|st)[a-z0-9]*\s.*\[x28[,\]]`)

// hasVecArg reports an xmm/ymm/mm operand (the SSE forms of MOVSD/CMPSD share the string opcodes).
func hasVecArg(inst x86asm.Inst) bool {
	for _, a := range inst.Args {
		if r, ok := a.(x86asm.Reg); ok && (r >= x86asm.X0 && r <= x86asm.Y15 || r >= x86asm.M0 && r <= x86asm.M7) {
			return true
		}
	}
	return false
}

// applyTSOStrict fences the accesses applyTSO leaves plain (tsoAppliesMode false): the guest
// loads and stores of vector, FP, x87 and MMX instructions through [x28, ...], and string
// instructions as a whole (their implicit RSI/RDI accesses). Loads get `dmb ishld` after the
// lowering, stores `dmb ish` before it; a string instruction gets a full fence on both sides.
func (l *Arm64Lifter) applyTSOStrict(inst disasm.Instruction, lines []string, start int) []string {
	str := isStringOp(inst.Inst.Op) && !hasVecArg(inst.Inst)
	if !str && tsoAppliesMode(inst.Inst, l.tsoElide, l.stackPriv) {
		return lines // scalar accesses: handled by applyTSO
	}
	if str {
		lines = append(lines[:start:start], append([]string{"    dmb ish"}, lines[start:]...)...)
		return append(lines, "    dmb ish")
	}
	ld, st := false, false
	for _, ln := range lines[start:] {
		code := ln
		if i := strings.Index(code, "//"); i >= 0 {
			code = code[:i]
		}
		if m := strictAccessRe.FindStringSubmatch(code); m != nil {
			if m[1] == "ld" {
				ld = true
			} else {
				st = true
			}
		}
	}
	if st {
		lines = append(lines[:start:start], append([]string{"    dmb ish"}, lines[start:]...)...)
	}
	if ld {
		lines = append(lines, "    dmb ishld")
	}
	return lines
}

func parseTSORelax(s string) map[uint64]bool {
	m := map[uint64]bool{}
	for _, f := range strings.FieldsFunc(s, func(r rune) bool { return r == ',' || r == ' ' }) {
		if v, err := strconv.ParseUint(strings.TrimPrefix(strings.ToLower(f), "0x"), 16, 64); err == nil {
			m[v] = true
		}
	}
	return m
}

// TSO reports whether x86 memory ordering is on.
func (l *Arm64Lifter) TSO() bool {
	return !l.noTSO
}

var (
	// A scalar GPR load or store to guest memory: op, data register, address register.
	scratchUseRe = regexp.MustCompile(`\b[wx]([0-7])\b`)
)

// tsoApplies reports whether the instruction's guest memory accesses are ordered: it has an
// explicit memory operand that is not RSP- or RBP-relative, no vector or MMX register operand,
// and it is neither an x87 nor a string instruction
// (its implicit RSI/RDI accesses are block copies, which x86 fast-string operations do not order
// either). The SSE forms of MOVSD/CMPSD share the opcode names; their accesses are vector
// accesses, which stay plain anyway.
func tsoApplies(inst x86asm.Inst) bool {
	return tsoAppliesMode(inst, TSOElideOff, 0)
}

// tsoAppliesMode is tsoApplies for a stack elision mode: with TSOElideStack the base register
// decides nothing; the access is plain only when the stack privacy pass proved it private
// (stackPriv = disasm.Instruction.StackPriv).
func tsoAppliesMode(inst x86asm.Inst, mode TSOElideMode, stackPriv uint8) bool {
	if isStringOp(inst.Op) || strings.HasPrefix(inst.Op.String(), "F") {
		return false // string instructions; x87
	}
	ordered := false
	for _, a := range inst.Args {
		if a == nil {
			break
		}
		if r, ok := a.(x86asm.Reg); ok && (r >= x86asm.X0 && r <= x86asm.Y15 || r >= x86asm.M0 && r <= x86asm.M7) {
			return false // vector, SIMD and MMX instructions (also their scalar element accesses)
		}
		if m, ok := a.(x86asm.Mem); ok {
			if mode != TSOElideOff {
				if stackPriv&mode.memBit() != 0 {
					return false
				}
			} else {
				switch m.Base {
				case x86asm.RSP, x86asm.ESP, x86asm.RBP, x86asm.EBP:
					return false
				}
			}
			ordered = true
		}
	}
	return ordered
}

// TSOElideMode selects the stack accesses that are emitted without x86 ordering
// (docs/m3/tsoelide.md).
type TSOElideMode uint8

const (
	// TSOElideOff: every RSP- or RBP-based access is plain, whatever it addresses (the rule
	// the lifter has always had; no analysis).
	TSOElideOff TSOElideMode = iota
	// TSOElideStack: an access is plain only when the stack privacy pass (pkg/disasm
	// stackpriv.go) proved that it addresses a thread-private slot of the frame; every other
	// access is ordered, RSP/RBP-based ones and the loads of POP and LEAVE included.
	TSOElideStack
	// TSOElideRegion: TSOElideStack, plus, in a function whose frame address escapes, the
	// slots at a known offset that lie entirely below the lowest escaping address (assumption
	// A6 of docs/m3/tsoelide.md: an escaped address is not used with a negative offset).
	TSOElideRegion
)

func (m TSOElideMode) String() string {
	switch m {
	case TSOElideStack:
		return "stack"
	case TSOElideRegion:
		return "stack-region"
	}
	return "off"
}

func (m TSOElideMode) memBit() uint8 {
	if m == TSOElideRegion {
		return disasm.StackPrivMemRegion
	}
	return disasm.StackPrivMem
}

func (m TSOElideMode) popBit() uint8 {
	if m == TSOElideRegion {
		return disasm.StackPrivPopRegion
	}
	return disasm.StackPrivPop
}

// ParseTSOElide parses a -tso-elide value.
func ParseTSOElide(s string) (TSOElideMode, error) {
	switch s {
	case "", "off":
		return TSOElideOff, nil
	case "stack":
		return TSOElideStack, nil
	case "stack-region":
		return TSOElideRegion, nil
	}
	return TSOElideOff, fmt.Errorf("unknown -tso-elide mode %q (off, stack, stack-region)", s)
}

// SetTSOElide selects the stack elision mode.
func (l *Arm64Lifter) SetTSOElide(m TSOElideMode) { l.tsoElide = m }

// TSOElide returns the stack elision mode.
func (l *Arm64Lifter) TSOElide() TSOElideMode { return l.tsoElide }

// plainStackOperand reports whether the memory operand of the instruction being lowered is a
// stack access that stays plain (memOperand may then fold its displacement in identity mode).
func (l *Arm64Lifter) plainStackOperand(mem x86asm.Mem) bool {
	if l.tsoElide != TSOElideOff && !l.noTSO {
		return l.stackPriv&l.tsoElide.memBit() != 0
	}
	return isStackBase(mem.Base)
}

// popOrdered reports whether the implicit stack load of the instruction (POP, LEAVE) must be
// an acquire load: in TSOElideStack mode, when the slot is neither private nor a register save
// slot (a slot whose address a callee may have published).
func (l *Arm64Lifter) popOrdered(inst x86asm.Inst) bool {
	if l.tsoElide == TSOElideOff {
		return false
	}
	switch inst.Op {
	case x86asm.POP, x86asm.LEAVE:
		return l.stackPriv&l.tsoElide.popBit() == 0
	}
	return false
}

func isStringOp(op x86asm.Op) bool {
	switch op {
	case x86asm.MOVSB, x86asm.MOVSW, x86asm.MOVSD, x86asm.MOVSQ, x86asm.STOSB, x86asm.STOSW, x86asm.STOSD,
		x86asm.STOSQ, x86asm.LODSB, x86asm.LODSW, x86asm.LODSD, x86asm.LODSQ, x86asm.SCASB, x86asm.SCASW,
		x86asm.SCASD, x86asm.SCASQ, x86asm.CMPSB, x86asm.CMPSW, x86asm.CMPSD, x86asm.CMPSQ:
		return true
	}
	return false
}

// applyTSO rewrites the scalar guest memory accesses of lines[start:] (the lowering of inst)
// into acquire loads and release stores.
func (l *Arm64Lifter) applyTSO(inst disasm.Instruction, lines []string, start int) []string {
	// explicit: the accesses through the instruction's memory operand are ordered. implicit:
	// its load through the stack pointer is (POP and LEAVE in TSOElideStack mode; the stack
	// pointer accesses of PUSH, CALL and RET are always plain: a pushed slot is new, and the
	// return address is a save slot).
	explicit := !l.roLoad && tsoAppliesMode(inst.Inst, l.tsoElide, l.stackPriv)
	implicit := l.popOrdered(inst.Inst)
	if !explicit && !implicit {
		return lines
	}
	re := l.guestAccessRe()
	body := lines[start:]
	skip := func(ln string, m []string) bool {
		if m == nil || strings.HasSuffix(ln, tcbMark) {
			return true
		}
		if m[4] != "x26" {
			return !explicit
		}
		// An access through the stack pointer itself.
		if l.tsoElide == TSOElideOff {
			return true
		}
		load := strings.HasPrefix(m[2], "ld")
		switch inst.Inst.Op {
		case x86asm.POP, x86asm.LEAVE:
			// The implicit access is the load; a store is the explicit operand (pop [rsp]).
			if load {
				return !implicit
			}
		case x86asm.RET, x86asm.LRET, x86asm.POPF, x86asm.POPFD, x86asm.POPFQ:
			if load {
				return true
			}
		case x86asm.PUSH, x86asm.CALL, x86asm.ENTER, x86asm.PUSHF, x86asm.PUSHFD, x86asm.PUSHFQ:
			// The implicit access is the store; a load is the explicit operand (push [rsp]).
			if !load {
				return true
			}
		}
		return !explicit
	}
	hit := false
	for _, ln := range body {
		if m := re.FindStringSubmatch(ln); !skip(ln, m) {
			hit = true
			break
		}
	}
	if !hit {
		return lines
	}
	used := map[string]bool{}
	for _, ln := range body {
		code := ln
		if i := strings.Index(code, "//"); i >= 0 {
			code = code[:i]
		}
		for _, m := range scratchUseRe.FindAllStringSubmatch(code, -1) {
			used[m[1]] = true
		}
	}
	// The scratch register is written and consumed inside each rewritten access only, so it
	// must merely not hold a value of the lowering at that point. A lowering that calls a
	// helper (bl/blr) passes and receives values in registers it names in its own text (the
	// helper's other caller-saved registers are dead after the call), so a register absent
	// from the text is free on both sides of the call as well.
	scratch := ""
	for _, n := range []string{"7", "6", "5", "4", "3", "2", "1", "0"} {
		if !used[n] {
			scratch = "x" + n
			break
		}
	}
	out := append([]string(nil), lines[:start]...)
	for k, ln := range body {
		m := re.FindStringSubmatch(ln)
		if skip(ln, m) {
			out = append(out, ln)
			continue
		}
		if len(out) > start {
			if folded := l.foldedAccess(m[1], m[2], m[3], m[4], scratch, out[len(out)-1], body, k); folded != nil {
				out = append(out[:len(out)-1], folded...)
				continue
			}
		}
		if c, ok := constAddr(out[start:], m[4]); ok && !l.noPeephole && scratch != "" {
			out = append(out, l.constAccess(m[1], m[2], m[3], m[4], scratch, ln, c)...)
			continue
		}
		out = append(out, l.orderedAccess(m[1], m[2], m[3], m[4], scratch, ln)...)
	}
	return out
}

var tsoMovImmRe = regexp.MustCompile(`^\s+(movz|movk)\s+(x\d+),\s*#0x([0-9a-f]+)(?:,\s*lsl\s+#(\d+))?\s*$`)

// constAddr reports the constant the lines just before an access built in reg with movz/movk
// (loadImm64: a RIP-relative or absolute guest address).
func constAddr(prev []string, reg string) (uint64, bool) {
	var v uint64
	for i := len(prev) - 1; i >= 0; i-- {
		m := tsoMovImmRe.FindStringSubmatch(prev[i])
		if m == nil || m[2] != reg {
			return 0, false
		}
		var chunk, shift uint64
		fmt.Sscanf(m[3], "%x", &chunk)
		if m[4] != "" {
			fmt.Sscanf(m[4], "%d", &shift)
		}
		v |= chunk << shift
		if m[1] == "movz" {
			return v, true
		}
	}
	return 0, false
}

// constAccess is the acquire/release form of an access at a translate-time constant guest address
// (a global: [rip+disp]): whether it crosses a 16-byte boundary is known, so there is no check.
func (l *Arm64Lifter) constAccess(ind, op, data, addr, a, plain string, c uint64) []string {
	size := accessSize(op, data)
	if int(c&15)+size > 16 {
		if strings.HasPrefix(op, "ldr") {
			return []string{plain, ind + "dmb ishld"}
		}
		return []string{ind + "dmb ish", plain}
	}
	host := ind + strings.TrimSpace(l.gAdd(a, addr))
	switch op {
	case "ldr":
		return []string{host, fmt.Sprintf("%sldapr %s, [%s]", ind, data, a)}
	case "ldrb", "ldrh":
		return []string{host, fmt.Sprintf("%sldapr%s %s, [%s]", ind, op[3:], data, a)}
	case "ldrsb", "ldrsh", "ldrsw":
		return []string{host, fmt.Sprintf("%sldapurs%s %s, [%s, #0]", ind, op[4:], data, a)}
	case "str":
		return []string{host, fmt.Sprintf("%sstlr %s, [%s]", ind, data, a)}
	}
	return []string{host, fmt.Sprintf("%sstlr%s %s, [%s]", ind, op[3:], data, a)}
}

var tsoEaLineRe = regexp.MustCompile(`^\s+(add|sub)\s+(x1[67]),\s*(x\d+),\s*#(\d+)\s*$`)

// accessSize is the width in bytes of a scalar access (op data, ...).
func accessSize(op, data string) int {
	switch op {
	case "ldrb", "ldrsb", "strb":
		return 1
	case "ldrh", "ldrsh", "strh":
		return 2
	case "ldrsw":
		return 4
	}
	if data[0] == 'x' {
		return 8
	}
	return 4
}

// foldedAccess is the acquire/release form of an access whose address the previous line built as
// base register + small displacement in a scratch register that is dead afterwards:
//
//	add x16, x23, #36          add  x7, x28, x23
//	ldrb w19, [x28, x16]   ->  ldapurb w19, [x7, #36]
//
// LDAPUR/STLUR (FEAT_LRCPC2) take a 9-bit displacement, so the guest address is never
// materialised. A 2-, 4- or 8-byte access whose displacement is a multiple of its size cannot
// cross a 16-byte boundary when the base is aligned to the size, which is the common case and is
// tested with two instructions; an unaligned base takes the exact check of orderedAccess:
//
//	add  x7, x28, x23
//	and  x16, x7, #3
//	cbz  x16, aligned
//	add  x16, x7, #20; add x7, x16, #3; eor x7, x7, x16; tbz x7, #4, ok
//	ldr  w9, [x16]; dmb ishld; b done        (crosses a 16-byte boundary: plain access with a fence)
//	ok: ldapr w9, [x16]; b done
//	aligned: ldapur w9, [x7, #20]
//	done:
//
// It returns nil when the shape does not apply (the caller then uses orderedAccess).
func (l *Arm64Lifter) foldedAccess(ind, op, data, addr, a, prev string, body []string, k int) []string {
	if l.noPeephole || a == "" || l.AddrMode() != AddrBase {
		return nil
	}
	m := tsoEaLineRe.FindStringSubmatch(prev)
	if m == nil || m[2] != addr || m[3] == addr || m[3] == "x31" || data[1:] == addr[1:] || a == m[3] {
		return nil
	}
	var disp int64
	fmt.Sscanf(m[4], "%d", &disp)
	if m[1] == "sub" {
		disp = -disp
	}
	size := accessSize(op, data)
	if !foldableDisp(disp) || disp%int64(size) != 0 || !phDeadAfter(body, k, addr[1:]) {
		return nil
	}
	isLoad := strings.HasPrefix(op, "ldr")
	w := "w" + data[1:]
	// The unscaled-offset acquire/release instruction, and the base-only one for [addr].
	var ur, base []string
	switch op {
	case "ldr":
		ur = []string{fmt.Sprintf("%sldapur %s, [%s, #%d]", ind, data, a, disp)}
		base = []string{fmt.Sprintf("%sldapr %s, [%s]", ind, data, addr)}
	case "ldrb", "ldrh":
		ur = []string{fmt.Sprintf("%sldapur%s %s, [%s, #%d]", ind, op[3:], data, a, disp)}
		base = []string{fmt.Sprintf("%sldapr%s %s, [%s]", ind, op[3:], data, addr)}
	case "ldrsb", "ldrsh", "ldrsw":
		ur = []string{fmt.Sprintf("%sldapurs%s %s, [%s, #%d]", ind, op[4:], data, a, disp)}
		if op == "ldrsw" {
			base = []string{fmt.Sprintf("%sldapr %s, [%s]", ind, w, addr), fmt.Sprintf("%ssxtw %s, %s", ind, data, w)}
		} else {
			base = []string{fmt.Sprintf("%sldapr%s %s, [%s]", ind, op[4:], w, addr), fmt.Sprintf("%ssxt%s %s, %s", ind, op[4:], data, w)}
		}
	case "str":
		ur = []string{fmt.Sprintf("%sstlur %s, [%s, #%d]", ind, data, a, disp)}
		base = []string{fmt.Sprintf("%sstlr %s, [%s]", ind, data, addr)}
	default: // strb, strh
		ur = []string{fmt.Sprintf("%sstlur%s %s, [%s, #%d]", ind, op[3:], data, a, disp)}
		base = []string{fmt.Sprintf("%sstlr%s %s, [%s]", ind, op[3:], data, addr)}
	}
	out := []string{fmt.Sprintf("%sadd %s, x28, %s", ind, a, m[3])}
	if size == 1 {
		return append(out, ur...)
	}
	l.labelCounter++
	aligned := fmt.Sprintf("L_tso_al_%d", l.labelCounter)
	ok := fmt.Sprintf("L_tso_ok_%d", l.labelCounter)
	done := fmt.Sprintf("L_tso_done_%d", l.labelCounter)
	out = append(out,
		fmt.Sprintf("%sand %s, %s, #%d", ind, addr, a, size-1),
		fmt.Sprintf("%scbz %s, %s", ind, addr, aligned))
	// The unaligned-base path: the exact crossing check, then the fenced plain access or the
	// ordered one at the byte address.
	var slow []string
	if disp >= 0 {
		slow = append(slow, fmt.Sprintf("%sadd %s, %s, #%d", ind, addr, a, disp))
	} else {
		slow = append(slow, fmt.Sprintf("%ssub %s, %s, #%d", ind, addr, a, -disp))
	}
	slow = append(slow,
		fmt.Sprintf("%sadd %s, %s, #%d", ind, a, addr, size-1),
		fmt.Sprintf("%seor %s, %s, %s", ind, a, a, addr),
		fmt.Sprintf("%stbz %s, #4, %s", ind, a, ok))
	plain := fmt.Sprintf("%s%s %s, [%s]", ind, op, data, addr)
	if isLoad {
		slow = append(slow, plain, ind+"dmb ishld")
	} else {
		slow = append(slow, ind+"dmb ish", plain)
	}
	slow = append(slow, fmt.Sprintf("%sb %s", ind, done), ok+":")
	slow = append(slow, base...)
	slow = append(slow, fmt.Sprintf("%sb %s", ind, done))
	if coldOutline {
		// cbz reaches +-1 MB, a chunk can be larger: skip one `b` (reach +-128 MB) instead.
		cold := l.coldLabel()
		out = append(out, fmt.Sprintf("%sb %s", ind, cold), aligned+":")
		out = append(out, ur...)
		out = append(out, done+":")
		l.cold = append(l.cold, cold+":")
		l.cold = append(l.cold, slow...)
		return out
	}
	out = append(out, slow...)
	out = append(out, aligned+":")
	out = append(out, ur...)
	return append(out, done+":")
}

// orderedAccess returns the acquire/release form of the plain access line (op data, [x28, addr]).
//
// LDAPR/STLR fault on an access that crosses a 16-byte boundary, also with FEAT_LSE2. Such an
// access (the guest address with its last byte differs in bit 4; the memory base is page
// aligned) takes the plain access with a fence instead, as recomp_ld*/recomp_st* did in the C
// backend: DMB ISHLD after a load (acquire), DMB ISH before a store (release). The check needs
// no flags. Byte accesses cannot cross. Without a free scratch register every access takes the
// fenced form.
func (l *Arm64Lifter) orderedAccess(ind, op, data, addr, a, plain string) []string {
	isLoad := strings.HasPrefix(op, "ldr")
	fence := func() []string {
		if isLoad {
			return []string{plain, ind + "dmb ishld"}
		}
		return []string{ind + "dmb ish", plain}
	}
	if a == "" {
		return fence()
	}
	var ordered []string
	w := "w" + data[1:]
	switch op {
	case "ldr":
		ordered = []string{fmt.Sprintf("%sldapr %s, [%s]", ind, data, a)}
	case "ldrb", "ldrh":
		ordered = []string{fmt.Sprintf("%sldapr%s %s, [%s]", ind, op[3:], data, a)}
	case "ldrsb", "ldrsh":
		ordered = []string{fmt.Sprintf("%sldapr%s %s, [%s]", ind, op[4:], w, a),
			fmt.Sprintf("%ssxt%s %s, %s", ind, op[4:], data, w)}
	case "ldrsw":
		ordered = []string{fmt.Sprintf("%sldapr %s, [%s]", ind, w, a), fmt.Sprintf("%ssxtw %s, %s", ind, data, w)}
	case "str":
		ordered = []string{fmt.Sprintf("%sstlr %s, [%s]", ind, data, a)}
	default: // strb, strh
		ordered = []string{fmt.Sprintf("%sstlr%s %s, [%s]", ind, op[3:], data, a)}
	}
	size := map[string]int{"ldrb": 1, "ldrsb": 1, "strb": 1, "ldrh": 2, "ldrsh": 2, "strh": 2, "ldrsw": 4}[op]
	if size == 0 {
		size = 4
		if data[0] == 'x' {
			size = 8
		}
	}
	host := ind + strings.TrimSpace(l.gAdd(a, addr))
	if size == 1 {
		return append([]string{host}, ordered...)
	}
	l.labelCounter++
	fast := fmt.Sprintf("L_tso_ok_%d", l.labelCounter)
	done := fmt.Sprintf("L_tso_done_%d", l.labelCounter)
	out := []string{
		fmt.Sprintf("%sadd %s, %s, #%d", ind, a, addr, size-1),
		fmt.Sprintf("%seor %s, %s, %s", ind, a, a, addr),
		fmt.Sprintf("%stbz %s, #4, %s", ind, a, fast), // no 16-byte boundary crossed
	}
	if coldOutline {
		cold := l.coldLabel()
		out = append(out, fmt.Sprintf("%sb %s", ind, cold), fast+":", host)
		out = append(out, ordered...)
		out = append(out, done+":")
		l.cold = append(l.cold, cold+":")
		l.cold = append(l.cold, fence()...)
		l.cold = append(l.cold, fmt.Sprintf("%sb %s", ind, done))
		return out
	}
	out = append(out, fence()...)
	out = append(out, fmt.Sprintf("%sb %s", ind, done), fast+":", host)
	out = append(out, ordered...)
	return append(out, done+":")
}

// EnvTSORelax is the relax list a new lifter takes from NPS5_TSO_RELAX_FUNCS (sorted); the translator records it in the
// tier-1 manifest so the fragment mode lifts with the same list whatever environment it runs in.
func EnvTSORelax() []uint64 { return NewArm64Lifter(nil).TSORelaxed() }

// EnvPeephole reports the peephole setting a new lifter takes from the environment (NPS5_NO_PEEPHOLE turns it off).
func EnvPeephole() bool { return !NewArm64Lifter(nil).noPeephole }
