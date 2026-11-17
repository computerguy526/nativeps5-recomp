package analyzer

import (
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"

	"ps4-recomp/pkg/disasm"
)

// DISCO_ELFS="a.elf:b.elf" go test ./pkg/analyzer -run TestDiscoveryStats -v prints the metrics.
func TestDiscoveryStats(t *testing.T) {
	list := os.Getenv("DISCO_ELFS")
	if list == "" {
		t.Skip("DISCO_ELFS not set")
	}
	for _, p := range strings.Split(list, ":") {
		opts := disasm.SeedOptions{}
		for _, o := range strings.Split(os.Getenv("DISCO_OPTS"), ",") {
			switch o {
			case "data":
				opts.WritableData = true
			case "lp":
				opts.LandingPads = true
			case "rs":
				opts.ReturnSites = true
			}
		}
		st, err := MeasureDiscoveryOpts(p, opts)
		if err != nil {
			t.Errorf("%s: %v", p, err)
			continue
		}
		t.Logf("%-28s %s", filepath.Base(p), st)
		t.Logf("    seed sources new=%v rejected=%v", st.SeedSources.New, st.SeedSources.Rejected)
		if len(st.OracleMissing) > 0 && os.Getenv("DISCO_ORACLE") != "" {
			t.Logf("    oracle tables not recovered: %#x", st.OracleMissing)
		}
		if os.Getenv("DISCO_KINDS") != "" {
			t.Logf("    unresolved kinds %v", st.OtherKinds)
		}
		if os.Getenv("DISCO_ORACLE") != "" {
			for _, x := range st.OracleInexact {
				t.Logf("    inexact table %s", x)
			}
		}
		if dump := os.Getenv("DISCO_DUMP"); dump != "" && strings.Contains(p, dump) {
			var addrs []uint64
			for a := range st.Unrecovered {
				addrs = append(addrs, a)
			}
			sort.Slice(addrs, func(i, j int) bool { return addrs[i] < addrs[j] })
			for _, a := range addrs {
				t.Logf("UNRESOLVED %#x\n  %s", a, strings.Join(st.Unrecovered[a], "\n  "))
			}
		}
	}
}
