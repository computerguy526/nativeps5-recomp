// SPDX-License-Identifier: GPL-2.0-or-later

package recompruntime

import (
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// TestPreciseSleep: the guest sleep helper (core/nps5_sleep.h) never wakes early, is not subject to timer
// coalescing and reports an interrupted sleep (docs/m3/perf.md).
func TestPreciseSleep(t *testing.T) {
	if runtime.GOOS != "darwin" {
		t.Skip("kqueue critical timers are macOS")
	}
	clang, err := exec.LookPath("clang")
	if err != nil {
		t.Skip("clang is required")
	}
	bin := filepath.Join(t.TempDir(), "sleep_test")
	if out, err := exec.Command(clang, "-O1", "-Icore", "../../tests/sleep_test/sleep_test.c", "-o", bin).CombinedOutput(); err != nil {
		t.Fatalf("build: %v\n%s", err, out)
	}
	out, err := exec.Command(bin).CombinedOutput()
	if err != nil || !strings.Contains(string(out), "SLEEP OK") {
		t.Fatalf("sleep test: %v\n%s", err, out)
	}
	t.Logf("%s", out)
}
