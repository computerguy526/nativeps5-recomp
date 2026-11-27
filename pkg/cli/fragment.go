package cli

// `ps4-recomp fragment`: the out-of-process translator of the tier-1 miss fallback
// (SPEC 3.4 / 3.10, docs/m1/tier1.md). It re-creates the guest layout of a finished translation
// from its lifter_manifest.json, lifts a bounded fragment starting at one or more guest
// addresses with the same lifter configuration, and assembles, links and ad-hoc signs a dylib
// whose descriptor the runtime registers. Exit status 0 = result.txt has ok=1; 2 = a structured
// failure (kind=... in result.txt); the runtime turns failures into crash.txt.

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"ps4-recomp/pkg/disasm"
	"ps4-recomp/pkg/elfloader"
	"ps4-recomp/pkg/emitter"
	"ps4-recomp/pkg/lifter"
)

// Failure kinds of a fragment (result.txt "kind", the runtime's miss-log "kind").
const (
	fragOutsideCode   = "outside_code"        // not an executable guest address of the title
	fragUndecodable   = "undecodable"         // no instruction decodes at the address
	fragUnsupported   = "unsupported"         // the fragment holds an instruction the lifter lacks
	fragCap           = "cap_exceeded"        // fragment larger than the caps
	fragCompile       = "compile_failed"      // clang / ld / codesign failed
	fragTimeout       = "timeout"             // compile exceeded the time cap
	fragMismatch      = "translator_mismatch" // this translator is not the one that built the title
	fragModuleChanged = "module_changed"      // the input ELF changed since the translation
	fragBadManifest   = "bad_manifest"
	fragInternal      = "internal"
)

type fragError struct {
	kind, msg string
}

func (e *fragError) Error() string { return e.kind + ": " + e.msg }

func ferr(kind, f string, a ...any) error { return &fragError{kind, fmt.Sprintf(f, a...)} }

type fragOpts struct {
	manifest string
	entries  []uint64
	out      string
	maxInsts int
	maxFuncs int
	timeout  time.Duration
}

type multiHex []uint64

func (m *multiHex) String() string { return fmt.Sprint([]uint64(*m)) }
func (m *multiHex) Set(s string) error {
	for _, p := range strings.Split(s, ",") {
		v, err := strconv.ParseUint(strings.TrimPrefix(strings.TrimSpace(p), "0x"), 16, 64)
		if err != nil {
			return fmt.Errorf("bad guest address %q (hex)", p)
		}
		*m = append(*m, v)
	}
	return nil
}

// fragmentCommand implements `ps4-recomp fragment`.
func fragmentCommand(args []string) error {
	fs := flag.NewFlagSet("fragment", flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	var o fragOpts
	var entries multiHex
	var timeoutMs int
	fs.StringVar(&o.manifest, "manifest", "", "lifter_manifest.json of the translation")
	fs.Var(&entries, "entry", "guest entry address (hex), repeatable or comma separated")
	fs.StringVar(&o.out, "out", "", "work/output directory (default: a new directory next to the manifest)")
	fs.IntVar(&o.maxInsts, "max-insts", 20000, "cap on guest instructions in the fragment")
	fs.IntVar(&o.maxFuncs, "max-funcs", 16, "cap on functions in the fragment")
	fs.IntVar(&timeoutMs, "timeout-ms", 120000, "cap on clang/link time")
	if err := fs.Parse(args); err != nil {
		return fmt.Errorf("fragment: %w\nusage: ps4-recomp fragment --manifest <lifter_manifest.json> --entry <hex> [--out dir] [--max-insts N] [--max-funcs N] [--timeout-ms N]", err)
	}
	o.entries = entries
	o.timeout = time.Duration(timeoutMs) * time.Millisecond
	if o.manifest == "" || len(o.entries) == 0 {
		return errors.New("fragment: --manifest and --entry are required")
	}
	if o.out == "" {
		d, err := os.MkdirTemp(filepath.Dir(o.manifest), "fragment-")
		if err != nil {
			return err
		}
		o.out = d
	}
	if err := os.MkdirAll(o.out, 0o755); err != nil {
		return err
	}
	res, err := runFragment(&o)
	return finishFragment(&o, res, err)
}

// finishFragment writes result.txt of a fragment build and returns the command's error.
func finishFragment(o *fragOpts, res *fragResult, err error) error {
	var b bytes.Buffer
	if err != nil {
		kind, msg := fragInternal, err.Error()
		var fe *fragError
		if errors.As(err, &fe) {
			kind, msg = fe.kind, fe.msg
		}
		fmt.Fprintf(&b, "ok=0\nkind=%s\nmessage=%s\n", kind, strings.ReplaceAll(msg, "\n", " | "))
		_ = os.WriteFile(filepath.Join(o.out, "result.txt"), b.Bytes(), 0o644)
		fmt.Fprintf(os.Stderr, "fragment failed: %s: %s\n", kind, msg)
		return &exitError{code: 2, err: err}
	}
	fmt.Fprintf(&b, "ok=1\ndylib=%s\nentries=", res.dylib)
	for i, a := range res.entries {
		if i > 0 {
			b.WriteByte(',')
		}
		fmt.Fprintf(&b, "0x%x", a)
	}
	fmt.Fprintf(&b, "\nfuncs=%d\ninsts=%d\nasm_bytes=%d\ndylib_bytes=%d\nms_translate=%d\nms_compile=%d\n",
		len(res.entries), res.insts, res.asmBytes, res.dylibBytes, res.msTranslate, res.msCompile)
	_ = os.WriteFile(filepath.Join(o.out, "result.txt"), b.Bytes(), 0o644)
	fmt.Fprintf(stdout, "fragment ok: %d functions, %d instructions, %s\n", len(res.entries), res.insts, res.dylib)
	return nil
}

// exitError makes Execute's caller exit with a specific status.
type exitError struct {
	code int
	err  error
}

func (e *exitError) Error() string { return e.err.Error() }
func (e *exitError) Unwrap() error { return e.err }

// ExitCode reports the process exit status an error asks for (1 when it names none).
func ExitCode(err error) int {
	var ee *exitError
	if errors.As(err, &ee) {
		return ee.code
	}
	return 1
}

type fragResult struct {
	dylib                string
	entries              []uint64
	insts                int
	asmBytes, dylibBytes int64
	msTranslate          int64
	msCompile            int64
}

// missingBuildMessage is what the player reads when a lazily translated function is needed and the
// build folder holding the lifter manifest is gone (moved, renamed or cleaned up).
func missingBuildMessage(manifest string) string {
	return fmt.Sprintf("the build folder is missing (%s not found): the game needs it to translate code it reaches for the first time; rebuild with nps5 build", manifest)
}

func readTier1Manifest(path string) (*tier1Info, error) {
	b, err := os.ReadFile(path)
	if errors.Is(err, fs.ErrNotExist) {
		return nil, ferr(fragBadManifest, "%s", missingBuildMessage(path))
	}
	if err != nil {
		return nil, ferr(fragBadManifest, "%v", err)
	}
	var m struct {
		Tier1 *tier1Info `json:"tier1"`
	}
	if err := json.Unmarshal(b, &m); err != nil {
		return nil, ferr(fragBadManifest, "%s: %v", path, err)
	}
	if m.Tier1 == nil {
		return nil, ferr(fragBadManifest, "%s has no tier1 section (translated before tier 1, or by another tool)", path)
	}
	if m.Tier1.ABI != Tier1ABI {
		return nil, ferr(fragMismatch, "manifest tier1 abi %d, translator abi %d", m.Tier1.ABI, Tier1ABI)
	}
	return m.Tier1, nil
}

// fragEnv is the loaded title a fragment is lifted against: the guest layout of the translation,
// re-created from its manifest. It is read only after prepareFragmentEnv, so a server builds
// fragments from one env concurrently (each build clones the decoder).
type fragEnv struct {
	info     *tier1Info
	manifest string
	loaded   *elfloader.LoadedELF
	d        *disasm.Disassembler
	aot      map[uint64]bool
	nsMu     sync.Mutex // NewAsmEmitter is not known to be concurrency safe
	loadedMs int64
}

func prepareFragmentEnv(manifest string) (*fragEnv, error) {
	t0 := time.Now()
	logw = io.Discard // the title load narrates to logw; only the result is of interest here
	info, err := readTier1Manifest(manifest)
	if err != nil {
		return nil, err
	}
	// The fragment must be lifted by the translator that built the title: a different lifter
	// would emit code for a different state/stack contract.
	if info.LoweringID != "" {
		if id := emitter.LoweringID(); id != info.LoweringID {
			return nil, ferr(fragMismatch, "this translator's lowering %s differs from the title's %s; retranslate", id, info.LoweringID)
		}
	} else if exe, err := os.Executable(); err == nil {
		if r, e2 := filepath.EvalSymlinks(exe); e2 == nil {
			exe = r
		}
		sum, err := sha256File(exe)
		if err != nil {
			return nil, ferr(fragInternal, "hash translator: %v", err)
		}
		if sum != info.TranslatorSHA256 {
			return nil, ferr(fragMismatch, "translator %s differs from the one that built the title (%s); retranslate", exe, info.Translator)
		}
	}
	// Cache identity: a fragment links into the title and bakes in its runtime layout. A manifest
	// without the fields predates them and cannot be trusted either (docs/m3/coldpath.md section 3).
	if err := checkTier1Layout(info); err != nil {
		return nil, ferr(fragMismatch, "%v", err)
	}
	// The fragment is lifted in the call model of the title; only a model this translator does
	// not know (or a lifter that is not in that model) is a mismatch.
	callModel, err := lifter.ParseCallModel(info.CallModel)
	if err != nil {
		return nil, ferr(fragMismatch, "call model %q is not supported by this translator", info.CallModel)
	}
	lifter.SetCallModel(callModel)
	lifter.SetCallIC(info.CallIC)
	lifter.SetFpCmpFuse(info.FpCmpFuse)
	lifter.SetPushPopFuse(info.PushPopFuse)
	lifter.SetJmpIC(info.JmpIC)
	lifter.SetROLoads(info.ROLoads)
	lifter.SetRedZoneLeaf(info.RedZoneLeaf)
	lifter.SetBMIPeep(info.BMIPeep)
	lifter.SetColdOutline(info.ColdOutline)
	disasm.SetPackedLayout(info.PackedLayout) // the title's discovery rules (entries behind ud2/jmp, fall-into kept)
	if fi, err := os.Stat(info.Elf); err != nil {
		return nil, ferr(fragModuleChanged, "input ELF: %v", err)
	} else if fi.Size() != info.ElfSize || fi.ModTime().UnixNano() != info.ElfMtimeNs {
		return nil, ferr(fragModuleChanged, "input ELF %s changed since the translation (size/mtime)", info.Elf)
	}

	// Same sequence as Execute steps 1 and 2: the guest layout must come out identical.
	cfg := &Config{ElfPath: info.Elf, AppDir: info.AppDir, AddrMode: info.AddrMode, TSO: info.TSO, TSOElide: info.TSOElide,
		AllowUnsupported: info.AllowUnsupported, AllSymbols: info.AllSymbols, X87: info.X87, GPU: info.GPU,
		ExcludeModules: info.ExcludeModules}
	if err := setGPU(cfg.GPU); err != nil {
		return nil, ferr(fragBadManifest, "%v", err)
	}
	loaded, err := elfloader.LoadELF(cfg.ElfPath)
	if err != nil {
		return nil, ferr(fragInternal, "load ELF: %v", err)
	}
	plan, err := planGuestMemory(cfg, loaded)
	if err != nil {
		return nil, ferr(fragInternal, "memory plan: %v", err)
	}
	if cfg.AddrMode != info.AddrMode {
		return nil, ferr(fragMismatch, "address mode resolves to %s, the title was built with %s", cfg.AddrMode, info.AddrMode)
	}
	elfloader.AddTLSModule(loaded, loaded)
	modules, err := loadCompanionModules(cfg, loaded, plan.layout)
	if err != nil {
		return nil, ferr(fragInternal, "modules: %v", err)
	}
	var exports []elfloader.Symbol
	for _, m := range modules {
		exports = append(exports, m.Exports...)
	}
	if len(cfg.ExcludeModules) > 0 {
		loaded.ClearExcluded(exports)
	}
	elfloader.LayoutStaticTLS(loaded.TLSModules)
	elfloader.ApplyTLSOffsets(loaded)
	if err := elfloader.ResolveModuleRelocations(loaded, exports); err != nil {
		return nil, ferr(fragInternal, "relocations: %v", err)
	}
	_ = emitter.NewHLEReport(loaded, modules)
	d, err := disasm.NewDisassembler(loaded)
	if err != nil {
		return nil, ferr(fragInternal, "disassembler: %v", err)
	}
	vecLive, okVec := disasm.ParseVecLiveMode(info.SimdLiveness)
	if !okVec {
		return nil, ferr(fragBadManifest, "simd_liveness %q", info.SimdLiveness)
	}
	d.VecLive = vecLive
	d.ROLoads = info.ROLoads

	aot := map[uint64]bool{}
	if info.AOTFuncs != "" {
		if b, err := os.ReadFile(filepath.Join(filepath.Dir(manifest), info.AOTFuncs)); err == nil {
			for i := 0; i+8 <= len(b); i += 8 {
				aot[binary.LittleEndian.Uint64(b[i:])] = true
			}
		}
	}

	return &fragEnv{info: info, manifest: manifest, loaded: loaded, d: d, aot: aot, loadedMs: time.Since(t0).Milliseconds()}, nil
}

func runFragment(o *fragOpts) (*fragResult, error) {
	env, err := prepareFragmentEnv(o.manifest)
	if err != nil {
		return nil, err
	}
	return env.build(o)
}

// build lifts and builds the fragment for o.entries.
func (env *fragEnv) build(o *fragOpts) (*fragResult, error) {
	t0 := time.Now()
	info, loaded, aot := env.info, env.loaded, env.aot
	d := env.d.CloneForEmit()
	// Bounded fragment: the function containing each entry, plus the not yet translated
	// functions it directly reaches, until a cap. Everything else goes through dispatch (and,
	// if it too is missing, another miss).
	var addrs []uint64
	seen := map[uint64]bool{}
	var queue []uint64
	for _, e := range o.entries {
		if !d.InCode(e) {
			return nil, ferr(fragOutsideCode, "0x%x is not in executable guest code of %s", e, filepath.Base(info.Elf))
		}
		if !seen[e] {
			seen[e] = true
			queue = append(queue, e)
		}
	}
	nInsts := 0
	roots := len(queue)
	for head := 0; head < len(queue); head++ {
		a := queue[head]
		root := head < roots
		fn, calls, derr := d.DisasmFunction(a)
		n := 0
		if fn != nil {
			for _, b := range fn.Blocks {
				n += len(b.Insts)
			}
		}
		if derr != nil || fn == nil || n == 0 {
			if root {
				msg := "no instruction decodes"
				if derr != nil {
					msg = derr.Error()
				}
				return nil, ferr(fragUndecodable, "0x%x: %s", a, msg)
			}
			continue
		}
		if !root && (nInsts+n > o.maxInsts || len(addrs) >= o.maxFuncs) {
			continue
		}
		if root && n > o.maxInsts*4 {
			return nil, ferr(fragCap, "function 0x%x has %d instructions (cap %d)", a, n, o.maxInsts*4)
		}
		addrs = append(addrs, a)
		nInsts += n
		for _, c := range calls {
			if !seen[c] && !aot[c] && d.InCode(c) {
				seen[c] = true
				queue = append(queue, c)
			}
		}
	}
	known := make(map[uint64]bool, len(addrs))
	for _, a := range addrs {
		known[a] = true
	}
	mode, err := lifter.ParseAddrMode(info.AddrMode)
	if err != nil {
		return nil, ferr(fragBadManifest, "%v", err)
	}
	l := lifter.NewArm64LifterMode(known, mode)
	l.SetTSO(info.TSO)
	// the translation's relax list and peephole setting, not this process's environment (fragments must agree with the title's lowering)
	l.SetTSORelax(info.TSORelax)
	if len(info.TSOStrict) != 0 {
		l.SetTSOStrict(info.TSOStrict)
	}
	l.SetPeephole(!info.NoPeephole)
	elide, err := lifter.ParseTSOElide(info.TSOElide)
	if err != nil {
		return nil, ferr(fragBadManifest, "%v", err)
	}
	l.SetTSOElide(elide)
	if info.X87 == "" {
		info.X87 = "f80"
	}
	if err := l.SetX87Mode(info.X87); err != nil {
		return nil, ferr(fragBadManifest, "%v", err)
	}
	l.SetAllowUnsupported(info.AllowUnsupported)
	emitter.ShimStubViaGOT = true
	env.nsMu.Lock()
	if vl, ok := disasm.ParseVecLiveMode(info.SimdLiveness); ok {
		l.SetVecLiveness(vl != disasm.VecLiveOff)
	}
	em := emitter.NewAsmEmitter(loaded, d, l)
	env.nsMu.Unlock()
	if err := applyFuncRepl(em, info.HLEMalloc, info.HLEReplace, true, io.Discard); err != nil {
		return nil, ferr(fragBadManifest, "%v", err)
	}
	if err := applyPatches(em, info.Patches, io.Discard); err != nil {
		return nil, ferr(fragBadManifest, "%v", err)
	}
	asmPath, sorted, err := em.EmitFragment(o.out, addrs)
	if err != nil {
		if strings.Contains(err.Error(), "UNSUPPORTED") || strings.Contains(err.Error(), "unsupported") {
			return nil, ferr(fragUnsupported, "%s", firstLines(err.Error(), 6))
		}
		return nil, ferr(fragInternal, "%v", err)
	}
	res := &fragResult{entries: sorted, insts: nInsts, msTranslate: time.Since(t0).Milliseconds()}
	if fi, err := os.Stat(asmPath); err == nil {
		res.asmBytes = fi.Size()
	}
	t1 := time.Now()
	dylib, err := buildFragmentDylib(o, info, asmPath, sorted)
	res.msCompile = time.Since(t1).Milliseconds()
	if err != nil {
		return nil, err
	}
	res.dylib = dylib
	if fi, err := os.Stat(dylib); err == nil {
		res.dylibBytes = fi.Size()
	}
	return res, nil
}

func firstLines(s string, n int) string {
	ls := strings.Split(s, "\n")
	if len(ls) > n {
		ls = ls[:n]
	}
	return strings.Join(ls, " | ")
}

// fragDescC is the dylib's descriptor: the runtime dlopens the library, dlsyms
// nps5_frag_desc and registers the entries (and the pc map) itself.
func fragDescC(entries []uint64) string {
	var b strings.Builder
	b.WriteString("// Tier-1 fragment descriptor (docs/m1/tier1.md); generated by ps4-recomp fragment.\n")
	b.WriteString("#include <stdint.h>\n#include <stddef.h>\n")
	for _, a := range entries {
		fmt.Fprintf(&b, "extern void fn_0x%x(void *ctx);\n", a)
	}
	b.WriteString("extern const uint8_t nps5_frag_pcmap_start[] __asm(\"section$start$__DATA$__nps5_pcmap\");\n")
	b.WriteString("extern const uint8_t nps5_frag_pcmap_end[] __asm(\"section$end$__DATA$__nps5_pcmap\");\n")
	b.WriteString("extern const uint64_t nps5_frag_pads_start[] __asm(\"section$start$__DATA$__nps5_pads\");\n")
	b.WriteString("extern const uint64_t nps5_frag_pads_end[] __asm(\"section$end$__DATA$__nps5_pads\");\n")
	b.WriteString("// keep both sections present when the fragment has no pc map / landing pads\n")
	b.WriteString("__attribute__((used, section(\"__DATA,__nps5_pcmap\"))) static const uint64_t s_pcmap_sentinel[4] = {0, 0, 0, 0}; /* an empty 32-byte pc-map record */\n")
	b.WriteString("__attribute__((used, section(\"__DATA,__nps5_pads\"))) static const uint64_t s_pads_sentinel[2] = {~0ull, 0};\n")
	b.WriteString("typedef struct { uint64_t guest; void (*fn)(void *); } NPS5FragEntry;\n")
	fmt.Fprintf(&b, "typedef struct { uint64_t magic; uint32_t abi; uint32_t n; const uint8_t *(*pcmap)(size_t *); const uint64_t *(*pads)(size_t *); NPS5FragEntry e[%d]; } NPS5FragDesc;\n", len(entries))
	b.WriteString("static const uint8_t *get_pcmap(size_t *n) { *n = (size_t)(nps5_frag_pcmap_end - nps5_frag_pcmap_start); return nps5_frag_pcmap_start; }\n")
	b.WriteString("static const uint64_t *get_pads(size_t *n) { *n = (size_t)(nps5_frag_pads_end - nps5_frag_pads_start) / 2; return nps5_frag_pads_start; }\n")
	fmt.Fprintf(&b, "__attribute__((visibility(\"default\"))) const NPS5FragDesc nps5_frag_desc = { 0x3147524650535035ull, %d, %d, get_pcmap, get_pads, {\n", Tier1ABI, len(entries))
	for _, a := range entries {
		fmt.Fprintf(&b, "  { 0x%xull, fn_0x%x },\n", a, a)
	}
	b.WriteString("} };\n")
	return b.String()
}

// findTool resolves a developer tool (clang, codesign) from PATH, then from the usual place.
func findTool(name string) string {
	if p, err := exec.LookPath(name); err == nil {
		return p
	}
	return filepath.Join("/usr/bin", name)
}

func runTool(ctx context.Context, dir string, argv ...string) ([]byte, error) {
	cmd := exec.CommandContext(ctx, argv[0], argv[1:]...)
	cmd.Dir = dir
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	cmd.Cancel = func() error { return syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL) }
	cmd.WaitDelay = 2 * time.Second
	out, err := cmd.CombinedOutput()
	return out, err
}

// buildFragmentDylib assembles the lifted code and the descriptor and links, then ad-hoc signs
// frag.dylib in o.out. Undefined symbols (the dispatch trampolines, runtime helpers) are bound
// at dlopen from the executable (-bind_at_load turns a missing one into a dlopen error).
func buildFragmentDylib(o *fragOpts, info *tier1Info, asmPath string, entries []uint64) (string, error) {
	descPath := filepath.Join(o.out, "frag_desc.c")
	if err := os.WriteFile(descPath, []byte(fragDescC(entries)), 0o644); err != nil {
		return "", ferr(fragInternal, "%v", err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), o.timeout)
	defer cancel()
	clang := findTool("clang")
	asm := asmTool()
	base := []string{"-target", "arm64-apple-darwin", "-mcpu=apple-m1"}
	cflags := append(append([]string{}, base...), "-O1", "-ffp-contract=off", "-fvisibility=hidden", "-DRECOMP_ARM64_ASM=1")
	if info.AddrMode == "identity" {
		cflags = append(cflags, "-DNPS5_ADDR_MODE_IDENTITY=1")
	}
	fail := func(what string, out []byte, err error) error {
		kind := fragCompile
		if ctx.Err() != nil {
			kind = fragTimeout
		}
		return ferr(kind, "%s: %v: %s", what, err, firstLines(strings.TrimSpace(string(out)), 12))
	}
	objS := filepath.Join(o.out, "code_000.o")
	objC := filepath.Join(o.out, "frag_desc.o")
	var wg sync.WaitGroup
	var outS, outC []byte
	var errS, errC error
	wg.Add(2)
	go func() {
		defer wg.Done()
		outS, errS = runTool(ctx, o.out, append(append([]string{asm}, cflags...), "-c", asmPath, "-o", objS)...)
	}()
	go func() {
		defer wg.Done()
		outC, errC = runTool(ctx, o.out, append(append([]string{clang}, cflags...), "-c", descPath, "-o", objC)...)
	}()
	wg.Wait()
	if errS != nil {
		return "", fail("assemble", outS, errS)
	}
	if errC != nil {
		return "", fail("compile descriptor", outC, errC)
	}
	dylib := filepath.Join(o.out, "frag.dylib")
	link := append(append([]string{clang}, base...), "-dynamiclib", "-undefined", "dynamic_lookup", "-Wl,-bind_at_load",
		"-Wl,-install_name,@rpath/frag.dylib", "-Wl,-adhoc_codesign", objS, objC, "-o", dylib)
	if out, err := runTool(ctx, o.out, link...); err != nil {
		return "", fail("link", out, err)
	}
	// ld64 signs an arm64 image ad hoc itself (-adhoc_codesign, the default): no separate codesign run.
	_ = os.Remove(objS)
	_ = os.Remove(objC)
	return dylib, nil
}
