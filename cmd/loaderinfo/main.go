// Command loaderinfo prints what the ELF/SELF loader and code-discovery
// seeding recover from a binary: segments, dynamic tags, relocations,
// imports, init arrays and the function seed count versus the FDE count.
package main

import (
	"fmt"
	"os"
	"sort"

	"ps4-recomp/pkg/disasm"
	"ps4-recomp/pkg/elfloader"
)

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: loaderinfo <eboot.bin|elf|prx> [-reach]")
		os.Exit(2)
	}
	l, err := elfloader.LoadELF(os.Args[1])
	if err != nil {
		fmt.Fprintln(os.Stderr, "load error:", err)
		os.Exit(1)
	}
	if len(os.Args) > 3 && os.Args[2] == "-rel" { // loaderinfo <file> -rel 0xOFFSET...: relocations at those offsets
		for _, a := range os.Args[3:] {
			var off uint64
			fmt.Sscanf(a, "0x%x", &off)
			for _, r := range l.Relocations {
				if r.Offset == off {
					fmt.Printf("0x%x type %d sym %q addend 0x%x\n", r.Offset, r.Type, r.SymName, r.Addend)
				}
			}
		}
		return
	}
	byType := map[uint32]int{}
	imports := map[string]int{}
	for _, r := range l.Relocations {
		byType[r.Type]++
		if r.SymName != "" && (r.Type == elfloader.R_X86_64_GLOB_DAT || r.Type == elfloader.R_X86_64_JUMP_SLOT || r.Type == elfloader.R_X86_64_64) {
			imports[r.SymName]++
		}
	}
	var types []int
	for t := range byType {
		types = append(types, int(t))
	}
	sort.Ints(types)
	fmt.Printf("file: %s\nentry: 0x%x  segments: %d  image: 0x%x\n", l.FileName, l.EntryPoint, len(l.Segments), l.MemoryImage.Size())
	fmt.Printf("dynsyms: %d  needed: %d  importlibs: %d\n", len(l.DynSymbols), len(l.NeededLibs), len(l.ImportLibs))
	for i, sg := range l.Segments {
		fmt.Printf("segment %d: vaddr 0x%x memsz 0x%x filesz 0x%x flags %v\n", i, sg.Vaddr, sg.Memsz, sg.Filesz, sg.Flags)
	}
	fmt.Printf("relocations: %d\n", len(l.Relocations))
	for _, t := range types {
		fmt.Printf("  type %-3d %d\n", t, byType[uint32(t)])
	}
	fmt.Printf("imports (distinct symbols): %d\n", len(imports))
	fmt.Printf("init/ctor entries: %d\nFDEs (FuncBounds): %d\n", len(l.InitArray), len(l.FuncBounds))
	seeds := disasm.SeedEntryPoints(l, false)
	fmt.Printf("code seeds: %d\n", len(seeds))
	if len(os.Args) > 2 && os.Args[2] == "-reach" {
		d, err := disasm.NewDisassembler(l)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		d.SkipFlagLiveness = true
		seen := map[uint64]bool{}
		q := []uint64{}
		for _, a := range seeds {
			if a != 0 && d.InCode(a) && !seen[a] {
				seen[a] = true
				q = append(q, a)
			}
		}
		n := 0
		for h := 0; h < len(q); h++ {
			fn, calls, err := d.DisasmFunction(q[h])
			if err != nil || fn == nil {
				continue
			}
			n++
			for _, t := range calls {
				if t != 0 && d.InCode(t) && !seen[t] {
					seen[t] = true
					q = append(q, t)
				}
			}
		}
		fmt.Printf("reachable functions: %d\n", n)
	}
}
