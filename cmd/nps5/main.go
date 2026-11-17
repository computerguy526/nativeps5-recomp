// SPDX-License-Identifier: GPL-2.0-or-later

// nps5 packages translated titles as macOS apps and lists/launches them (docs/m3/packaging.md).
//
//	nps5 build <title dir> [-o <out dir>] [--profile <name>] [-j N] [--force]
//	nps5 list
//	nps5 play <title id|name> [VAR=value ...]
//	nps5 launcher [-o <out dir>]
//
// Every other command line is the ps4-recomp translator (the tier-1 miss fallback of a built app runs
// `nps5 fragment ...`).
package main

import (
	"flag"
	"fmt"
	"os"

	"ps4-recomp/pkg/cli"
	"ps4-recomp/pkg/pack"
)

func fail(err error) {
	fmt.Fprintf(os.Stderr, "nps5: %v\n", err)
	os.Exit(1)
}

// flags parses interspersed flags and positionals; -- and - prefixed long forms are both accepted.
func flags(name string, args []string, def func(*flag.FlagSet)) (*flag.FlagSet, []string) {
	fs := flag.NewFlagSet(name, flag.ExitOnError)
	def(fs)
	var pos []string
	for len(args) > 0 {
		_ = fs.Parse(args)
		args = fs.Args()
		if len(args) > 0 {
			pos = append(pos, args[0])
			args = args[1:]
		}
	}
	return fs, pos
}

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: nps5 build <title dir> [-o dir] [--profile name] | list | play <title> | launcher [-o dir]")
		os.Exit(2)
	}
	cmd, rest := os.Args[1], os.Args[2:]
	switch cmd {
	case "build":
		var o pack.Options
		_, pos := flags("build", rest, func(fs *flag.FlagSet) {
			fs.StringVar(&o.Out, "o", "", "output directory for <Title>.app (default: current directory)")
			fs.StringVar(&o.Profile, "profile", "", "profile name or path")
			fs.IntVar(&o.Jobs, "j", 0, "parallel jobs")
			fs.BoolVar(&o.Force, "force", false, "retranslate even when up to date")
		})
		if len(pos) != 1 {
			fail(fmt.Errorf("usage: nps5 build <title dir> [-o dir] [--profile name]"))
		}
		o.TitleDir, o.Log = pos[0], os.Stderr
		app, err := pack.Build(o)
		if err != nil {
			fail(err)
		}
		fmt.Println(app)
	case "list":
		if err := pack.PrintList(os.Stdout); err != nil {
			fail(err)
		}
	case "play":
		if len(rest) < 1 {
			fail(fmt.Errorf("usage: nps5 play <title id|name> [VAR=value ...]"))
		}
		if err := pack.Play(rest[0], rest[1:]); err != nil {
			fail(err)
		}
	case "launcher":
		out := "."
		_, _ = flags("launcher", rest, func(fs *flag.FlagSet) { fs.StringVar(&out, "o", ".", "output directory") })
		app, err := pack.BuildLauncher(out)
		if err != nil {
			fail(err)
		}
		fmt.Println(app)
	default:
		if err := cli.Execute(os.Args[1:]); err != nil {
			fmt.Fprintf(os.Stderr, "Error: %v\n", err)
			os.Exit(cli.ExitCode(err))
		}
	}
}
