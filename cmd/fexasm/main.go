// fexasm: instruction-level differential suite driven by the FEX-Emu ASM unit tests (MIT).
//
// For each unittests/ASM/**/*.asm test the tool assembles the source with nasm into a flat
// binary, wraps it in a minimal x86-64 ELF, lifts it with this repository's translator
// (pkg/disasm + pkg/lifter, as ps4-recomp does), assembles the ARM64 output with clang, links it
// with a small host harness (harness/fex_main.c) and runs the result (arm64 only, through
// tools/run_sandboxed.sh). The final registers are compared with the test's "RegData". No x86 code
// is ever executed; the FEX tests are only assembled and lifted.
//
// Usage: fexasm -fex <FEX clone>/unittests/ASM -work DIR -out results.json [-filter RE] [-j N]
//
//	[-timeout S] [-baseline old.json] [-md report.md]
package main

import (
	"bytes"
	"context"
	_ "embed"
	"encoding/json"
	"flag"
	"fmt"
	"math/big"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"ps4-recomp/pkg/emitter"
	"ps4-recomp/pkg/lifter"
)

//go:embed harness/fex_main.c
var harnessC string

// Result is one test's outcome (the schema of results.json).
type Result struct {
	Name      string   `json:"name"`
	Dir       string   `json:"dir"`
	Status    string   `json:"status"` // PASS, FAIL, SKIP
	Reason    string   `json:"reason,omitempty"`
	Detail    []string `json:"detail,omitempty"`    // mismatches / failure text
	Forms     []string `json:"forms,omitempty"`     // unsupported instruction forms
	Mnemonics []string `json:"mnemonics,omitempty"` // non-setup mnemonics in the test
}

type header struct {
	RegData  map[string]any    `json:"RegData"`
	Regions  map[string]string `json:"MemoryRegions"`
	MemData  map[string]string `json:"MemoryData"`
	Features []string          `json:"HostFeatures"`
	Ignore   []string          `json:"Ignore"`
	Mode     string            `json:"Mode"`
	Env      map[string]string `json:"Env"`
}

var (
	flFex       = flag.String("fex", "", "path to FEX unittests/ASM")
	flWork      = flag.String("work", "tests/fex/work", "work directory")
	flOut       = flag.String("out", "tests/fex/results.json", "results file")
	flFilter    = flag.String("filter", "", "regexp on the test path relative to unittests/ASM")
	flJ         = flag.Int("j", runtime.NumCPU(), "parallel tests")
	flTimeout   = flag.Int("timeout", 20, "seconds per test run")
	flBaseline  = flag.String("baseline", "", "previous results.json to diff against")
	flMD        = flag.String("md", "", "write the markdown report here")
	flTSO       = flag.Bool("tso", true, "lift with x86 memory ordering (ps4-recomp default)")
	flTSOElide  = flag.String("tso-elide", "stack", "stack accesses without ordering: stack (default) or off (ps4-recomp -tso-elide)")
	flX87       = flag.String("x87", "auto", "x87 model: f80, f64, or auto (f64 for tests with Env FEX_X87REDUCEDPRECISION=1, f80 otherwise)")
	flRoot      = flag.String("root", "", "repo root (default: working directory)")
	flKeep      = flag.Bool("keep", false, "keep per-test work files")
	flList      = flag.Bool("list", false, "list the selected tests and exit")
	flFexRev    = flag.String("fexrev", "", "FEX commit to record in the report")
	flCold      = flag.Bool("cold-outline", true, "outline the rare TSO paths of the lowering to the end of each function (the translator default)")
	flCallModel = flag.String("call-model", "nested", "call/return model of the lifted code: nested or flat")
	flCallIC    = flag.Bool("call-ic", false, "emit call-site inline caches (flat model; the harness miss path forwards to the plain indirect call)")
	flSimdLive  = flag.String("simd-liveness", "strict", "vector lane liveness: strict (default here: the tests follow no calling convention and compare every register after the final ret), abi or off")
)

const codeBase = 0x10000

// Host features a PS5 (Zen 2) CPU lacks, or that are host-specific.
var skipFeatures = map[string]bool{
	"AVX_NE_CONVERT": true, "AVX_VNNI": true, "AVX_VNNI_INT8": true, "AVX_VNNI_INT16": true,
	"AVX_IFMA": true, "AES256": true, "SHA512": true, "3DNOW": true, "EMMI": true, "AFP": true,
	"SERIALIZE": true, "RAND": true, "Linux": true, "FSGSBASE": true,
}

// Setup-style mnemonics ignored when ranking failing instructions.
var setupOps = map[string]bool{"MOV": true, "LEA": true, "HLT": true, "RET": true, "NOP": true, "MOVABS": true,
	"CALL": true, "JMP": true, "POP": true, "PUSH": true}

var (
	reHeader = regexp.MustCompile(`(?s)%ifdef CONFIG\s*(\{.*?\})\s*%endif`)
	reComma  = regexp.MustCompile(`,(\s*[}\]])`)
	reSys    = regexp.MustCompile(`(?mi)^\s*(syscall|sysenter|int\s+0x?[0-9a-f]+|cpuid|rdtsc|rdtscp|xgetbv|rdrand|rdseed)\b`)
	reBits32 = regexp.MustCompile(`(?mi)^\s*\[?bits\s+32\]?|^\s*use32`)
	reTword  = regexp.MustCompile(`(?i)\btword\b|\bfbstp\b|\bfbld\b`)
)

func main() {
	flag.Parse()
	if *flFex == "" {
		fmt.Fprintln(os.Stderr, "fexasm: -fex is required")
		os.Exit(2)
	}
	root := *flRoot
	if root == "" {
		root, _ = os.Getwd()
	}
	root, _ = filepath.Abs(root)
	work, _ := filepath.Abs(*flWork)
	fex, _ := filepath.Abs(*flFex)
	os.MkdirAll(work, 0o755)

	var filt *regexp.Regexp
	if *flFilter != "" {
		filt = regexp.MustCompile(*flFilter)
	}
	var tests []string
	filepath.WalkDir(fex, func(p string, d os.DirEntry, err error) error {
		if err == nil && !d.IsDir() && strings.HasSuffix(p, ".asm") {
			rel, _ := filepath.Rel(fex, p)
			if strings.HasPrefix(rel, "Includes/") || (filt != nil && !filt.MatchString(rel)) {
				return nil
			}
			tests = append(tests, rel)
		}
		return nil
	})
	sort.Strings(tests)
	if *flList {
		for _, t := range tests {
			fmt.Println(t)
		}
		return
	}

	rt := &runner{root: root, fex: fex, work: work}
	if err := rt.buildHarness(); err != nil {
		fmt.Fprintln(os.Stderr, "fexasm: harness:", err)
		os.Exit(1)
	}

	cm, err := lifter.ParseCallModel(*flCallModel)
	if err != nil {
		fmt.Fprintln(os.Stderr, "fexasm:", err)
		os.Exit(1)
	}
	lifter.SetCallModel(cm)
	lifter.SetCallIC(*flCallIC)
	lifter.SetColdOutline(*flCold)

	results := make([]Result, len(tests))
	var wg sync.WaitGroup
	sem := make(chan struct{}, *flJ)
	for i, t := range tests {
		wg.Add(1)
		sem <- struct{}{}
		go func(i int, t string) {
			defer wg.Done()
			defer func() { <-sem }()
			results[i] = rt.process(t)
		}(i, t)
	}
	wg.Wait()
	sort.Slice(results, func(i, j int) bool { return results[i].Name < results[j].Name })

	var base map[string]Result
	if *flBaseline != "" {
		base = loadResults(*flBaseline)
	}
	if filt != nil && *flOut != "" {
		// A filtered run updates only its own entries of an existing results file.
		old := loadResults(*flOut)
		for _, r := range results {
			old[r.Name] = r
		}
		results = results[:0]
		for _, r := range old {
			results = append(results, r)
		}
		sort.Slice(results, func(i, j int) bool { return results[i].Name < results[j].Name })
	}
	if *flOut != "" {
		os.MkdirAll(filepath.Dir(*flOut), 0o755)
		b, _ := json.MarshalIndent(results, "", " ")
		os.WriteFile(*flOut, append(b, '\n'), 0o644)
	}
	summarize(results, base, *flMD)
}

func loadResults(p string) map[string]Result {
	m := map[string]Result{}
	b, err := os.ReadFile(p)
	if err != nil {
		return m
	}
	var rs []Result
	if json.Unmarshal(b, &rs) == nil {
		for _, r := range rs {
			m[r.Name] = r
		}
	}
	return m
}

type runner struct {
	root, fex, work string
	harnessO        string
	x87Lib          string // the x87=f80 runtime (SoftFloat, nps5_x87.c, the bridge), linked into f80 tests
	helperLib       string // mid-instruction runtime helpers (nps5_pcmpstr.c and its bridge), linked into every test
}

func (rt *runner) buildHarness() error {
	dir := filepath.Join(rt.work, "harness")
	os.MkdirAll(dir, 0o755)
	src := filepath.Join(dir, "fex_main.c")
	os.WriteFile(src, []byte(harnessC), 0o644)
	rt.harnessO = filepath.Join(dir, "fex_main.o")
	out, err := exec.Command("clang", "-arch", "arm64", "-O1", "-w", "-I"+filepath.Join(rt.root, "pkg/runtime/core"),
		"-c", src, "-o", rt.harnessO).CombinedOutput()
	if err != nil {
		return fmt.Errorf("%v\n%s", err, out)
	}
	return rt.buildX87Runtime(dir)
}

// buildX87Runtime compiles the x87=f80 runtime into one static library: the SoftFloat subset and
// nps5_x87.c of pkg/runtime/core, and the bridge the lifter calls (emitter.X87BridgeAsm).
func (rt *runner) buildX87Runtime(dir string) error {
	odir := filepath.Join(dir, "x87o")
	os.MkdirAll(odir, 0o755)
	core := filepath.Join(rt.root, "pkg/runtime/core")
	srcs, _ := filepath.Glob(filepath.Join(core, "softfloat", "*.c"))
	srcs = append(srcs, filepath.Join(core, "nps5_x87.c"))
	bridge := filepath.Join(odir, "bridge.s")
	os.WriteFile(bridge, []byte(emitter.X87BridgeSource(lifter.GetCallModel())), 0o644)
	srcs = append(srcs, bridge)
	objs := make([]string, len(srcs))
	errs := make([]error, len(srcs))
	var wg sync.WaitGroup
	sem := make(chan struct{}, *flJ)
	for i, src := range srcs {
		objs[i] = filepath.Join(odir, strings.TrimSuffix(filepath.Base(src), filepath.Ext(src))+".o")
		wg.Add(1)
		sem <- struct{}{}
		go func(i int, src string) {
			defer wg.Done()
			defer func() { <-sem }()
			out, err := exec.Command("clang", "-arch", "arm64", "-O2", "-w", "-ffp-contract=off",
				"-I"+core, "-I"+filepath.Join(core, "softfloat"), "-c", src, "-o", objs[i]).CombinedOutput()
			if err != nil {
				errs[i] = fmt.Errorf("%s: %v\n%s", src, err, out)
			}
		}(i, src)
	}
	wg.Wait()
	for _, err := range errs {
		if err != nil {
			return err
		}
	}
	rt.x87Lib = filepath.Join(dir, "x87rt.a")
	os.Remove(rt.x87Lib)
	if out, err := exec.Command("ar", append([]string{"crs", rt.x87Lib}, objs...)...).CombinedOutput(); err != nil {
		return fmt.Errorf("ar: %v\n%s", err, out)
	}
	return rt.buildHelperRuntime(dir)
}

// pcmpstrBridge is the harness's stand-in for the trampoline entries _recomp_vpcmpistri and
// _recomp_vpcmpe_stri (pkg/emitter/asm_trampoline.go): C signature (ctx, xmm2/m128, xmm1, imm8),
// every vector register preserved. The lifter itself saves r8-r15 around the call, stores
// rax/rdx to ctx for the explicit-length form and reads rcx and the flags back from ctx.
func pcmpstrBridge() string {
	var b strings.Builder
	b.WriteString(".text\n.p2align 2\n")
	for _, n := range []string{"_recomp_vpcmpistri", "_recomp_vpcmpe_stri"} {
		fmt.Fprintf(&b, ".globl %s\n%s:\n    stp  x29, x30, [sp, #-16]!\n    sub  sp, sp, #512\n", n, n)
		for i := 0; i < 32; i += 2 {
			fmt.Fprintf(&b, "    stp  q%d, q%d, [sp, #%d]\n", i, i+1, i*16)
		}
		fmt.Fprintf(&b, "    bl   %s_impl\n", n)
		for i := 0; i < 32; i += 2 {
			fmt.Fprintf(&b, "    ldp  q%d, q%d, [sp, #%d]\n", i, i+1, i*16)
		}
		b.WriteString("    add  sp, sp, #512\n    ldp  x29, x30, [sp], #16\n    ret\n")
	}
	return b.String()
}

// buildHelperRuntime compiles the runtime helpers the lifter calls inside one instruction and
// that need nothing but recomp_context.h (the SSE4.2 string compares, nps5_pcmpstr.c), with the
// bridge above, into a static library every test links.
func (rt *runner) buildHelperRuntime(dir string) error {
	core := filepath.Join(rt.root, "pkg/runtime/core")
	bridge := filepath.Join(dir, "pcmpstr_bridge.s")
	os.WriteFile(bridge, []byte(pcmpstrBridge()), 0o644)
	var objs []string
	for _, src := range []string{filepath.Join(core, "nps5_pcmpstr.c"), bridge} {
		o := filepath.Join(dir, strings.TrimSuffix(filepath.Base(src), filepath.Ext(src))+".o")
		if out, err := exec.Command("clang", "-arch", "arm64", "-O2", "-w", "-I"+core, "-c", src, "-o", o).CombinedOutput(); err != nil {
			return fmt.Errorf("%s: %v\n%s", src, err, out)
		}
		objs = append(objs, o)
	}
	rt.helperLib = filepath.Join(dir, "helpers.a")
	os.Remove(rt.helperLib)
	if out, err := exec.Command("ar", append([]string{"crs", rt.helperLib}, objs...)...).CombinedOutput(); err != nil {
		return fmt.Errorf("ar: %v\n%s", err, out)
	}
	return nil
}

func skip(r *Result, reason string, detail ...string) Result {
	r.Status, r.Reason, r.Detail = "SKIP", reason, detail
	return *r
}

func fail(r *Result, reason string, detail ...string) Result {
	r.Status, r.Reason, r.Detail = "FAIL", reason, detail
	return *r
}

func (rt *runner) process(rel string) (res Result) {
	res = Result{Name: rel, Dir: topDir(rel)}
	defer func() {
		if e := recover(); e != nil {
			res = fail(&res, "translator-panic", fmt.Sprint(e))
		}
	}()
	srcPath := filepath.Join(rt.fex, rel)
	srcB, err := os.ReadFile(srcPath)
	if err != nil {
		return fail(&res, "read", err.Error())
	}
	src := string(srcB)
	var hd header
	if m := reHeader.FindStringSubmatch(src); m != nil {
		js := reComma.ReplaceAllString(m[1], "$1")
		if err := json.Unmarshal([]byte(js), &hd); err != nil {
			return skip(&res, "bad-header", err.Error())
		}
	}
	switch {
	case hd.Mode != "" && strings.Contains(hd.Mode, "32") || reBits32.MatchString(src):
		return skip(&res, "32bit")
	case strings.HasPrefix(rel, "SelfModifyingCode/"):
		return skip(&res, "self-modifying-code")
	case strings.HasPrefix(rel, "Disabled_Tests") || strings.HasPrefix(rel, "Known_Failures"):
		return skip(&res, "disabled-upstream")
	}
	for _, f := range hd.Features {
		if skipFeatures[f] || strings.HasPrefix(f, "AVX512") {
			return skip(&res, "host-feature:"+f)
		}
	}
	if m := reSys.FindStringSubmatch(src); m != nil {
		return skip(&res, "syscall/host-specific:"+strings.ToLower(strings.Fields(m[1])[0]))
	}

	wd := filepath.Join(rt.work, "t", strings.ReplaceAll(strings.TrimSuffix(rel, ".asm"), "/", "__"))
	os.RemoveAll(wd)
	os.MkdirAll(wd, 0o755)
	if !*flKeep {
		defer os.RemoveAll(wd)
	}

	// 1. nasm -> flat binary (FEX's CMake: prepend "BITS 64", append "ret").
	tmp := filepath.Join(wd, "t.asm")
	os.WriteFile(tmp, []byte("BITS 64\n"+src+"\nret\n"), 0o644)
	binp := filepath.Join(wd, "t.bin")
	cmd := exec.Command("nasm", "-i", filepath.Join(rt.fex, "Includes")+"/", "-i", filepath.Dir(srcPath)+"/", tmp, "-o", binp)
	if out, err := cmd.CombinedOutput(); err != nil {
		return skip(&res, "nasm-error", firstLine(string(out)))
	}
	blob, _ := os.ReadFile(binp)

	// 2. memory map and code placement.
	regions, err := parseRegions(&hd)
	if err != nil {
		return skip(&res, "bad-header", err.Error())
	}
	base := uint64(codeBase)
	for _, c := range []uint64{0x10000, 0x400000, 0x20000000, 0x30000000, 0x40000000} {
		if !overlaps(c, uint64(len(blob))+0x2000, regions) {
			base = c
			break
		}
	}
	spec := &strings.Builder{}
	fmt.Fprintf(spec, "code %x 0\n", base)
	for _, r := range regions {
		fmt.Fprintf(spec, "map %x %x\nhash %x %x\n", r[0], r[1], r[0], r[1])
	}
	var addrs []string
	for k := range hd.MemData {
		addrs = append(addrs, k)
	}
	sort.Strings(addrs)
	for _, k := range addrs {
		a, ok := parseNum(k)
		if !ok {
			return skip(&res, "bad-header", "MemoryData "+k)
		}
		fmt.Fprintf(spec, "data %x %s\n", a, strings.Join(strings.Fields(hd.MemData[k]), " "))
	}
	os.WriteFile(filepath.Join(wd, "spec.txt"), []byte(spec.String()), 0o644)
	os.WriteFile(filepath.Join(wd, "code.bin"), blob, 0o644)

	// 3. lift.
	f80 := false
	switch *flX87 {
	case "f80":
		f80 = true
	case "auto":
		f80 = hd.Env["FEX_X87REDUCEDPRECISION"] != "1"
	}
	asmText, mn, forms, lerr := liftBlob(blob, base, f80)
	res.Mnemonics = mn
	if lerr != nil {
		return fail(&res, "translate-error", lerr.Error())
	}
	if len(forms) > 0 {
		res.Forms = forms
		return skip(&res, "unsupported-instruction", forms...)
	}
	sPath := filepath.Join(wd, "t.s")
	os.WriteFile(sPath, []byte(asmText), 0o644)

	// 4. assemble + link.
	exe := filepath.Join(wd, "t.exe")
	link := []string{"-arch", "arm64", "-w", sPath, rt.harnessO, rt.helperLib}
	if f80 {
		link = append(link, rt.x87Lib)
	}
	out, err := exec.Command("clang", append(link, "-o", exe)...).CombinedOutput()
	if err != nil {
		s := string(out)
		if syms := undefinedSyms(s); len(syms) > 0 {
			return skip(&res, "runtime-helper:"+strings.Join(syms, ","))
		}
		return fail(&res, "assemble-error", firstLines(s, 3))
	}

	// 5. run (arm64 only, sandboxed, with a timeout; a crash only takes down this subprocess).
	ctx, cancel := context.WithTimeout(context.Background(), time.Duration(*flTimeout+5)*time.Second)
	defer cancel()
	c := exec.CommandContext(ctx, filepath.Join(rt.root, "tools/run_sandboxed.sh"), strconv.Itoa(*flTimeout), exe,
		filepath.Join(wd, "spec.txt"), filepath.Join(wd, "code.bin"), map[bool]string{true: "f80", false: "f64"}[f80])
	c.Env = append(os.Environ(), "NPS5_WORKDIR="+filepath.Join(rt.work, "sandbox"))
	var so bytes.Buffer
	c.Stdout = &so
	runErr := c.Run()
	got := parseDump(so.String())
	if got.crash != "" {
		return fail(&res, crashKind(got.crash), got.crash)
	}
	if !got.end {
		reason := "no-result"
		if ee, ok := runErr.(*exec.ExitError); ok {
			if ee.ExitCode() == 124 || ee.ExitCode() == 137 {
				reason = "timeout"
			} else {
				reason = fmt.Sprintf("exit-%d", ee.ExitCode())
			}
		}
		return fail(&res, reason)
	}

	// 6. compare.
	diffs := compare(&hd, got)
	if len(diffs) == 0 {
		res.Status = "PASS"
		return res
	}
	if res.Dir == "X87" && !f80 && reTword.MatchString(src) {
		return skip(&res, "precision-x87-80bit", diffs...)
	}
	return fail(&res, "mismatch", diffs...)
}

func topDir(rel string) string {
	if i := strings.Index(rel, "/"); i > 0 {
		return rel[:i]
	}
	return "(top)"
}

func firstLine(s string) string { return firstLines(s, 1) }
func firstLines(s string, n int) string {
	ls := strings.Split(strings.TrimSpace(s), "\n")
	if len(ls) > n {
		ls = ls[:n]
	}
	return strings.Join(ls, " | ")
}

var reUndef = regexp.MustCompile(`"(_[A-Za-z0-9_]+)", referenced from`)

func undefinedSyms(s string) []string {
	seen := map[string]bool{}
	var out []string
	for _, m := range reUndef.FindAllStringSubmatch(s, -1) {
		if !seen[m[1]] {
			seen[m[1]] = true
			out = append(out, m[1])
		}
	}
	sort.Strings(out)
	return out
}

func parseNum(s string) (uint64, bool) {
	s = strings.TrimSpace(s)
	neg := strings.HasPrefix(s, "-")
	s = strings.TrimPrefix(s, "-")
	n, ok := new(big.Int).SetString(strings.ToLower(s), 0)
	if !ok {
		return 0, false
	}
	if neg {
		n.Neg(n)
	}
	return new(big.Int).And(n, new(big.Int).SetUint64(^uint64(0))).Uint64(), true
}

func parseRegions(hd *header) ([][2]uint64, error) {
	var out [][2]uint64
	for k, v := range hd.Regions {
		a, ok1 := parseNum(k)
		s, ok2 := parseNum(v)
		if !ok1 || !ok2 {
			return nil, fmt.Errorf("MemoryRegions %s:%s", k, v)
		}
		out = append(out, [2]uint64{a, s})
	}
	sort.Slice(out, func(i, j int) bool { return out[i][0] < out[j][0] })
	return out, nil
}

func overlaps(a, n uint64, regs [][2]uint64) bool {
	all := append([][2]uint64{{0xe0000000, 0x8001000}, {0xd0000000, 0x1000}}, regs...)
	for _, r := range all {
		if a < r[0]+r[1] && r[0] < a+n {
			return true
		}
	}
	return false
}

// ---- result parsing and comparison ----

type dump struct {
	vals  map[string][]uint64
	end   bool
	crash string
}

func parseDump(s string) dump {
	d := dump{vals: map[string][]uint64{}}
	for _, ln := range strings.Split(s, "\n") {
		f := strings.Fields(ln)
		if len(f) == 0 {
			continue
		}
		switch f[0] {
		case "END":
			d.end = true
		case "CRASH", "SETUP-FAIL":
			d.crash = strings.TrimSpace(ln)
		case "MEM":
		default:
			var v []uint64
			for _, x := range f[1:] {
				n, _ := strconv.ParseUint(strings.TrimPrefix(x, "0x"), 16, 64)
				v = append(v, n)
			}
			d.vals[f[0]] = v
		}
	}
	return d
}

func compare(hd *header, got dump) []string {
	ign := map[string]bool{}
	for _, i := range hd.Ignore {
		ign[strings.ToUpper(i)] = true
	}
	keys := make([]string, 0, len(hd.RegData))
	for k := range hd.RegData {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	var diffs []string
	for _, k := range keys {
		K := strings.ToUpper(k)
		if ign[K] {
			continue
		}
		var exp []uint64
		bad := false
		switch v := hd.RegData[k].(type) {
		case string:
			n, ok := parseNum(v)
			bad = !ok
			exp = []uint64{n}
		case []any:
			for _, e := range v {
				s, _ := e.(string)
				n, ok := parseNum(s)
				bad = bad || !ok
				exp = append(exp, n)
			}
		default:
			bad = true
		}
		if bad {
			diffs = append(diffs, K+" unparsable expectation")
			continue
		}
		var have []uint64
		if strings.HasPrefix(K, "XMM") && len(exp) == 4 {
			have = append(append(have, got.vals[K]...), got.vals["YMMH"+K[3:]]...)
		} else {
			have = got.vals[K]
		}
		if len(have) < len(exp) || len(exp) == 0 {
			diffs = append(diffs, K+" not produced")
			continue
		}
		for i := range exp {
			if have[i] != exp[i] {
				diffs = append(diffs, fmt.Sprintf("%s expected %s got %s", K, hexList(exp), hexList(have[:len(exp)])))
				break
			}
		}
	}
	return diffs
}

func hexList(v []uint64) string {
	var s []string
	for _, x := range v {
		s = append(s, fmt.Sprintf("0x%x", x))
	}
	return strings.Join(s, ",")
}

// crashKind names a harness CRASH line: brk immediates tell lifter traps (0x0), unsupported stubs
// (0x777) and unknown indirect-branch targets (0x778 call, 0x779 jump) apart; faults are
// split by whether the address is a guest address.
func crashKind(line string) string {
	sig := map[string]string{"4": "SIGILL", "5": "SIGTRAP", "6": "SIGABRT", "8": "SIGFPE", "10": "SIGBUS", "11": "SIGSEGV"}
	f := strings.Fields(line)
	if len(f) < 3 || f[0] != "CRASH" {
		return "crash"
	}
	name := sig[strings.TrimPrefix(f[1], "sig=")]
	if name == "" {
		name = f[1]
	}
	k := strings.SplitN(f[2], "=", 2)
	if k[0] == "brk" {
		return "crash:brk " + k[1]
	}
	return "crash:" + name + " " + k[0]
}
