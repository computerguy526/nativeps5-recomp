// SPDX-License-Identifier: GPL-2.0-or-later

package recompruntime

import (
	"fmt"
	"os"
	"sort"
	"strings"
	"testing"

	"ps4-recomp/pkg/thunkgen"
)

// TestKytyRouteTable: every NID of the kyty route table (pkg/thunkgen/kyty_nids.tsv) has exactly one AnyPS5 export, only
// those thunks changed shape, and the generated thunk table agrees with the route table. The counts are derived from the
// table, never typed in here, so adding a route cannot leave this test stale. No build tag: it needs no C compiler, so
// the GitHub runner (linux) really executes it.
func TestKytyRouteTable(t *testing.T) {
	routes, err := thunkgen.KytyRoutes()
	if err != nil {
		t.Fatal(err)
	}
	if len(routes) == 0 {
		t.Fatal("empty route table")
	}
	libs := map[string]int{}
	for _, r := range routes {
		libs[r.Lib]++
	}
	descs, err := thunkgen.AnyPS5Descs()
	if err != nil {
		t.Fatal(err)
	}
	routed, direct := 0, 0
	routedLibs := map[string]int{}
	for _, d := range descs {
		if d.RouteNID == "" {
			if strings.HasPrefix(d.HLE, "aps5_rt_") {
				t.Errorf("%s calls through a route pointer but is not routed", d.Shim)
			}
			continue
		}
		routed++
		routedLibs[d.RouteLib]++
		if d.RouteTarget != "" {
			direct++
			if d.HLE != "aps5_rt_"+d.RouteTarget {
				t.Errorf("%s: pointer %s for %s", d.Shim, d.HLE, d.RouteTarget)
			}
		}
	}
	if routed != len(routes) {
		t.Fatalf("%d routes in the table, %d routed exports (%v vs %v)", len(routes), routed, libs, routedLibs)
	}
	for _, problem := range oneExportPerNID(routes, descs) {
		t.Error(problem)
	}
	for lib, n := range libs {
		if routedLibs[lib] != n {
			t.Errorf("%s: %d routes, %d routed exports", lib, n, routedLibs[lib])
		}
	}
	src, err := os.ReadFile("modules/libSceAnyPS5/nps5_anyps5_thunks.c")
	if err != nil {
		t.Fatal(err)
	}
	if n := strings.Count(string(src), "(void *)kyty_raw_"); n != len(routes) {
		t.Fatalf("generated table has %d routes, the route table %d: run go run ./pkg/thunkgen/gen", n, len(routes))
	}
	if direct == 0 || direct > routed {
		t.Fatalf("direct routes %d of %d", direct, routed)
	}
}

// oneExportPerNID: every NID of the route table is routed by exactly one export, and no export routes a NID the table does
// not have. Equal per-library totals do not show that (one NID routed twice and another not at all keep the totals).
func oneExportPerNID(routes map[string]thunkgen.KytyRoute, descs []thunkgen.Desc) []string {
	var problems []string
	count := map[string]int{}
	for _, d := range descs {
		if d.RouteNID != "" {
			count[d.RouteNID]++
		}
	}
	for nid, r := range routes {
		if n := count[nid]; n != 1 {
			problems = append(problems, fmt.Sprintf("NID %s (%s %s) is routed by %d exports, want 1", nid, r.Lib, r.Function, n))
		}
	}
	for nid, n := range count {
		if _, ok := routes[nid]; !ok {
			problems = append(problems, fmt.Sprintf("NID %s is routed by %d exports but is not in the route table", nid, n))
		}
	}
	sort.Strings(problems)
	return problems
}

// The check itself: the totals of a double route plus a missing one are equal, the per-NID check still sees both.
func TestOneExportPerNIDCatches(t *testing.T) {
	routes := map[string]thunkgen.KytyRoute{"a": {NID: "a", Lib: "L"}, "b": {NID: "b", Lib: "L"}}
	good := []thunkgen.Desc{{RouteNID: "a"}, {RouteNID: "b"}}
	if p := oneExportPerNID(routes, good); len(p) != 0 {
		t.Fatalf("a correct table reported %v", p)
	}
	bad := []thunkgen.Desc{{RouteNID: "a"}, {RouteNID: "a"}}
	if p := oneExportPerNID(routes, bad); len(p) != 2 {
		t.Fatalf("a double route plus a missing one: %v", p)
	}
	extra := []thunkgen.Desc{{RouteNID: "a"}, {RouteNID: "b"}, {RouteNID: "zz"}}
	if p := oneExportPerNID(routes, extra); len(p) != 1 {
		t.Fatalf("a route outside the table: %v", p)
	}
}
