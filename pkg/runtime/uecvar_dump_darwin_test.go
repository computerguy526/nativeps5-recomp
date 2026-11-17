// SPDX-License-Identifier: GPL-2.0-or-later

package recompruntime

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// TestUecvarDump builds tests/uecvar_dump/dump.c with core/nps5_uecvar.c and checks the values the
// NPS5_UECVAR_DUMP thread reads back from fake TAutoConsoleVariable / Ref objects in the arena.
func TestUecvarDump(t *testing.T) {
	if runtime.GOARCH != "arm64" {
		t.Skip("the arena layout is defined for arm64 macOS")
	}
	clang, err := exec.LookPath("clang")
	if err != nil {
		t.Skip("clang is required")
	}
	dir := t.TempDir()
	bin := filepath.Join(dir, "dump")
	if out, err := exec.Command(clang, "-O1", "-Wall", "-Wextra", "-Werror", "-Icore", "../../tests/uecvar_dump/dump.c",
		"core/nps5_mem.c", "core/nps5_uecvar.c", "-o", bin).CombinedOutput(); err != nil {
		t.Fatalf("build: %v\n%s", err, out)
	}
	list := filepath.Join(dir, "list.txt")
	os.WriteFile(list, []byte("a.int tacv int 500000\na.float tacv float 500100\na.bool tacv bool 500200\na.ref ref int 502000\na.bad ref int 3f000000\na.none tacv int 500300\n"), 0o644)
	dump := filepath.Join(dir, "out.txt")
	cmd := exec.Command(bin)
	cmd.Env = append(os.Environ(), "NPS5_UECVAR_DUMP="+dump, "NPS5_UECVAR_LIST="+list, "NPS5_UECVAR_AT=0.5")
	if out, err := cmd.CombinedOutput(); err != nil || !strings.Contains(string(out), "TEST DONE") {
		t.Fatalf("run: %v\n%s", err, out)
	}
	b, _ := os.ReadFile(dump)
	got := map[string]string{}
	for _, l := range strings.Split(string(b), "\n") {
		f := strings.Fields(l)
		if len(f) >= 4 && !strings.HasPrefix(l, "#") {
			got[f[0]] = f[3]
		}
	}
	for k, v := range map[string]string{"a.int": "7", "a.float": "0.5", "a.bool": "1", "a.ref": "-3", "a.bad": "unmapped", "a.none": "unregistered"} {
		if got[k] != v {
			t.Errorf("%s = %q, want %q\n%s", k, got[k], v, b)
		}
	}
}

// TestUecvarSetReadOnlyPage: NPS5_UECVAR_SET stores into a writable cvar and leaves one in a read-only page alone (the
// helper thread has no fault resolver; a bare memcpy into a RELRO page used to crash the process).
func TestUecvarSetReadOnlyPage(t *testing.T) {
	if runtime.GOARCH != "arm64" {
		t.Skip("the arena layout is defined for arm64 macOS")
	}
	clang, err := exec.LookPath("clang")
	if err != nil {
		t.Skip("clang is required")
	}
	dir := t.TempDir()
	bin := filepath.Join(dir, "dump")
	if out, err := exec.Command(clang, "-O1", "-Wall", "-Wextra", "-Werror", "-Icore", "../../tests/uecvar_dump/dump.c",
		"core/nps5_mem.c", "core/nps5_uecvar.c", "-o", bin).CombinedOutput(); err != nil {
		t.Fatalf("build: %v\n%s", err, out)
	}
	list := filepath.Join(dir, "list.txt")
	os.WriteFile(list, []byte("a.w ref int 502000\na.ro ref int 510000\n"), 0o644)
	cmd := exec.Command(bin, "set")
	cmd.Env = append(os.Environ(), "NPS5_UECVAR_LIST="+list, "NPS5_UECVAR_SET=a.w=42,a.ro=9")
	out, err := cmd.CombinedOutput()
	if err != nil || !strings.Contains(string(out), "TEST DONE") {
		t.Fatalf("run: %v\n%s", err, out)
	}
	if !strings.Contains(string(out), "SET w=42 ro=5") {
		t.Fatalf("want the writable cvar set and the read-only one untouched:\n%s", out)
	}
}
