// simdstat measures what vector lane liveness (docs/m3/simdregs.md) does to the lowering of an
// ELF's functions: it lifts them with the pass off and in each mode and prints host instruction
// counts. Nothing is assembled or run.
//
//	simdstat [-fp-share 0.35] [-min 8] [-dump mode] [-emit dir] <elf> [0xADDR ...]
//
// With addresses: those functions, one line each. Without: every reachable function whose
// share of SSE/AVX instructions is at least -fp-share (and that has at least -min of them),
// summed.
//
// -emit dir (with addresses) also writes, per function and mode, dir/fn_<addr>_<mode>.s: the
// lowering as an assemblable function _kern_<addr>_<mode> (nested call model: it returns with
// ret), and dir/fn_<addr>.consts: "<guest address> <hex bytes>" for the data the function
// addresses RIP-relatively. A timing harness can map those bytes and call the function with
// the guest registers set up; that is how the per-kernel timings in docs/m3/simdregs.md were
// taken.
package main

import (
	"flag"
	"fmt"
	"os"
	"regexp"
	"strconv"
	"strings"

	"ps4-recomp/pkg/disasm"
	"ps4-recomp/pkg/elfloader"
	"ps4-recomp/pkg/lifter"

	"golang.org/x/arch/x86/x86asm"
)

type counts struct {
	x86, vec       int
	insts          int // host instructions
	upperZero      int // movi of v16-v31
	spill          int // vector save/restore on the host stack
	merge          int // lane inserts "mov vD.s[0]/d[0], vT..."
	unsupported    int
	fns, vexScalar int
}

func (c *counts) add(o counts) {
	c.x86 += o.x86
	c.vec += o.vec
	c.insts += o.insts
	c.upperZero += o.upperZero
	c.spill += o.spill
	c.merge += o.merge
	c.unsupported += o.unsupported
	c.fns += o.fns
}

var (
	upperZeroRe = regexp.MustCompile(`^movi v(1[6-9]|2[0-9]|3[01])\.16b, #0$`)
	spillRe     = regexp.MustCompile(`^(stp|str|ldp|ldr) q\d+.*\[sp`)
	mergeRe     = regexp.MustCompile(`^mov v\d+\.[sd]\[0\], v\d+\.[sd]\[0\]$`)
)

func isVec(inst x86asm.Inst) bool {
	for _, a := range inst.Args {
		if a == nil {
			break
		}
		if r, ok := a.(x86asm.Reg); ok && r >= x86asm.X0 && r <= x86asm.Y15 {
			return true
		}
	}
	return false
}

func vecShare(fn *disasm.Function) (n, vec int) {
	for _, b := range fn.Blocks {
		for _, in := range b.Insts {
			n++
			if isVec(in.Inst) {
				vec++
			}
		}
	}
	return
}

// lift lowers fn the way the emitter's block loop does and counts the result.
func lift(l *lifter.Arm64Lifter, fn *disasm.Function, dump bool, emit *strings.Builder) counts {
	var c counts
	c.fns = 1
	c.x86, c.vec = vecShare(fn)
	for bi, addr := range fn.BlockOrder {
		b := fn.Blocks[addr]
		if b == nil {
			continue
		}
		l.ResetFlags()
		l.SetBlock(b)
		var lines []string
		if emit != nil {
			fmt.Fprintf(emit, "L_fn_0x%x_loc_0x%x:\n", fn.EntryAddr, addr)
		}
		for _, in := range b.Insts {
			out, err := l.LiftInstructionToBuf(in, in.Address+uint64(in.Inst.Len), fn, nil)
			if err != nil {
				c.unsupported++
				continue
			}
			lines = append(lines, out...)
		}
		if n := len(b.Insts); n > 0 {
			lines = append(lines, l.EndBlock(b.Insts[n-1])...)
			var next uint64
			hasNext := false
			for _, a := range fn.BlockOrder[bi+1:] {
				if fn.Blocks[a] != nil {
					next, hasNext = a, true
					break
				}
			}
			lines = append(lines, l.FallThrough(fn, b.Insts[n-1], next, hasNext)...)
		}
		for _, ln := range lines {
			if dump {
				fmt.Println(ln)
			}
			if emit != nil {
				emit.WriteString(ln + "\n")
			}
			code := ln
			if i := strings.Index(code, "//"); i >= 0 {
				code = code[:i]
			}
			code = strings.TrimSpace(code)
			if code == "" || code[0] == '.' || strings.HasSuffix(code, ":") {
				continue
			}
			if strings.Contains(ln, "UNSUPPORTED") {
				c.unsupported++
			}
			c.insts++
			switch {
			case upperZeroRe.MatchString(code):
				c.upperZero++
			case spillRe.MatchString(code):
				c.spill++
			case mergeRe.MatchString(code):
				c.merge++
			}
		}
	}
	return c
}

func main() {
	share := flag.Float64("fp-share", 0.35, "minimum share of SSE/AVX instructions (walk mode)")
	min := flag.Int("min", 8, "minimum number of SSE/AVX instructions (walk mode)")
	dump := flag.String("dump", "", "print the lowering in this mode (off, strict, abi)")
	emitDir := flag.String("emit", "", "write assemblable lowerings and referenced constants of the given functions here")
	flag.Parse()
	if flag.NArg() < 1 {
		fmt.Fprintln(os.Stderr, "usage: simdstat [flags] <elf> [0xADDR ...]")
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
	modes := []disasm.VecLiveMode{disasm.VecLiveOff, disasm.VecLiveStrict, disasm.VecLiveABI}
	lifters := map[disasm.VecLiveMode]*lifter.Arm64Lifter{}
	for _, m := range modes {
		l := lifter.NewArm64Lifter(nil)
		l.SetAllowUnsupported(true)
		l.SetX87Mode("f80")
		l.SetVecLiveness(m != disasm.VecLiveOff)
		lifters[m] = l
	}
	measure := func(fn *disasm.Function) map[disasm.VecLiveMode]counts {
		r := map[disasm.VecLiveMode]counts{}
		for _, m := range modes {
			disasm.AnalyzeVecLiveness(fn, m)
			var emit *strings.Builder
			if *emitDir != "" {
				emit = &strings.Builder{}
				fmt.Fprintf(emit, ".text\n.globl _kern_%x_%s\n.p2align 2\n_kern_%x_%s:\n    str x30, [sp, #-16]!\n", fn.EntryAddr, m, fn.EntryAddr, m)
			}
			r[m] = lift(lifters[m], fn, *dump == m.String(), emit)
			if emit != nil {
				emit.WriteString("    brk #0x3e7\n")
				name := fmt.Sprintf("%s/fn_%x_%s.s", *emitDir, fn.EntryAddr, m)
				if err := os.WriteFile(name, []byte(emit.String()), 0o644); err != nil {
					fmt.Fprintln(os.Stderr, err)
					os.Exit(1)
				}
			}
		}
		if *emitDir != "" {
			var b strings.Builder
			seen := map[uint64]bool{}
			for _, blk := range fn.Blocks {
				for _, in := range blk.Insts {
					for _, a := range in.Inst.Args {
						mem, ok := a.(x86asm.Mem)
						if !ok || !(mem.Base == x86asm.RIP || (mem.Segment == 0 && mem.Base == 0 && mem.Index == 0 && mem.Scale == 0)) {
							continue
						}
						t := uint64(int64(in.Address) + int64(in.Inst.Len) + mem.Disp)
						if data := loaded.MemoryImage.Bytes(t, 32); data != nil && !seen[t] {
							seen[t] = true
							fmt.Fprintf(&b, "%x %x\n", t, data)
						}
					}
				}
			}
			if err := os.WriteFile(fmt.Sprintf("%s/fn_%x.consts", *emitDir, fn.EntryAddr), []byte(b.String()), 0o644); err != nil {
				fmt.Fprintln(os.Stderr, err)
				os.Exit(1)
			}
		}
		return r
	}
	row := func(name string, r map[disasm.VecLiveMode]counts) {
		off := r[disasm.VecLiveOff]
		fmt.Printf("%-14s fns=%d x86=%d vec=%d\n", name, off.fns, off.x86, off.vec)
		for _, m := range modes {
			c := r[m]
			fmt.Printf("  %-6s arm64=%-9d per-x86=%.2f  upper-zero=%-7d spill=%-7d lane-insert=%-7d vs off %+.1f%%\n",
				m, c.insts, float64(c.insts)/float64(max(c.x86, 1)), c.upperZero, c.spill, c.merge,
				100*float64(c.insts-off.insts)/float64(max(off.insts, 1)))
		}
	}
	if flag.NArg() > 1 {
		for _, a := range flag.Args()[1:] {
			addr, err := strconv.ParseUint(strings.TrimPrefix(a, "0x"), 16, 64)
			if err != nil {
				fmt.Fprintln(os.Stderr, "bad address", a)
				os.Exit(2)
			}
			fn, _, err := d.DisasmFunction(addr)
			if err != nil || fn == nil {
				fmt.Fprintf(os.Stderr, "0x%x: %v\n", addr, err)
				continue
			}
			row(fmt.Sprintf("0x%x", addr), measure(fn))
		}
		return
	}
	total := map[disasm.VecLiveMode]counts{}
	all := map[disasm.VecLiveMode]counts{}
	nAll := 0
	err = d.WalkReachable(disasm.SeedEntryPoints(loaded, false), func(fn *disasm.Function) error {
		nAll++
		n, vec := vecShare(fn)
		if vec == 0 {
			return nil
		}
		r := measure(fn)
		heavy := vec >= *min && float64(vec) >= *share*float64(n)
		for _, m := range modes {
			a := all[m]
			a.add(r[m])
			all[m] = a
			if heavy {
				t := total[m]
				t.add(r[m])
				total[m] = t
			}
		}
		return nil
	})
	if err != nil {
		fmt.Fprintln(os.Stderr, "walk:", err)
	}
	fmt.Printf("%d reachable functions\n", nAll)
	row("vector-heavy", total)
	row("any-vector", all)
}
