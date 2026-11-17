// SPDX-License-Identifier: GPL-2.0-or-later

package recompruntime

import (
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// TestShadowStack builds and runs tests/shadow_test/shadow_test.c against core/nps5_shadow.c: the
// flat call model's shadow return stack (mapping, guard-page overflow compaction through the
// real fault path, trimming, the RET slow path, continuation table, invalidation).
func TestShadowStack(t *testing.T) {
	if runtime.GOARCH != "arm64" || runtime.GOOS != "darwin" {
		t.Skip("the shadow stack fault path is written for arm64 macOS")
	}
	clang, err := exec.LookPath("clang")
	if err != nil {
		t.Skip("clang is required")
	}
	var inc []string
	for _, d := range []string{"core", "kernel/memory", "kernel/syscalls", "kernel/vfs", "kernel/threading", "modules/libkernel"} {
		inc = append(inc, "-I"+d)
	}
	bin := filepath.Join(t.TempDir(), "shadow_test")
	args := append([]string{"-g", "-O1", "-w"}, inc...)
	args = append(args, "../../tests/shadow_test/shadow_test.c", "core/nps5_shadow.c", "-o", bin)
	if out, err := exec.Command(clang, args...).CombinedOutput(); err != nil {
		t.Fatalf("build: %v\n%s", err, out)
	}
	out, err := exec.Command(bin).CombinedOutput()
	if err != nil || !strings.Contains(string(out), "SHADOW OK") {
		t.Fatalf("shadow test: %v\n%s", err, out)
	}
}
