package cli

// Tier 1 (docs/m1/tier1.md): the translate-time side. A full translation records everything a
// later `ps4-recomp fragment` needs to lift more code the same way (the "tier1" section of
// lifter_manifest.json, aot_funcs.bin, nps5_tier1_info.c for the runtime), and `-seeds-from`
// folds the misses a run logged into the next translation.

import (
	"bufio"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"

	"ps4-recomp/pkg/disasm"
	"ps4-recomp/pkg/emitter"
	"ps4-recomp/pkg/lifter"
	"ps4-recomp/pkg/tcache"
)

// Tier1ABI is bumped when the fragment descriptor / entry contract changes.
const Tier1ABI = 2 // 2: flat call model, pc map in the compact record format, pads through recomp_cont_register

// tier1Info is the "tier1" section of lifter_manifest.json: the inputs and choices of the
// translation, so the fragment mode reproduces its guest layout, address mode and lowering.
type tier1Info struct {
	ABI              int      `json:"abi"`
	Elf              string   `json:"elf"`
	ElfSHA256        string   `json:"elf_sha256"`
	ElfSize          int64    `json:"elf_size"`
	ElfMtimeNs       int64    `json:"elf_mtime_ns"`
	AppDir           string   `json:"app_dir,omitempty"`
	AddrMode         string   `json:"addr_mode"` // resolved: base or identity
	TSO              bool     `json:"tso"`
	CallIC           bool     `json:"call_ic"`
	FpCmpFuse        bool     `json:"fp_cmp_fuse"`
	PushPopFuse      bool     `json:"pushpop_fuse"`
	JmpIC            bool     `json:"jmp_ic"`
	ROLoads          bool     `json:"ro_loads,omitempty"`     // -ro-loads (docs/m3/lift2.md)
	RedZoneLeaf      bool     `json:"redzone_leaf,omitempty"` // -redzone-leaf
	BMIPeep          bool     `json:"bmi_peep,omitempty"`     // -bmi-peep
	TSOElide         string   `json:"tso_elide,omitempty"`    // -tso-elide: fragments are lifted in the same mode
	ColdOutline      bool     `json:"cold_outline"`
	LayoutHash       string   `json:"layout_hash,omitempty"` // emitter.LayoutHash(): runtime struct offsets the text bakes in
	CodeABI          int      `json:"code_abi,omitempty"`    // emitter.ABIVersion
	AllowUnsupported bool     `json:"allow_unsupported"`
	PackedLayout     bool     `json:"packed_layout,omitempty"` // -packed-layout / NPS5_SEED_AFTER_UD2: fragments discover the same way
	AllSymbols       bool     `json:"all_symbols"`
	CallModel        string   `json:"call_model"` // flat or nested: the lifter's model, fragments are lifted in it
	X87              string   `json:"x87,omitempty"`
	SimdLiveness     string   `json:"simd_liveness,omitempty"` // abi (also when absent), strict or off
	TSOStrict        []uint64 `json:"tso_strict,omitempty"`    // functions whose vector/string accesses are fenced (NPS5_TSO_STRICT_FUNCS of the translation)
	TSORelax         []uint64 `json:"tso_relax,omitempty"`     // functions lifted without ordering (NPS5_TSO_RELAX_FUNCS of the translation)
	NoPeephole       bool     `json:"no_peephole,omitempty"`   // the translation ran with the peephole pass off (NPS5_NO_PEEPHOLE)
	GPU              string   `json:"gpu,omitempty"`
	ExcludeModules   []string `json:"exclude_modules,omitempty"`
	HLEMalloc        string   `json:"hle_malloc,omitempty"`  // -hle-malloc: fragments must bind the same replacements
	HLEReplace       string   `json:"hle_replace,omitempty"` // -hle-replace (absolute path)
	Patches          string   `json:"patches,omitempty"`     // -patches (absolute path): fragments lower the same patch points
	Translator       string   `json:"translator"`
	TranslatorSHA256 string   `json:"translator_sha256"`
	LoweringID       string   `json:"lowering_id,omitempty"` // emitter.LoweringID(): what a fragment must agree on with the title
	CacheRoot        string   `json:"cache_root,omitempty"`  // shared tier-1 fragment cache (survives retranslation)
	AOTFuncs         string   `json:"aot_funcs,omitempty"`   // file of u64le guest entries, next to the manifest
	OutDir           string   `json:"out_dir"`
}

func sha256File(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

// writeTier1Artifacts is called after the emitter ran: it writes aot_funcs.bin, merges the
// tier1 section into lifter_manifest.json and writes nps5_tier1_info.c (returned for the
// compile list). Failures are not fatal to the translation: tier 1 is then absent and a miss
// is fatal as before; the returned warning says why.
func writeTier1Artifacts(cfg *Config, funcs []disasm.ReachableFunc) (cfile string, warn error) {
	outAbs, err := filepath.Abs(cfg.OutDir)
	if err != nil {
		return "", err
	}
	elfAbs, err := filepath.Abs(cfg.ElfPath)
	if err != nil {
		return "", err
	}
	sum, err := sha256File(elfAbs)
	if err != nil {
		return "", err
	}
	fi, err := os.Stat(elfAbs)
	if err != nil {
		return "", err
	}
	exe, err := os.Executable()
	if err != nil {
		return "", err
	}
	if r, err := filepath.EvalSymlinks(exe); err == nil {
		exe = r
	}
	exeSum, err := sha256File(exe)
	if err != nil {
		return "", err
	}
	buf := make([]byte, 8*len(funcs))
	for i, f := range funcs {
		binary.LittleEndian.PutUint64(buf[8*i:], f.Addr)
	}
	if err := writeIfChanged(filepath.Join(outAbs, "aot_funcs.bin"), buf); err != nil {
		return "", err
	}
	info := tier1Info{
		ABI: Tier1ABI, Elf: elfAbs, ElfSHA256: sum, ElfSize: fi.Size(), ElfMtimeNs: fi.ModTime().UnixNano(),
		AppDir: cfg.AppDir, AddrMode: cfg.AddrMode, TSO: cfg.TSO, CallIC: cfg.CallIC, FpCmpFuse: cfg.FpCmpFuse, PushPopFuse: cfg.PushPopFuse, JmpIC: cfg.JmpIC, ColdOutline: cfg.ColdOutline, LayoutHash: emitter.KeyLayoutHash(), CodeABI: emitter.ABIVersion, ROLoads: cfg.ROLoads, RedZoneLeaf: cfg.RedZoneLeaf, BMIPeep: cfg.BMIPeep, TSOElide: cfg.TSOElide, AllowUnsupported: cfg.AllowUnsupported, PackedLayout: cfg.PackedLayout,
		AllSymbols: cfg.AllSymbols, CallModel: lifter.GetCallModel().String(), X87: cfg.X87, SimdLiveness: cfg.SimdLiveness, GPU: cfg.GPU,
		ExcludeModules: cfg.ExcludeModules, HLEMalloc: cfg.HLEMalloc, HLEReplace: absOrEmpty(cfg.HLEReplace), Patches: absOrEmpty(cfg.Patches),
		Translator: exe, TranslatorSHA256: exeSum,
		AOTFuncs: "aot_funcs.bin", OutDir: outAbs, LoweringID: emitter.LoweringID(),
		TSORelax: lifter.EnvTSORelax(), TSOStrict: lifter.EnvTSOStrict(), NoPeephole: !lifter.EnvPeephole(),
	}
	// Fragments are cached below the output directory unless -cache-dir names a shared place
	// (a title translated into several directories then shares its tier-1 fragments).
	info.CacheRoot = filepath.Join(outAbs, "tier1_cache")
	if cfg.CacheDir != "" {
		cd, err := filepath.Abs(cfg.CacheDir)
		if err != nil {
			return "", err
		}
		info.CacheRoot = filepath.Join(cd, "tier1")
	}
	mpath := filepath.Join(outAbs, "lifter_manifest.json")
	var m map[string]any
	if b, err := os.ReadFile(mpath); err == nil {
		if err := json.Unmarshal(b, &m); err != nil {
			return "", fmt.Errorf("lifter_manifest.json: %w", err)
		}
	}
	if m == nil {
		m = map[string]any{}
	}
	if cm, ok := m["call_model"].(string); ok && cm != "" { // what the emitter recorded
		info.CallModel = cm
	}
	m["tier1"] = info
	b, err := json.MarshalIndent(m, "", "  ")
	if err != nil {
		return "", err
	}
	if err := writeIfChanged(mpath, append(b, '\n')); err != nil {
		return "", err
	}
	c := fmt.Sprintf("// Generated by ps4-recomp: tier-1 miss fallback configuration (docs/m1/tier1.md).\n"+
		"const char nps5_tier1_manifest[] = %q;\n"+
		"const char nps5_tier1_translator[] = %q;\n"+
		"const char nps5_tier1_xver[] = %q;\n"+
		"const char nps5_tier1_elf_sha[] = %q;\n"+
		"const char nps5_tier1_outdir[] = %q;\n"+
		"const char nps5_tier1_call_model[] = %q;\n"+
		"const char nps5_tier1_cache_root[] = %q;\n",
		mpath, exe, tier1Version(&info), sum, outAbs, info.CallModel, info.CacheRoot)
	cpath := filepath.Join(outAbs, "nps5_tier1_info.c")
	if err := writeIfChanged(cpath, []byte(c)); err != nil {
		return "", err
	}
	registerTier1Fragments(cfg, &info, sum)
	return cpath, nil
}

// registerTier1Fragments lists the cached tier-1 fragments valid for this translation (same ELF,
// same lowering/configuration version) in the translation cache index, one "frag" row per guest
// address with key = sha256(elf sha, version, rip). A retranslation that keeps the version
// starts with all earlier misses already compiled; with -cache-dir pointing at a shared place
// this holds across output directories too.
func registerTier1Fragments(cfg *Config, info *tier1Info, elfSHA string) {
	if cfg.NoCache {
		return
	}
	ver := tier1Version(info)
	dir := filepath.Join(info.CacheRoot, fmt.Sprintf("%.16s-%.16s", elfSHA, ver))
	ents, err := os.ReadDir(dir)
	if err != nil {
		return
	}
	x := tcache.Open(cacheDirOf(cfg))
	keep := map[string]bool{}
	n := 0
	for _, e := range ents {
		name := e.Name()
		if !strings.HasPrefix(name, "f_") || !strings.HasSuffix(name, ".dylib") {
			continue
		}
		rip := strings.TrimSuffix(strings.TrimPrefix(name, "f_"), ".dylib")
		fi, err := e.Info()
		if err != nil {
			continue
		}
		if _, err := os.Stat(filepath.Join(dir, "f_"+rip+".meta")); err != nil {
			continue
		}
		a, _ := strconv.ParseUint(rip, 16, 64)
		keep[rip] = true
		x.Put(tcache.Row{Kind: "frag", ID: rip, Key: tcache.HashBytes([]byte(elfSHA + "|" + ver + "|" + rip)), First: a, Last: a,
			Lowering: info.LoweringID, ObjSize: fi.Size()})
		n++
	}
	x.Drop("frag", keep)
	_ = x.Save()
	if n > 0 {
		fmt.Fprintf(logw, "             Tier-1: %d cached fragment(s) valid for this translation (%s)\n", n, dir)
	}
}

// tier1Version names what a cached fragment depends on besides the title's ELF and the guest
// address: the lowering, the fragment ABI and the configuration the title was translated with.
// (Not the translator binary: a change that leaves the lowering alone keeps the cache.)
func tier1Version(i *tier1Info) string {
	h := sha256.Sum256([]byte(fmt.Sprintf("%d|%s|%s|%s|%t|%t|%t|%s|%s|%s|%s|%x|%t|ic=%t,%t,%t,%t|cold=%t|layout=%s|abi=%d|simd=%s", i.ABI, i.LoweringID, i.AddrMode, i.CallModel, i.TSO,
		i.AllowUnsupported, i.AllSymbols, i.X87, i.GPU, strings.Join(i.ExcludeModules, ","), i.Elf, i.TSORelax, i.NoPeephole,
		i.CallIC, i.FpCmpFuse, i.PushPopFuse, i.JmpIC, i.ColdOutline, i.LayoutHash, i.CodeABI, i.SimdLiveness)))
	if i.PackedLayout {
		// Only when set: the version of a title translated without it is what it was before the option existed.
		h = sha256.Sum256([]byte(fmt.Sprintf("%x|packed", h)))
	}
	if len(i.TSOStrict) != 0 {
		// Only for a non-empty list: the version of a title without one is what it was before the option existed.
		h = sha256.Sum256([]byte(fmt.Sprintf("%x|strict=%x", h, i.TSOStrict)))
	}
	if i.Patches != "" {
		// The profile's content: a fragment that holds a patch point depends on it.
		pb, _ := os.ReadFile(i.Patches)
		ph := sha256.Sum256(pb)
		h = sha256.Sum256(append(h[:], []byte("|patches="+hex.EncodeToString(ph[:]))...))
	}
	if i.TSOElide != "" && i.TSOElide != "off" {
		// Only a non-default lowering enters the hash: the caches of existing translations stay valid.
		h = sha256.Sum256(append(h[:], []byte("|elide="+i.TSOElide)...))
	}
	if i.ROLoads || i.RedZoneLeaf || i.BMIPeep {
		h = sha256.Sum256(append(h[:], []byte(fmt.Sprintf("|lift2=%t|%t|%t", i.ROLoads, i.RedZoneLeaf, i.BMIPeep))...))
	}
	return hex.EncodeToString(h[:])[:16]
}

func writeIfChanged(path string, data []byte) error {
	if old, err := os.ReadFile(path); err == nil && string(old) == string(data) {
		return nil
	}
	return os.WriteFile(path, data, 0o644)
}

// readTier1Seeds returns the guest entries that tier 1 compiled during earlier runs, or that a
// profile lists. src is a tier1_misses.jsonl file, a run bundle directory holding one, a runs
// directory (every bundle below it is read) or a `.seeds` file (seedsFileExt); several sources
// may be joined with commas. A miss-log seed counts when its miss was served (compiled, cache
// hit or waited for): failed misses are not code. The `elf` guards of `.seeds` files are not
// applied here (see readTier1SeedSet).
func readTier1Seeds(src string) ([]uint64, error) {
	seeds, _, err := readTier1SeedSet(src)
	return seeds, err
}

// seedsFileExt marks the hand-written seed list a title profile ships: one guest address per line,
// `#` starts a comment, and an optional `elf <sha256 prefix>` line names the input the addresses
// belong to. Misses of another input never become entry points.
const seedsFileExt = ".seeds"

// readTier1SeedSet is readTier1Seeds plus the `elf` guards (lower-case hex prefixes of the input's
// SHA-256) found in `.seeds` files.
func readTier1SeedSet(src string) (seeds []uint64, guards []string, err error) {
	seen := map[uint64]bool{}
	add := func(as []uint64) {
		for _, a := range as {
			if !seen[a] {
				seen[a] = true
				seeds = append(seeds, a)
			}
		}
	}
	for _, s := range strings.Split(src, ",") {
		s = strings.TrimSpace(s)
		if s == "" {
			continue
		}
		var files []string
		fi, err := os.Stat(s)
		if err != nil {
			return nil, nil, fmt.Errorf("-seeds-from: %w", err)
		}
		if !fi.IsDir() {
			if strings.HasSuffix(s, seedsFileExt) {
				as, gs, err := parseSeedsFile(s)
				if err != nil {
					return nil, nil, err
				}
				add(as)
				guards = append(guards, gs...)
				continue
			}
			files = []string{s}
		} else {
			if _, err := os.Stat(filepath.Join(s, "tier1_misses.jsonl")); err == nil {
				files = []string{filepath.Join(s, "tier1_misses.jsonl")}
			} else {
				ms, _ := filepath.Glob(filepath.Join(s, "*", "tier1_misses.jsonl"))
				files = ms
			}
		}
		for _, f := range files {
			as, err := parseMissLog(f)
			if err != nil {
				return nil, nil, err
			}
			add(as)
		}
	}
	sort.Slice(seeds, func(i, j int) bool { return seeds[i] < seeds[j] })
	return seeds, guards, nil
}

// parseSeedsFile reads a `.seeds` file (seedsFileExt): addresses in hex (0x optional, never 0) and
// at most the `elf` guards; anything else on a line is an error that names the line.
func parseSeedsFile(path string) (seeds []uint64, guards []string, err error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, nil, fmt.Errorf("-seeds-from: %w", err)
	}
	for n, line := range strings.Split(string(b), "\n") {
		if i := strings.IndexByte(line, '#'); i >= 0 {
			line = line[:i]
		}
		f := strings.Fields(line)
		switch {
		case len(f) == 0:
		case len(f) == 2 && f[0] == "elf":
			g := strings.ToLower(f[1])
			if _, err := hex.DecodeString(g + strings.Repeat("0", len(g)%2)); err != nil || len(g) < 8 || len(g) > 64 {
				return nil, nil, fmt.Errorf("-seeds-from: %s:%d: bad elf guard %q (want 8 to 64 hex digits)", path, n+1, f[1])
			}
			guards = append(guards, g)
		case len(f) == 1:
			a, err := strconv.ParseUint(strings.TrimPrefix(strings.TrimPrefix(f[0], "0x"), "0X"), 16, 64)
			if err != nil || a == 0 {
				return nil, nil, fmt.Errorf("-seeds-from: %s:%d: bad address %q", path, n+1, f[0])
			}
			seeds = append(seeds, a)
		default:
			return nil, nil, fmt.Errorf("-seeds-from: %s:%d: want one address or `elf <sha256 prefix>`, got %q", path, n+1, strings.TrimSpace(line))
		}
	}
	return seeds, guards, nil
}

// seedsForElf is the seeds of src that apply to the input at elfPath: when a `.seeds` file names
// an input (`elf` line) that this one is not, its addresses are dropped and skipped says why.
func seedsForElf(src, elfPath string) (seeds []uint64, skipped string, err error) {
	all, guards, err := readTier1SeedSet(src)
	if err != nil || len(guards) == 0 {
		return all, "", err
	}
	sum, err := sha256File(elfPath)
	if err != nil {
		return nil, "", err
	}
	for _, g := range guards {
		if !strings.HasPrefix(sum, g) {
			return nil, fmt.Sprintf("seeds name input %s..., this input is %s...", g[:8], sum[:8]), nil
		}
	}
	return all, "", nil
}

// MissRecord is one line of tier1_misses.jsonl (written by pkg/runtime/core/nps5_tier1.c).
type MissRecord struct {
	T          float64 `json:"t"`
	RIP        string  `json:"rip"`
	Module     string  `json:"module"`
	Offset     string  `json:"offset"`
	Reason     string  `json:"reason"`
	Outcome    string  `json:"outcome"`
	Kind       string  `json:"kind,omitempty"`
	CompileMs  float64 `json:"compile_ms"`
	FragBytes  int64   `json:"fragment_bytes"`
	Entries    int     `json:"entries"`
	Insts      int     `json:"insts"`
	Generation int     `json:"generation"`
	TID        int64   `json:"tid"`
	Detail     string  `json:"detail,omitempty"`
}

func parseMissLog(path string) ([]uint64, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, fmt.Errorf("-seeds-from: %w", err)
	}
	defer f.Close()
	var out []uint64
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 1<<16), 1<<22)
	for n := 1; sc.Scan(); n++ {
		line := strings.TrimSpace(sc.Text())
		if line == "" {
			continue
		}
		var r MissRecord
		if err := json.Unmarshal([]byte(line), &r); err != nil {
			return nil, fmt.Errorf("-seeds-from: %s:%d: %w", path, n, err)
		}
		switch r.Outcome {
		case "compiled", "cache_hit", "waited":
		default:
			continue
		}
		a, err := strconv.ParseUint(strings.TrimPrefix(r.RIP, "0x"), 16, 64)
		if err != nil || a == 0 {
			return nil, fmt.Errorf("-seeds-from: %s:%d: bad rip %q", path, n, r.RIP)
		}
		out = append(out, a)
	}
	return out, sc.Err()
}

func absOrEmpty(p string) string {
	if p == "" {
		return ""
	}
	if a, err := filepath.Abs(p); err == nil {
		return a
	}
	return p
}

// checkTier1Layout refuses a title whose manifest records another runtime layout or code ABI than
// this translator was built with: its fragments would bake in offsets the title does not have.
func checkTier1Layout(info *tier1Info) error {
	if info.LayoutHash != emitter.KeyLayoutHash() {
		return fmt.Errorf("this translator's runtime layout %s differs from the title's %q; retranslate", emitter.KeyLayoutHash(), info.LayoutHash)
	}
	if info.CodeABI != emitter.ABIVersion {
		return fmt.Errorf("this translator's code ABI %d differs from the title's %d; retranslate", emitter.ABIVersion, info.CodeABI)
	}
	return nil
}
