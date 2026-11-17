package emitter

import (
	"bufio"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"

	"ps4-recomp/pkg/elfloader"
)

type onlineImport struct{ lib, nid, name string }

// loadOnlineFixture reads testdata/online_stub_imports.txt: Sony API identifiers (library, NID, symbol
// name) of the imports the online/system stub block serves. No game content.
func loadOnlineFixture(t *testing.T) []onlineImport {
	t.Helper()
	f, err := os.Open("testdata/online_stub_imports.txt")
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	var rows []onlineImport
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 1<<20), 1<<20)
	for sc.Scan() {
		l := sc.Text()
		if l == "" || strings.HasPrefix(l, "#") {
			continue
		}
		p := strings.Split(l, "\t")
		if len(p) != 3 {
			t.Fatalf("bad fixture line %q", l)
		}
		if p[2] == "-" {
			p[2] = ""
		}
		rows = append(rows, onlineImport{p[0], p[1], p[2]})
	}
	return rows
}

func expectedOnlineShim(r onlineImport) string {
	if r.name != "" {
		return "shim_" + r.name
	}
	return "shim_npcppwebapi_nid_" + strings.NewReplacer("+", "_P_", "-", "_M_").Replace(r.nid)
}

// Every import of the block resolves, by NID (how a retail PS5 dynsym names it, NID#lib#mod) and by name.
func TestOnlineStubsBindByNameAndNID(t *testing.T) {
	rows := loadOnlineFixture(t)
	if len(rows) != 1123 {
		t.Fatalf("fixture has %d imports, want 1123", len(rows))
	}
	seen := map[string]bool{}
	perLib := map[string]int{}
	for _, r := range rows {
		perLib[r.lib]++
		if seen[r.nid] {
			t.Errorf("duplicate NID %s", r.nid)
		}
		seen[r.nid] = true
		want := expectedOnlineShim(r)
		if got, ok := LookupShim(r.nid + "#A#B"); !ok || got != want {
			t.Errorf("%s (%s): LookupShim by NID = %q,%v want %q", r.nid, r.name, got, ok, want)
		}
		if r.name == "" {
			continue
		}
		if got, ok := LookupShim(r.name); !ok || got != want {
			t.Errorf("%s: LookupShim by name = %q,%v want %q", r.name, got, ok, want)
		}
		if got := elfloader.CalculateNID(r.name); got != r.nid {
			t.Errorf("%s: NID of the name is %q, fixture says %q", r.name, got, r.nid)
		}
		if got, ok := shimByNID(r.nid); !ok || got != want {
			t.Errorf("%s: NID-derived binding = %q,%v want %q", r.name, got, ok, want)
		}
	}
	for lib, n := range map[string]int{
		"libSceNpCppWebApi": 998, "libSceNpUniversalDataSystem": 29, "libSceNpWebApi2": 23, "libSceHttp2": 18,
		"libSceNpTrophy2": 9, "libSceNpEntitlementAccess": 6, "libSceAcm": 6, "libScePlayGoDialog": 5, "libSceGameUpdate": 5,
		"libScePlayGo": 4, "libScePlayerInvitationDialog": 4, "libSceShare": 4, "libSceNpManager": 3,
		"libSceNpGameIntent": 2, "libSceNpAuth": 2, "libSceWebBrowserDialog": 2, "libSceSystemService": 2, "libSceUserService": 1,
	} {
		if perLib[lib] != n {
			t.Errorf("%s: %d imports in fixture, want %d", lib, perLib[lib], n)
		}
	}
}

// Every bound stub has a C definition and every C definition is bound (a name missing from the C
// sources would be an undefined symbol at link time).
func TestOnlineStubsMatchCSources(t *testing.T) {
	defs := map[string]bool{}
	patterns := []*regexp.Regexp{
		regexp.MustCompile(`(?m)^(?:ON_FIXED|HTTP2_TOKEN|TOKEN_RET)\((\w+)[,)]`),
		regexp.MustCompile(`(?m)^void shim_(\w+)\(GuestContext`),
		regexp.MustCompile(`(?m)^CPP_[A-Z_]+\((\w+),`),
	}
	for _, dir := range []string{"libSceNpOnlineStubs", "libSceNpCppWebApi"} {
		files, _ := filepath.Glob(filepath.Join("..", "runtime", "modules", dir, "*.c"))
		if len(files) == 0 {
			t.Fatalf("no C sources in %s", dir)
		}
		for _, f := range files {
			b, err := os.ReadFile(f)
			if err != nil {
				t.Fatal(err)
			}
			for _, re := range patterns {
				for _, m := range re.FindAllStringSubmatch(string(b), -1) {
					defs["shim_"+m[1]] = true
				}
			}
		}
	}
	want := map[string]bool{}
	for _, r := range loadOnlineFixture(t) {
		want[expectedOnlineShim(r)] = true
	}
	for s := range want {
		if !defs[s] {
			t.Errorf("%s is bound but not defined in the C sources", s)
		}
	}
	for s := range defs {
		if !want[s] {
			t.Errorf("%s is defined in the C sources but not in the fixture/binding", s)
		}
	}
}
