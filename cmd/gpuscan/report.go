package main

import (
	"fmt"
	"sort"
	"strings"
	"time"
)

// SchemaName and SchemaVersion identify the JSON document. Adding fields keeps the version;
// renaming/removing a field or changing its meaning bumps it (see docs/m3/gpuscan.md).
const (
	SchemaName    = "gpuscan-report"
	SchemaVersion = 1
	ToolVersion   = "0.1.0"
)

type Needed struct {
	Shaders   int      `json:"shaders"`
	ShaderIDs []string `json:"shaderIds,omitempty"` // first 25, sorted
	Pipelines int      `json:"pipelines"`
	Hints     []string `json:"hints,omitempty"` // static-string predictions (not proof)
}

type FeatureReport struct {
	ID         string         `json:"id"`
	Category   string         `json:"category"`
	Title      string         `json:"title"`
	Severity   string         `json:"severity"`
	Supported  string         `json:"supported"` // yes | no | partial | unknown
	Evidence   string         `json:"evidence"`
	Workaround string         `json:"workaround,omitempty"`
	Needed     Needed         `json:"neededBy"`
	Rank       int            `json:"rank,omitempty"`                     // 1 = worst; only for unsupported features that are used
	Known      bool           `json:"known,omitempty"`                    // listed in --known (already being worked on)
	Predicted  bool           `json:"predicted,omitempty"`                // only a title hint, no shader/pipeline needs it yet
	CoNeeded   map[string]int `json:"alsoNeededByThoseShaders,omitempty"` // other non-trivial features used by the same shaders
}

type PipelineSummary struct {
	Source       string         `json:"source"`
	JournalFmt   uint32         `json:"journalFormat"`
	Graphics     int            `json:"graphics"`
	Compute      int            `json:"compute"`
	Modules      int            `json:"modules"`
	Signatures   map[string]int `json:"stageCombinations,omitempty"`
	Topologies   map[string]int `json:"topologies,omitempty"`
	Samples      map[string]int `json:"sampleCounts,omitempty"`
	ColorFormats map[string]int `json:"colorFormats,omitempty"`
	DepthFormats map[string]int `json:"depthFormats,omitempty"`
	Notes        []string       `json:"notes,omitempty"`
}

type Inputs struct {
	Title       string   `json:"title,omitempty"`
	CacheDirs   []string `json:"cacheDirs,omitempty"`
	Captures    []string `json:"captures,omitempty"`
	CacheFiles  int      `json:"cacheShaderModules"`
	PlanEntries int      `json:"cacheResourcePlanEntries"`
	Journals    int      `json:"journals"`
	Device      string   `json:"device,omitempty"`
}

type Peaks struct {
	SharedBytes     uint32            `json:"sharedMemoryBytes"`
	SharedShader    string            `json:"sharedMemoryShader,omitempty"`
	PushBytes       uint32            `json:"pushConstantBytes"`
	MaxSet          int               `json:"maxDescriptorSet"`
	Descriptors     map[string]uint32 `json:"descriptorsPerStage,omitempty"`
	MaxLocalInvoc   uint32            `json:"maxLocalSizeInvocations,omitempty"`
	UnboundedShader int               `json:"shadersWithUnboundedDescriptorArrays"`
}

type Summary struct {
	Shaders       int            `json:"shaders"`
	ByStage       map[string]int `json:"shadersByStage"`
	FeaturesUsed  int            `json:"featuresUsed"`
	Unsupported   int            `json:"unsupportedFeatures"`
	Blockers      int            `json:"blockers"`
	UnknownSupp   int            `json:"unknownSupport"`
	Peaks         Peaks          `json:"peaks"`
	DeviceKnown   bool           `json:"deviceKnown"`
	ExtensionsTop map[string]int `json:"spirvExtensions,omitempty"`
}

type Report struct {
	Schema    string           `json:"schema"`
	Version   int              `json:"schemaVersion"`
	Tool      string           `json:"tool"`
	Generated string           `json:"generated,omitempty"`
	Name      string           `json:"name"`
	Inputs    Inputs           `json:"inputs"`
	Device    *Device          `json:"device,omitempty"`
	Title     *TitleInfo       `json:"title,omitempty"`
	Summary   Summary          `json:"summary"`
	Blockers  []string         `json:"blockers"`                     // ranked ids: unsupported features of severity blocker that are used
	NextUp    []string         `json:"blockersAfterKnown,omitempty"` // Blockers minus --known
	Predicted []string         `json:"predictedBlockers,omitempty"`  // unsupported features only predicted by title strings/imports, ranked
	Known     []string         `json:"known,omitempty"`
	Features  []FeatureReport  `json:"features"`
	Pipelines *PipelineSummary `json:"pipelines,omitempty"`
	Shaders   []*ShaderInfo    `json:"shaders"`
	Notes     []string         `json:"notes,omitempty"`
}

type BuildInput struct {
	Name    string
	Device  *Device
	Title   *TitleInfo
	Shaders []*ShaderInfo
	Recipes []GraphicsRecipe
	Journal []*Journal
	Inputs  Inputs
	Known   []string
	Now     bool
}

var vkTopologies = []string{"POINT_LIST", "LINE_LIST", "LINE_STRIP", "TRIANGLE_LIST", "TRIANGLE_STRIP", "TRIANGLE_FAN", "LINE_LIST_WITH_ADJACENCY", "LINE_STRIP_WITH_ADJACENCY", "TRIANGLE_LIST_WITH_ADJACENCY", "TRIANGLE_STRIP_WITH_ADJACENCY", "PATCH_LIST"}

func fmtName(f uint32) string {
	if n, ok := vkFormatNames[f]; ok {
		return n
	}
	return fmt.Sprintf("VK_FORMAT_%d", f)
}

// recipeFeatures derives the pipeline-level feature ids of one graphics pipeline.
func recipeFeatures(g *GraphicsRecipe) []string {
	var f []string
	add := func(s string) { f = append(f, s) }
	if g.Mesh {
		add("exec.mesh")
	}
	if g.Tess {
		add("exec.tessellation")
	}
	if g.Path == "Geometry" || g.Path == "TessellationGeometry" || g.TessGeom {
		add("exec.geometry")
	}
	if g.Samples > 1 {
		add(fmt.Sprintf("pipe.msaa.%d", g.Samples))
	}
	if g.CustomSample {
		add("pipe.sample_locations")
	}
	if g.PerSample {
		add("pipe.sample_shading")
	}
	if g.DepthBounds {
		add("pipe.depth_bounds")
	}
	if g.DepthClamp {
		add("pipe.depth_clamp")
	}
	if g.DepthBias && g.BiasClamp != 0 {
		add("pipe.depth_bias_clamp")
	}
	if g.NegOneToOne {
		add("pipe.depth_clip_control")
	}
	if g.DualSource {
		add("pipe.dual_source_blend")
	}
	if g.PolygonMode != 0 {
		add("pipe.polygon_line")
	}
	if g.LineWidth > 1 && (g.Topology == 1 || g.Topology == 2 || g.Topology == 6 || g.Topology == 7) {
		add("pipe.wide_lines")
	}
	if g.Conservative != 0 {
		add("pipe.conservative_raster")
	}
	if g.ProvokingVtx != 0 {
		add("pipe.provoking_vertex")
	}
	if g.PrimRestart {
		add("pipe.primitive_restart")
	}
	seen := map[uint32]bool{}
	for _, c := range g.ColorFormats {
		if c != 0 && !seen[c] {
			seen[c] = true
			add("pipe.format.color." + fmtName(c))
		}
	}
	if g.DepthFormat != 0 {
		add("pipe.format.depth." + fmtName(g.DepthFormat))
	}
	return f
}

type limitUse struct {
	worst    float64
	worstID  string
	limit    float64
	limitKey string
	over     []string
}

func Build(in BuildInput) *Report {
	r := &Report{Schema: SchemaName, Version: SchemaVersion, Tool: "gpuscan " + ToolVersion, Name: in.Name, Inputs: in.Inputs, Title: in.Title, Shaders: in.Shaders, Known: in.Known}
	if in.Now {
		r.Generated = time.Now().UTC().Format(time.RFC3339)
	}
	r.Device = in.Device
	dev := in.Device
	sort.Slice(r.Shaders, func(i, j int) bool { return r.Shaders[i].ID < r.Shaders[j].ID })
	needShaders := map[string]map[string]bool{}
	needPipes := map[string]int{}
	hintsFor := map[string][]string{}
	mark := func(id, shader string) {
		if needShaders[id] == nil {
			needShaders[id] = map[string]bool{}
		}
		needShaders[id][shader] = true
	}
	lim := map[string]*limitUse{
		"limits.shared_memory":   {limitKey: "maxComputeSharedMemorySize"},
		"limits.push_constants":  {limitKey: "maxPushConstantsSize"},
		"limits.descriptor_sets": {limitKey: "maxBoundDescriptorSets"},
		"limits.descriptors":     {limitKey: "maxPerStageDescriptor*"},
	}
	for k, u := range lim {
		if dev != nil && !strings.HasSuffix(u.limitKey, "*") {
			u.limit, _ = dev.Limit(u.limitKey)
		}
		_ = k
	}
	stageCount := map[string]int{}
	extCount := map[string]int{}
	var pk Peaks
	pk.Descriptors = map[string]uint32{}
	observe := func(id string, v float64, sid string, over bool) {
		u := lim[id]
		if v > u.worst {
			u.worst, u.worstID = v, sid
		}
		if over {
			u.over = append(u.over, sid)
		}
	}
	for _, s := range r.Shaders {
		stageCount[s.Stage]++
		for _, e := range s.Extensions {
			extCount[e]++
		}
		for _, f := range s.Features {
			mark(f, s.ID)
		}
		if s.SharedBytes > pk.SharedBytes {
			pk.SharedBytes, pk.SharedShader = s.SharedBytes, s.ID
		}
		if s.PushBytes > pk.PushBytes {
			pk.PushBytes = s.PushBytes
		}
		if s.MaxSet > pk.MaxSet {
			pk.MaxSet = s.MaxSet
		}
		if s.Unbounded {
			pk.UnboundedShader++
		}
		if n := s.LocalSize; len(n) == 3 && n[0]*n[1]*n[2] > pk.MaxLocalInvoc {
			pk.MaxLocalInvoc = n[0] * n[1] * n[2]
		}
		for k, v := range s.Descriptors {
			if v > pk.Descriptors[k] {
				pk.Descriptors[k] = v
			}
		}
		if dev == nil {
			continue
		}
		observe("limits.shared_memory", float64(s.SharedBytes), s.ID, float64(s.SharedBytes) > lim["limits.shared_memory"].limit)
		observe("limits.push_constants", float64(s.PushBytes), s.ID, float64(s.PushBytes) > lim["limits.push_constants"].limit)
		observe("limits.descriptor_sets", float64(s.MaxSet+1), s.ID, float64(s.MaxSet+1) > lim["limits.descriptor_sets"].limit)
		over := false
		for kind, key := range map[string]string{"sampler": "maxPerStageDescriptorSamplers", "uniformBuffer": "maxPerStageDescriptorUniformBuffers", "storageBuffer": "maxPerStageDescriptorStorageBuffers", "sampledImage": "maxPerStageDescriptorSampledImages", "storageImage": "maxPerStageDescriptorStorageImages", "inputAttachment": "maxPerStageDescriptorInputAttachments"} {
			if l, ok := dev.Limit(key); ok && float64(s.Descriptors[kind]) > l {
				over = true
			}
		}
		observe("limits.descriptors", 0, s.ID, over)
	}
	for id, u := range lim {
		if dev == nil {
			continue
		}
		for _, sid := range u.over {
			mark(id, sid)
		}
		_ = id
	}
	// pipelines
	var ps *PipelineSummary
	if len(in.Journal) > 0 {
		ps = &PipelineSummary{Source: "pipelines.journal", Signatures: map[string]int{}, Topologies: map[string]int{}, Samples: map[string]int{}, ColorFormats: map[string]int{}, DepthFormats: map[string]int{}}
		for _, j := range in.Journal {
			ps.JournalFmt = j.Format
			ps.Compute += j.Computes
			ps.Modules += len(j.Modules)
			if j.NoZstd {
				ps.Notes = append(ps.Notes, "zstd CLI not found: journal shader modules could not be decoded")
			}
			if j.Dropped {
				ps.Notes = append(ps.Notes, "some journal records were unreadable (format change or torn tail); they were skipped")
			}
		}
		for i := range in.Recipes {
			g := &in.Recipes[i]
			ps.Graphics++
			ps.Signatures[g.Signature]++
			if int(g.Topology) < len(vkTopologies) {
				ps.Topologies[vkTopologies[g.Topology]]++
			}
			ps.Samples[fmt.Sprint(max(g.Samples, 1))]++
			for _, c := range g.ColorFormats {
				ps.ColorFormats[fmtName(c)]++
			}
			if g.DepthFormat != 0 {
				ps.DepthFormats[fmtName(g.DepthFormat)]++
			}
			for _, f := range recipeFeatures(g) {
				needPipes[f]++
			}
		}
		r.Pipelines = ps
	}
	// title hints
	if in.Title != nil {
		for _, h := range in.Title.Hints {
			for _, id := range h.Implies {
				hintsFor[id] = append(hintsFor[id], h.Pattern)
			}
		}
	}
	ids := map[string]bool{}
	for id := range needShaders {
		ids[id] = true
	}
	for id := range needPipes {
		ids[id] = true
	}
	for id := range hintsFor {
		ids[id] = true
	}
	if len(r.Shaders) > 0 && dev != nil {
		for id := range lim {
			ids[id] = true
		}
	}
	known := map[string]bool{}
	for _, k := range in.Known {
		known[k] = true
	}
	for id := range ids {
		def := LookupFeature(id)
		fr := FeatureReport{ID: id, Category: def.Category, Title: def.Title, Severity: def.Severity, Workaround: def.Workaround, Known: known[id]}
		if u, ok := lim[id]; ok {
			fr.Supported, fr.Evidence = limitSupport(id, u, dev, pk)
		} else if def.Check != nil {
			fr.Supported, fr.Evidence = def.Check(dev)
		}
		var sids []string
		for s := range needShaders[id] {
			sids = append(sids, s)
		}
		sort.Strings(sids)
		fr.Needed.Shaders = len(sids)
		if len(sids) > 25 {
			sids = sids[:25]
		}
		fr.Needed.ShaderIDs = sids
		fr.Needed.Pipelines = needPipes[id]
		fr.Needed.Hints = hintsFor[id]
		fr.Predicted = fr.Needed.Shaders == 0 && fr.Needed.Pipelines == 0 && len(hintsFor[id]) > 0
		r.Features = append(r.Features, fr)
	}
	sort.Slice(r.Features, func(i, j int) bool { return r.Features[i].ID < r.Features[j].ID })
	// rank: unsupported + actually used
	var ranked []*FeatureReport
	for i := range r.Features {
		f := &r.Features[i]
		used := f.Needed.Shaders > 0 || f.Needed.Pipelines > 0
		if used {
			r.Summary.FeaturesUsed++
		}
		if f.Supported == Unknown && used && f.Severity != SevInfo {
			r.Summary.UnknownSupp++
		}
		if (f.Supported == No || f.Supported == Partial) && (used || f.Predicted) {
			ranked = append(ranked, f)
		}
	}
	sort.SliceStable(ranked, func(i, j int) bool {
		a, b := ranked[i], ranked[j]
		if a.Predicted != b.Predicted {
			return !a.Predicted
		}
		if sevWeight(a.Severity) != sevWeight(b.Severity) {
			return sevWeight(a.Severity) > sevWeight(b.Severity)
		}
		ua, ub := a.Needed.Shaders+a.Needed.Pipelines, b.Needed.Shaders+b.Needed.Pipelines
		if ua != ub {
			return ua > ub
		}
		return a.ID < b.ID
	})
	r.Blockers = []string{}
	for i, f := range ranked {
		f.Rank = i + 1
		r.Summary.Unsupported++
		if f.Severity == SevBlocker && !f.Predicted {
			r.Summary.Blockers++
			r.Blockers = append(r.Blockers, f.ID)
			if !known[f.ID] {
				r.NextUp = append(r.NextUp, f.ID)
			}
		}
	}
	if len(in.Known) == 0 {
		r.NextUp = nil
	}
	for _, f := range ranked {
		if f.Needed.Shaders == 0 {
			continue
		}
		co := map[string]int{}
		for _, s := range r.Shaders {
			if !needShaders[f.ID][s.ID] {
				continue
			}
			for _, g := range s.Features {
				if g == f.ID {
					continue
				}
				if d := LookupFeature(g); d.Severity == SevInfo || d.Category == "limits" || d.Category == "format" {
					continue
				}
				co[g]++
			}
		}
		f.CoNeeded = topN(co, 8)
	}
	for _, f := range ranked {
		if f.Predicted && (f.Severity == SevBlocker || f.Severity == SevMajor) {
			r.Predicted = append(r.Predicted, f.ID)
		}
	}
	r.Summary.Shaders = len(r.Shaders)
	r.Summary.ByStage = stageCount
	r.Summary.Peaks = pk
	r.Summary.DeviceKnown = dev != nil
	r.Summary.ExtensionsTop = extCount
	if dev == nil {
		r.Notes = append(r.Notes, "no device data: support is reported as unknown; pass --icd <ICD manifest|install dir|vulkaninfo.json>")
	}
	return r
}

func limitSupport(id string, u *limitUse, dev *Device, pk Peaks) (string, string) {
	if dev == nil {
		return Unknown, "no device data"
	}
	var ev string
	switch id {
	case "limits.shared_memory":
		ev = fmt.Sprintf("peak %d B%s vs %s=%.0f", pk.SharedBytes, map[bool]string{true: " (shader " + pk.SharedShader + ")", false: ""}[pk.SharedShader != ""], u.limitKey, u.limit)
	case "limits.push_constants":
		ev = fmt.Sprintf("peak %d B vs %s=%.0f", pk.PushBytes, u.limitKey, u.limit)
	case "limits.descriptor_sets":
		ev = fmt.Sprintf("highest set index %d vs %s=%.0f", pk.MaxSet, u.limitKey, u.limit)
	default:
		ev = fmt.Sprintf("peak per-stage counts %v vs per-stage limits", pk.Descriptors)
	}
	if len(u.over) > 0 {
		return No, ev
	}
	return Yes, ev
}

// ---- markdown ---------------------------------------------------------------------------------

func (r *Report) Markdown() string {
	var b strings.Builder
	w := func(f string, a ...any) { fmt.Fprintf(&b, f, a...) }
	w("# GPU feature scan: %s\n\n", r.Name)
	w("Schema `%s` v%d, %s.", r.Schema, r.Version, r.Tool)
	if r.Generated != "" {
		w(" Generated %s.", r.Generated)
	}
	w("\n\n")
	if r.Device != nil {
		w("**Device**: %s, driver %s, Vulkan %s (%s)\n\n", r.Device.Name, r.Device.Driver, r.Device.APIVersion, r.Device.Source)
	} else {
		w("**Device**: not provided (support unknown)\n\n")
	}
	w("**Inputs**: %d shader modules (%d from cache dirs, resource-plan entries skipped: %d), %d pipeline journal(s)", r.Summary.Shaders, r.Inputs.CacheFiles, r.Inputs.PlanEntries, r.Inputs.Journals)
	if r.Title != nil {
		w(", title `%s`", r.Title.Path)
	}
	w("\n\n")
	// blockers
	w("## Ranked blockers\n\n")
	if len(r.Blockers) == 0 {
		w("No blocker-severity feature is both used and unsupported on this device.\n\n")
	} else {
		w("| # | feature | needed by | support | evidence | workaround |\n|---|---|---|---|---|---|\n")
		for _, id := range r.Blockers {
			f := r.feature(id)
			k := ""
			if f.Known {
				k = " (known)"
			}
			w("| %d | `%s`%s | %s | %s | %s | %s |\n", f.Rank, f.ID, k, f.usage(), f.Supported, esc(f.Evidence), esc(f.Workaround))
		}
		w("\n")
	}
	if len(r.Known) > 0 {
		w("### Next blockers after known (%s)\n\n", strings.Join(r.Known, ", "))
		if len(r.NextUp) == 0 {
			w("None.\n\n")
		}
		for _, id := range r.NextUp {
			f := r.feature(id)
			w("%d. `%s`: %s; needed by %s. %s\n", f.Rank, f.ID, f.Title, f.usage(), esc(f.Workaround))
		}
		w("\n")
	}
	if len(r.Predicted) > 0 {
		w("## Predicted next blockers (from title strings and imports; no compiled shader needs them yet)\n\n| # | feature | severity | support | why |\n|---|---|---|---|---|\n")
		for _, id := range r.Predicted {
			f := r.feature(id)
			w("| %d | `%s` | %s | %s | %s |\n", f.Rank, f.ID, f.Severity, f.Supported, esc(strings.Join(f.Needed.Hints, ", ")+"; "+f.Evidence))
		}
		w("\n")
	}
	// co-needed
	for _, id := range r.Blockers {
		if f := r.feature(id); len(f.CoNeeded) > 0 {
			w("`%s` shaders also need: %s.\n\n", f.ID, mapStr(f.CoNeeded))
		}
	}
	// other unsupported
	var others []*FeatureReport
	for i := range r.Features {
		f := &r.Features[i]
		if f.Rank > 0 && (f.Severity != SevBlocker || f.Predicted) {
			others = append(others, f)
		}
	}
	sort.Slice(others, func(i, j int) bool { return others[i].Rank < others[j].Rank })
	if len(others) > 0 {
		w("## Other unsupported or partially supported features\n\n| # | feature | severity | needed by | evidence | workaround |\n|---|---|---|---|---|---|\n")
		for _, f := range others {
			pred := ""
			if f.Predicted {
				pred = " (predicted from title strings or imports)"
			}
			w("| %d | `%s`%s | %s | %s | %s | %s |\n", f.Rank, f.ID, pred, f.Severity, f.usage(), esc(f.Evidence), esc(f.Workaround))
		}
		w("\n")
	}
	// unknown
	var unk []string
	for _, f := range r.Features {
		if f.Supported == Unknown && (f.Needed.Shaders+f.Needed.Pipelines) > 0 {
			unk = append(unk, fmt.Sprintf("`%s` (%d shaders)", f.ID, f.Needed.Shaders))
		}
	}
	if len(unk) > 0 {
		w("## Needed, support not determinable\n\n%s\n\n", strings.Join(unk, ", "))
	}
	w("## Limits headroom\n\n| check | status | evidence |\n|---|---|---|\n")
	for _, f := range r.Features {
		if f.Category == "limits" {
			w("| `%s` | %s | %s |\n", f.ID, f.Supported, esc(f.Evidence))
		}
	}
	w("\n")
	// supported used
	w("## Used and supported\n\n| feature | needed by | evidence |\n|---|---|---|\n")
	for _, f := range r.Features {
		if f.Supported == Yes && (f.Needed.Shaders+f.Needed.Pipelines) > 0 && f.Severity != SevInfo {
			w("| `%s` | %s | %s |\n", f.ID, f.usage(), esc(f.Evidence))
		}
	}
	w("\n")
	if r.Pipelines != nil {
		p := r.Pipelines
		w("## Pipelines (journal)\n\n%d graphics, %d compute, %d modules. Stage combinations: %s. Topologies: %s. Sample counts: %s.\n\nColor formats: %s. Depth formats: %s.\n\n", p.Graphics, p.Compute, p.Modules, mapStr(p.Signatures), mapStr(p.Topologies), mapStr(p.Samples), mapStr(p.ColorFormats), mapStr(p.DepthFormats))
		for _, n := range p.Notes {
			w("- %s\n", n)
		}
	}
	w("## Shaders\n\n%d modules; by stage: %s. Peaks: shared memory %d B, push constants %d B, highest descriptor set %d, unbounded descriptor arrays in %d shaders.\n\n", r.Summary.Shaders, mapStr(r.Summary.ByStage), r.Summary.Peaks.SharedBytes, r.Summary.Peaks.PushBytes, r.Summary.Peaks.MaxSet, r.Summary.Peaks.UnboundedShader)
	bad := map[string]bool{}
	for _, f := range r.Features {
		if f.Rank > 0 {
			bad[f.ID] = true
		}
	}
	rows := 0
	w("Shaders needing an unsupported feature (full per-shader detail is in the JSON):\n\n| shader | stage | unsupported features needed |\n|---|---|---|\n")
	for _, s := range r.Shaders {
		var l []string
		for _, f := range s.Features {
			if bad[f] {
				l = append(l, "`"+f+"`")
			}
		}
		if len(l) > 0 && rows < 60 {
			w("| %s | %s | %s |\n", s.ID, s.Stage, strings.Join(l, ", "))
			rows++
		}
	}
	if rows == 0 {
		w("| none | | |\n")
	}
	w("\n")
	if t := r.Title; t != nil {
		w("## Title (static)\n\n")
		if t.TitleID != "" || t.Name != "" {
			w("%s %s. ", t.TitleID, t.Name)
		}
		if t.Engine != "" {
			w("Engine string: `%s`. ", t.Engine)
		}
		w("%d modules scanned.\n\n", len(t.Modules))
		w("GPU imports by library: %s.\n\n", mapStr(t.GPULibraries))
		if len(t.GPUImports) > 0 {
			var names []string
			for _, g := range t.GPUImports {
				names = append(names, g.Name)
			}
			w("<details><summary>%d GPU-related imports</summary>\n\n%s\n\n</details>\n\n", len(names), "`"+strings.Join(names, "`, `")+"`")
		}
		e := t.EmbeddedShaders
		w("Embedded AGC shader headers in module images: %d (%s), %d bytes of code.\n\n", e.Headers, mapStr(e.ByType), e.CodeBytes)
		for _, h := range t.Hints {
			w("- hint `%s` x%d: %s %s\n", h.Pattern, h.Count, h.Note, impl(h.Implies))
		}
		for _, n := range t.Notes {
			w("- %s\n", n)
		}
		w("\n")
	}
	for _, n := range r.Notes {
		w("- %s\n", n)
	}
	return b.String()
}

func impl(i []string) string {
	if len(i) == 0 {
		return ""
	}
	return "(implies " + strings.Join(i, ", ") + ")"
}

func (r *Report) feature(id string) *FeatureReport {
	for i := range r.Features {
		if r.Features[i].ID == id {
			return &r.Features[i]
		}
	}
	return &FeatureReport{ID: id}
}

func (f *FeatureReport) usage() string {
	s := fmt.Sprintf("%d shaders", f.Needed.Shaders)
	if f.Needed.Pipelines > 0 {
		s += fmt.Sprintf(", %d pipelines", f.Needed.Pipelines)
	}
	if len(f.Needed.Hints) > 0 {
		s += ", hint: " + strings.Join(f.Needed.Hints, "/")
	}
	return s
}

func esc(s string) string { return strings.ReplaceAll(strings.ReplaceAll(s, "|", "\\|"), "\n", " ") }

func mapStr(m map[string]int) string {
	if len(m) == 0 {
		return "none"
	}
	type kv struct {
		k string
		v int
	}
	var l []kv
	for k, v := range m {
		l = append(l, kv{k, v})
	}
	sort.Slice(l, func(i, j int) bool {
		if l[i].v != l[j].v {
			return l[i].v > l[j].v
		}
		return l[i].k < l[j].k
	})
	var p []string
	for _, e := range l {
		p = append(p, fmt.Sprintf("%s=%d", e.k, e.v))
	}
	return strings.Join(p, ", ")
}

func topN(m map[string]int, n int) map[string]int {
	type kv struct {
		k string
		v int
	}
	var l []kv
	for k, v := range m {
		l = append(l, kv{k, v})
	}
	sort.Slice(l, func(i, j int) bool {
		if l[i].v != l[j].v {
			return l[i].v > l[j].v
		}
		return l[i].k < l[j].k
	})
	out := map[string]int{}
	for i := 0; i < len(l) && i < n; i++ {
		out[l[i].k] = l[i].v
	}
	if len(out) == 0 {
		return nil
	}
	return out
}
