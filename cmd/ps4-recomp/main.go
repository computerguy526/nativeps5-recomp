// SPDX-License-Identifier: GPL-2.0-or-later

package main

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"runtime/pprof"

	"ps4-recomp/pkg/cli"
)

// NPS5_PPROF=<dir> writes cpu.pprof, heap.pprof and allocs.pprof of the run (translator profiling,
// docs/m3/iteration-speed.md).
func startProfile() func() {
	dir := os.Getenv("NPS5_PPROF")
	if dir == "" {
		return func() {}
	}
	_ = os.MkdirAll(dir, 0o755)
	cpu, err := os.Create(filepath.Join(dir, "cpu.pprof"))
	if err != nil {
		return func() {}
	}
	_ = pprof.StartCPUProfile(cpu)
	return func() {
		pprof.StopCPUProfile()
		cpu.Close()
		runtime.GC()
		for _, n := range []string{"heap", "allocs"} {
			if f, err := os.Create(filepath.Join(dir, n+".pprof")); err == nil {
				_ = pprof.Lookup(n).WriteTo(f, 0)
				f.Close()
			}
		}
	}
}

func main() {
	stop := startProfile()
	err := cli.Execute(os.Args[1:])
	stop()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error: %v\n", err)
		os.Exit(cli.ExitCode(err))
	}
}
