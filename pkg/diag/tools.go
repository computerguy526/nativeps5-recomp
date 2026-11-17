package diag

import (
	"bufio"
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"
)

// KosmicKrispICDDir is where the KosmicKrisp install lives: $NPS5_KK_INSTALL, else
// $NPS5_ROOT/work/s3/install (see BUILDING.md).
var KosmicKrispICDDir = kosmicKrispICDDir()

func kosmicKrispICDDir() string {
	if d := os.Getenv("NPS5_KK_INSTALL"); d != "" {
		return d
	}
	return filepath.Join(os.Getenv("NPS5_ROOT"), "work", "s3", "install")
}

// Check is one `nps5 doctor` result.
type Check struct {
	Name   string
	OK     bool
	Detail string
	Warn   bool // not required, only informational when it fails
}

const arenaProbeC = `
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdio.h>
int main(void) {
  mach_vm_address_t a = 0x7000000000ULL;
  mach_vm_size_t sz = 0x8C00000000ULL; /* 0x70_0000_0000 .. 0xFC_0000_0000 */
  kern_return_t kr = mach_vm_allocate(mach_task_self(), &a, sz, VM_FLAGS_FIXED);
  if (kr != KERN_SUCCESS) { printf("FAIL kr=%d (%s)\n", kr, mach_error_string(kr)); return 1; }
  printf("OK reserved 0x%llx bytes at 0x%llx\n", (unsigned long long)sz, (unsigned long long)a);
  return 0;
}
`

func runCmd(timeout time.Duration, name string, args ...string) (string, error) {
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, name, args...)
	var buf bytes.Buffer
	cmd.Stdout = &buf
	cmd.Stderr = &buf
	err := cmd.Run()
	return strings.TrimSpace(buf.String()), err
}

// Doctor runs the environment checks.
func Doctor() []Check {
	var out []Check
	clang, err := exec.LookPath("clang")
	if err != nil {
		out = append(out, Check{Name: "clang", Detail: "not found in PATH (install Xcode command line tools)"})
	} else {
		v, _ := runCmd(5*time.Second, clang, "--version")
		out = append(out, Check{Name: "clang", OK: true, Detail: clang + ": " + firstLine(v)})
	}
	for _, t := range []struct{ name, bin string }{{"go", "go"}, {"ninja", "ninja"}, {"git", "git"}} {
		p, err := exec.LookPath(t.bin)
		if err != nil {
			out = append(out, Check{Name: t.name, Warn: true, Detail: "not found"})
			continue
		}
		v, _ := runCmd(5*time.Second, p, "version")
		if t.bin == "ninja" || t.bin == "git" {
			v, _ = runCmd(5*time.Second, p, "--version")
		}
		out = append(out, Check{Name: t.name, OK: true, Detail: p + ": " + firstLine(v)})
	}
	out = append(out, Check{Name: "host", OK: true, Detail: fmt.Sprintf("%s, macOS build %s", sysctl("hw.model"), sysctl("kern.osversion"))})
	out = append(out, arenaCheck(clang))
	if _, err := runCmd(10*time.Second, "/usr/bin/arch", "-x86_64", "/usr/bin/true"); err == nil {
		out = append(out, Check{Name: "rosetta", OK: true, Detail: "arch -x86_64 /usr/bin/true works"})
	} else {
		out = append(out, Check{Name: "rosetta", Warn: true, Detail: "arch -x86_64 /usr/bin/true failed: " + err.Error()})
	}
	out = append(out, kosmicCheck())
	return out
}

func firstLine(s string) string {
	if i := strings.IndexByte(s, '\n'); i >= 0 {
		return s[:i]
	}
	return s
}

func arenaCheck(clang string) Check {
	c := Check{Name: "arena 0x7000000000"}
	if clang == "" {
		c.Detail = "cannot probe without clang"
		return c
	}
	dir, err := os.MkdirTemp("", "nps5-doctor-")
	if err != nil {
		c.Detail = err.Error()
		return c
	}
	defer os.RemoveAll(dir)
	src := filepath.Join(dir, "probe.c")
	exe := filepath.Join(dir, "probe")
	_ = os.WriteFile(src, []byte(arenaProbeC), 0o644)
	if o, err := runCmd(60*time.Second, clang, "-target", "arm64-apple-darwin", "-O1", src, "-o", exe); err != nil {
		c.Detail = "probe compile failed: " + o
		return c
	}
	o, err := runCmd(10*time.Second, exe)
	c.OK = err == nil
	c.Detail = o
	return c
}

func kosmicCheck() Check {
	c := Check{Name: "kosmickrisp ICD", Warn: true}
	if st, err := os.Stat(KosmicKrispICDDir); err != nil || !st.IsDir() {
		c.Detail = KosmicKrispICDDir + " not present"
		return c
	}
	var icds []string
	filepath.Walk(KosmicKrispICDDir, func(p string, info os.FileInfo, err error) error {
		if err == nil && !info.IsDir() && strings.HasSuffix(p, ".json") && strings.Contains(strings.ToLower(p), "icd") {
			icds = append(icds, p)
		}
		return nil
	})
	if len(icds) == 0 {
		c.Detail = KosmicKrispICDDir + " present but no *icd*.json found"
		return c
	}
	sel := os.Getenv("VK_ICD_FILENAMES")
	if sel == "" {
		sel = os.Getenv("VK_DRIVER_FILES")
	}
	c.OK = true
	c.Detail = icds[0]
	if sel == "" {
		c.Detail += " (not selected: VK_ICD_FILENAMES/VK_DRIVER_FILES unset)"
	} else {
		c.Detail += " (selected via env: " + sel + ")"
	}
	return c
}

// FormatChecks renders doctor results; ok is false if a required check failed.
func FormatChecks(cs []Check) (string, bool) {
	var b strings.Builder
	ok := true
	for _, c := range cs {
		tag := "ok  "
		if !c.OK {
			if c.Warn {
				tag = "warn"
			} else {
				tag = "FAIL"
				ok = false
			}
		}
		fmt.Fprintf(&b, "[%s] %-20s %s\n", tag, c.Name, c.Detail)
	}
	return b.String(), ok
}

// ResolveBundle returns the bundle directory: arg if given, else root/latest.
func ResolveBundle(root, arg string) (string, error) {
	if arg != "" {
		return arg, nil
	}
	p := filepath.Join(root, "latest")
	d, err := filepath.EvalSymlinks(p)
	if err != nil {
		return "", fmt.Errorf("no bundles found (%s): %w", p, err)
	}
	return d, nil
}

// Last prints the summary of a bundle (manifest, log level counts, census, summary.txt, crash.txt).
func Last(w io.Writer, dir string) error {
	var man Manifest
	data, err := os.ReadFile(filepath.Join(dir, "manifest.json"))
	if err != nil {
		return fmt.Errorf("%s: %w", dir, err)
	}
	if err := json.Unmarshal(data, &man); err != nil {
		return err
	}
	fmt.Fprintf(w, "bundle   %s\n", dir)
	fmt.Fprintf(w, "phase    %s   title %s\n", man.Phase, man.Title)
	fmt.Fprintf(w, "started  %s   wall %d ms   result %s\n", man.Started, man.WallMs, orDash(man.Result))
	if man.Error != "" {
		fmt.Fprintf(w, "error    %s\n", man.Error)
	}
	fmt.Fprintf(w, "tool     %s%s\n", man.Tool.GitCommit, map[bool]string{true: " (dirty)"}[man.Tool.GitDirty])
	for _, in := range man.Inputs {
		fmt.Fprintf(w, "input    %s sha256=%s\n", in.Path, in.SHA256)
	}
	fmt.Fprintf(w, "host     %s / %s\n", man.Host.Model, man.Host.OSBuild)
	if len(man.Artifacts) > 0 {
		fmt.Fprintf(w, "files    %s\n", strings.Join(man.Artifacts, " "))
	}
	if counts, n := logCounts(filepath.Join(dir, "log.jsonl")); n > 0 {
		fmt.Fprintf(w, "log      %d records: %s\n", n, counts)
	}
	if cdata, err := os.ReadFile(filepath.Join(dir, "census.json")); err == nil {
		var c Census
		if json.Unmarshal(cdata, &c) == nil {
			fmt.Fprintf(w, "census   %s\n", c.SummaryLine())
		}
	}
	if idata, err := os.ReadFile(filepath.Join(dir, "imports.json")); err == nil {
		var im Imports
		if json.Unmarshal(idata, &im) == nil {
			fmt.Fprintf(w, "imports  %d bound, %d stub, %d missing\n", im.Bound, im.Stub, im.Missing)
		}
	}
	if s, err := os.ReadFile(filepath.Join(dir, "summary.txt")); err == nil {
		fmt.Fprintf(w, "\n--- summary.txt ---\n%s", s)
	}
	if s, err := os.ReadFile(filepath.Join(dir, "crash.txt")); err == nil && len(bytes.TrimSpace(s)) > 0 {
		fmt.Fprintf(w, "\n--- crash.txt ---\n%s", s)
	}
	return nil
}

func orDash(s string) string {
	if s == "" {
		return "-"
	}
	return s
}

func logCounts(path string) (string, int) {
	f, err := os.Open(path)
	if err != nil {
		return "", 0
	}
	defer f.Close()
	counts := map[string]int{}
	n := 0
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 1<<20), 1<<24)
	for sc.Scan() {
		var r struct {
			Level string `json:"level"`
		}
		if json.Unmarshal(sc.Bytes(), &r) == nil {
			counts[r.Level]++
			n++
		}
	}
	var keys []string
	for k := range counts {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	var parts []string
	for _, k := range keys {
		parts = append(parts, fmt.Sprintf("%s=%d", k, counts[k]))
	}
	return strings.Join(parts, " "), n
}

// PCRecord is one record of pcmap.bin: host offset from the image base, guest PC|flags<<56 and
// the guest entry of the enclosing function.
type PCRecord struct {
	Off   uint64
	Guest uint64
	Func  uint64
}

// PCMagic opens pcmap.bin.
const PCMagic = "NPS5PCM1"

// ReadPCMap parses pcmap.bin (header: magic, image_base, count; then records), sorted by Off.
func ReadPCMap(path string) (base uint64, recs []PCRecord, err error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return 0, nil, err
	}
	if len(data) < 24 || string(data[:8]) != PCMagic {
		return 0, nil, fmt.Errorf("%s: not a pcmap.bin", path)
	}
	base = binary.LittleEndian.Uint64(data[8:])
	n := binary.LittleEndian.Uint64(data[16:])
	if uint64(len(data)-24) < n*24 {
		return 0, nil, fmt.Errorf("%s: truncated", path)
	}
	recs = make([]PCRecord, n)
	for i := range recs {
		o := 24 + i*24
		recs[i] = PCRecord{binary.LittleEndian.Uint64(data[o:]), binary.LittleEndian.Uint64(data[o+8:]), binary.LittleEndian.Uint64(data[o+16:])}
	}
	// stable: the runtime writes a function's end sentinel before the next function's first record
	sort.SliceStable(recs, func(i, j int) bool { return recs[i].Off < recs[j].Off })
	return base, recs, nil
}

// LookupPC finds the record covering off (the last record at or before it).
func LookupPC(recs []PCRecord, off uint64) (PCRecord, bool) {
	i := sort.Search(len(recs), func(i int) bool { return recs[i].Off > off })
	if i == 0 {
		return PCRecord{}, false
	}
	return recs[i-1], true
}

// AddrMapNames reads the F lines of addrmap.txt: guest entry -> name.
func AddrMapNames(path string) (map[uint64]string, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	m := map[uint64]string{}
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 1<<20), 1<<24)
	for sc.Scan() {
		p := strings.Fields(sc.Text())
		if len(p) >= 4 && p[0] == "F" {
			a, err := strconv.ParseUint(strings.TrimPrefix(p[1], "0x"), 16, 64)
			if err == nil {
				if p[3] != "-" {
					m[a] = p[3]
				} else {
					m[a] = strings.TrimPrefix(p[2], "_")
				}
			}
		}
	}
	return m, sc.Err()
}

func crashField(crash []byte, key string) string {
	for _, ln := range strings.Split(string(crash), "\n") {
		if strings.HasPrefix(ln, key+":") {
			return strings.TrimSpace(strings.TrimPrefix(ln, key+":"))
		}
	}
	return ""
}

// Addr2Guest maps a host PC (absolute, or "+0xOFF" relative to the image base) to guest
// function and RIP using pcmap.bin of the bundle (written by the crash handler).
func Addr2Guest(root, bundleArg, addrmapArg, pcArg string) (string, error) {
	dir, err := ResolveBundle(root, bundleArg)
	if err != nil {
		return "", err
	}
	base, recs, err := ReadPCMap(filepath.Join(dir, "pcmap.bin"))
	if err != nil {
		return "", fmt.Errorf("%w (the runtime writes pcmap.bin on a crash or with NPS5_DUMP_PCMAP=1)", err)
	}
	var off uint64
	if strings.HasPrefix(pcArg, "+") {
		off, err = strconv.ParseUint(strings.TrimPrefix(strings.TrimPrefix(pcArg, "+"), "0x"), 16, 64)
	} else {
		var pc uint64
		pc, err = strconv.ParseUint(strings.TrimPrefix(pcArg, "0x"), 16, 64)
		off = pc - base
	}
	if err != nil {
		return "", err
	}
	rec, ok := LookupPC(recs, off)
	if !ok {
		return "", fmt.Errorf("host pc 0x%x (image+0x%x) is not in guest code", base+off, off)
	}
	if rec.Guest>>56&FlagEnd != 0 {
		return "", fmt.Errorf("host pc 0x%x (image+0x%x) is in translated code that is not a guest function (a shim stub or padding after the function at 0x%x)", base+off, off, rec.Func)
	}
	guest := rec.Guest & (1<<56 - 1)
	names := map[uint64]string{}
	amPath := addrmapArg
	if amPath == "" {
		if c, err := os.ReadFile(filepath.Join(dir, "crash.txt")); err == nil {
			amPath = crashField(c, "addrmap")
		}
	}
	if amPath == "" {
		amPath = filepath.Join(root, "latest-translate", "addrmap.txt")
	}
	if m, err := AddrMapNames(amPath); err == nil {
		names = m
	}
	fn := names[rec.Func]
	if fn == "" {
		fn = fmt.Sprintf("fn_0x%x", rec.Func)
	}
	return fmt.Sprintf("host 0x%x (image+0x%x) -> guest RIP 0x%x in %s (entry 0x%x, +0x%x), flags=%d",
		base+off, off, guest, fn, rec.Func, guest-rec.Func, rec.Guest>>56), nil
}
