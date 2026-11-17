// SPDX-License-Identifier: GPL-2.0-or-later

package recompruntime

import (
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// TestFontGlyphAPI: libSceFont glyph API (modules/libSceFont/ps4_font.c): render-scale glyph metrics (the game's
// text layout drops every character whose metrics call fails), measured vs drawn advance, glyph objects
// (generate / attribute / delete / pool exhaustion / random alloc-free against a model), vertical layout.
func TestFontGlyphAPI(t *testing.T) {
	if runtime.GOOS != "darwin" {
		t.Skip("host test is macOS")
	}
	clang, err := exec.LookPath("clang")
	if err != nil {
		t.Skip("clang is required")
	}
	ft := "/opt/homebrew/opt/freetype"
	args := []string{"-O1", "-Wall", "-Wno-unused-function", "-Wno-unused-parameter"}
	for _, d := range []string{"core", "core/softfloat", "kernel", "kernel/vfs", "kernel/memory", "modules", "modules/libSceFont"} {
		args = append(args, "-I"+d)
	}
	bin := filepath.Join(t.TempDir(), "font_glyph_unit")
	args = append(args, "-I"+ft+"/include/freetype2", "../../tests/font/font_glyph_unit.c", "-L"+ft+"/lib", "-lfreetype", "-o", bin)
	if out, err := exec.Command(clang, args...).CombinedOutput(); err != nil {
		t.Skipf("build (needs Homebrew freetype): %v\n%s", err, out)
	}
	out, err := exec.Command(bin).CombinedOutput()
	if err != nil || !(strings.Contains(string(out), "FONT OK") || strings.Contains(string(out), "FONT SKIP")) {
		t.Fatalf("font glyph test: %v\n%s", err, out)
	}
	t.Logf("%s", out)
}
