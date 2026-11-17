// liftmiscstat counts the sites the m3-liftmisc lifter changes (docs/m3/liftmisc.md) in an ELF and the
// host instructions they save, statically: FP compare + consumer windows, PUSH/POP runs, and
// indirect jumps. Nothing is assembled or run.
//
//	liftmiscstat [-json] <elf>
package main

import (
	"flag"
	"fmt"
	"os"
	"sort"
	"strings"

	"ps4-recomp/pkg/disasm"
	"ps4-recomp/pkg/elfloader"
	"ps4-recomp/pkg/lifter"

	"golang.org/x/arch/x86/x86asm"
)

type stats struct {
	fns, blocks, x86Insts int
	// FP compares
	fpCmp, fpFused, fpConsumers int
	fpByOp                      map[string]int
	fpWhy                       [8]int
	otherOps                    map[string]int
	fpHost                      [2]int // host instructions of the blocks that hold a fused window: [off, on]
	// push/pop
	pushes, pops          int
	pushRuns, popRuns     int
	pushInRuns, popInRuns int
	runLen                map[int]int
	ppHost                [2]int
	ppBlocks              int
	// indirect jumps
	jmpReg, jmpMem, jmpTable int
	jmpIC                    int
	callReg, callMem         int
	jmpRegIC, jmpMemIC       int
	unsupported              int
}

func hostCount(l *lifter.Arm64Lifter, fn *disasm.Function, b *disasm.BasicBlock) int {
	l.ResetFlags()
	l.SetBlock(b)
	n := 0
	for _, in := range b.Insts {
		out, err := l.LiftInstructionToBuf(in, in.Address+uint64(in.Inst.Len), fn, nil)
		if err != nil {
			continue
		}
		for _, ln := range out {
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
	}
	return n
}

func isPush(in x86asm.Inst) bool {
	r, ok := in.Args[0].(x86asm.Reg)
	return in.Op == x86asm.PUSH && ok && r != x86asm.RSP && r >= x86asm.RAX && r <= x86asm.R15
}
func isPop(in x86asm.Inst) bool {
	r, ok := in.Args[0].(x86asm.Reg)
	return in.Op == x86asm.POP && ok && r != x86asm.RSP && r >= x86asm.RAX && r <= x86asm.R15
}

func main() {
	flag.Parse()
	if flag.NArg() != 1 {
		fmt.Fprintln(os.Stderr, "usage: liftmiscstat <elf>")
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
	l := lifter.NewArm64Lifter(nil)
	l.SetAllowUnsupported(true)
	l.SetX87Mode("f80")
	s := stats{otherOps: map[string]int{}, fpByOp: map[string]int{}, runLen: map[int]int{}}
	err = d.WalkReachable(disasm.SeedEntryPoints(loaded, false), func(fn *disasm.Function) error {
		s.fns++
		for _, b := range fn.Blocks {
			s.blocks++
			fusedHere, ppHere := false, false
			for i := 0; i < len(b.Insts); i++ {
				in := b.Insts[i]
				s.x86Insts++
				switch op := in.Inst.Op; {
				case op == x86asm.UCOMISS || op == x86asm.UCOMISD || op == x86asm.COMISS || op == x86asm.COMISD ||
					op == x86asm.VUCOMISS || op == x86asm.VUCOMISD || op == x86asm.VCOMISS || op == x86asm.VCOMISD:
					s.fpCmp++
					s.fpWhy[in.FpWhy]++
					if in.FpWhy == disasm.FpWhyOther {
						for k := i + 1; k < len(b.Insts); k++ {
							o := b.Insts[k].Inst.Op
							if !strings.HasPrefix(o.String(), "J") && !strings.HasPrefix(o.String(), "SET") && !strings.HasPrefix(o.String(), "CMOV") && o != x86asm.MOV && o != x86asm.MOVSS && o != x86asm.MOVAPS && o != x86asm.LEA {
								s.otherOps[o.String()]++
								break
							}
						}
					}
					if in.FpFlagsLocal {
						s.fpFused++
						fusedHere = true
					}
				case op == x86asm.JMP:
					if _, rel := in.Inst.Args[0].(x86asm.Rel); rel {
						break
					}
					_, isReg := in.Inst.Args[0].(x86asm.Reg)
					if len(fn.JumpTables[in.Address]) > 0 {
						s.jmpTable++
					} else if isReg {
						s.jmpReg++
					} else {
						s.jmpMem++
					}
				case op == x86asm.CALL:
					if _, rel := in.Inst.Args[0].(x86asm.Rel); !rel {
						if _, isReg := in.Inst.Args[0].(x86asm.Reg); isReg {
							s.callReg++
						} else {
							s.callMem++
						}
					}
				}
				if in.FpRaw {
					s.fpConsumers++
					s.fpByOp[in.Inst.Op.String()]++
				}
				if isPush(in.Inst) || isPop(in.Inst) {
					push := isPush(in.Inst)
					j := i
					for j < len(b.Insts) && ((push && isPush(b.Insts[j].Inst)) || (!push && isPop(b.Insts[j].Inst))) {
						j++
					}
					if push {
						s.pushes += j - i
					} else {
						s.pops += j - i
					}
					n := min(j-i, 16)
					if j-i >= 2 {
						ppHere = true
						// pop runs stop at a repeated destination; count conservatively by splitting.
						s.runLen[n]++
						if push {
							s.pushRuns++
							s.pushInRuns += j - i
						} else {
							s.popRuns++
							s.popInRuns += j - i
						}
					}
					i = j - 1
					if j-i > 1 {
						s.x86Insts += j - i - 1
					}
				}
			}
			if fusedHere || ppHere {
				lifter.SetFpCmpFuse(false)
				lifter.SetPushPopFuse(false)
				off := hostCount(l, fn, b)
				if fusedHere {
					lifter.SetFpCmpFuse(true)
					s.fpHost[0] += off
					s.fpHost[1] += hostCount(l, fn, b)
					lifter.SetFpCmpFuse(false)
				}
				if ppHere {
					lifter.SetPushPopFuse(true)
					s.ppHost[0] += off
					s.ppHost[1] += hostCount(l, fn, b)
					s.ppBlocks++
				}
			}
		}
		return nil
	})
	if err != nil {
		fmt.Fprintln(os.Stderr, "walk:", err)
	}
	fmt.Printf("functions %d, blocks %d, x86 instructions %d\n", s.fns, s.blocks, s.x86Insts)
	fmt.Printf("FP compares (UCOMIS/COMIS, SSE+VEX): %d; fused (all readers local, flags dead after): %d (%.1f%%); consumers rewritten: %d\n",
		s.fpCmp, s.fpFused, 100*float64(s.fpFused)/float64(max(s.fpCmp, 1)), s.fpConsumers)
	fmt.Printf("  not fused: no mappable consumer %d, flags live out of the block %d, other instruction in the window %d\n", s.fpWhy[disasm.FpWhyNoConsumer], s.fpWhy[disasm.FpWhyLiveOut], s.fpWhy[disasm.FpWhyOther])
	{
		var ks []string
		for k := range s.otherOps {
			ks = append(ks, k)
		}
		sort.Slice(ks, func(i, j int) bool { return s.otherOps[ks[i]] > s.otherOps[ks[j]] })
		for i, k := range ks {
			if i < 12 {
				fmt.Printf("    blocked by %-10s %d\n", k, s.otherOps[k])
			}
		}
	}
	var ops []string
	for k := range s.fpByOp {
		ops = append(ops, k)
	}
	sort.Strings(ops)
	for _, k := range ops {
		fmt.Printf("  %-8s %d\n", k, s.fpByOp[k])
	}
	fmt.Printf("  host instructions of the %d-compare blocks (peephole on, TSO on): off %d, on %d (%+d)\n", s.fpFused, s.fpHost[0], s.fpHost[1], s.fpHost[1]-s.fpHost[0])
	fmt.Printf("PUSH r64: %d, POP r64: %d; runs of >=2: push %d (%d insts), pop %d (%d insts)\n", s.pushes, s.pops, s.pushRuns, s.pushInRuns, s.popRuns, s.popInRuns)
	var ls []int
	for k := range s.runLen {
		ls = append(ls, k)
	}
	sort.Ints(ls)
	for _, k := range ls {
		fmt.Printf("  run length %-2d %d\n", k, s.runLen[k])
	}
	fmt.Printf("  host instructions of the %d blocks that hold a run: off %d, on %d (%+d)\n", s.ppBlocks, s.ppHost[0], s.ppHost[1], s.ppHost[1]-s.ppHost[0])
	fmt.Printf("indirect jmp reg %d, jmp [mem] %d (these get the cache), with a recovered jump table %d (no cache); for comparison call reg %d, call [mem] %d\n",
		s.jmpReg, s.jmpMem, s.jmpTable, s.callReg, s.callMem)
}
