package main

import (
	"bytes"
	_ "embed"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"sync"

	"ps4-recomp/pkg/elfloader"
)

// gpunames.txt: exported function names of AnyPS5's GPU libraries (libSceAgc, libSceAgcDriver,
// libSceVideoOut, libSceAmpr, libSceGnmDriver ...), extracted from the APS5_VABI definitions in
// the AnyPS5 tree. NIDs are computed from them so PS5 NID imports print with names.
//
//go:embed gpunames.txt
var gpuNamesTxt string

var (
	nidNames     map[string]string
	nidNamesOnce sync.Once
)

func nidName(dynName string) (string, bool) {
	nidNamesOnce.Do(func() {
		nidNames = map[string]string{}
		for _, n := range strings.Fields(gpuNamesTxt) {
			nidNames[elfloader.CalculateNID(n)] = n
		}
	})
	nid := elfloader.NIDPrefix(dynName)
	if n, ok := nidNames[nid]; ok {
		return n, true
	}
	return elfloader.ResolveNID(dynName)
}

var gpuLibPrefixes = []string{"libSceAgc", "libSceGnm", "libSceVideoOut", "libSceAmpr", "libSceGpu", "libSceRazorCpu"}

func isGPULib(lib string) bool {
	for _, p := range gpuLibPrefixes {
		if strings.HasPrefix(lib, p) {
			return true
		}
	}
	return false
}

// GPUImport is one imported symbol of a GPU-related library.
type GPUImport struct {
	Library string   `json:"library"`
	Name    string   `json:"name"`
	NID     string   `json:"nid"`
	Known   bool     `json:"nameKnown"`
	Modules []string `json:"modules"`
}

// EmbeddedShaders summarises AGC shader headers (magic 0x34333231, version 0x18) found in module images.
type EmbeddedShaders struct {
	Headers   int            `json:"headers"`
	ByType    map[string]int `json:"byType,omitempty"`
	ByTarget  map[string]int `json:"byTarget,omitempty"`
	CodeBytes uint64         `json:"codeBytes"`
	MaxBytes  uint32         `json:"largestShaderBytes,omitempty"`
}

// Hint is a static string found in the binary that predicts feature use (e.g. a UE console
// variable name); it is a prediction, not proof.
type Hint struct {
	Pattern string   `json:"pattern"`
	Count   int      `json:"count"`
	Implies []string `json:"impliesFeatures,omitempty"`
	Note    string   `json:"note"`
}

type ModuleInfo struct {
	Name       string `json:"name"`
	Bytes      int64  `json:"bytes"`
	Loaded     bool   `json:"loaded"`
	Error      string `json:"error,omitempty"`
	Imports    int    `json:"imports"`
	GPUImports int    `json:"gpuImports"`
}

type TitleInfo struct {
	Path            string          `json:"path"`
	TitleID         string          `json:"titleId,omitempty"`
	Name            string          `json:"name,omitempty"`
	Engine          string          `json:"engine,omitempty"`
	Modules         []ModuleInfo    `json:"modules"`
	GPUImports      []GPUImport     `json:"gpuImports"`
	GPULibraries    map[string]int  `json:"gpuLibraries"`
	Libraries       map[string]int  `json:"importedLibraries"`
	EmbeddedShaders EmbeddedShaders `json:"embeddedShaders"`
	Hints           []Hint          `json:"hints,omitempty"`
	Notes           []string        `json:"notes,omitempty"`
}

type hintDef struct {
	pat     string
	implies []string
	note    string
}

var hintDefs = []hintDef{
	{"r.Nanite.MeshShaderRasterization", []string{"exec.mesh"}, "UE5 Nanite hardware rasteriser via mesh shaders"},
	{"r.Nanite", []string{"atomics.int64.buffer"}, "UE5 Nanite is built in: its software rasteriser needs 64-bit atomic max (buffer or image depending on RHI) and its hardware path needs mesh/primitive shaders"},
	{"r.Lumen", nil, "UE5 Lumen global illumination (heavy compute; software ray tracing uses distance fields, no RT hardware required)"},
	{"r.RayTracing", []string{"exec.raytracing"}, "UE ray tracing console variables present (may be disabled at runtime)"},
	{"r.VirtualShadowMaps", []string{"atomics.int64.buffer"}, "UE5 virtual shadow maps (rely on Nanite and 64-bit atomics)"},
	{"r.VRS", []string{"frag.vrs"}, "UE variable rate shading variables"},
	{"r.VariableRateShading", []string{"frag.vrs"}, "UE variable rate shading variables"},
	{"r.Shadow.Virtual", nil, "UE5 virtual shadow map settings"},
	{"r.TemporalAA", nil, "UE temporal AA (compute heavy, no special feature)"},
}

var ueVersion = regexp.MustCompile(`\+\+UE[45]\+Release-[0-9]+\.[0-9]+`)

func utf16le(s string) []byte {
	b := make([]byte, 0, len(s)*2)
	for _, c := range []byte(s) {
		b = append(b, c, 0)
	}
	return b
}

// shaderTypeNames follows ShaderBinaryType in libSceAgc/Shader/include/ShaderConstants.hpp.
var shaderTypeNames = []string{"Cs", "Ps", "Gs", "Hs", "GsFront", "HsFront", "GsBack", "HsBack", "Fs"}

// scanShaderHeaders looks for AGC `Shader` headers (struct Shader in libs/SceShaders.hpp): file_header
// 0x34333231, version 0x18, header_size @+64, shader_size @+68, target @+76, type @+90.
func scanShaderHeaders(img []byte, out *EmbeddedShaders) {
	magic := []byte{0x31, 0x32, 0x33, 0x34, 0x18, 0, 0, 0}
	off := 0
	for {
		i := bytes.Index(img[off:], magic)
		if i < 0 {
			return
		}
		p := off + i
		off = p + 8
		if p+96 > len(img) || p%4 != 0 {
			continue
		}
		hs := binary.LittleEndian.Uint32(img[p+64:])
		ss := binary.LittleEndian.Uint32(img[p+68:])
		target := binary.LittleEndian.Uint32(img[p+76:])
		typ := img[p+90]
		if hs < 96 || hs > 0x10000 || ss == 0 || ss&3 != 0 || ss > 16<<20 || int(typ) >= len(shaderTypeNames) {
			continue
		}
		if out.ByType == nil {
			out.ByType, out.ByTarget = map[string]int{}, map[string]int{}
		}
		out.Headers++
		out.ByType[shaderTypeNames[typ]]++
		out.ByTarget[hex32(target)]++
		out.CodeBytes += uint64(ss)
		if ss > out.MaxBytes {
			out.MaxBytes = ss
		}
	}
}

func hex32(v uint32) string { return fmt.Sprintf("0x%x", v) }

type importRule struct {
	re      *regexp.Regexp
	implies []string
	note    string
}

// importRules turn imported AGC entry points into predictions: a title that imports a function
// may call it. They are hints (reported as predicted), not proof.
var importRules = []importRule{
	{regexp.MustCompile(`^sceAgcAcb|SubmitAcb|SubmitMultiAcbs`), []string{"queue.async_compute"}, "asynchronous compute command buffers (ACB) are imported"},
	{regexp.MustCompile(`Predication`), []string{"pipe.conditional_rendering"}, "predicated rendering is imported"},
	{regexp.MustCompile(`Gds`), []string{"mem.gds"}, "global data share (GDS) functions are imported"},
}

func importHints(imps []GPUImport) []Hint {
	var out []Hint
	for _, r := range importRules {
		n := 0
		for _, g := range imps {
			if r.re.MatchString(g.Name) {
				n++
			}
		}
		if n > 0 {
			out = append(out, Hint{"import:" + r.re.String(), n, r.implies, r.note})
		}
	}
	return out
}

// ScanTitle inspects a title directory: eboot.bin plus every module under sce_module/.
func ScanTitle(dir string, skip []string, maxModuleMB int) *TitleInfo {
	t := &TitleInfo{Path: dir, GPULibraries: map[string]int{}, Libraries: map[string]int{}}
	if b, err := os.ReadFile(filepath.Join(dir, "sce_sys", "param.json")); err == nil {
		var p struct {
			TitleID string `json:"titleId"`
			Names   struct {
				Default string `json:"defaultLanguage"`
			}
			Loc map[string]map[string]any `json:"localizedParameters"`
		}
		if json.Unmarshal(b, &p) == nil {
			t.TitleID = p.TitleID
			var raw map[string]any
			if json.Unmarshal(b, &raw) == nil {
				if lp, ok := raw["localizedParameters"].(map[string]any); ok {
					def, _ := lp["defaultLanguage"].(string)
					if m, ok := lp[def].(map[string]any); ok {
						t.Name, _ = m["titleName"].(string)
					}
				}
			}
		}
	}
	var files []string
	for _, n := range []string{"eboot.bin", "eboot.elf"} {
		if _, err := os.Stat(filepath.Join(dir, n)); err == nil {
			files = append(files, filepath.Join(dir, n))
			break
		}
	}
	for _, pat := range []string{"*.prx", "*.sprx", "*.elf"} {
		m, _ := filepath.Glob(filepath.Join(dir, "sce_module", pat))
		sort.Strings(m)
		files = append(files, m...)
	}
	imports := map[string]*GPUImport{}
	for _, f := range files {
		mi := ModuleInfo{Name: filepath.Base(f)}
		if fi, err := os.Stat(f); err == nil {
			mi.Bytes = fi.Size()
		}
		skipped := false
		for _, s := range skip {
			if s != "" && strings.Contains(mi.Name, s) {
				skipped = true
			}
		}
		if skipped || (maxModuleMB > 0 && mi.Bytes > int64(maxModuleMB)<<20 && filepath.Base(f) != "eboot.bin") {
			mi.Error = "skipped (size limit or --skip-module)"
			t.Modules = append(t.Modules, mi)
			continue
		}
		l, err := elfloader.LoadELF(f)
		if err != nil {
			mi.Error = err.Error()
			t.Modules = append(t.Modules, mi)
			continue
		}
		mi.Loaded = true
		seen := map[string]bool{}
		for _, s := range l.DynSymbols {
			if s.Section != 0 || s.Name == "" || seen[s.Name] {
				continue
			}
			seen[s.Name] = true
			mi.Imports++
			lib := l.LibraryForNID(s.Name)
			t.Libraries[lib]++
			if !isGPULib(lib) {
				continue
			}
			mi.GPUImports++
			nm, known := nidName(s.Name)
			if !known {
				nm = elfloader.NIDPrefix(s.Name)
			}
			key := lib + "/" + elfloader.NIDPrefix(s.Name)
			gi := imports[key]
			if gi == nil {
				gi = &GPUImport{Library: lib, Name: nm, NID: elfloader.NIDPrefix(s.Name), Known: known}
				imports[key] = gi
				t.GPULibraries[lib]++
			}
			gi.Modules = append(gi.Modules, mi.Name)
		}
		for _, sg := range l.Segments {
			scanShaderHeaders(sg.Data, &t.EmbeddedShaders)
		}
		t.Modules = append(t.Modules, mi)
	}
	for _, g := range imports {
		t.GPUImports = append(t.GPUImports, *g)
	}
	sort.Slice(t.GPUImports, func(i, j int) bool {
		a, b := t.GPUImports[i], t.GPUImports[j]
		if a.Library != b.Library {
			return a.Library < b.Library
		}
		return a.Name < b.Name
	})
	t.Hints = append(t.Hints, importHints(t.GPUImports)...)
	// string hints from the main executable
	if len(files) > 0 {
		if raw, err := os.ReadFile(files[0]); err == nil {
			for _, h := range hintDefs {
				n := bytes.Count(raw, []byte(h.pat)) + bytes.Count(raw, utf16le(h.pat))
				if n > 0 {
					t.Hints = append(t.Hints, Hint{h.pat, n, h.implies, h.note})
				}
			}
			if v := ueVersion.Find(raw); v != nil {
				t.Engine = string(v)
			}
		}
	}
	if t.EmbeddedShaders.Headers == 0 {
		t.Notes = append(t.Notes, "no AGC shader headers found in module images: shaders are most likely loaded from data files (paks / shader libraries) at run time, so use the run's shader cache for per-shader analysis")
	}
	return t
}
