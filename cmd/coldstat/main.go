// coldstat measures cold-section outlining (docs/m3/coldpath.md) statically: every reachable
// function of an ELF is lifted with outlining off and on, and the host instructions of the
// lowering are counted in the hot stream and in the outlined bodies. Nothing is assembled or run.
//
//	coldstat [-n maxFuncs] <elf>
package main

import (
	"flag"
	"fmt"
	"os"
	"strings"

	"ps4-recomp/pkg/disasm"
	"ps4-recomp/pkg/elfloader"
	"ps4-recomp/pkg/lifter"
)

func countInsts(lines []string) int {
	n := 0
	for _, ln := range lines {
		code := ln
		if i := strings.Index(code, "//"); i >= 0 {
			code = code[:i]
		}
		code = strings.TrimSpace(code)
		if code == "" || code[0] == '.' || strings.HasSuffix(code, ":") {
			continue
		}
		n++
	}
	return n
}

func main() {
	maxFns := flag.Int("n", 0, "stop after this many functions (0: all)")
	flag.Parse()
	if flag.NArg() != 1 {
		fmt.Fprintln(os.Stderr, "usage: coldstat [-n N] <elf>")
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
	lOff := lifter.NewArm64Lifter(nil)
	lOn := lifter.NewArm64Lifter(nil)
	for _, l := range []*lifter.Arm64Lifter{lOff, lOn} {
		l.SetAllowUnsupported(true)
		l.SetX87Mode("f80")
	}
	var fns, blocks, x86 int
	var offHot, onHot, onCold, sitesOrdered int
	err = d.WalkReachable(disasm.SeedEntryPoints(loaded, false), func(fn *disasm.Function) error {
		if *maxFns > 0 && fns >= *maxFns {
			return nil
		}
		fns++
		for _, b := range fn.Blocks {
			blocks++
			lOff.ResetFlags()
			lOff.SetBlock(b)
			lOn.ResetFlags()
			lOn.SetBlock(b)
			for _, in := range b.Insts {
				x86++
				next := in.Address + uint64(in.Inst.Len)
				lifter.SetColdOutline(false)
				a, e1 := lOff.LiftInstructionToBuf(in, next, fn, nil)
				lifter.SetColdOutline(true)
				c, e2 := lOn.LiftInstructionToBuf(in, next, fn, nil)
				cold := lOn.TakeCold()
				if e1 != nil || e2 != nil {
					continue
				}
				offHot += countInsts(a)
				onHot += countInsts(c)
				onCold += countInsts(cold)
				for _, ln := range cold {
					if strings.HasPrefix(ln, "L_tso_cold_") {
						sitesOrdered++
					}
				}
			}
		}
		return nil
	})
	if err != nil {
		fmt.Fprintln(os.Stderr, "walk:", err)
	}
	fmt.Printf("functions %d, blocks %d, x86 instructions %d\n", fns, blocks, x86)
	fmt.Printf("host instructions of the lowering (TSO on, peephole on): outlining off %d\n", offHot)
	fmt.Printf("  outlining on: hot %d (%+d, %.2f%%), cold %d, total %d (%+d)\n", onHot, onHot-offHot, 100*float64(onHot-offHot)/float64(max(offHot, 1)), onCold, onHot+onCold, onHot+onCold-offHot)
	fmt.Printf("  outlined bodies (one per ordered/unaligned TSO access site): %d, %.2f cold instructions each\n", sitesOrdered, float64(onCold)/float64(max(sitesOrdered, 1)))
	fmt.Printf("  hot share of all lowered instructions: %.1f%% -> %.1f%%\n", 100.0, 100*float64(onHot)/float64(max(onHot+onCold, 1)))
}
