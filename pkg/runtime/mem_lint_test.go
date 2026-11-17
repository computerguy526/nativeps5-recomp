// SPDX-License-Identifier: GPL-2.0-or-later

package recompruntime

import (
	"io/fs"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// TestMemBaseNotBoolean guards the identity addressing mode (docs/m1/memory.md): there the guest
// address is the host address, so ctx->mem_base is NULL by design. Code must ask "is guest memory
// set up" with RECOMP_MEM_OK(ctx) (mem_size != 0), never with mem_base in a boolean context
// (review 1, M4: GNM video-out readback and surface upload silently switched off).
func TestMemBaseNotBoolean(t *testing.T) {
	// !mem_base, !x->mem_base_, if (mem_base), a && mem_base && b, mem_base ? :, mem_base == NULL / != 0
	bad := regexp.MustCompile(`!\s*(\w+(->|\.))?mem_base_?\b|(\bif\s*\(|&&|\|\||\?)\s*(\w+(->|\.))?mem_base_?\s*(&&|\|\||\)|\?|:)|\b(\w+(->|\.))?mem_base_?\s*(==|!=)\s*(NULL|nullptr|0)\b`)
	err := filepath.WalkDir(".", func(path string, d fs.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return err
		}
		switch filepath.Ext(path) {
		case ".c", ".cpp", ".m", ".mm", ".h":
		default:
			return nil
		}
		if filepath.Base(path) == "nps5_thunk.h" { // defines the identity/base translation itself
			return nil
		}
		data, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		for i, line := range strings.Split(string(data), "\n") {
			code := line
			if j := strings.Index(code, "//"); j >= 0 {
				code = code[:j]
			}
			if bad.MatchString(code) {
				t.Errorf("%s:%d: mem_base used as a boolean (NULL in identity mode); use RECOMP_MEM_OK / mem_size != 0:\n\t%s", path, i+1, strings.TrimSpace(line))
			}
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
}
