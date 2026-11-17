package main

import (
	"encoding/json"
	"fmt"
	"os"
	"sort"
	"strings"
)

type FeatureDelta struct {
	ID         string `json:"id"`
	Change     string `json:"change"` // added | removed | changed
	Severity   string `json:"severity,omitempty"`
	SupportedA string `json:"supportedA,omitempty"`
	SupportedB string `json:"supportedB,omitempty"`
	ShadersA   int    `json:"shadersA"`
	ShadersB   int    `json:"shadersB"`
	PipelinesA int    `json:"pipelinesA"`
	PipelinesB int    `json:"pipelinesB"`
	RankA      int    `json:"rankA,omitempty"`
	RankB      int    `json:"rankB,omitempty"`
}

type Comparison struct {
	Schema        string         `json:"schema"`
	Version       int            `json:"schemaVersion"`
	A, B          string         `json:"-"`
	NameA         string         `json:"nameA"`
	NameB         string         `json:"nameB"`
	DeviceA       string         `json:"deviceA,omitempty"`
	DeviceB       string         `json:"deviceB,omitempty"`
	ShadersA      int            `json:"shadersA"`
	ShadersB      int            `json:"shadersB"`
	ShadersCommon int            `json:"shadersCommon"`
	BlockersOnlyA []string       `json:"blockersOnlyA"`
	BlockersOnlyB []string       `json:"blockersOnlyB"`
	BlockersBoth  []string       `json:"blockersBoth"`
	Resolved      []string       `json:"resolvedInB"`  // unsupported+used in A, supported or unused in B
	Regressed     []string       `json:"regressedInB"` // the reverse
	Deltas        []FeatureDelta `json:"featureDeltas"`
}

func LoadReport(path string) (*Report, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var r Report
	if err := json.Unmarshal(b, &r); err != nil {
		return nil, err
	}
	if r.Schema != SchemaName {
		return nil, fmt.Errorf("%s: not a %s document", path, SchemaName)
	}
	if r.Version > SchemaVersion {
		return nil, fmt.Errorf("%s: schemaVersion %d is newer than this tool (%d)", path, r.Version, SchemaVersion)
	}
	return &r, nil
}

func devName(r *Report) string {
	if r.Device == nil {
		return ""
	}
	return r.Device.Name + " / " + r.Device.Driver
}

func Compare(a, b *Report) *Comparison {
	c := &Comparison{Schema: "gpuscan-compare", Version: 1, NameA: a.Name, NameB: b.Name, DeviceA: devName(a), DeviceB: devName(b), ShadersA: len(a.Shaders), ShadersB: len(b.Shaders)}
	ids := map[string]bool{}
	for _, s := range a.Shaders {
		ids[s.ID] = true
	}
	for _, s := range b.Shaders {
		if ids[s.ID] {
			c.ShadersCommon++
		}
	}
	am, bm := map[string]FeatureReport{}, map[string]FeatureReport{}
	for _, f := range a.Features {
		am[f.ID] = f
	}
	for _, f := range b.Features {
		bm[f.ID] = f
	}
	set := func(l []string) map[string]bool {
		m := map[string]bool{}
		for _, x := range l {
			m[x] = true
		}
		return m
	}
	ba, bb := set(a.Blockers), set(b.Blockers)
	c.BlockersOnlyA, c.BlockersOnlyB, c.BlockersBoth = []string{}, []string{}, []string{}
	for id := range ba {
		if bb[id] {
			c.BlockersBoth = append(c.BlockersBoth, id)
		} else {
			c.BlockersOnlyA = append(c.BlockersOnlyA, id)
		}
	}
	for id := range bb {
		if !ba[id] {
			c.BlockersOnlyB = append(c.BlockersOnlyB, id)
		}
	}
	bad := func(f FeatureReport, ok bool) bool {
		return ok && f.Rank > 0
	}
	all := map[string]bool{}
	for id := range am {
		all[id] = true
	}
	for id := range bm {
		all[id] = true
	}
	c.Resolved, c.Regressed = []string{}, []string{}
	for id := range all {
		fa, oka := am[id]
		fb, okb := bm[id]
		if bad(fa, oka) && !bad(fb, okb) {
			c.Resolved = append(c.Resolved, id)
		}
		if bad(fb, okb) && !bad(fa, oka) {
			c.Regressed = append(c.Regressed, id)
		}
		d := FeatureDelta{ID: id, ShadersA: fa.Needed.Shaders, ShadersB: fb.Needed.Shaders, PipelinesA: fa.Needed.Pipelines, PipelinesB: fb.Needed.Pipelines, RankA: fa.Rank, RankB: fb.Rank}
		switch {
		case !oka:
			d.Change, d.Severity, d.SupportedB = "added", fb.Severity, fb.Supported
		case !okb:
			d.Change, d.Severity, d.SupportedA = "removed", fa.Severity, fa.Supported
		default:
			d.Severity, d.SupportedA, d.SupportedB = fa.Severity, fa.Supported, fb.Supported
			if fa.Supported == fb.Supported && fa.Needed.Shaders == fb.Needed.Shaders && fa.Needed.Pipelines == fb.Needed.Pipelines {
				continue
			}
			d.Change = "changed"
		}
		c.Deltas = append(c.Deltas, d)
	}
	for _, l := range [][]string{c.BlockersOnlyA, c.BlockersOnlyB, c.BlockersBoth, c.Resolved, c.Regressed} {
		sort.Strings(l)
	}
	sort.Slice(c.Deltas, func(i, j int) bool { return c.Deltas[i].ID < c.Deltas[j].ID })
	return c
}

func (c *Comparison) Markdown() string {
	var b strings.Builder
	w := func(f string, a ...any) { fmt.Fprintf(&b, f, a...) }
	w("# GPU scan comparison\n\nA: **%s** %s\n\nB: **%s** %s\n\n", c.NameA, c.DeviceA, c.NameB, c.DeviceB)
	w("Shaders: A=%d, B=%d, in both=%d.\n\n", c.ShadersA, c.ShadersB, c.ShadersCommon)
	list := func(t string, l []string) {
		w("**%s**: ", t)
		if len(l) == 0 {
			w("none\n\n")
			return
		}
		w("`%s`\n\n", strings.Join(l, "`, `"))
	}
	list("Blockers only in A", c.BlockersOnlyA)
	list("Blockers only in B", c.BlockersOnlyB)
	list("Blockers in both", c.BlockersBoth)
	list("Resolved in B (unsupported+used in A, not in B)", c.Resolved)
	list("Regressed in B (new unsupported+used in B)", c.Regressed)
	w("## Feature deltas\n\n| feature | change | severity | support A->B | shaders A->B | pipelines A->B | rank A->B |\n|---|---|---|---|---|---|---|\n")
	for _, d := range c.Deltas {
		w("| `%s` | %s | %s | %s -> %s | %d -> %d | %d -> %d | %d -> %d |\n", d.ID, d.Change, d.Severity, d.SupportedA, d.SupportedB, d.ShadersA, d.ShadersB, d.PipelinesA, d.PipelinesB, d.RankA, d.RankB)
	}
	return b.String()
}
