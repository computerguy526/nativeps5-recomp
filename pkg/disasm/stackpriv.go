package disasm

import (
	"math"
	"strings"

	"golang.org/x/arch/x86/x86asm"
)

// Stack privacy (docs/m3/tsoelide.md).
//
// A forward dataflow pass over one translated function that marks the memory accesses which
// provably address a thread-private slot of the frame this activation allocated itself. The
// lifter (-tso-elide stack) emits plain loads and stores for those and x86-ordered
// (acquire/release) accesses for every other one, stack-relative or not.
//
// The unit of analysis is the translated function, which may be a fragment entered by a jump
// with a live frame (a cold part, a shared tail, a landing pad, a tier-1 fragment). Nothing is
// assumed about the state at its entry: only memory in [rsp, R), R being the stack pointer at
// the entry of the analysed code, i.e. memory the analysed code allocated by lowering rsp
// itself, can be private, plus the red zone below rsp once the code has lowered rsp (it then
// owns everything below R). The return address, incoming stack arguments, the frame of an
// enclosing function and the red zone of code that never lowered rsp are never private.
//
// A function in which the address of such a slot can leave the registers (stored to memory,
// live in an argument register at a call, read by an instruction the pass does not model) is
// "escaped": none of its accesses is private except pops of register save slots. The flag is
// per function, not per program point: the continuation table lets a longjmp or an unwinder
// resume a function at any return site, so "before the first escape" is not a property the
// control-flow graph can prove.

// Instruction.StackPriv bits.
const (
	// StackPrivMem: the explicit memory operand addresses a private slot.
	StackPrivMem uint8 = 1 << iota
	// StackPrivPop: the implicit load of POP / LEAVE reads a private slot or a register save slot.
	StackPrivPop
	// StackPrivSeen: the pass classified this instruction (the other bits are meaningful).
	StackPrivSeen
	// StackPrivMemRegion / StackPrivPopRegion: private under the region rule (assumption A6):
	// also in a function whose frame address escapes, a slot that lies entirely below the
	// lowest escaping frame address. Supersets of StackPrivMem / StackPrivPop.
	StackPrivMemRegion
	StackPrivPopRegion
)

// StackPrivReason says why an access with an explicit memory operand is not private (statistics).
type StackPrivReason uint8

const (
	SPPrivate  StackPrivReason = iota
	SPNotStack                 // the address is not derived from this activation's stack pointer
	SPEscaped                  // a frame address escapes somewhere in the function
	SPPoisoned                 // the function could not be analysed (see StackPrivInfo.Poison)
	SPAbove                    // at or above the entry stack pointer: return address, stack arguments, enclosing frame
	SPRedZone                  // below RSP in code that never had a frame of its own, or beyond the red zone
	SPInexact                  // stack-derived, offset or frame extent unknown
	SPSegment                  // segment override
	SPRegion                   // escaped function, slot below every escaping address: private under the region rule only
	spReasonCount
)

func (r StackPrivReason) String() string {
	return [...]string{"private", "not-stack", "escaped", "poisoned", "above-entry-rsp", "red-zone", "inexact", "segment", "below-escapes"}[r]
}

// StackPrivInfo is the per-function result.
type StackPrivInfo struct {
	// Escaped: an own-frame address may leave the registers. EscapeAt is the first such
	// instruction in block order, EscapeWhy its kind.
	Escaped   bool
	EscapeAt  uint64
	EscapeWhy string
	// EscapeLow: the lowest offset (relative to the entry stack pointer) of a frame address
	// that escapes, math.MinInt64 when an escaping value has no known offset. Meaningful
	// when Escaped.
	EscapeLow int64
	// Poison: non-empty when the pass gave up (no access is private).
	Poison string
	// Unreached: blocks no path from the entry reaches in the recovered graph.
	Unreached int
}

type spKind uint8

const (
	spClean spKind = iota // not derived from an address of this activation's frame
	spExact               // entry rsp + off
	spOwn                 // points into an object in the self-allocated frame, offset unknown
	spMixed               // may be derived from a frame address; nothing else is known
)

type spVal struct {
	k   spKind
	off int64
}

const (
	rAX = iota
	rCX
	rDX
	rBX
	rSP
	rBP
	rSI
	rDI
	rR8
	rR9
	rR10
	rR11
	rR12
	rR13
	rR14
	rR15
)

const calleeSavedMask = 1<<rBX | 1<<rBP | 1<<rR12 | 1<<rR13 | 1<<rR14 | 1<<rR15

// StackPrivOpts are the interprocedural extensions of the pass. The translator does not use
// them yet; cmd/tsostat -ipa measures what they would give (docs/m3/tsoelide.md, section 9).
type StackPrivOpts struct {
	// Params: also treat the pointer arguments (RDI, RSI, RDX, RCX, R8, R9 and loads from the
	// incoming stack arguments) as addresses that must not leave the registers, and a
	// return or tail call with one in a caller-saved register as an escape. A function that
	// is not escaped in this mode neither keeps nor forwards any pointer it is given.
	Params bool
	// CleanCallee reports whether the function at a direct call target is known not to keep
	// or forward its pointer arguments (a previous Params run): a frame address passed to it
	// is not an escape.
	CleanCallee func(target uint64) bool
	// ImportClean reports whether addr (the PLT stub or GOT slot a call goes through) is an import
	// that keeps no pointer (cleanimports.go). Unlike CleanCallee it changes nothing else: the
	// translator uses it.
	ImportClean func(addr uint64) bool
	// CallEntered: the function is entered by a call (not by a jump from code with a live
	// frame), so the red zone below the entry stack pointer is its own from the first
	// instruction: assumption A5 with the call as the point where the caller's red zone died
	// (docs/m3/lift2.md, -redzone-leaf).
	CallEntered bool
}

type spCtx struct {
	StackPrivOpts
	fn *Function
}

func (c *spCtx) clean(target uint64) bool {
	return c != nil && c.CleanCallee != nil && c.CleanCallee(target)
}

func (c *spCtx) cleanImport(addr uint64) bool {
	return c != nil && c.ImportClean != nil && c.ImportClean(addr)
}

type spState struct {
	ok bool // reached
	o  *spCtx
	r  [16]spVal
	// low: memory in [R+low, R) is allocated by this activation (rsp <= R+low). low <= 0, or
	// spLowUnknown. With r[rSP] exact it is min(off, 0).
	low int64
	// entry: registers that still hold the value they had at the entry of the analysed code.
	entry uint16
	// saved: bit i set = the slot at offset -8*(i+1) holds a callee-saved register pushed
	// with its entry value (a register save slot), on every path.
	saved uint64
	// lowered: on every path to here this activation has moved RSP below R at least once.
	// From then on nothing the code before the entry left below R is live (assumption A5),
	// so the red zone below the current RSP is this activation's own.
	lowered bool
}

// redZone is the size of the System V x86-64 red zone.
const redZone = 128

func (v spVal) tainted() bool { return v.k != spClean && !(v.k == spExact && v.off >= 0) }

func gprIndex(r x86asm.Reg) (idx, bits int, ok bool) {
	switch {
	case r >= x86asm.AL && r <= x86asm.R15B:
		i := int(r - x86asm.AL)
		switch {
		case i < 4:
			return i, 8, true
		case i < 8: // AH CH DH BH
			return i - 4, 8, true
		default: // SPB BPB SIB DIB R8B..R15B
			return i - 4, 8, true
		}
	case r >= x86asm.AX && r <= x86asm.R15W:
		return int(r - x86asm.AX), 16, true
	case r >= x86asm.EAX && r <= x86asm.R15L:
		return int(r - x86asm.EAX), 32, true
	case r >= x86asm.RAX && r <= x86asm.R15:
		return int(r - x86asm.RAX), 64, true
	}
	return 0, 0, false
}

func isVecOrFPReg(r x86asm.Reg) bool {
	return r >= x86asm.X0 && r <= x86asm.Y15 || r >= x86asm.M0 && r <= x86asm.M7 || r >= x86asm.F0 && r <= x86asm.F7
}

func slotBit(off int64) uint64 {
	if off >= 0 || off%8 != 0 || off < -8*64 {
		return 0
	}
	return 1 << uint(-off/8-1)
}

// inFrame reports whether [a, a+size) lies in the self-allocated frame.
func (s *spState) inFrame(a, size int64) bool { return s.low <= a && a+size <= 0 }

// setRSP installs a new stack pointer value.
func (s *spState) setRSP(v spVal, keepLow bool) {
	if v.k == spExact {
		s.r[rSP] = v
		s.low = min(v.off, 0)
		if v.off < 0 {
			s.lowered = true
		}
		// Slots below the new stack pointer are gone.
		if v.off >= 0 {
			s.saved = 0
		} else if n := uint(-v.off / 8); n < 64 {
			s.saved &= 1<<n - 1
		}
		return
	}
	s.r[rSP] = spVal{k: spMixed}
	if !keepLow {
		s.low = spLowUnknown
		s.saved = 0
	}
}

// spLowUnknown as spState.low: nothing is known to be allocated (no offset is in the frame).
const spLowUnknown = int64(1) << 40

func (s *spState) moveRSP(d int64) {
	if s.r[rSP].k == spExact {
		s.setRSP(spVal{spExact, s.r[rSP].off + d}, false)
		return
	}
	// Inexact (rsp <= R+low is all that is known): a decrement extends what is known to be
	// allocated, an increment loses it.
	s.setRSP(spVal{k: spMixed}, d <= 0)
	if d < 0 && s.low != spLowUnknown {
		s.low += d
		s.lowered = true
	}
}

func (s *spState) write(idx int, v spVal) {
	if idx == rSP {
		s.setRSP(v, false)
	} else {
		s.r[idx] = v
	}
	s.entry &^= 1 << uint(idx)
}

func joinVal(a, b spVal, low int64) spVal {
	if a == b {
		return a
	}
	if a.k == spClean && b.k == spClean {
		return a
	}
	own := func(v spVal) bool {
		return v.k == spOwn || v.k == spExact && low <= v.off && v.off < 0
	}
	if own(a) && own(b) {
		return spVal{k: spOwn}
	}
	if !a.tainted() && !b.tainted() {
		// Exact offsets at or above the entry stack pointer, or one of those and a clean
		// value: no own-frame address, but no longer a known offset either.
		return spVal{k: spClean}
	}
	return spVal{k: spMixed}
}

// join merges b into a and reports whether a changed.
func (a *spState) join(b *spState) bool {
	if !b.ok {
		return false
	}
	if !a.ok {
		*a = *b
		return true
	}
	old := *a
	a.low = max(a.low, b.low)
	a.lowered = a.lowered && b.lowered
	a.entry &= b.entry
	a.saved &= b.saved
	if a.r[rSP] != b.r[rSP] {
		a.r[rSP] = spVal{k: spMixed}
	}
	for i := range a.r {
		if i != rSP {
			a.r[i] = joinVal(a.r[i], b.r[i], a.low)
		}
	}
	// Save slots below what is still known to be allocated are forgotten.
	switch {
	case a.low >= 0:
		a.saved = 0
	case -a.low/8 < 64:
		a.saved &= 1<<uint(-a.low/8) - 1
	}
	return *a != old
}

func spTop() spState {
	s := spState{ok: true}
	for i := range s.r {
		s.r[i] = spVal{k: spMixed}
	}
	return s
}

// memAddr evaluates the address of a memory operand.
func (s *spState) memAddr(m x86asm.Mem) (v spVal, indexed bool) {
	if m.Base == x86asm.RIP || m.Base == 0 && m.Index == 0 {
		return spVal{k: spClean}, false
	}
	base := spVal{k: spClean}
	if m.Base != 0 {
		i, bits, ok := gprIndex(m.Base)
		if !ok {
			return spVal{k: spMixed}, false
		}
		base = s.r[i]
		if bits != 64 && base.k != spClean {
			return spVal{k: spMixed}, false
		}
	}
	idx := spVal{k: spClean}
	if m.Index != 0 {
		i, bits, ok := gprIndex(m.Index)
		if !ok {
			return spVal{k: spMixed}, false
		}
		idx = s.r[i]
		if bits != 64 && idx.k != spClean {
			return spVal{k: spMixed}, false
		}
		indexed = true
	}
	disp := int64(int32(m.Disp))
	if idx.k != spClean {
		// A stack-derived value as the index: no known object.
		if !base.tainted() && !idx.tainted() {
			return spVal{k: spClean}, true
		}
		return spVal{k: spMixed}, true
	}
	switch base.k {
	case spClean:
		return base, indexed
	case spExact:
		if !indexed {
			return spVal{spExact, base.off + disp}, false
		}
		// An index into the object at base+disp (in bounds: assumption A2).
		if a := base.off + disp; s.low <= a && a < 0 {
			return spVal{k: spOwn}, true
		}
		if base.off+disp >= 0 && base.off >= 0 {
			return spVal{k: spClean}, true
		}
		return spVal{k: spMixed}, true
	case spOwn:
		return spVal{k: spOwn}, indexed
	}
	return spVal{k: spMixed}, indexed
}

// classify reports whether a size-byte access through m is private in state s.
func (s *spState) classify(m x86asm.Mem, size int64) StackPrivReason {
	if m.Segment != 0 {
		return SPSegment
	}
	if size <= 0 {
		size = 64
	}
	v, _ := s.memAddr(m)
	switch v.k {
	case spClean:
		return SPNotStack
	case spOwn:
		return SPPrivate
	case spExact:
		if s.inFrame(v.off, size) {
			return SPPrivate
		}
		if v.off+size > 0 {
			return SPAbove
		}
		if sp := s.r[rSP]; sp.k == spExact {
			// Below RSP. Private once the activation has had a frame of its own (A5) and the
			// access is inside the red zone.
			if s.lowered && v.off >= sp.off-redZone && v.off+size <= min(sp.off, 0) {
				return SPPrivate
			}
			return SPRedZone
		}
		return SPInexact
	}
	return SPInexact
}

type spEvent struct {
	escape string // non-empty: an own-frame address may leave the registers here
	escLow int64  // lowest offset of the escaping addresses (math.MinInt64: unknown)
	poison string
}

func memArg(inst x86asm.Inst) (x86asm.Mem, int, bool) {
	for i, a := range inst.Args {
		if a == nil {
			break
		}
		if m, ok := a.(x86asm.Mem); ok {
			return m, i, true
		}
	}
	return x86asm.Mem{}, -1, false
}

// isRegOnlyALU: two-operand integer instructions "dst = f(dst, src)" (or dst = f(src)) whose only
// GPR write is Args[0] and that have no implicit register operand.
func isRegALU(op x86asm.Op) (known, readsDst bool) {
	switch op {
	case x86asm.ADD, x86asm.SUB, x86asm.AND, x86asm.OR, x86asm.XOR, x86asm.ADC, x86asm.SBB,
		x86asm.SHL, x86asm.SHR, x86asm.SAR, x86asm.ROL, x86asm.ROR, x86asm.RCL, x86asm.RCR,
		x86asm.NEG, x86asm.NOT, x86asm.INC, x86asm.DEC, x86asm.BSWAP, x86asm.IMUL,
		x86asm.SHLD, x86asm.SHRD, x86asm.BTS, x86asm.BTR, x86asm.BTC,
		x86asm.BSF, x86asm.BSR, // the destination is unchanged for a zero source
		x86asm.CMOVA, x86asm.CMOVAE, x86asm.CMOVB, x86asm.CMOVBE, x86asm.CMOVE, x86asm.CMOVG,
		x86asm.CMOVGE, x86asm.CMOVL, x86asm.CMOVLE, x86asm.CMOVNE, x86asm.CMOVNO, x86asm.CMOVNP,
		x86asm.CMOVNS, x86asm.CMOVO, x86asm.CMOVP, x86asm.CMOVS:
		return true, true
	case x86asm.MOVZX, x86asm.MOVSX, x86asm.MOVSXD, x86asm.MOVBE,
		x86asm.POPCNT, x86asm.LZCNT, x86asm.TZCNT,
		x86asm.ANDN, x86asm.BEXTR, x86asm.BZHI, x86asm.BLSI, x86asm.BLSR, x86asm.BLSMSK,
		x86asm.SHLX, x86asm.SHRX, x86asm.SARX, x86asm.RORX,
		x86asm.SETA, x86asm.SETAE, x86asm.SETB, x86asm.SETBE, x86asm.SETE, x86asm.SETG,
		x86asm.SETGE, x86asm.SETL, x86asm.SETLE, x86asm.SETNE, x86asm.SETNO, x86asm.SETNP,
		x86asm.SETNS, x86asm.SETO, x86asm.SETP, x86asm.SETS:
		return true, false
	}
	return false, false
}

// readsOnly: instructions that write no register and store no register value.
func readsOnly(op x86asm.Op) bool {
	switch op {
	case x86asm.CMP, x86asm.TEST, x86asm.BT, x86asm.NOP, x86asm.PAUSE,
		x86asm.PREFETCHNTA, x86asm.PREFETCHT0, x86asm.PREFETCHT1, x86asm.PREFETCHT2, x86asm.PREFETCHW,
		x86asm.LFENCE, x86asm.SFENCE, x86asm.MFENCE, x86asm.CLFLUSH,
		x86asm.CLC, x86asm.STC, x86asm.CMC, x86asm.CLD, x86asm.STD, x86asm.SAHF,
		x86asm.UD1, x86asm.UD2, x86asm.INT, x86asm.HLT:
		return true
	}
	return isJcc(op)
}

// implicitWrites lists the registers an instruction writes without naming them, and reports
// whether the table knows the instruction's implicit operands.
func implicitWrites(inst x86asm.Inst) (regs []int, known bool) {
	switch inst.Op {
	case x86asm.MUL, x86asm.DIV, x86asm.IDIV:
		return []int{rAX, rDX}, true
	case x86asm.IMUL:
		if inst.Args[1] == nil {
			return []int{rAX, rDX}, true
		}
		return nil, true
	case x86asm.CBW, x86asm.CWDE, x86asm.CDQE, x86asm.LAHF, x86asm.XLATB:
		return []int{rAX}, true
	case x86asm.CWD, x86asm.CDQ, x86asm.CQO:
		return []int{rDX}, true
	case x86asm.CPUID:
		return []int{rAX, rBX, rCX, rDX}, true
	case x86asm.RDTSC, x86asm.XGETBV, x86asm.RDPMC:
		return []int{rAX, rDX}, true
	case x86asm.RDTSCP:
		return []int{rAX, rCX, rDX}, true
	case x86asm.CMPXCHG, x86asm.XBEGIN:
		return []int{rAX}, true
	case x86asm.CMPXCHG8B, x86asm.CMPXCHG16B:
		return []int{rAX, rDX}, true
	case x86asm.LOOP, x86asm.LOOPE, x86asm.LOOPNE,
		x86asm.PCMPISTRI, x86asm.PCMPESTRI, x86asm.VPCMPISTRI, x86asm.VPCMPESTRI:
		return []int{rCX}, true
	case x86asm.XCHG, x86asm.XADD, x86asm.MULX, x86asm.MOVNTI, x86asm.CRC32,
		x86asm.RDRAND, x86asm.RDFSBASE, x86asm.RDGSBASE, x86asm.WRFSBASE, x86asm.WRGSBASE:
		return nil, true
	}
	return nil, false
}

// step applies one instruction to s. jumpResolved: an indirect JMP with recovered targets.
func (s *spState) step(in *Instruction, jumpResolved bool) (ev spEvent) {
	inst := in.Inst
	args := inst.Args
	ev.escLow = math.MaxInt64
	// escV: value v may leave the registers; esc: a value of unknown offset may.
	escV := func(why string, v spVal) {
		if ev.escape == "" {
			ev.escape = why
		}
		lo := int64(math.MinInt64)
		if v.k == spExact {
			lo = v.off
		}
		ev.escLow = min(ev.escLow, lo)
	}
	esc := func(why string) { escV(why, spVal{k: spMixed}) }
	regVal := func(a x86asm.Arg) (spVal, int, int, bool) {
		r, ok := a.(x86asm.Reg)
		if !ok {
			return spVal{}, 0, 0, false
		}
		i, bits, ok := gprIndex(r)
		if !ok {
			return spVal{}, 0, 0, false
		}
		return s.r[i], i, bits, true
	}
	// derived: the value of a register computed from sources of which some may be tainted.
	derived := func(taint bool) spVal {
		if taint {
			return spVal{k: spMixed}
		}
		return spVal{k: spClean}
	}

	switch inst.Op {
	case x86asm.MOV:
		if _, di, dbits, ok := regVal(args[0]); ok {
			if sv, _, sbits, ok := regVal(args[1]); ok {
				if dbits == 64 && sbits == 64 {
					s.write(di, sv)
				} else if dbits >= 32 {
					s.write(di, derived(sv.tainted()))
				} else {
					s.write(di, derived(sv.tainted() || s.r[di].tainted()))
				}
				return
			}
			if _, isSeg := args[1].(x86asm.Reg); isSeg {
				s.write(di, spVal{k: spClean})
				return
			}
			if m, ok := args[1].(x86asm.Mem); ok && s.o != nil && s.o.Params {
				// An argument passed on the stack.
				if v, _ := s.memAddr(m); v.k == spExact && v.off >= 0 {
					s.write(di, spVal{k: spMixed})
					return
				}
			}
			// Immediate or memory source. A frame address can only be in memory after an
			// escape, which makes the whole function non-private anyway.
			if dbits >= 32 {
				s.write(di, spVal{k: spClean})
			} else {
				s.write(di, derived(s.r[di].tainted()))
			}
			return
		}
		if _, ok := args[0].(x86asm.Mem); ok {
			if sv, _, _, ok := regVal(args[1]); ok && sv.tainted() {
				escV("stored", sv)
			}
			return
		}
	case x86asm.LEA:
		_, di, dbits, ok := regVal(args[0])
		m, isMem := args[1].(x86asm.Mem)
		if ok && isMem {
			v, _ := s.memAddr(m)
			if dbits != 64 || inst.AddrSize != 64 {
				v = derived(v.tainted())
			}
			s.write(di, v)
			return
		}
	case x86asm.ADD, x86asm.SUB:
		if dv, di, dbits, ok := regVal(args[0]); ok && dbits == 64 {
			if imm, ok := args[1].(x86asm.Imm); ok {
				d := int64(imm)
				if inst.Op == x86asm.SUB {
					d = -d
				}
				switch {
				case di == rSP:
					s.moveRSP(d)
					s.entry &^= 1 << rSP
				case dv.k == spExact:
					s.write(di, spVal{spExact, dv.off + d})
				default:
					s.write(di, dv) // clean stays clean, own stays own (in bounds), mixed stays mixed
				}
				return
			}
			if sv, si, sbits, ok := regVal(args[1]); ok {
				switch {
				case di == rSP:
					// Dynamic allocation or release: the extent of the frame is unknown from here.
					s.setRSP(spVal{k: spMixed}, false)
					s.entry &^= 1 << rSP
				case inst.Op == x86asm.SUB && si == di:
					s.write(di, spVal{k: spClean})
				case sv.k == spClean && sbits == 64 || !sv.tainted() && sbits < 64:
					// pointer +- integer
					switch {
					case dv.k == spClean, dv.k == spOwn:
						s.write(di, dv)
					case dv.k == spExact && s.low <= dv.off && dv.off < 0:
						s.write(di, spVal{k: spOwn})
					default:
						s.write(di, derived(dv.tainted()))
					}
				case dv.k == spClean && inst.Op == x86asm.ADD && sbits == 64:
					// integer + pointer
					switch {
					case sv.k == spOwn:
						s.write(di, sv)
					case sv.k == spExact && s.low <= sv.off && sv.off < 0:
						s.write(di, spVal{k: spOwn})
					default:
						s.write(di, derived(sv.tainted()))
					}
				default:
					s.write(di, derived(dv.tainted() || sv.tainted()))
				}
				return
			}
		}
	case x86asm.AND:
		if _, di, dbits, ok := regVal(args[0]); ok && di == rSP && dbits == 64 {
			if imm, ok := args[1].(x86asm.Imm); ok && int64(imm) < 0 && int64(imm) >= -4096 {
				// Stack realignment: rsp only moves down.
				s.setRSP(spVal{k: spMixed}, true)
				s.entry &^= 1 << rSP
				return
			}
		}
	case x86asm.XOR:
		if _, di, _, ok := regVal(args[0]); ok {
			if _, si, _, ok := regVal(args[1]); ok && si == di && args[0] == args[1] {
				if _, bits, _ := gprIndex(args[0].(x86asm.Reg)); bits >= 32 {
					s.write(di, spVal{k: spClean})
					return
				}
			}
		}
	case x86asm.PUSH:
		if inst.DataSize == 16 {
			// A 2-byte push: not a frame the pass follows.
			if sv, _, _, ok := regVal(args[0]); ok && sv.tainted() {
				esc("pushed")
			}
			s.setRSP(spVal{k: spMixed}, false)
			return
		}
		if sv, si, _, ok := regVal(args[0]); ok {
			if si == rSP || sv.tainted() {
				escV("pushed", sv)
			}
			wasEntry := s.entry&(1<<uint(si)) != 0 && calleeSavedMask&(1<<uint(si)) != 0
			s.moveRSP(-8)
			if wasEntry && s.r[rSP].k == spExact {
				s.saved |= slotBit(s.r[rSP].off)
			}
			return
		}
		s.moveRSP(-8)
		return
	case x86asm.POP:
		if inst.DataSize == 16 {
			s.setRSP(spVal{k: spMixed}, false)
			if _, di, _, ok := regVal(args[0]); ok && di != rSP {
				s.write(di, derived(s.r[di].tainted()))
			}
			return
		}
		if _, di, _, ok := regVal(args[0]); ok {
			if di == rSP {
				s.setRSP(spVal{k: spMixed}, false)
				s.entry &^= 1 << rSP
				return
			}
			s.moveRSP(8)
			s.write(di, spVal{k: spClean})
			return
		}
		s.moveRSP(8)
		return
	case x86asm.PUSHF, x86asm.PUSHFD, x86asm.PUSHFQ:
		s.moveRSP(-8)
		return
	case x86asm.POPF, x86asm.POPFD, x86asm.POPFQ:
		s.moveRSP(8)
		return
	case x86asm.LEAVE:
		s.setRSP(s.r[rBP], false)
		s.moveRSP(8)
		s.write(rBP, spVal{k: spClean})
		s.entry &^= 1 << rSP
		return
	case x86asm.ENTER:
		ev.poison = "enter"
		return
	case x86asm.CALL, x86asm.SYSCALL:
		if rel, ok := args[0].(x86asm.Rel); ok && rel == 0 && inst.Op == x86asm.CALL {
			// call next: the "callee" is the following instruction, which finds the return
			// address on the stack (the get-pc idiom). A push of a code address.
			s.moveRSP(-8)
			return
		}
		// The callee may keep any argument. RSP is the callee's by construction and RBP is
		// exempt as the frame pointer (assumption A3: a callee only saves and restores it).
		params := s.o != nil && s.o.Params
		cleanCallee := false
		if rel, ok := args[0].(x86asm.Rel); ok && inst.Op == x86asm.CALL {
			cleanCallee = s.o.clean(uint64(int64(in.Address) + int64(inst.Len) + int64(rel)))
		}
		// A clean import (a libc function that keeps no pointer): through its PLT stub or its GOT slot.
		importClean := false
		if s.o != nil && s.o.ImportClean != nil && inst.Op == x86asm.CALL {
			if rel, ok := args[0].(x86asm.Rel); ok {
				importClean = s.o.cleanImport(uint64(int64(in.Address) + int64(inst.Len) + int64(rel)))
			} else if m, ok := args[0].(x86asm.Mem); ok && m.Base == x86asm.RIP && m.Index == 0 && m.Segment == 0 {
				importClean = s.o.cleanImport(uint64(int64(in.Address) + int64(inst.Len) + m.Disp))
			}
		}
		if importClean {
			// No escape, but the result may point into an argument (memcpy, strchr): taint RAX then.
			argTaint := false
			for _, i := range []int{rDI, rSI, rDX, rCX, rR8, rR9} {
				argTaint = argTaint || s.r[i].tainted()
			}
			for _, i := range []int{rCX, rDX, rSI, rDI, rR8, rR9, rR10, rR11} {
				s.write(i, spVal{k: spClean})
			}
			s.write(rAX, derived(argTaint))
			return
		}
		for i := range s.r {
			if cleanCallee || i == rSP || i == rBP || !s.r[i].tainted() {
				continue
			}
			if (params || s.o != nil && s.o.CleanCallee != nil) && calleeSavedMask&(1<<uint(i)) != 0 {
				continue // interprocedural mode: every callee-saved register is opaque to a callee (A3)
			}
			escV("call", s.r[i])
		}
		if inst.Op == x86asm.CALL {
			if v, _, _, ok := regVal(args[0]); ok && v.tainted() {
				esc("call")
			}
		}
		for _, i := range []int{rAX, rCX, rDX, rSI, rDI, rR8, rR9, rR10, rR11} {
			s.write(i, spVal{k: spClean})
		}
		return
	case x86asm.RET:
		if s.o != nil && s.o.Params && s.r[rAX].tainted() {
			esc("returned")
		}
		return
	case x86asm.JMP:
		leaves := func() {
			if s.o == nil || !s.o.Params {
				return
			}
			for _, i := range []int{rAX, rCX, rDX, rSI, rDI, rR8, rR9, rR10, rR11} {
				if s.r[i].tainted() {
					esc("tail call")
				}
			}
		}
		if rel, ok := args[0].(x86asm.Rel); ok {
			if t := uint64(int64(in.Address) + int64(inst.Len) + int64(rel)); s.o != nil && s.o.fn.Blocks[t] == nil && !s.o.clean(t) {
				leaves()
			}
			return
		}
		if !jumpResolved {
			leaves()
		}
		if !jumpResolved && !(s.r[rSP].k == spExact && s.r[rSP].off == 0) {
			// An indirect jump with a live frame is a switch whose targets were not
			// recovered: blocks of this function may be entered on edges the graph lacks.
			ev.poison = "unresolved indirect jump"
		}
		return
	case x86asm.XCHG:
		av, ai, abits, aok := regVal(args[0])
		bv, bi, bbits, bok := regVal(args[1])
		if aok && bok {
			if abits == 64 && bbits == 64 {
				s.write(ai, bv)
				s.write(bi, av)
			} else {
				t := av.tainted() || bv.tainted()
				s.write(ai, derived(t))
				s.write(bi, derived(t))
			}
			return
		}
		// Register <-> memory.
		for _, a := range args[:2] {
			if v, i, bits, ok := regVal(a); ok {
				if v.tainted() {
					esc("stored")
				}
				if bits >= 32 {
					s.write(i, spVal{k: spClean})
				} else {
					s.write(i, derived(v.tainted()))
				}
			}
		}
		return
	}

	if readsOnly(inst.Op) {
		if rel, ok := args[0].(x86asm.Rel); ok && isJcc(inst.Op) && s.o != nil && s.o.Params {
			if t := uint64(int64(in.Address) + int64(inst.Len) + int64(rel)); s.o.fn.Blocks[t] == nil {
				for _, i := range []int{rAX, rCX, rDX, rSI, rDI, rR8, rR9, rR10, rR11} {
					if s.r[i].tainted() {
						esc("tail call")
					}
				}
			}
		}
		return
	}
	if isStringOp(inst.Op) && !hasVecArg(inst) {
		// Block operations address memory through RSI/RDI and advance them; STOS stores RAX.
		switch inst.Op {
		case x86asm.STOSB, x86asm.STOSW, x86asm.STOSD, x86asm.STOSQ:
			if s.r[rAX].tainted() {
				esc("stored")
			}
		case x86asm.LODSB, x86asm.LODSW, x86asm.LODSD, x86asm.LODSQ:
			s.write(rAX, derived(inst.Op != x86asm.LODSD && inst.Op != x86asm.LODSQ && s.r[rAX].tainted()))
		}
		for _, i := range []int{rSI, rDI} {
			v := s.r[i]
			switch {
			case v.k == spExact && s.low <= v.off && v.off < 0:
				v = spVal{k: spOwn}
			case v.k == spExact:
				v = derived(v.tainted())
			}
			s.write(i, v)
		}
		s.write(rCX, derived(s.r[rCX].tainted()))
		return
	}

	// Generic instruction. Every explicit GPR operand is treated as read and written, plus
	// the implicit registers of the table; a tainted value read by an instruction that also
	// has a non-GPR destination (memory, vector, x87) may leave the registers.
	impl, known := implicitWrites(inst)
	aluKnown, readsDst := isRegALU(inst.Op)
	vec := hasVecArg(inst) || strings.HasPrefix(inst.Op.String(), "F")
	taint := false
	var regs [4]int
	nreg := 0
	memDst := false
	for i, a := range args {
		if a == nil {
			break
		}
		switch x := a.(type) {
		case x86asm.Reg:
			if v, ri, _, ok := regVal(x); ok {
				if i > 0 || !aluKnown || readsDst {
					taint = taint || v.tainted()
				} else if _, bits, _ := gprIndex(x); bits < 32 {
					taint = taint || v.tainted() // partial write keeps the upper bits
				}
				if nreg < len(regs) {
					regs[nreg] = ri
					nreg++
				}
			}
		case x86asm.Mem:
			if i == 0 {
				memDst = true
			}
		}
	}
	if !known && !aluKnown && !vec {
		// Unknown integer instruction: assume the usual implicit registers.
		impl = []int{rAX, rCX, rDX}
	}
	if taint && (memDst || vec || !aluKnown) {
		esc("unmodelled use")
	}
	// Implicit operands are computed from each other (MUL, DIV, CMPXCHG) or overwritten (CPUID);
	// none of these instructions stores one of them.
	for _, i := range impl {
		taint = taint || s.r[i].tainted()
	}
	if aluKnown {
		// Only Args[0] is written.
		if _, di, _, ok := regVal(args[0]); ok {
			s.write(di, derived(taint))
		}
	} else {
		for _, ri := range regs[:nreg] {
			s.write(ri, derived(taint))
		}
	}
	for _, i := range impl {
		s.write(i, derived(taint))
	}
	return
}

func hasVecArg(inst x86asm.Inst) bool {
	for _, a := range inst.Args {
		if a == nil {
			break
		}
		if r, ok := a.(x86asm.Reg); ok && isVecOrFPReg(r) {
			return true
		}
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

// StackPrivVisit is called by AnalyzeStackPrivacyVisit for every instruction with an explicit
// memory operand (reason: why it is or is not private) after the bits are set.
type StackPrivVisit func(in *Instruction, reason StackPrivReason)

// AnalyzeStackPrivacy sets Instruction.StackPriv for every instruction of fn.
func AnalyzeStackPrivacy(fn *Function) StackPrivInfo {
	return AnalyzeStackPrivacyVisit(fn, nil)
}

// AnalyzeStackPrivacyVisit is AnalyzeStackPrivacy with a per-access callback (statistics).
func AnalyzeStackPrivacyVisit(fn *Function, visit StackPrivVisit) StackPrivInfo {
	return AnalyzeStackPrivacyOpts(fn, StackPrivOpts{}, visit)
}

// AnalyzeStackPrivacyOpts is AnalyzeStackPrivacyVisit with the interprocedural extensions.
func AnalyzeStackPrivacyOpts(fn *Function, opts StackPrivOpts, visit StackPrivVisit) StackPrivInfo {
	var info StackPrivInfo
	if fn == nil || len(fn.Blocks) == 0 {
		return info
	}
	in := make(map[uint64]*spState, len(fn.Blocks))
	for a := range fn.Blocks {
		in[a] = &spState{}
	}
	entry := in[fn.EntryAddr]
	if entry == nil {
		info.Poison = "no entry block"
	} else {
		*entry = spState{ok: true, entry: 0xffff, r: [16]spVal{rSP: {k: spExact}}, lowered: opts.CallEntered}
		if opts.Params || opts.CleanCallee != nil || opts.ImportClean != nil {
			entry.o = &spCtx{StackPrivOpts: opts, fn: fn}
		}
		if opts.Params {
			for _, i := range []int{rDI, rSI, rDX, rCX, rR8, rR9} {
				entry.r[i] = spVal{k: spMixed}
			}
		}
	}

	order := fn.BlockOrder
	succs := make(map[uint64][]uint64, len(fn.Blocks))
	for a, b := range fn.Blocks {
		succs[a], _ = blockSuccessors(fn, b, nil)
	}
	run := func(b *BasicBlock, s spState, each func(i int, pre *spState, ev spEvent)) spState {
		for i := range b.Insts {
			ins := &b.Insts[i]
			pre := s
			_, resolved := fn.JumpTables[ins.Address]
			ev := s.step(ins, resolved)
			if each != nil {
				each(i, &pre, ev)
			}
		}
		return s
	}
	if info.Poison == "" {
		const maxRounds = 64
		changed := true
		for round := 0; changed; round++ {
			if round == maxRounds {
				info.Poison = "no fixpoint"
				break
			}
			changed = false
			for _, a := range order {
				b := fn.Blocks[a]
				st := in[a]
				if b == nil || !st.ok {
					continue
				}
				out := run(b, *st, nil)
				for _, succ := range succs[a] {
					if t := in[succ]; t != nil && t.join(&out) {
						changed = true
					}
				}
			}
		}
	}
	// Final pass: events, then bits.
	if info.Poison == "" {
		for _, a := range order {
			b := fn.Blocks[a]
			st := in[a]
			if b == nil {
				continue
			}
			if !st.ok {
				// No path from the entry in the graph (a block only an unrecovered edge
				// reaches): nothing is known there, and a register it stores may hold a
				// frame address.
				*st = spTop()
				info.Unreached++
			}
			run(b, *st, func(i int, _ *spState, ev spEvent) {
				if ev.poison != "" && info.Poison == "" {
					info.Poison = ev.poison
				}
				if ev.escape != "" {
					if !info.Escaped {
						info.Escaped, info.EscapeAt, info.EscapeWhy, info.EscapeLow = true, b.Insts[i].Address, ev.escape, math.MaxInt64
					}
					info.EscapeLow = min(info.EscapeLow, ev.escLow)
				}
			})
		}
	}
	for _, a := range order {
		b := fn.Blocks[a]
		if b == nil {
			continue
		}
		st := in[a]
		if info.Poison != "" {
			top := spTop()
			st = &top
		}
		run(b, *st, func(i int, pre *spState, _ spEvent) {
			ins := &b.Insts[i]
			ins.StackPriv = StackPrivSeen
			switch ins.Inst.Op {
			case x86asm.POP, x86asm.LEAVE:
				sp := pre.r[rSP]
				if ins.Inst.Op == x86asm.LEAVE {
					sp = pre.r[rBP]
				}
				if info.Poison == "" && sp.k == spExact {
					if pre.saved&slotBit(sp.off) != 0 && (ins.Inst.Op == x86asm.POP || pre.r[rSP].k == spExact) {
						ins.StackPriv |= StackPrivPop
					} else if !info.Escaped && pre.inFrame(sp.off, 8) {
						ins.StackPriv |= StackPrivPop
					} else if info.Escaped && pre.inFrame(sp.off, 8) && sp.off+8 <= info.EscapeLow {
						ins.StackPriv |= StackPrivPopRegion
					}
				}
				if ins.StackPriv&StackPrivPop != 0 {
					ins.StackPriv |= StackPrivPopRegion
				}
			}
			m, _, ok := memArg(ins.Inst)
			if !ok || ins.Inst.Op == x86asm.LEA {
				return
			}
			if ins.Inst.Op == x86asm.POP && (m.Base == x86asm.RSP || m.Index == x86asm.RSP) {
				// pop [rsp+k] addresses its operand with the incremented stack pointer; the
				// state here is the one before the instruction.
				if visit != nil {
					visit(ins, SPInexact)
				}
				return
			}
			size := int64(ins.Inst.MemBytes)
			reason := pre.classify(m, size)
			if reason == SPPrivate || reason == SPAbove || reason == SPRedZone || reason == SPInexact {
				switch {
				case info.Poison != "":
					reason = SPPoisoned
				case info.Escaped:
					// The function-wide verdict comes first: the other reasons then count
					// what precision alone would gain. Under the region rule a slot at a
					// known offset below every escaping address stays private.
					if v, indexed := pre.memAddr(m); reason == SPPrivate && v.k == spExact && !indexed && size > 0 && v.off+size <= info.EscapeLow {
						reason = SPRegion
					} else {
						reason = SPEscaped
					}
				}
			}
			switch reason {
			case SPPrivate:
				ins.StackPriv |= StackPrivMem | StackPrivMemRegion
			case SPRegion:
				ins.StackPriv |= StackPrivMemRegion
			}
			if visit != nil {
				visit(ins, reason)
			}
		})
	}
	return info
}
