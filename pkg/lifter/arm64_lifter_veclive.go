package lifter

import (
	"fmt"
	"regexp"
	"strconv"
	"strings"

	"ps4-recomp/pkg/disasm"

	"golang.org/x/arch/x86/x86asm"
)

// Vector lane liveness in the lifter (docs/m3/simdregs.md).
//
// The lowerings are written for the worst case: every lane of every guest vector register is
// observable after every instruction. disasm.AnalyzeVecLiveness says which lanes are not
// (Instruction.VecDead), and this file removes from the lowering of one instruction the work
// that only produces dead lanes:
//
//   - "movi vU<n>.16b, #0", the zeroing of the YMM upper half that a VEX-encoded write to
//     xmm<n> implies, when that upper half is dead (and, in a legacy SSE instruction lowered
//     with the code of its VEX form, always: such an instruction keeps the upper half);
//   - the lane merge of a scalar instruction ("mov vD.16b, vS.16b; mov vD.s[0], vT.s[0]"),
//     when the lanes above the scalar are dead: the scalar is then computed straight into the
//     destination, whose other lanes the arm64 scalar instruction zeroes;
//   - VZEROUPPER/VZEROALL zeroing of dead registers.
//
// fixUpperHalvesLive then handles the vector scratch registers (v16-v31, which are also the
// upper halves): a scratch register is moved to one whose upper half is dead, and saved and
// restored only when all candidates are live.
//
// A lane merge is also dropped when the lanes that would be merged in are known to be zero in
// the host register (vecZero, tracked forward through a block from the emitted text): the arm64
// scalar instruction then produces exactly the architectural value, live or not. This is what
// keeps scalar code short when its result is live in full, e.g. a double returned in xmm0,
// whose bits 64..127 the caller could in principle read.
//
// Two rewrites do not depend on liveness and are applied whenever the pass runs: a "movi" of a
// register that the next line loads with a scalar "ldr" (which zeroes the rest itself), and a
// register copied onto itself.

// SetVecLiveness turns the use of vector lane liveness on (the default) or off. Off, or when
// the function was not analysed (disasm.Function.VecLive), every lane is kept as before.
func (l *Arm64Lifter) SetVecLiveness(on bool) {
	l.noVecLive = !on
}

// VecLiveness reports whether vector lane liveness is used.
func (l *Arm64Lifter) VecLiveness() bool {
	return !l.noVecLive
}

var (
	vlMoviUpperRe = regexp.MustCompile(`^movi vU(\d+)\.16b, #0$`)
	vlMoviRe      = regexp.MustCompile(`^movi v(\d+)\.16b, #0$`)
	vlLoadRe      = regexp.MustCompile(`^ldr [sdq](\d+), `)
	vlInsRe       = regexp.MustCompile(`^mov v(\d+)\.([sd])\[0\], v(\d+)\.([sd])\[0\]$`)
	vlCopyRe      = regexp.MustCompile(`^mov v(\d+)\.16b, v(\d+)\.16b$`)
	vlDefRe       = regexp.MustCompile(`^([a-z0-9]+) ([sd])(\d+), (.*)$`)
)

// vlScalarDefs: arm64 instructions whose first operand, a scalar S or D register, is only
// written (the rest of the vector register is zeroed) and whose other operands are only read.
var vlScalarDefs = map[string]bool{
	"fadd": true, "fsub": true, "fmul": true, "fdiv": true, "fnmul": true,
	"fmin": true, "fmax": true, "fminnm": true, "fmaxnm": true,
	"fsqrt": true, "fneg": true, "fabs": true, "fmov": true,
	"fmadd": true, "fmsub": true, "fnmadd": true, "fnmsub": true,
	"frintn": true, "frintm": true, "frintp": true, "frintz": true, "frinta": true, "frintx": true, "frinti": true,
	"scvtf": true, "ucvtf": true, "fcvt": true, "fcsel": true, "ldr": true,
}

func vlCode(ln string) string {
	if i := strings.Index(ln, "//"); i >= 0 {
		ln = ln[:i]
	}
	return strings.TrimSpace(ln)
}

// vlRefs reports whether the code of ln names vector register n in any view (b/h/s/d/q/v).
func vlRefs(ln string, n int) bool {
	code := vlCode(ln)
	if code == "" {
		return false
	}
	num := strconv.Itoa(n)
	for i := 0; i+len(num) < len(code); i++ {
		switch code[i] {
		case 'b', 'h', 's', 'd', 'q', 'v':
		default:
			continue
		}
		if i > 0 && isWordByte(code[i-1]) {
			continue
		}
		j := i + 1
		if !strings.HasPrefix(code[j:], num) {
			continue
		}
		j += len(num)
		if j < len(code) && isWordByte(code[j]) {
			continue
		}
		return true
	}
	return false
}

// vlMayWrite reports whether the code of ln may write vector register n: it names n in its
// first operand, is a load (ldp writes its second operand too), or uses a register list.
func vlMayWrite(ln string, n int) bool {
	code := vlCode(ln)
	if !vlRefs(code, n) {
		return false
	}
	op, rest, _ := strings.Cut(code, " ")
	if strings.HasPrefix(op, "ld") || strings.Contains(code, "{") {
		return true
	}
	first, _, _ := strings.Cut(rest, ",")
	return vlRefs(first+" ", n)
}

// vlRefsUpper reports whether the code of ln names the upper-half placeholder U<n>.
func vlRefsUpper(ln string, n int) bool {
	code := vlCode(ln)
	tok := "U" + strconv.Itoa(n)
	for from := 0; ; {
		i := strings.Index(code[from:], tok)
		if i < 0 {
			return false
		}
		i += from
		j := i + len(tok)
		if (j >= len(code) || !isWordByte(code[j])) && i > 0 && strings.IndexByte("bhsdqv", code[i-1]) >= 0 {
			return true
		}
		from = i + 1
	}
}

func isWordByte(c byte) bool {
	return c == '_' || c >= '0' && c <= '9' || c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z'
}

// vecLivePeephole rewrites the lowering of one instruction (body, with the upper-half
// placeholders still in place) using the lanes that are dead after it. It returns a new slice.
func vecLivePeephole(inst x86asm.Inst, body []string, dead uint64, zero *vecZero) []string {
	isDead := func(n int, m uint64) bool {
		return n >= 0 && n < 16 && dead&disasm.VecLanes(n, m) == disasm.VecLanes(n, m)
	}
	out := make([]string, 0, len(body))

	if inst.Op == x86asm.VZEROUPPER || inst.Op == x86asm.VZEROALL {
		for _, ln := range body {
			if m := vlMoviRe.FindStringSubmatch(vlCode(ln)); m != nil {
				n, _ := strconv.Atoi(m[1])
				if n >= 16 && isDead(n-16, disasm.VecU) || n < 16 && isDead(n, disasm.VecLow) {
					continue
				}
			}
			out = append(out, ln)
		}
		return out
	}

	// The destination register of the x86 instruction, if it is a vector register.
	dstReg := -1
	if r, ok := inst.Args[0].(x86asm.Reg); ok {
		switch {
		case r >= x86asm.X0 && r <= x86asm.X15:
			dstReg = int(r - x86asm.X0)
		case r >= x86asm.Y0 && r <= x86asm.Y15:
			dstReg = int(r - x86asm.Y0)
		}
	}
	vex := strings.HasPrefix(inst.Op.String(), "V")

	// 1. Zeroing of an upper half that is dead (VEX write) or must be kept (legacy SSE
	// instruction lowered with the code of its VEX form).
	keep := make([]bool, len(body))
	for i, ln := range body {
		keep[i] = true
		m := vlMoviUpperRe.FindStringSubmatch(vlCode(ln))
		if m == nil {
			continue
		}
		n, _ := strconv.Atoi(m[1])
		later := false
		for _, l2 := range body[i+1:] {
			if vlRefsUpper(l2, n) {
				later = true
				break
			}
		}
		if later {
			continue
		}
		// Nothing later in the lowering reads the register: zeroing a dead upper half is
		// unobservable, whatever the instruction.
		if isDead(n-16, disasm.VecU) {
			keep[i] = false
			continue
		}
		if vex {
			continue
		}
		other := false
		for k, l2 := range body {
			if k != i && vlRefsUpper(l2, n) {
				other = true
				break
			}
		}
		if !other {
			keep[i] = false
		}
	}
	for i, ln := range body {
		if keep[i] {
			out = append(out, ln)
		}
	}

	// 2. "movi vD, #0" directly before a scalar load of the same register; self copies.
	body, out = out, make([]string, 0, len(out))
	for i, ln := range body {
		code := vlCode(ln)
		if m := vlMoviRe.FindStringSubmatch(code); m != nil && i+1 < len(body) {
			if m2 := vlLoadRe.FindStringSubmatch(vlCode(body[i+1])); m2 != nil && m2[1] == m[1] {
				continue
			}
		}
		if m := vlCopyRe.FindStringSubmatch(code); m != nil && m[1] == m[2] {
			continue
		}
		out = append(out, ln)
	}

	// 3. Scalar lane merge into the destination when the lanes above the scalar are dead.
	if dstReg < 0 {
		return out
	}
	for j := 0; j < len(out); j++ {
		m := vlInsRe.FindStringSubmatch(vlCode(out[j]))
		if m == nil || m[2] != m[4] {
			continue
		}
		d, _ := strconv.Atoi(m[1])
		t, _ := strconv.Atoi(m[3])
		e := m[2]
		if d != dstReg || t == d {
			continue
		}
		// The lanes above the scalar come from the register copied into the destination right
		// before the insert, or stay those of the destination.
		src := d
		hasCopy := false
		if j > 0 {
			if c := vlCopyRe.FindStringSubmatch(vlCode(out[j-1])); c != nil && c[1] == m[1] {
				src, _ = strconv.Atoi(c[2])
				hasCopy = true
			}
		}
		// What is known about src's lanes describes it before the instruction: it only holds
		// here if the lowering has not written src so far.
		srcKnown := true
		last := j
		if hasCopy {
			last = j - 1
		}
		for _, l2 := range out[:last] {
			if vlMayWrite(l2, src) {
				srcKnown = false
				break
			}
		}
		foldable := true
		for _, lane := range []uint64{disasm.VecL1, disasm.VecH} {
			if lane == disasm.VecL1 && e == "d" {
				continue
			}
			if !isDead(d, lane) && !(srcKnown && zero.has(src, lane)) {
				foldable = false
			}
		}
		if !foldable {
			continue
		}
		later := false
		for _, l2 := range out[j+1:] {
			if vlRefs(l2, d) {
				later = true
				break
			}
		}
		if later {
			continue
		}
		ind := out[j][:len(out[j])-len(strings.TrimLeft(out[j], " \t"))]
		// The full copy into the destination right before the insert is dead now.
		if hasCopy {
			out = append(out[:j-1], out[j:]...)
			j--
		}
		out[j] = fmt.Sprintf("%sfmov %s%d, %s%d", ind, e, d, e, t)
		// Fold the scalar definition of the temporary into the destination.
		if j == 0 {
			continue
		}
		tLater := false
		for _, l2 := range out[j+1:] {
			if vlRefs(l2, t) {
				tLater = true
				break
			}
		}
		if tLater {
			continue
		}
		prev := vlCode(out[j-1])
		pind := out[j-1][:len(out[j-1])-len(strings.TrimLeft(out[j-1], " \t"))]
		if c := vlCopyRe.FindStringSubmatch(prev); c != nil && c[1] == m[3] {
			// mov vT.16b, vX.16b; fmov sD, sT  ->  fmov sD, sX
			out[j-1] = fmt.Sprintf("%sfmov %s%d, %s%s", pind, e, d, e, c[2])
			out = append(out[:j], out[j+1:]...)
			j--
			continue
		}
		if p := vlDefRe.FindStringSubmatch(prev); p != nil && vlScalarDefs[p[1]] && p[2] == e && p[3] == m[3] {
			out[j-1] = fmt.Sprintf("%s%s %s%d, %s", pind, p[1], e, d, p[4])
			out = append(out[:j], out[j+1:]...)
			j--
		}
	}
	return out
}

// vecZero tracks, through the instructions of one block, which lanes above the low scalar of
// the host registers v0-v15 are known to be zero: bit VecL1 (bits 32..63) and VecH (bits
// 64..127) per register. It is derived from the emitted text only, so it describes the host
// registers whatever the lowering was; anything it does not recognise clears what it names.
type vecZero struct {
	z    [16]uint8
	fn   *disasm.Function
	next uint64
}

func (v *vecZero) has(n int, lane uint64) bool {
	return v != nil && n >= 0 && n < 16 && uint64(v.z[n])&lane != 0
}

func (v *vecZero) reset() { v.z = [16]uint8{} }

// begin is called before an instruction is lifted: the facts survive only from the previous
// instruction of the same block (same function, contiguous address, not a block leader).
func (v *vecZero) begin(fn *disasm.Function, addr, next uint64) {
	if fn == nil || fn != v.fn || addr != v.next || fn.Blocks[addr] != nil {
		v.reset()
	}
	v.fn, v.next = fn, next
}

var (
	vlRegTokRe = regexp.MustCompile(`\b[bhsdqv](\d+)\b`)
	vlBitRe    = regexp.MustCompile(`^(and|orr|eor|bic) v(\d+)\.16b, v(\d+)\.16b, v(\d+)\.16b$`)
)

// vlFlow reports a line that is a label or transfers control.
func vlFlow(code string) (flow, call bool) {
	if strings.HasSuffix(code, ":") {
		return true, false
	}
	op, _, _ := strings.Cut(code, " ")
	switch {
	case op == "bl" || op == "blr":
		return true, true
	case op == "b" || op == "br" || op == "ret" || op == "cbz" || op == "cbnz" || op == "tbz" || op == "tbnz" ||
		strings.HasPrefix(op, "b."):
		return true, false
	}
	return false, false
}

// update applies the final lowering of one instruction.
func (v *vecZero) update(lines []string) {
	const all = disasm.VecL1 | disasm.VecH
	var codes []string
	flow, call, list := false, false, false
	for _, ln := range lines {
		c := vlCode(ln)
		if c == "" || c[0] == '.' {
			continue
		}
		f, cl := vlFlow(c)
		// Code outside the lowering may run: a helper or a guest function entered with bl/blr,
		// or with a branch to a global symbol after the return address was put in x30 by hand.
		if f && (strings.Contains(c, " _") || strings.Contains(c, "x30")) || strings.Contains(c, "x30") {
			cl = true
		}
		flow, call = flow || f, call || cl
		list = list || strings.Contains(c, "{")
		codes = append(codes, c)
	}
	clearNamed := func(c string) {
		for _, m := range vlRegTokRe.FindAllStringSubmatch(c, -1) {
			if n, _ := strconv.Atoi(m[1]); n < 16 {
				v.z[n] = 0
			}
		}
	}
	if call || list {
		v.reset()
		return
	}
	if flow {
		// More than one path through the lowering: nothing is known about what it names.
		for _, c := range codes {
			clearNamed(c)
		}
		return
	}
	get := func(n int) uint8 {
		if n < 16 {
			return v.z[n]
		}
		return 0
	}
	for _, c := range codes {
		op, rest, _ := strings.Cut(c, " ")
		first, _, _ := strings.Cut(strings.TrimSpace(rest), ",")
		if m := vlDefRe.FindStringSubmatch(c); m != nil && vlScalarDefs[m[1]] {
			if n, _ := strconv.Atoi(m[3]); n < 16 {
				if m[2] == "s" {
					v.z[n] = all
				} else {
					v.z[n] = disasm.VecH
				}
			}
			continue
		}
		if m := vlCopyRe.FindStringSubmatch(c); m != nil {
			d, _ := strconv.Atoi(m[1])
			src, _ := strconv.Atoi(m[2])
			if d < 16 {
				v.z[d] = get(src)
			}
			continue
		}
		if m := vlInsRe.FindStringSubmatch(c); m != nil && m[2] == m[4] {
			if d, _ := strconv.Atoi(m[1]); d < 16 && m[2] == "d" {
				v.z[d] &= disasm.VecH
			}
			continue
		}
		if m := vlMoviRe.FindStringSubmatch(c); m != nil {
			if d, _ := strconv.Atoi(m[1]); d < 16 {
				v.z[d] = all
			}
			continue
		}
		if m := vlBitRe.FindStringSubmatch(c); m != nil {
			d, _ := strconv.Atoi(m[2])
			a, _ := strconv.Atoi(m[3])
			b, _ := strconv.Atoi(m[4])
			if d < 16 {
				switch {
				case m[1] == "eor" && a == b: // zero idiom
					v.z[d] = all
				case m[1] == "and":
					v.z[d] = get(a) | get(b)
				case m[1] == "bic":
					v.z[d] = get(a)
				default:
					v.z[d] = get(a) & get(b)
				}
			}
			continue
		}
		// Instructions that only read vector registers: compares, stores, and anything whose
		// destination is a general-purpose register.
		switch op {
		case "fcmp", "fcmpe", "str", "stur", "stp":
			continue
		}
		if len(first) > 1 && (first[0] == 'w' || first[0] == 'x') && first[1] >= '0' && first[1] <= '9' &&
			op != "ldp" && op != "ldr" && op != "ldur" {
			continue
		}
		clearNamed(c)
	}
}
