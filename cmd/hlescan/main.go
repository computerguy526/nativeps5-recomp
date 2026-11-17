// hlescan lists, per library, how each import of the given ELF/SELF/PRX files is satisfied at AOT time
// (host shim / defined / unresolved), with the symbol names of the unresolved ones. Nothing is written.
//
//	hlescan <file> [<file>...]
package main

import (
	"fmt"
	"os"
	"sort"

	"ps4-recomp/pkg/elfloader"
	"ps4-recomp/pkg/emitter"
)

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: hlescan <eboot.bin|prx>...")
		os.Exit(2)
	}
	for _, p := range os.Args[1:] {
		l, err := elfloader.LoadELF(p)
		if err != nil {
			fmt.Printf("%s: load error: %v\n", p, err)
			continue
		}
		r := emitter.NewHLEReport(l, nil)
		fmt.Printf("== %s: %d imports (%d host shims, %d defined, %d unresolved)\n", p, len(r.Imports), r.HostShimCount, r.DefinedCount, r.UnresolvedCount)
		by := map[string][]string{}
		for _, im := range r.Imports {
			if im.Class != emitter.ImportUnresolved {
				continue
			}
			n := im.Name
			if res, ok := elfloader.ResolveNID(n); ok {
				n = res + " (" + im.Name + ")"
			}
			by[im.Library] = append(by[im.Library], n)
		}
		var libs []string
		for k := range by {
			libs = append(libs, k)
		}
		sort.Strings(libs)
		for _, k := range libs {
			sort.Strings(by[k])
			fmt.Printf("%s %d\n", k, len(by[k]))
			for _, n := range by[k] {
				fmt.Printf("    %s\n", n)
			}
		}
	}
}
