package lifter

import (
	"regexp"
	"strings"
)

// Peephole pass over the lowering of one guest instruction (docs/m3/perf.md).
//
// The lowerings are written for clarity: an effective address is built in a scratch register
// with a register copy first (mov x16, x23; add x16, x16, #36; ldrb w19, [x28, x16]) and a result is
// often produced in a scratch register and then copied to the guest register (movz x16, ...;
// movk x16, ...; mov x24, x16). On a per-pixel loop of a software blitter that is two to four host
// instructions per guest instruction for nothing. The pass removes the copies:
//
//  1. Copy propagation: after `mov xT, xS` (T one of the lowering scratch registers x16/x17), reads
//     of T become reads of S until S or T is written; the copy is deleted when T is dead.
//  2. Destination renaming: `op xT, ...; mov xD, xT` with T dead afterwards becomes `op xD, ...`.
//
// It only looks at straight-line code: a label, a branch, a call or any instruction it does not
// know ends the analysis of a copy (the copy is then kept). The scratch registers x16/x17 are dead
// at the end of an instruction's lowering unless a branch or call can read them (trampolines
// take arguments in registers the lowering text does not name), which is the same case.
//
// It runs before the TSO rewrite, which sees the same access shapes as before ([x28, xN]) with a
// guest register in place of the scratch register.

var (
	phRegTok   = regexp.MustCompile(`\b([wx])(\d+)\b`)
	phXRegOnly = regexp.MustCompile(`^x(\d+)$`)
	phWRegOnly = regexp.MustCompile(`^w(\d+)$`)
)

// The first operand is written and not read; the others are read.
var phDefOps = map[string]bool{
	"mov": true, "movz": true, "movn": true, "mvn": true, "neg": true, "negs": true,
	"add": true, "adds": true, "sub": true, "subs": true, "and": true, "ands": true, "orr": true, "orn": true,
	"eor": true, "eon": true, "bic": true, "bics": true, "lsl": true, "lsr": true, "asr": true, "ror": true,
	"mul": true, "madd": true, "msub": true, "mneg": true, "umulh": true, "smulh": true, "umull": true, "smull": true,
	"umaddl": true, "smaddl": true, "udiv": true, "sdiv": true,
	"ubfx": true, "sbfx": true, "ubfiz": true, "sbfiz": true, "uxtb": true, "uxth": true, "uxtw": true,
	"sxtb": true, "sxth": true, "sxtw": true, "clz": true, "cls": true, "rbit": true, "rev": true, "rev16": true, "rev32": true,
	"csel": true, "csinc": true, "csinv": true, "csneg": true, "cset": true, "csetm": true, "cinc": true, "cinv": true, "cneg": true,
	"adc": true, "adcs": true, "sbc": true, "sbcs": true, "ngc": true, "ngcs": true, "extr": true,
	"ldr": true, "ldrb": true, "ldrh": true, "ldrsb": true, "ldrsh": true, "ldrsw": true,
	"ldur": true, "ldurb": true, "ldurh": true, "ldursb": true, "ldursh": true, "ldursw": true,
	"ldapr": true, "ldaprb": true, "ldaprh": true, "mrs": true,
}

// Every operand is read.
var phUseOps = map[string]bool{
	"cmp": true, "cmn": true, "tst": true, "ccmp": true, "ccmn": true,
	"str": true, "strb": true, "strh": true, "stur": true, "sturb": true, "sturh": true,
	"stlr": true, "stlrb": true, "stlrh": true, "dmb": true, "nop": true,
}

// The first operand is read and written.
var phRmwOps = map[string]bool{"movk": true, "bfi": true, "bfxil": true, "bfc": true}

type phLine struct {
	ok      bool // an instruction of one of the three classes above, without writeback addressing
	indent  string
	op      string
	args    []string
	comment string
}

func phParse(ln string) phLine {
	var p phLine
	code := ln
	if i := strings.Index(code, "//"); i >= 0 {
		p.comment = code[i:]
		code = code[:i]
	}
	trim := strings.TrimLeft(code, " \t")
	p.indent = code[:len(code)-len(trim)]
	trim = strings.TrimRight(trim, " \t")
	if trim == "" || p.indent == "" || strings.HasSuffix(trim, ":") || trim[0] == '.' {
		return p
	}
	sp := strings.IndexAny(trim, " \t")
	if sp < 0 {
		p.op = trim
	} else {
		p.op = trim[:sp]
		rest := strings.TrimSpace(trim[sp:])
		depth, last := 0, 0
		for i := 0; i < len(rest); i++ {
			switch rest[i] {
			case '[':
				depth++
			case ']':
				depth--
			case ',':
				if depth == 0 {
					p.args = append(p.args, strings.TrimSpace(rest[last:i]))
					last = i + 1
				}
			}
		}
		p.args = append(p.args, strings.TrimSpace(rest[last:]))
	}
	if !phDefOps[p.op] && !phUseOps[p.op] && !phRmwOps[p.op] {
		return p
	}
	if strings.Contains(trim, "!") {
		return p // pre-index writeback
	}
	for i, a := range p.args {
		if strings.HasPrefix(a, "[") && i != len(p.args)-1 {
			return p // post-index writeback: an operand after the memory operand
		}
	}
	p.ok = true
	return p
}

func (p phLine) String() string {
	s := p.indent + p.op
	if len(p.args) > 0 {
		s += " " + strings.Join(p.args, ", ")
	}
	if p.comment != "" {
		s += " " + p.comment
	}
	return s
}

// phMentions reports whether the text names register number n (as wN or xN) outside a comment.
func phMentions(ln, n string) bool {
	if i := strings.Index(ln, "//"); i >= 0 {
		ln = ln[:i]
	}
	for _, m := range phRegTok.FindAllStringSubmatch(ln, -1) {
		if m[2] == n {
			return true
		}
	}
	return false
}

func phReplaceReg(arg, from, to string) string {
	return phRegTok.ReplaceAllStringFunc(arg, func(tok string) string {
		if tok[1:] == from {
			return tok[:1] + to
		}
		return tok
	})
}

// phDest returns the number of the general register the line writes ("" when none).
func phDest(p phLine) string {
	if !phDefOps[p.op] && !phRmwOps[p.op] || len(p.args) == 0 {
		return ""
	}
	if m := phRegTok.FindStringSubmatch(p.args[0]); m != nil && m[0] == p.args[0] {
		return m[2]
	}
	return ""
}

// phDeadAfter reports whether scratch register n is dead after body[idx]: no later line names it
// and no later line is anything but a known straight-line instruction.
func phDeadAfter(body []string, idx int, n string) bool {
	for j := idx + 1; j < len(body); j++ {
		t := strings.TrimSpace(body[j])
		if t == "" || strings.HasPrefix(t, "//") {
			continue
		}
		if !phParse(body[j]).ok || phMentions(body[j], n) {
			return false
		}
	}
	return true
}

func isPhScratch(n string) bool { return n == "16" || n == "17" }

// peephole optimises the lowering lines[start:] of one guest instruction.
func (l *Arm64Lifter) peephole(lines []string, start int) []string {
	if l.noPeephole || len(lines)-start < 2 {
		return lines
	}
	body := lines[start:]
	changed := false

	// 1. Copy propagation of `mov xT, xS`.
	for i := 0; i < len(body); i++ {
		p := phParse(body[i])
		if !p.ok || p.op != "mov" || len(p.args) != 2 {
			continue
		}
		mt, ms := phXRegOnly.FindStringSubmatch(p.args[0]), phXRegOnly.FindStringSubmatch(p.args[1])
		if mt == nil || ms == nil || !isPhScratch(mt[1]) || mt[1] == ms[1] || ms[1] == "31" {
			continue
		}
		T, S := mt[1], ms[1]
		dead := true // T is dead at the end of the lowering
		for j := i + 1; j < len(body); j++ {
			t := strings.TrimSpace(body[j])
			if t == "" || strings.HasPrefix(t, "//") {
				continue
			}
			q := phParse(body[j])
			if !q.ok {
				dead = false
				break
			}
			first := 0
			if phDefOps[q.op] {
				first = 1
			} else if phRmwOps[q.op] {
				if len(q.args) > 0 && phMentions(q.args[0], T) {
					dead = false // T is modified in place: the copy is needed
					break
				}
				first = 1
			}
			for k := first; k < len(q.args); k++ {
				if n := phReplaceReg(q.args[k], T, S); n != q.args[k] {
					q.args[k] = n
					changed = true
				}
			}
			body[j] = q.String()
			d := phDest(q)
			if d == T {
				break // redefined: the copy is dead from here
			}
			if d == S {
				dead = phDeadAfter(body, j, T)
				break
			}
		}
		if dead {
			body[i] = ""
			changed = true
		}
	}
	if changed {
		out := lines[:start]
		for _, ln := range body {
			if ln != "" {
				out = append(out, ln)
			}
		}
		lines = out
		body = lines[start:]
	}

	// 2. Destination renaming: `op T, ...` (or movz T + movk T ...) followed by `mov D, T`, T dead after.
	for i := 1; i < len(body); i++ {
		p := phParse(body[i])
		if !p.ok || p.op != "mov" || len(p.args) != 2 {
			continue
		}
		wide := true
		md, mt := phXRegOnly.FindStringSubmatch(p.args[0]), phXRegOnly.FindStringSubmatch(p.args[1])
		if md == nil || mt == nil {
			wide = false
			md, mt = phWRegOnly.FindStringSubmatch(p.args[0]), phWRegOnly.FindStringSubmatch(p.args[1])
		}
		if md == nil || mt == nil || !isPhScratch(mt[1]) || isPhScratch(md[1]) || md[1] == "31" {
			continue
		}
		T, D := mt[1], md[1]
		if !phDeadAfter(body, i, T) {
			continue
		}
		// The run of lines that builds T: one defining instruction, then movk's of T.
		j := i - 1
		for j >= 0 {
			q := phParse(body[j])
			if q.ok && q.op == "movk" && len(q.args) > 0 && q.args[0] == "x"+T && wide {
				j--
				continue
			}
			break
		}
		if j < 0 {
			continue
		}
		q := phParse(body[j])
		pre := "w"
		if wide {
			pre = "x"
		}
		if !q.ok || !phDefOps[q.op] || len(q.args) == 0 || q.args[0] != pre+T {
			continue
		}
		// D must not be named between the definition and the copy (the movk lines name only T).
		clash := false
		for k := j + 1; k < i; k++ {
			if phMentions(body[k], D) {
				clash = true
			}
		}
		if clash {
			continue
		}
		for k := j; k < i; k++ {
			r := phParse(body[k])
			r.args[0] = pre + D
			body[k] = r.String()
		}
		body[i] = ""
	}
	out := lines[:start]
	for _, ln := range body {
		if ln != "" {
			out = append(out, ln)
		}
	}
	return out
}
