package emitter

import (
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"ps4-recomp/pkg/lifter"
)

// With cold-section outlining the chunk gets a cold area after its last function: it assembles
// (the pc-map size check of the strict mode still holds for the hot code), the lean and commented
// variants give the same code, every branch to a cold label finds exactly one body, and with the
// switch off the chunk is the one without any cold label.
func TestColdAreaAssembles(t *testing.T) {
	if runtime.GOOS != "darwin" {
		t.Skip("needs the Apple arm64 assembler")
	}
	if _, err := exec.LookPath("clang"); err != nil {
		t.Skip("no clang")
	}
	defer lifter.SetColdOutline(lifter.GetColdOutline())
	dir := t.TempDir()
	emit := func(cold, comments bool) (string, string) {
		lifter.SetColdOutline(cold)
		em := pcmapTestEmitter(t)
		em.Comments = comments
		name := map[bool]string{false: "off", true: "on"}[cold] + map[bool]string{false: "", true: "_com"}[comments]
		src := filepath.Join(dir, name+".s")
		if err := em.emitSingleAsmChunk(src, 0, []uint64{0x40}); err != nil {
			t.Fatal(err)
		}
		obj := strings.TrimSuffix(src, ".s") + ".o"
		assembleTest(t, src, obj)
		return src, obj
	}
	offSrc, _ := emit(false, false)
	onSrc, onObj := emit(true, false)
	_, onComObj := emit(true, true)

	off, _ := os.ReadFile(offSrc)
	on, _ := os.ReadFile(onSrc)
	if bytes.Contains(off, []byte("\nLc")) {
		t.Errorf("cold labels with outlining off:\n%s", off)
	}
	text := string(on)
	end := strings.Index(text, "\nLe40:")
	first := strings.Index(text, "\nLc")
	if end < 0 || first < 0 || first < end {
		t.Fatalf("no cold area after the function:\n%s", text)
	}
	for _, br := range strings.Split(text, "\n") {
		if l, ok := strings.CutPrefix(br, "\tb Lc"); ok {
			if n := strings.Count(text, "\nLc"+l+":\n"); n != 1 {
				t.Errorf("branch to Lc%s: %d bodies", l, n)
			}
		}
	}
	if a, b := machoSection(t, onObj, "__TEXT", "__recomp_guest"), machoSection(t, onComObj, "__TEXT", "__recomp_guest"); !bytes.Equal(a, b) {
		t.Error("lean and commented cold assembly produce different code")
	}
}
