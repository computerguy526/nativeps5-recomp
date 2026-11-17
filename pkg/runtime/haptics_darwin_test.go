// SPDX-License-Identifier: GPL-2.0-or-later

package recompruntime

import (
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// TestHapticsLogic: the controller-haptics logic (modules/libSceAudioOut/nps5_haptics_logic.h): envelope downmix,
// channel routing, mode per controller type, rate limiting, hotplug, NPS5_HAPTICS=0 and the audio-thread
// non-blocking proof, against mock sinks (no hardware).
func TestHapticsLogic(t *testing.T) {
	if runtime.GOOS != "darwin" {
		t.Skip("host test is macOS")
	}
	clang, err := exec.LookPath("clang")
	if err != nil {
		t.Skip("clang is required")
	}
	bin := filepath.Join(t.TempDir(), "haptics_unit")
	if out, err := exec.Command(clang, "-O1", "-Wall", "../../tests/haptics/haptics_unit.c", "-lm", "-lpthread", "-o", bin).CombinedOutput(); err != nil {
		t.Fatalf("build: %v\n%s", err, out)
	}
	out, err := exec.Command(bin).CombinedOutput()
	if err != nil || !strings.Contains(string(out), "HAPTICS OK") {
		t.Fatalf("haptics test: %v\n%s", err, out)
	}
	t.Logf("%s", out)
}
