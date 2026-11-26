package emitter

import (
	"encoding/json"
	"os"
	"regexp"
	"strings"
	"testing"
)

// Every [apediag] wrap in the ASTRO BOT physsub table must have its shim in nps5_apediag_shims.h (a table entry without a shim
// fails the link), and every shim there must be wrapped (else the diagnostic silently watches nothing).
func TestApediagTableAndShimsAgree(t *testing.T) {
	src, err := os.ReadFile("../runtime/modules/libSceAnyPS5/nps5_apediag_shims.h")
	if err != nil {
		t.Fatal(err)
	}
	defs := map[string]bool{}
	for _, m := range regexp.MustCompile(`(?m)^void (nps5_apediag_\w+)\(GuestContext \*ctx\)`).FindAllSubmatch(src, -1) {
		defs[string(m[1])] = true
	}
	raw, err := os.ReadFile("../../profiles/funcrepl/PPSA21564-physsub.json")
	if err != nil {
		t.Fatal(err)
	}
	var tab struct {
		Funcs []struct {
			Name, Addr, Shim, Sha256 string
			Wrap                     bool
		} `json:"funcs"`
	}
	if err := json.Unmarshal(raw, &tab); err != nil {
		t.Fatal(err)
	}
	wrapped := map[string]bool{}
	for _, f := range tab.Funcs {
		if !strings.HasPrefix(f.Shim, "nps5_apediag_") {
			continue
		}
		wrapped[f.Shim] = true
		if !defs[f.Shim] {
			t.Errorf("table wraps %s (%s) but nps5_apediag_shims.h defines no such shim", f.Shim, f.Name)
		}
		if !f.Wrap || len(f.Sha256) != 64 {
			t.Errorf("%s: must be a wrap with a sha256", f.Shim)
		}
	}
	for s := range defs {
		if !wrapped[s] {
			t.Errorf("shim %s is defined but not in the table", s)
		}
	}
	if len(defs) != 5 {
		t.Errorf("expected 5 apediag shims, found %d", len(defs))
	}
}
