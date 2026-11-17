//go:build darwin

package recompruntime

import (
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The shared log convention header (modules/libSceAnyPS5/nps5_log.h): levels per subsystem, NPS5_LOG forms, rate limiting.
func TestSharedLogHeader(t *testing.T) {
	bin := filepath.Join(t.TempDir(), "log_test")
	if out, err := exec.Command("clang", "-Wall", "-Wextra", "-Werror", "-Imodules/libSceAnyPS5", "../../tests/nps5_log/log_test.c", "-o", bin).CombinedOutput(); err != nil {
		t.Fatalf("clang: %v\n%s", err, out)
	}
	out, err := exec.Command(bin).CombinedOutput()
	if err != nil || !strings.Contains(string(out), "LOG OK") {
		t.Fatalf("log_test: %v\n%s", err, out)
	}
	if !strings.Contains(string(out), "[sub] info 1\n") || !strings.Contains(string(out), "[sub] W warn 2\n") {
		t.Fatalf("line format wrong:\n%s", out)
	}
}
