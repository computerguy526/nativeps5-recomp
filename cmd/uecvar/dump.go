package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"sort"
	"strings"

	"ps4-recomp/pkg/uecvar"
)

// dumplistCmd: uecvar dumplist [-base 0x400000] [-profile p.uecvars] <xref.json>
// Writes the NPS5_UECVAR_LIST file for the runtime dump: "<name> <kind> <type> <guest address hex>".
func dumplistCmd(args []string) {
	fs := flag.NewFlagSet("dumplist", flag.ExitOnError)
	base := fs.Uint64("base", 0x400000, "guest address of linked VA 0 (the eboot's load address)")
	prof := fs.String("profile", "", "only the cvars named in this profile (default: all)")
	fs.Parse(args)
	if fs.NArg() != 1 {
		die("dumplist [-base N] [-profile p] <xref.json>")
	}
	b, err := os.ReadFile(fs.Arg(0))
	if err != nil {
		die("%v", err)
	}
	var cv []uecvar.Cvar
	if err := json.Unmarshal(b, &cv); err != nil {
		die("%v", err)
	}
	want := map[string]bool{}
	if *prof != "" {
		for _, e := range loadProfile(*prof).Entries {
			want[e.Name] = true
		}
	}
	for _, c := range cv {
		if c.Storage == 0 || (*prof != "" && !want[c.Name]) || (c.Type != "int" && c.Type != "float" && c.Type != "bool") {
			continue
		}
		fmt.Printf("%s %s %s %x\n", c.Name, c.Kind, c.Type, c.Storage+*base)
	}
}

func readDump(path string) (last map[string]string, order []string) {
	b, err := os.ReadFile(path)
	if err != nil {
		die("%v", err)
	}
	last = map[string]string{}
	for _, l := range strings.Split(string(b), "\n") {
		f := strings.Fields(l)
		if len(f) >= 4 && f[0] != "#" {
			if _, ok := last[f[0]]; !ok {
				order = append(order, f[0])
			}
			last[f[0]] = f[3] // the latest dump of a file wins
		}
	}
	return
}

// dumpcmpCmd: uecvar dumpcmp [-profile p] [-xref x.json] <baseline-dump> <overlay-dump>
// Prints name, default (from the xref, when given), baseline value, overlay value, profile value.
func dumpcmpCmd(args []string) {
	fs := flag.NewFlagSet("dumpcmp", flag.ExitOnError)
	prof := fs.String("profile", "", "profile with the intended values")
	xr := fs.String("xref", "", "xref.json for the static defaults")
	fs.Parse(args)
	if fs.NArg() != 2 {
		die("dumpcmp [-profile p] [-xref x.json] <baseline> <overlay>")
	}
	a, order := readDump(fs.Arg(0))
	b, _ := readDump(fs.Arg(1))
	want := map[string]string{}
	if *prof != "" {
		for _, e := range loadProfile(*prof).Entries {
			want[e.Name] = e.Value
		}
	}
	def := map[string]string{}
	if *xr != "" {
		var cv []uecvar.Cvar
		if raw, err := os.ReadFile(*xr); err == nil && json.Unmarshal(raw, &cv) == nil {
			for _, c := range cv {
				def[c.Name] = c.Default
			}
		}
	}
	sort.Strings(order)
	fmt.Printf("%-52s %10s %10s %10s %10s\n", "cvar", "static", "baseline", "overlay", "profile")
	applied, differ, same := 0, 0, 0
	for _, n := range order {
		w := want[n]
		if *prof != "" && w == "" {
			continue
		}
		mark := ""
		if a[n] != b[n] {
			differ++
			mark = " CHANGED"
		} else {
			same++
		}
		if w != "" && sameNum(b[n], w) {
			applied++
		}
		fmt.Printf("%-52s %10s %10s %10s %10s%s\n", n, def[n], a[n], b[n], w, mark)
	}
	fmt.Printf("%d changed by the overlay, %d unchanged, %d equal to the profile value in the overlay run\n", differ, same, applied)
}

func sameNum(a, b string) bool {
	var x, y float64
	if _, err := fmt.Sscanf(a, "%g", &x); err != nil {
		return a == b
	}
	if _, err := fmt.Sscanf(b, "%g", &y); err != nil {
		return a == b
	}
	return x == y
}
