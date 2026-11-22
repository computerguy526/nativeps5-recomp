// SPDX-License-Identifier: GPL-2.0-or-later

package recompruntime

// Runs the adversarial C host tests of the guest memory model (tests/memtests/*.c) and reports each
// case as a Go subtest. The C programs print one line per case:
//     RESULT|<name>|<area>|<PASS|FAIL|KNOWN-FAIL|SKIP>|<detail>
// KNOWN-FAIL cases are recorded failures of the current implementation (docs/m1/memtests.md); they
// skip here so main CI stays green. MEMTESTS_STRICT=1 turns them into failures.

import (
	"bufio"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strings"
	"testing"
	"time"
)

type memCase struct{ name, area, status, detail string }

func parseResults(path string) []memCase {
	f, err := os.Open(path)
	if err != nil {
		return nil
	}
	defer f.Close()
	var out []memCase
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 1<<20), 1<<20)
	for sc.Scan() {
		p := strings.SplitN(sc.Text(), "|", 5)
		if len(p) == 5 && p[0] == "RESULT" {
			out = append(out, memCase{p[1], p[2], p[3], p[4]})
		}
	}
	return out
}

func memtestsRoot(t *testing.T) string {
	t.Helper()
	r, err := filepath.Abs(filepath.Join("..", ".."))
	if err != nil {
		t.Fatal(err)
	}
	return r
}

func reportCases(t *testing.T, cases []memCase) {
	t.Helper()
	if len(cases) == 0 {
		t.Fatal("the test program produced no RESULT lines")
	}
	for _, c := range cases {
		c := c
		t.Run(c.name, func(t *testing.T) {
			switch c.status {
			case "PASS":
			case "KNOWN-FAIL":
				if os.Getenv("MEMTESTS_STRICT") == "1" {
					t.Fatalf("KNOWN-FAIL (strict): %s", c.detail)
				}
				t.Skipf("KNOWN-FAIL: %s", c.detail)
			case "SKIP":
				t.Skip(c.detail)
			default:
				t.Errorf("%s: %s", c.status, c.detail)
			}
		})
	}
}

func needHost(t *testing.T) string {
	t.Helper()
	if runtime.GOOS != "darwin" || runtime.GOARCH != "arm64" {
		t.Skip("the arena layout is defined for arm64 macOS")
	}
	clang, err := exec.LookPath("clang")
	if err != nil {
		t.Skip("clang is required")
	}
	return clang
}

func runHost(t *testing.T, bin string, limit time.Duration) []memCase {
	t.Helper()
	res := filepath.Join(t.TempDir(), "results.txt")
	cmd := exec.Command(bin)
	cmd.Env = append(os.Environ(), "MEMTESTS_OUT="+res, "NPS5_NO_BUNDLE=1")
	if os.Getenv("MEMTESTS_STRICT") == "1" {
		cmd.Env = append(cmd.Env, "MEMTESTS_STRICT=1")
	}
	done := make(chan error, 1)
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	go func() { done <- cmd.Wait() }()
	select {
	case err := <-done:
		cases := parseResults(res)
		if err != nil && len(cases) == 0 {
			t.Fatalf("%s: %v", filepath.Base(bin), err)
		}
		return cases
	case <-time.After(limit):
		_ = cmd.Process.Kill()
		cases := parseResults(res)
		t.Errorf("%s did not finish in %v (hang?); %d cases reported before that", filepath.Base(bin), limit, len(cases))
		return cases
	}
}

// TestMemArenaAdversarial: tests/memtests/arena_adv.c against core/nps5_mem.c, both modes.
func TestMemArenaAdversarial(t *testing.T) {
	clang := needHost(t)
	root := memtestsRoot(t)
	for _, mode := range []struct{ name, flag string }{{"base", "-DNPS5_ADDR_MODE_IDENTITY=0"}, {"identity", "-DNPS5_ADDR_MODE_IDENTITY=1"}} {
		t.Run(mode.name, func(t *testing.T) {
			bin := filepath.Join(t.TempDir(), "arena-"+mode.name)
			cmd := exec.Command(clang, "-O1", "-g", "-Wall", "-Wno-unused-function", "-Wno-unused-variable", mode.flag,
				"-I"+filepath.Join(root, "tests", "memtests"), "-Icore", filepath.Join(root, "tests", "memtests", "arena_adv.c"), "core/nps5_mem.c",
				"-framework", "Metal", "-framework", "Foundation", "-lobjc", "-o", bin)
			if out, err := cmd.CombinedOutput(); err != nil {
				t.Fatalf("build: %v\n%s", err, out)
			}
			reportCases(t, runHost(t, bin, 3*time.Minute))
		})
	}
}

// hostProgram builds one of the runtime-linked harnesses (vm_adv, xlate_adv, conc_adv).
func hostProgram(t *testing.T, root, prog, mode string, extra ...string) string {
	t.Helper()
	bin := filepath.Join(t.TempDir(), prog+"-"+mode)
	args := append([]string{bin, mode, filepath.Join(root, "tests", "memtests", prog+".c")}, extra...)
	cmd := exec.Command(filepath.Join(root, "tools", "memtests_hostcc.sh"), args...)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("build %s (%s): %v\n%s", prog, mode, err, out)
	}
	return bin
}

func TestMemVMAdversarial(t *testing.T) {
	needHost(t)
	root := memtestsRoot(t)
	for _, prog := range []string{"vm_adv", "xlate_adv", "thread_attr_adv"} {
		for _, mode := range []string{"base", "identity"} {
			t.Run(prog+"/"+mode, func(t *testing.T) {
				var extra []string
				if prog == "xlate_adv" { // the AGC driver configuration hook is compiled in only for this harness
					extra = append(extra, "-DNPS5_HAVE_ANYPS5_AGC=1")
				}
				if prog == "thread_attr_adv" { // the pthread attribute shims
					extra = append(extra, "pkg/runtime/kernel/threading/ps4_threading.c")
				}
				reportCases(t, runHost(t, hostProgram(t, root, prog, mode, extra...), 4*time.Minute))
			})
		}
	}
}

func TestMemConcurrencyAdversarial(t *testing.T) {
	needHost(t)
	root := memtestsRoot(t)
	secs := "-DSECS=3"
	if testing.Short() {
		secs = "-DSECS=1"
	}
	for _, mode := range []string{"base", "identity"} {
		t.Run(mode, func(t *testing.T) {
			reportCases(t, runHost(t, hostProgram(t, root, "conc_adv", mode, secs), 4*time.Minute))
		})
	}
}

// Identity mode "compiles to no-ops" (ADR section 7): the generated sceKernelWrite thunk must not
// contain the bias arithmetic, and the base one must.
func TestMemIdentityThunkIsNoOp(t *testing.T) {
	clang := needHost(t)
	root := memtestsRoot(t)
	asm := func(flag string) string {
		args := []string{"-O2", "-S", "-o", "-", flag, "-DRECOMP_ARM64_ASM=1"}
		for _, d := range []string{"core", "modules/libkernel", "kernel/syscalls", "kernel/memory", "kernel/vfs", "kernel/sync", "kernel/threading"} {
			args = append(args, "-I"+filepath.Join(root, "pkg", "runtime", d))
		}
		args = append(args, filepath.Join(root, "pkg", "runtime", "modules", "libkernel", "nps5_thunks_sys.c"))
		out, err := exec.Command(clang, args...).CombinedOutput()
		if err != nil {
			t.Fatalf("clang -S: %v\n%s", err, out)
		}
		return string(out)
	}
	fn := func(src, name string) []string {
		re := regexp.MustCompile(`(?m)^_` + name + `:[^\n]*\n((?:.*\n)*?)\s*\.cfi_endproc`)
		m := re.FindStringSubmatch(src)
		if m == nil {
			t.Fatalf("no function %s in the assembly", name)
		}
		var ins []string
		for _, l := range strings.Split(m[1], "\n") {
			l = strings.TrimSpace(l)
			if l != "" && !strings.HasPrefix(l, ".") && !strings.HasPrefix(l, ";") && !strings.Contains(strings.SplitN(l, ";", 2)[0], ":") {
				ins = append(ins, l)
			}
		}
		return ins
	}
	id := fn(asm("-DNPS5_ADDR_MODE_IDENTITY=1"), "shim_sceKernelWrite")
	base := fn(asm("-DNPS5_ADDR_MODE_IDENTITY=0"), "shim_sceKernelWrite")
	t.Logf("shim_sceKernelWrite: identity %d instructions, base %d", len(id), len(base))
	joined := strings.Join(id, "\n")
	for _, bad := range []string{"cbz", "csel", "cmp\t", "cset", "tbz"} {
		if strings.Contains(joined, bad) {
			t.Errorf("identity thunk contains %q (translation not compiled out):\n%s", bad, joined)
		}
	}
	if len(id) > 24 {
		t.Errorf("identity thunk has %d instructions (ADR: three loads, the call, the result store)", len(id))
	}
	if !strings.Contains(strings.Join(base, "\n"), "csel") && !strings.Contains(strings.Join(base, "\n"), "cbz") {
		t.Errorf("base thunk lacks the NULL-preserving select:\n%s", strings.Join(base, "\n"))
	}
	if len(base) <= len(id) {
		t.Errorf("base thunk (%d) is not longer than identity (%d)", len(base), len(id))
	}
	_ = fmt.Sprint
}
