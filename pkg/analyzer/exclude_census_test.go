package analyzer

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"ps4-recomp/pkg/elfloader"
)

// DISCO_EXCLUDED=<module> DISCO_MODULES="a:b:c" go test ./pkg/analyzer -run TestExcludedImportCensus -v
// prints, per module, how many distinct imported NIDs the excluded module exports (counts only).
func TestExcludedImportCensus(t *testing.T) {
	ex := os.Getenv("DISCO_EXCLUDED")
	if ex == "" {
		t.Skip("DISCO_EXCLUDED not set")
	}
	mod, err := elfloader.LoadELF(ex)
	if err != nil {
		t.Fatal(err)
	}
	exp := map[string]bool{}
	for _, s := range mod.ExportedFunctions() {
		exp[elfloader.NIDPrefix(s.Name)] = true
	}
	t.Logf("%s exports %d NIDs", filepath.Base(ex), len(exp))
	for _, p := range strings.Split(os.Getenv("DISCO_MODULES"), ":") {
		l, err := elfloader.LoadELF(p)
		if err != nil {
			t.Errorf("%s: %v", p, err)
			continue
		}
		distinct, relocs := map[string]bool{}, 0
		for _, r := range l.Relocations {
			if r.SymName == "" || !exp[elfloader.NIDPrefix(r.SymName)] {
				continue
			}
			distinct[elfloader.NIDPrefix(r.SymName)] = true
			relocs++
		}
		t.Logf("%-40s imports %d distinct NIDs from the excluded module (%d relocs)", filepath.Base(p), len(distinct), relocs)
	}
}
