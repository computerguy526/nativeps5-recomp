// SPDX-License-Identifier: GPL-2.0-or-later

package recompruntime

import (
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// TestGuestArena builds and runs tests/memory_arena_test/arena.c against core/nps5_mem.c in both
// addressing modes: collision-safe reservation (fallback slot in base mode, a listed conflict in
// identity mode, nothing clobbered), commit/decommit and fault classification.
func TestGuestArena(t *testing.T) {
	if runtime.GOARCH != "arm64" {
		t.Skip("the arena layout is defined for arm64 macOS")
	}
	clang, err := exec.LookPath("clang")
	if err != nil {
		t.Skip("clang is required")
	}
	for _, mode := range []struct{ name, flag string }{{"base", "-DNPS5_ADDR_MODE_IDENTITY=0"}, {"identity", "-DNPS5_ADDR_MODE_IDENTITY=1"}} {
		t.Run(mode.name, func(t *testing.T) {
			bin := filepath.Join(t.TempDir(), "arena-"+mode.name)
			cmd := exec.Command(clang, "-O1", "-Wall", "-Wextra", "-Werror", mode.flag, "-Icore",
				"../../tests/memory_arena_test/arena.c", "core/nps5_mem.c", "-o", bin)
			if out, err := cmd.CombinedOutput(); err != nil {
				t.Fatalf("build: %v\n%s", err, out)
			}
			out, err := exec.Command(bin).CombinedOutput()
			if err != nil || !strings.Contains(string(out), "ARENA OK") {
				t.Fatalf("arena test (%s): %v\n%s", mode.name, err, out)
			}
			if mode.name == "identity" && !strings.Contains(string(out), "Host regions in the way") {
				t.Errorf("no conflict listing in:\n%s", out)
			}
		})
	}
}
