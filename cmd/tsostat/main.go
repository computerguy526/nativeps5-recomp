// tsostat counts, statically, what stack privacy (docs/m3/tsoelide.md, -tso-elide) does to the
// memory accesses of an ELF's reachable functions. Nothing is lifted, assembled or run.
//
//	tsostat [-fn 0xADDR] [-examples N] [-lift] <elf>
//
// -lift also lowers every function once per -tso-elide mode (no files are written) and prints
// the host instruction counts and the numbers of acquire loads and release stores.
//
// Only accesses the TSO rewrite can order are counted: an explicit memory operand of a scalar
// integer instruction (no vector, MMX or x87 operand, not a string instruction, not LEA or a
// prefetch), plus the implicit loads of POP and LEAVE.
package main

import (
	"flag"
	"fmt"
	"os"
	"sort"
	"strconv"
	"strings"

	"ps4-recomp/pkg/disasm"
	"ps4-recomp/pkg/elfloader"
	"ps4-recomp/pkg/lifter"

	"golang.org/x/arch/x86/x86asm"
)

func legacyStack(m x86asm.Mem) bool {
	switch m.Base {
	case x86asm.RSP, x86asm.ESP, x86asm.RBP, x86asm.EBP:
		return true
	}
	return false
}

// orderable mirrors lifter.tsoApplies without the base register test.
func orderable(inst x86asm.Inst) (x86asm.Mem, bool) {
	switch inst.Op {
	case x86asm.LEA, x86asm.NOP, x86asm.PREFETCHNTA, x86asm.PREFETCHT0, x86asm.PREFETCHT1, x86asm.PREFETCHT2, x86asm.PREFETCHW,
		x86asm.MOVSB, x86asm.MOVSW, x86asm.MOVSD, x86asm.MOVSQ, x86asm.STOSB, x86asm.STOSW, x86asm.STOSD,
		x86asm.STOSQ, x86asm.LODSB, x86asm.LODSW, x86asm.LODSD, x86asm.LODSQ, x86asm.SCASB, x86asm.SCASW,
		x86asm.SCASD, x86asm.SCASQ, x86asm.CMPSB, x86asm.CMPSW, x86asm.CMPSD, x86asm.CMPSQ:
		return x86asm.Mem{}, false
	}
	if strings.HasPrefix(inst.Op.String(), "F") {
		return x86asm.Mem{}, false
	}
	var mem x86asm.Mem
	found := false
	for _, a := range inst.Args {
		if a == nil {
			break
		}
		switch x := a.(type) {
		case x86asm.Reg:
			if x >= x86asm.X0 && x <= x86asm.Y15 || x >= x86asm.M0 && x <= x86asm.M7 {
				return x86asm.Mem{}, false
			}
		case x86asm.Mem:
			mem, found = x, true
		}
	}
	return mem, found
}

func main() {
	only := flag.String("fn", "", "analyse this function only and print every access")
	examples := flag.Int("examples", 0, "print up to N example functions per escape kind")
	ipa := flag.Int("ipa", 0, "interprocedural estimate: up to N rounds of callee summaries (a callee that neither keeps nor forwards its pointer arguments does not make a frame address escape); counts only, the translator does not do this")
	doLift := flag.Bool("lift", false, "lower every function in each -tso-elide mode and count host instructions")
	flag.Parse()
	if flag.NArg() != 1 {
		fmt.Fprintln(os.Stderr, "usage: tsostat [-fn 0xADDR] [-examples N] <elf>")
		os.Exit(2)
	}
	loaded, err := elfloader.LoadELF(flag.Arg(0))
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	d, err := disasm.NewDisassembler(loaded)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	d.SkipFlagLiveness = !*doLift // lifting needs the flag passes; they run with the stack privacy pass
	modes := []lifter.TSOElideMode{lifter.TSOElideOff, lifter.TSOElideStack, lifter.TSOElideRegion}
	var lifters []*lifter.Arm64Lifter
	var liftN [3]liftCount
	if *doLift {
		for _, m := range modes {
			l := lifter.NewArm64Lifter(nil)
			l.SetAllowUnsupported(true)
			l.SetX87Mode("f80")
			l.SetTSOElide(m)
			lifters = append(lifters, l)
		}
	}

	if *only != "" {
		addr, err := strconv.ParseUint(strings.TrimPrefix(*only, "0x"), 16, 64)
		if err != nil {
			fmt.Fprintln(os.Stderr, "bad address", *only)
			os.Exit(2)
		}
		fn, _, err := d.DisasmFunction(addr)
		if err != nil || fn == nil {
			fmt.Fprintln(os.Stderr, "disasm:", err)
			os.Exit(1)
		}
		why := map[uint64]disasm.StackPrivReason{}
		info := disasm.AnalyzeStackPrivacyOpts(fn, disasm.StackPrivOpts{ImportClean: d.ImportCleanFn()}, func(in *disasm.Instruction, r disasm.StackPrivReason) { why[in.Address] = r })
		fmt.Printf("fn 0x%x: escaped=%t (%s at 0x%x) poison=%q unreached=%d\n", addr, info.Escaped, info.EscapeWhy, info.EscapeAt, info.Poison, info.Unreached)
		for _, a := range fn.BlockOrder {
			for _, in := range fn.Blocks[a].Insts {
				note := ""
				if r, ok := why[in.Address]; ok {
					note = r.String()
				}
				if in.Inst.Op == x86asm.POP || in.Inst.Op == x86asm.LEAVE {
					note += fmt.Sprintf(" pop-private=%t", in.StackPriv&disasm.StackPrivPop != 0)
				}
				fmt.Printf("  0x%x  %-44s %s\n", in.Address, in.Inst.String(), note)
			}
		}
		return
	}

	var (
		nFn, nEsc, nPoison, nUnreached     int
		nInst                              int
		legacyPlain, legacyOrdered         int
		plainStay, plainToOrdered          int
		orderedToPlain, orderedStay        int
		popPrivate, popOrdered             int
		fnWithStack, fnWithStackEsc        int
		reasonLegacyPlain, reasonLegacyOrd [16]int
		escWhy                             = map[string]int{}
		poisonWhy                          = map[string]int{}
		ex                                 = map[string][]uint64{}
	)
	seeds := disasm.SeedEntryPoints(loaded, false)
	opts := disasm.StackPrivOpts{ImportClean: d.ImportCleanFn()} // what the translator uses
	if *ipa > 0 {
		clean := map[uint64]bool{}
		for round := 1; round <= *ipa; round++ {
			prev := clean
			next := map[uint64]bool{}
			o := disasm.StackPrivOpts{Params: true, CleanCallee: func(t uint64) bool { return prev[t] }}
			d.WalkReachable(seeds, func(fn *disasm.Function) error {
				if info := disasm.AnalyzeStackPrivacyOpts(fn, o, nil); !info.Escaped && info.Poison == "" && info.Unreached == 0 {
					next[fn.EntryAddr] = true
				}
				return nil
			})
			fmt.Printf("ipa round %d: %d functions neither keep nor forward a pointer argument\n", round, len(next))
			grew := len(next) > len(clean)
			clean = next
			if !grew {
				break
			}
		}
		opts.CleanCallee = func(t uint64) bool { return clean[t] }
	}
	err = d.WalkReachable(seeds, func(fn *disasm.Function) error {
		nFn++
		stack := 0
		for i, l := range lifters {
			liftN[i].add(l, fn)
		}
		info := disasm.AnalyzeStackPrivacyOpts(fn, opts, func(in *disasm.Instruction, r disasm.StackPrivReason) {
			m, ok := orderable(in.Inst)
			if !ok {
				return
			}
			if legacyStack(m) {
				legacyPlain++
				stack++
				reasonLegacyPlain[r]++
				if k := "plain-now-ordered:" + r.String(); r != disasm.SPPrivate && len(ex[k]) < *examples {
					ex[k] = append(ex[k], fn.EntryAddr)
				}
				if r == disasm.SPPrivate {
					plainStay++
				} else {
					plainToOrdered++
				}
			} else {
				legacyOrdered++
				reasonLegacyOrd[r]++
				if r == disasm.SPPrivate {
					orderedToPlain++
				} else {
					orderedStay++
				}
			}
		})
		for _, b := range fn.Blocks {
			nInst += len(b.Insts)
			for _, in := range b.Insts {
				if in.Inst.Op == x86asm.POP || in.Inst.Op == x86asm.LEAVE {
					if in.StackPriv&disasm.StackPrivPop != 0 {
						popPrivate++
					} else {
						popOrdered++
					}
				}
			}
		}
		if stack > 0 {
			fnWithStack++
		}
		nUnreached += info.Unreached
		switch {
		case info.Poison != "":
			nPoison++
			poisonWhy[info.Poison]++
			if len(ex["poison:"+info.Poison]) < *examples {
				ex["poison:"+info.Poison] = append(ex["poison:"+info.Poison], fn.EntryAddr)
			}
		case info.Escaped:
			nEsc++
			escWhy[info.EscapeWhy]++
			if stack > 0 {
				fnWithStackEsc++
			}
			if len(ex["escape:"+info.EscapeWhy]) < *examples {
				ex["escape:"+info.EscapeWhy] = append(ex["escape:"+info.EscapeWhy], fn.EntryAddr)
			}
		}
		return nil
	})
	if err != nil {
		fmt.Fprintln(os.Stderr, "walk:", err)
	}
	pct := func(a, b int) string {
		if b == 0 {
			return "-"
		}
		return fmt.Sprintf("%.1f%%", 100*float64(a)/float64(b))
	}
	fmt.Printf("functions %d, instructions %d\n", nFn, nInst)
	fmt.Printf("  escaped %d (%s), not analysable %d (%s), blocks without a path from the entry %d\n", nEsc, pct(nEsc, nFn), nPoison, pct(nPoison, nFn), nUnreached)
	fmt.Printf("  with rsp/rbp-relative scalar accesses %d, of those escaped %d (%s)\n", fnWithStack, fnWithStackEsc, pct(fnWithStackEsc, fnWithStack))
	keys := func(m map[string]int) []string {
		var k []string
		for s := range m {
			k = append(k, s)
		}
		sort.Strings(k)
		return k
	}
	for _, k := range keys(escWhy) {
		fmt.Printf("  first escape %-16s %d\n", k, escWhy[k])
	}
	for _, k := range keys(poisonWhy) {
		fmt.Printf("  not analysable: %-24s %d\n", k, poisonWhy[k])
	}
	total := legacyPlain + legacyOrdered
	fmt.Printf("orderable scalar accesses %d\n", total)
	fmt.Printf("  today (-tso-elide off): plain (rsp/rbp base) %d (%s), ordered %d (%s)\n", legacyPlain, pct(legacyPlain, total), legacyOrdered, pct(legacyOrdered, total))
	fmt.Printf("  -tso-elide stack:       plain %d (%s), ordered %d (%s)\n", plainStay+orderedToPlain, pct(plainStay+orderedToPlain, total), plainToOrdered+orderedStay, pct(plainToOrdered+orderedStay, total))
	region := plainStay + orderedToPlain + reasonLegacyPlain[disasm.SPRegion] + reasonLegacyOrd[disasm.SPRegion]
	fmt.Printf("  -tso-elide stack-region: plain %d (%s), ordered %d (stack plus slots below every escaping frame address)\n", region, pct(region, total), total-region)
	fmt.Printf("    rsp/rbp base:   stays plain %d (%s), becomes ordered %d (%s)\n", plainStay, pct(plainStay, legacyPlain), plainToOrdered, pct(plainToOrdered, legacyPlain))
	for r := disasm.StackPrivReason(1); int(r) < len(reasonLegacyPlain); r++ {
		if n := reasonLegacyPlain[r]; n > 0 {
			fmt.Printf("      %-16s %d (%s)\n", r, n, pct(n, legacyPlain))
		}
	}
	fmt.Printf("    other base:     becomes plain %d (%s), stays ordered %d\n", orderedToPlain, pct(orderedToPlain, legacyOrdered), orderedStay)
	for r := disasm.StackPrivReason(1); int(r) < len(reasonLegacyOrd); r++ {
		if n := reasonLegacyOrd[r]; n > 0 {
			fmt.Printf("      %-16s %d (%s)\n", r, n, pct(n, legacyOrdered))
		}
	}
	if *doLift {
		fmt.Printf("lowering (host instructions; acquire loads; release stores; fenced split accesses are in both paths)\n")
		for i, m := range modes {
			c := liftN[i]
			fmt.Printf("  %-13s insts %d (%+.2f%%)  ldapr/ldapur %d (%+.1f%%)  stlr/stlur %d (%+.1f%%)\n", m, c.insts,
				100*float64(c.insts-liftN[0].insts)/float64(max(liftN[0].insts, 1)), c.acq, 100*float64(c.acq-liftN[0].acq)/float64(max(liftN[0].acq, 1)),
				c.rel, 100*float64(c.rel-liftN[0].rel)/float64(max(liftN[0].rel, 1)))
		}
	}
	fmt.Printf("pop/leave loads %d: private or save slot %d (%s), ordered under stack %d\n", popPrivate+popOrdered, popPrivate, pct(popPrivate, popPrivate+popOrdered), popOrdered)
	for _, k := range keys(map[string]int(func() map[string]int {
		m := map[string]int{}
		for s := range ex {
			m[s] = 1
		}
		return m
	}())) {
		fmt.Printf("examples %s:", k)
		for _, a := range ex[k] {
			fmt.Printf(" 0x%x", a)
		}
		fmt.Println()
	}
}

type liftCount struct{ insts, acq, rel int }

// add lowers fn the way the emitter's block loop does and counts the lines.
func (c *liftCount) add(l *lifter.Arm64Lifter, fn *disasm.Function) {
	for _, addr := range fn.BlockOrder {
		b := fn.Blocks[addr]
		if b == nil {
			continue
		}
		l.ResetFlags()
		l.SetBlock(b)
		var lines []string
		for _, in := range b.Insts {
			out, err := l.LiftInstructionToBuf(in, in.Address+uint64(in.Inst.Len), fn, lines[:0])
			if err != nil {
				continue
			}
			lines = out
			for _, ln := range lines {
				code := strings.TrimSpace(ln)
				if code == "" || code[0] == '.' || code[0] == '/' || strings.HasSuffix(code, ":") {
					continue
				}
				c.insts++
				switch {
				case strings.HasPrefix(code, "ldap"):
					c.acq++
				case strings.HasPrefix(code, "stlr"), strings.HasPrefix(code, "stlur"):
					c.rel++
				}
			}
		}
	}
	l.ResetChunk()
}
