package emitter

import (
	"encoding/json"
	"os"
	"regexp"
	"strconv"
	"testing"
)

// The v1.3 flower fix shipped without its wraps once: NPS5_PHYS_DTCLAMP_OBJ=all was in the profile env but the committed physsub table had no
// nps5_physsub_dtclamp_* entry, so nothing was wrapped ("no table entry in this build"). This keeps the table and the runtime's named slots in step.
// A named slot without an entry must be listed in dtclampNoAddress (its function address is not known yet: such a slot has no effect).
var dtclampNoAddress = map[string]bool{"slingshot": true, "stretchplatform": true}

func TestPhyssubTableWrapsEveryNamedDtclampSlot(t *testing.T) {
	src, err := os.ReadFile("../runtime/modules/libSceAnyPS5/nps5_physsub.c")
	if err != nil {
		t.Fatal(err)
	}
	m := regexp.MustCompile(`(?s)static const char \*(?:const )?g_dc_names\[PS_DC_SLOTS\] = \{([^}]*)\}`).FindSubmatch(src)
	if m == nil {
		t.Fatal("g_dc_names initializer not found in nps5_physsub.c")
	}
	names := regexp.MustCompile(`"([^"]+)"`).FindAllSubmatch(m[1], -1)
	nm := regexp.MustCompile(`#define PS_DC_(?:NAMED|SLOTS) (\d+)`).FindSubmatch(src)
	if nm == nil {
		t.Fatal("PS_DC_SLOTS not found")
	}
	if n, _ := strconv.Atoi(string(nm[1])); n != len(names) {
		t.Fatalf("PS_DC_SLOTS=%d but %d names listed", n, len(names))
	}
	raw, err := os.ReadFile("../../profiles/funcrepl/PPSA21564-physsub.json")
	if err != nil {
		t.Fatal(err)
	}
	var tab struct {
		Funcs []struct{ Name, Addr, Shim string } `json:"funcs"`
	}
	if err := json.Unmarshal(raw, &tab); err != nil {
		t.Fatal(err)
	}
	byShim, addrs := map[string]string{}, map[string]bool{}
	for _, f := range tab.Funcs {
		if addrs[f.Addr] {
			t.Errorf("address %s wrapped twice", f.Addr)
		}
		addrs[f.Addr] = true
		byShim[f.Shim] = f.Name
	}
	for i, n := range names {
		shim := "nps5_physsub_dtclamp_" + strconv.Itoa(i)
		if _, ok := byShim[shim]; !ok && !dtclampNoAddress[string(n[1])] {
			t.Errorf("NPS5_PHYS_DTCLAMP_OBJ=all selects %q (%s) but profiles/funcrepl/PPSA21564-physsub.json has no wrap entry for it", n[1], shim)
		}
		if !regexp.MustCompile(`PS_DC_SHIM\(` + strconv.Itoa(i) + `\)`).Match(src) {
			t.Errorf("shim %s is not defined in nps5_physsub.c", shim)
		}
	}
	// the profile turns the clamp on, so the table must carry at least the verified plant (slot 0)
	if _, ok := byShim["nps5_physsub_dtclamp_0"]; !ok {
		t.Error("BigLeafPlant dt clamp (slot 0) missing from the table")
	}
}
