// Command uecvar is the Unreal "feature switcher": it lists the console-variable names in a UE
// binary, checks a per-game profile against them (unknown names are reported, never fatal), and
// writes the overlay (Engine.ini + UECommandLine.txt) the runtime serves over /app0.
//
//	uecvar scan  <eboot.bin>                         names found, one per line
//	uecvar check <profile.uecvars> <eboot.bin>       report profile names the binary does not know
//	uecvar emit  <profile.uecvars> <out-overlay-dir> [-app-dir <dump>] [-ini <rel files,...>]
package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"ps4-recomp/pkg/uecvar"
)

func die(f string, a ...any) { fmt.Fprintf(os.Stderr, "uecvar: "+f+"\n", a...); os.Exit(1) }

func loadProfile(path string) *uecvar.Profile {
	f, err := os.Open(path)
	if err != nil {
		die("%v", err)
	}
	defer f.Close()
	p, err := uecvar.Parse(f)
	if err != nil {
		die("%s: %v", path, err)
	}
	return p
}

func main() {
	a := os.Args[1:]
	if len(a) < 2 {
		fmt.Fprintln(os.Stderr, "usage: uecvar scan <bin> | check <profile> <bin> | emit <profile> <outdir> [-app-dir D] [-ini files]")
		os.Exit(2)
	}
	switch a[0] {
	case "scan":
		data, err := os.ReadFile(a[1])
		if err != nil {
			die("%v", err)
		}
		for _, n := range uecvar.Scan(data) {
			fmt.Println(n)
		}
	case "check":
		if len(a) < 3 {
			die("check <profile> <bin>")
		}
		p := loadProfile(a[1])
		data, err := os.ReadFile(a[2])
		if err != nil {
			die("%v", err)
		}
		names := uecvar.Scan(data)
		unk := p.Unknown(names)
		for _, e := range unk {
			fmt.Printf("UNKNOWN line %d: %s (not registered in this binary; the engine will ignore it)\n", e.Line, e.Name)
		}
		fmt.Printf("%d entries, %d unknown, %d names in binary\n", len(p.Entries), len(unk), len(names))
	case "dumplist":
		dumplistCmd(a[1:])
	case "dumpcmp":
		dumpcmpCmd(a[1:])
	case "refs":
		refsCmd(a[1:])
	case "dis":
		disCmd(a[1:])
	case "xref":
		xrefCmd(a[1:])
	case "emit":
		if len(a) < 3 {
			die("emit <profile> <outdir>")
		}
		p := loadProfile(a[1])
		out := a[2]
		inifiles := []string{"digginggame/config/ps5/ps5engine.ini"} // the project's PS5 platform Engine layer (probed by stat; not in the pak)
		orig := ""
		for i := 3; i+1 < len(a); i += 2 {
			switch a[i] {
			case "-app-dir":
				b, err := os.ReadFile(filepath.Join(a[i+1], "uecommandline.txt"))
				if err != nil {
					b, err = os.ReadFile(filepath.Join(a[i+1], "UECommandLine.txt"))
				}
				if err != nil {
					die("original command line: %v", err)
				}
				orig = string(b)
			case "-ini":
				inifiles = strings.Split(a[i+1], ",")
			}
		}
		write := func(rel, s string) {
			full := filepath.Join(out, rel)
			if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
				die("%v", err)
			}
			if err := os.WriteFile(full, []byte(s), 0o644); err != nil {
				die("%v", err)
			}
			fmt.Println("wrote", full)
		}
		if len(p.Entries) > 0 {
			for _, f := range inifiles {
				write(f, p.EngineINI())
			}
		}
		if len(p.Cmdline) > 0 {
			if orig == "" {
				die("@cmdline needs -app-dir <dump> (the original UECommandLine.txt is kept and extended)")
			}
			write("uecommandline.txt", p.CommandLine(orig))
		}
	default:
		die("unknown command %q", a[0])
	}
}
