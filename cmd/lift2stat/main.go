// lift2stat counts, statically, the sites the m3-lift2 lifter options change (docs/m3/lift2.md) in an
// ELF's reachable functions and the host instructions they save. Nothing is assembled or run.
//
//	lift2stat [-lift] [-fn 0xADDR] <elf>
//
// -lift lowers every block that holds a site with the option off and on and sums the host instructions.
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

func hostCount(l *lifter.Arm64Lifter, fn *disasm.Function, b *disasm.BasicBlock) (n, acq int) {
	l.ResetFlags()
	l.SetBlock(b)
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
			if strings.HasPrefix(code, "ldap") || strings.HasPrefix(code, "stlr") || strings.HasPrefix(code, "stlur") {
				acq++
			}
		}
	}
	return
}

func isReg8(a x86asm.Arg, r x86asm.Reg) bool { x, ok := a.(x86asm.Reg); return ok && x == r }

type flagSet struct{ ro, rz, bmi bool }

func main() {
	doLift := flag.Bool("lift", false, "lower the blocks with sites with each option off and on")
	guardGap := flag.Bool("guard-gap", false, "print examples of guard pointer loads whose value load is not recognised")
	only := flag.String("fn", "", "print the idiom census of one function only")
	flag.Parse()
	if flag.NArg() != 1 {
		fmt.Fprintln(os.Stderr, "usage: lift2stat [-lift] [-fn 0xADDR] <elf>")
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
	d.SkipFlagLiveness = !*doLift
	d.ROLoads = true
	d.LeafRedZone = true
	fmt.Printf("RO ranges:")
	for _, r := range loaded.ROAddrRanges {
		fmt.Printf(" [0x%x,0x%x)", r.Start, r.End)
	}
	fmt.Printf("  canary 0x%x\n", loaded.CanaryAddr)
	l := lifter.NewArm64Lifter(nil)
	l.SetAllowUnsupported(true)
	l.SetX87Mode("f80")
	l.SetTSOElide(lifter.TSOElideStack)

	var (
		nFn, nInst, nBlocks int
		ro, roGot, roGuard  int
		orderableAll        int
		idiom               = map[string]int{}
		hostOff, hostOn     = map[string]int{}, map[string]int{}
		acqOff, acqOn       = map[string]int{}, map[string]int{}
		rzPriv, rzOff       int
	)
	_ = rzPriv
	_ = rzOff
	seeds := disasm.SeedEntryPoints(loaded, false)
	if *only != "" {
		a, _ := strconv.ParseUint(strings.TrimPrefix(*only, "0x"), 16, 64)
		seeds = []uint64{a}
	}
	// Pass 1: discovery (records the jump-entered functions for the leaf red zone rule).
	{
		prev := d.SkipFlagLiveness
		d.SkipFlagLiveness = true
		d.WalkReachable(seeds, func(*disasm.Function) error { return nil })
		d.SkipFlagLiveness = prev
		fmt.Printf("leaf red zone: %d functions are targets of a branch from unlowered code\n", d.JumpEnteredCount())
	}
	var nCallEntered, rzAccOff, rzAccOn, accTotal int
	gapEx := 0
	walk := func(fn *disasm.Function) error {
		nFn++
		if !*doLift {
			disasm.AnalyzeROLoads(fn, disasm.ROInfoOf(loaded))
		}
		{
			// Stack privacy with and without the call-entered red zone: private accesses of orderable instructions.
			count := func(ce bool) int {
				disasm.AnalyzeStackPrivacyOpts(fn, disasm.StackPrivOpts{ImportClean: d.ImportCleanFn(), CallEntered: ce}, nil)
				n := 0
				for _, b := range fn.Blocks {
					for _, in := range b.Insts {
						if _, ok := orderableMem(in.Inst); ok && in.StackPriv&disasm.StackPrivMem != 0 {
							n++
						}
					}
				}
				return n
			}
			ce := d.CallEntered(fn.EntryAddr)
			a := count(false)
			if ce {
				nCallEntered++
				rzAccOff += a
				rzAccOn += count(true)
			}
			for _, b := range fn.Blocks {
				for _, in := range b.Insts {
					if _, ok := orderableMem(in.Inst); ok {
						accTotal++
					}
				}
			}
		}
		for _, addr := range fn.BlockOrder {
			b := fn.Blocks[addr]
			nBlocks++
			nInst += len(b.Insts)
			cats := map[string]bool{}
			for i, in := range b.Insts {
				inst := in.Inst
				if *guardGap && in.ROLoad && inst.Op == x86asm.MOV && inst.MemBytes == 8 && gapEx < 12 {
					if m, ok := firstMem(inst); ok && m.Base == x86asm.RIP {
						if v, ok2 := loaded.MemoryImage.U64(in.Address + uint64(inst.Len) + uint64(int64(m.Disp))); ok2 && v == loaded.CanaryAddr {
							used := false
							for k := i + 1; k < len(b.Insts) && k < i+8; k++ {
								if b.Insts[k].ROLoad {
									used = true
								}
							}
							if !used {
								gapEx++
								fmt.Printf("guard pointer load without a value load in the block at 0x%x:\n", in.Address)
								for k := i; k < len(b.Insts) && k < i+6; k++ {
									fmt.Printf("    0x%x %s\n", b.Insts[k].Address, b.Insts[k].Inst)
								}
							}
						}
					}
				}
				if in.ROLoad {
					ro++
					if m, ok := firstMem(inst); ok && m.Base == x86asm.RIP {
						roGot++
					} else {
						roGuard++
					}
					cats["ro"] = true
				}
				switch inst.Op {
				case x86asm.BLSI, x86asm.BLSR, x86asm.BLSMSK, x86asm.BEXTR, x86asm.ANDN, x86asm.LZCNT, x86asm.TZCNT, x86asm.BZHI:
					k := strings.ToLower(inst.Op.String())
					if in.SkipFlags {
						k += "/flags-dead"
					} else if in.CarryDead {
						k += "/cf-dead"
					} else {
						k += "/flags-live"
					}
					idiom[k]++
					cats["bmi"] = true
				case x86asm.TEST:
					if r0, ok := inst.Args[0].(x86asm.Reg); ok {
						if r1, ok := inst.Args[1].(x86asm.Reg); ok && r0 == r1 && (r0 == x86asm.AL || r0 >= x86asm.CL && r0 <= x86asm.BL || r0 >= x86asm.SPB && r0 <= x86asm.R15B) {
							idiom["test r8,r8"]++
							cats["bmi"] = true
						}
					}
				case x86asm.CMP:
					if m, ok := inst.Args[0].(x86asm.Mem); ok && inst.MemBytes == 1 {
						if imm, ok := inst.Args[1].(x86asm.Imm); ok && imm == 0 {
							k := "cmp byte [m],0"
							if i+1 < len(b.Insts) {
								switch b.Insts[i+1].Inst.Op {
								case x86asm.JE, x86asm.JNE:
									k += "; je/jne"
									cats["bmi"] = true
								}
							}
							_ = m
							idiom[k]++
						}
					}
				case x86asm.XOR:
					if r0, ok := inst.Args[0].(x86asm.Reg); ok {
						if r1, ok := inst.Args[1].(x86asm.Reg); ok && r0 == r1 {
							if i+1 < len(b.Insts) && b.Insts[i+1].Inst.Op == x86asm.TEST {
								idiom["xor r,r; test"]++
								cats["bmi"] = true
							}
						} else if imm, ok := inst.Args[1].(x86asm.Imm); ok && imm == 1 && r0 >= x86asm.AL && r0 <= x86asm.R15B {
							if i+1 < len(b.Insts) && b.Insts[i+1].Inst.Op == x86asm.MOVZX {
								idiom["xor r8,1; movzx"]++
								cats["bmi"] = true
							}
						}
					}
				}
			}
			if *doLift && len(cats) > 0 {
				for _, c := range []string{"ro", "bmi"} {
					if !cats[c] {
						continue
					}
					set := func(on bool) {
						switch c {
						case "ro":
							lifter.SetROLoads(on)
						case "bmi":
							lifter.SetBMIPeep(on)
						}
					}
					set(false)
					n0, a0 := hostCount(l, fn, b)
					set(true)
					n1, a1 := hostCount(l, fn, b)
					hostOff[c] += n0
					hostOn[c] += n1
					acqOff[c] += a0
					acqOn[c] += a1
				}
			}
		}
		_ = orderableAll
		return nil
	}
	if err := d.WalkReachable(seeds, walk); err != nil {
		fmt.Fprintln(os.Stderr, "walk:", err)
	}
	fmt.Printf("functions %d, blocks %d, instructions %d\n", nFn, nBlocks, nInst)
	fmt.Printf("leaf red zone (-redzone-leaf): %d of %d functions are call entered; their private orderable accesses: %d -> %d (%+d); all orderable scalar accesses %d\n", nCallEntered, nFn, rzAccOff, rzAccOn, rzAccOn-rzAccOff, accTotal)
	fmt.Printf("plain loads from read-only memory (-ro-loads): %d  (RIP-relative RELRO/rodata %d, stack guard value %d)\n", ro, roGot, roGuard)
	var ks []string
	for k := range idiom {
		ks = append(ks, k)
	}
	sort.Strings(ks)
	for _, k := range ks {
		fmt.Printf("  %-32s %d\n", k, idiom[k])
	}
	for _, c := range []string{"ro", "bmi"} {
		if hostOff[c] > 0 {
			fmt.Printf("host instructions of the blocks with %s sites: off %d, on %d (%+d); acquire/release: off %d, on %d (%+d)\n", c, hostOff[c], hostOn[c], hostOn[c]-hostOff[c], acqOff[c], acqOn[c], acqOn[c]-acqOff[c])
		}
	}
}

// orderableMem: an explicit memory operand of a scalar integer instruction the TSO rewrite can order.
func orderableMem(inst x86asm.Inst) (x86asm.Mem, bool) {
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

func firstMem(inst x86asm.Inst) (x86asm.Mem, bool) {
	for _, a := range inst.Args {
		if m, ok := a.(x86asm.Mem); ok {
			return m, true
		}
	}
	return x86asm.Mem{}, false
}
