package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"math"
	"os"
	"sort"

	"golang.org/x/arch/x86/x86asm"
	"ps4-recomp/pkg/elfloader"
	"ps4-recomp/pkg/uecvar"
)

// xrefCmd: uecvar xref [-o out.json] [-stats] <eboot>
func xrefCmd(args []string) {
	fs := flag.NewFlagSet("xref", flag.ExitOnError)
	out := fs.String("o", "", "write the JSON here (default stdout)")
	stats := fs.Bool("stats", false, "print per-slot statistics instead")
	fs.Parse(args)
	if fs.NArg() != 1 {
		die("xref [-o out.json] [-stats] <eboot>")
	}
	l, err := elfloader.LoadELF(fs.Arg(0))
	if err != nil {
		die("%v", err)
	}
	var code uecvar.Range
	var ro []uecvar.Range
	for _, s := range l.Segments {
		r := uecvar.Range{Start: s.Vaddr, End: s.Vaddr + uint64(len(s.Data))}
		if s.Flags&1 != 0 {
			code = r
		} else {
			ro = append(ro, r)
		}
	}
	names := uecvar.UTF16Names(l.MemoryImage, ro)
	cv := uecvar.Xref(l.MemoryImage, code, names)
	uecvar.FillRefDefaults(l.MemoryImage, cv)
	if *stats {
		by := map[int][]uecvar.Cvar{}
		for _, c := range cv {
			by[c.Slot] = append(by[c.Slot], c)
		}
		var slots []int
		for s := range by {
			slots = append(slots, s)
		}
		sort.Ints(slots)
		fmt.Printf("names %d, registrations %d\n", len(names), len(cv))
		for _, s := range slots {
			n, plausF, small, zero := 0, 0, 0, 0
			for _, c := range by[s] {
				n++
				if c.Kind != uecvar.KindRef {
					continue
				}
				v := uint32(c.DefaultV)
				f := math.Float32frombits(v)
				if v == 0 {
					zero++
				} else if v < 1<<16 {
					small++
				} else if f > 1e-6 && f < 1e7 {
					plausF++
				}
			}
			fmt.Printf("slot %#x: %d  ref zero=%d smallint=%d plausibleFloat=%d\n", s, n, zero, small, plausF)
		}
		return
	}
	b, _ := json.MarshalIndent(cv, "", " ")
	if *out == "" {
		os.Stdout.Write(append(b, '\n'))
		return
	}
	if err := os.WriteFile(*out, append(b, '\n'), 0o644); err != nil {
		die("%v", err)
	}
	fmt.Fprintf(os.Stderr, "uecvar xref: %d names, %d registrations -> %s\n", len(names), len(cv), *out)
}

// disCmd: uecvar dis <eboot> <addr> <bytes>: Intel-syntax listing (debug aid for the xref finder).
func disCmd(args []string) {
	if len(args) != 3 {
		die("dis <eboot> <addr> <bytes>")
	}
	l, err := elfloader.LoadELF(args[0])
	if err != nil {
		die("%v", err)
	}
	var a, n uint64
	fmt.Sscanf(args[1], "%v", &a)
	fmt.Sscanf(args[2], "%d", &n)
	buf := l.MemoryImage.Bytes(a, n+16)
	for p := uint64(0); p < n; {
		in, e := x86asm.Decode(buf[p:], 64)
		if e != nil {
			p++
			continue
		}
		fmt.Printf("%#x  %s\n", a+p, x86asm.IntelSyntax(in, a+p, nil))
		p += uint64(in.Len)
	}
}

// refsCmd: uecvar refs <eboot> <va>...  lists the instructions that address the VA RIP-relatively
// (reads and writes of a cvar's storage), with the containing instruction listing.
func refsCmd(args []string) {
	if len(args) < 2 {
		die("refs <eboot> <va>...")
	}
	l, err := elfloader.LoadELF(args[0])
	if err != nil {
		die("%v", err)
	}
	var code uecvar.Range
	for _, s := range l.Segments {
		if s.Flags&1 != 0 {
			code = uecvar.Range{Start: s.Vaddr, End: s.Vaddr + uint64(len(s.Data))}
		}
	}
	b := l.MemoryImage.Bytes(code.Start, code.End-code.Start)
	for _, a := range args[1:] {
		var va uint64
		fmt.Sscanf(a, "%v", &va)
		fmt.Printf("== %#x\n", va)
		for _, h := range uecvar.FindRIPRefs(b, code.Start, va) {
			buf := l.MemoryImage.Bytes(h.Addr, 16)
			in, _ := x86asm.Decode(buf, 64)
			fmt.Printf("%#x  %s\n", h.Addr, x86asm.IntelSyntax(in, h.Addr, nil))
		}
	}
}
