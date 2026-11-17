// Command gpuscan reports which GPU features a PS5 title needs versus what the target Vulkan
// device (KosmicKrisp on Metal) provides, ranked as blockers. See docs/m3/gpuscan.md.
//
//	gpuscan [-title DIR] [-cache DIR]... [-icd PATH] [-json OUT.json] [-md OUT.md] [-name N]
//	gpuscan -compare A.json B.json [-json OUT.json] [-md OUT.md]
//
// Prior art consulted (design only, no code copied): nxnative's dm/tools census/triage scripts
// (MIT): finding ids that an agent can act on, a bracketed severity vocabulary, and scanning
// binaries for engine console-variable names (hints here).
package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

type multi []string

func (m *multi) String() string     { return strings.Join(*m, ",") }
func (m *multi) Set(v string) error { *m = append(*m, v); return nil }

func die(f string, a ...any) {
	fmt.Fprintf(os.Stderr, "gpuscan: "+f+"\n", a...)
	os.Exit(1)
}

func main() {
	var caches, skipMods multi
	title := flag.String("title", "", "PS5 title directory (eboot.bin + sce_module/*.prx)")
	flag.Var(&caches, "cache", "AnyPS5 run/cache directory (repeatable): searched for the shader disk cache and pipelines.journal")
	icd := flag.String("icd", "", "KosmicKrisp ICD manifest, install dir (share/vulkan/icd.d), or a saved `vulkaninfo --json` file; default: the system loader")
	noCache := flag.Bool("no-device-cache", false, "re-run vulkaninfo instead of using the cached result")
	spv := flag.String("spv", "", "also analyse this .spv file or directory of .spv files")
	name := flag.String("name", "", "report name (default: title id/dir or cache dir name)")
	jsonOut := flag.String("json", "", "write the JSON report here (default: stdout when -md is not given)")
	mdOut := flag.String("md", "", "write the Markdown summary here")
	known := flag.String("known", "", "comma-separated feature ids already being worked on (e.g. exec.mesh,exec.task); the report lists blockers beyond them")
	jobs := flag.Int("j", 3, "parallel jobs (zstd decodes)")
	cmp := flag.Bool("compare", false, "compare two JSON reports given as arguments: gpuscan -compare A.json B.json")
	capture := flag.String("capture", "", "APS5_FRAME_CAPTURE directory: replayed with AnyPS5 agc_frame_replay into a scratch cache that is then scanned (untested: no captures were available)")
	anyBuild := flag.String("anyps5-build", os.Getenv("NPS5_ANYPS5_BUILD"), "AnyPS5 arm64 build dir (for -capture / -req)")
	var reqs multi
	flag.Var(&reqs, "req", "saved AGC shader recompile request (shader_*.req, from APS5_DUMP_SHADERS=1), converted with agc_shader_replay --spv (repeatable; untested)")
	noTime := flag.Bool("no-timestamp", false, "omit the generated timestamp (reproducible output)")
	maxMB := flag.Int("max-module-mb", 600, "skip non-eboot modules larger than this when scanning a title")
	flag.Var(&skipMods, "skip-module", "skip modules whose file name contains this (repeatable), e.g. kitt_webkit")
	flag.Parse()

	if *cmp {
		if flag.NArg() != 2 {
			die("usage: gpuscan -compare A.json B.json")
		}
		a, err := LoadReport(flag.Arg(0))
		if err != nil {
			die("%v", err)
		}
		b, err := LoadReport(flag.Arg(1))
		if err != nil {
			die("%v", err)
		}
		c := Compare(a, b)
		emit(*jsonOut, *mdOut, c, c.Markdown())
		return
	}
	if *title == "" && len(caches) == 0 && *spv == "" && *capture == "" && len(reqs) == 0 {
		flag.Usage()
		os.Exit(2)
	}

	var dev *Device
	if d, err := LoadDevice(*icd, *noCache); err != nil {
		fmt.Fprintf(os.Stderr, "gpuscan: warning: no device data (%v); support will be unknown\n", err)
	} else {
		dev = d
	}

	in := BuildInput{Device: dev, Now: !*noTime, Known: splitList(*known)}
	if *title != "" {
		in.Title = ScanTitle(*title, skipMods, *maxMB)
		in.Inputs.Title = *title
	}
	byID := map[string]*ShaderInfo{}
	addModule := func(w []uint32, origin string) *ShaderInfo {
		s, err := AnalyzeSPIRV(w)
		if err != nil {
			return nil
		}
		if old, ok := byID[s.ID]; ok {
			if !strings.Contains(old.Origin, origin) {
				old.Origin += "+" + origin
			}
			return old
		}
		s.Origin = origin
		byID[s.ID] = s
		in.Shaders = append(in.Shaders, s)
		return s
	}
	if *capture != "" {
		dir, out, err := ReplayCapture(*anyBuild, *capture, *icd, 20*time.Minute)
		if err != nil {
			fmt.Fprintf(os.Stderr, "gpuscan: warning: capture replay failed: %v\n%s\n", err, out)
		}
		caches = append(caches, dir)
		in.Inputs.Captures = append(in.Inputs.Captures, *capture)
	}
	for _, c := range caches {
		for _, dir := range FindCacheDirs(c) {
			in.Inputs.CacheDirs = append(in.Inputs.CacheDirs, dir)
			mods, plans, err := LoadCacheShaders(dir)
			if err != nil {
				fmt.Fprintf(os.Stderr, "gpuscan: %s: %v\n", dir, err)
			}
			in.Inputs.PlanEntries += plans
			for _, w := range mods {
				if addModule(w, "cache") != nil {
					in.Inputs.CacheFiles++
				}
			}
			if jp := filepath.Join(dir, "pipelines.journal"); fileExists(jp) {
				j, err := LoadJournal(jp, *jobs)
				if err != nil {
					fmt.Fprintf(os.Stderr, "gpuscan: %s: %v\n", jp, err)
					continue
				}
				in.Inputs.Journals++
				in.Journal = append(in.Journal, j)
				in.Recipes = append(in.Recipes, j.Graphics...)
				for _, m := range j.Modules {
					if s := addModule(m.Words, "journal"); s != nil && m.Stage == "Mesh" {
						s.add("exec.mesh")
						s.Features = featureList(s)
					}
				}
			}
		}
	}
	if len(reqs) > 0 {
		dir, err := ReplayRequests(*anyBuild, reqs)
		if err != nil {
			fmt.Fprintf(os.Stderr, "gpuscan: warning: %v\n", err)
		}
		if dir != "" && *spv == "" {
			*spv = dir
		}
	}
	if *spv != "" {
		files := []string{*spv}
		if fi, err := os.Stat(*spv); err == nil && fi.IsDir() {
			files, _ = filepath.Glob(filepath.Join(*spv, "*.spv"))
		}
		for _, f := range files {
			if b, err := os.ReadFile(f); err == nil {
				if w, ok := WordsFromBytes(b); ok {
					addModule(w, "spv")
				}
			}
		}
	}
	if len(caches) > 0 && len(in.Inputs.CacheDirs) == 0 {
		fmt.Fprintln(os.Stderr, "gpuscan: warning: no shader cache or pipeline journal found under the -cache directories")
	}
	in.Name = *name
	if in.Name == "" {
		switch {
		case in.Title != nil && in.Title.TitleID != "":
			in.Name = in.Title.TitleID
		case *title != "":
			in.Name = filepath.Base(filepath.Clean(*title))
		case len(caches) > 0:
			in.Name = filepath.Base(filepath.Clean(caches[0]))
		default:
			in.Name = "scan"
		}
	}
	if dev != nil {
		in.Inputs.Device = dev.Name + " / " + dev.Driver
		d := *dev
		in.Device = &d
	}
	rep := Build(in)
	emit(*jsonOut, *mdOut, rep, rep.Markdown())
}

func featureList(s *ShaderInfo) []string {
	var l []string
	for f := range s.need {
		l = append(l, f)
	}
	sort.Strings(l)
	return l
}

func splitList(s string) []string {
	var o []string
	for _, p := range strings.Split(s, ",") {
		if p = strings.TrimSpace(p); p != "" {
			o = append(o, p)
		}
	}
	return o
}

func emit(jsonPath, mdPath string, v any, md string) {
	b, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		die("%v", err)
	}
	b = append(b, '\n')
	if jsonPath != "" {
		if err := os.WriteFile(jsonPath, b, 0o644); err != nil {
			die("%v", err)
		}
	}
	if mdPath != "" {
		if err := os.WriteFile(mdPath, []byte(md), 0o644); err != nil {
			die("%v", err)
		}
	}
	if jsonPath == "" && mdPath == "" {
		os.Stdout.Write(b)
	}
}
