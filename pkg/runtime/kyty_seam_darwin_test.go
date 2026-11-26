// SPDX-License-Identifier: GPL-2.0-or-later

package recompruntime

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"

	"ps4-recomp/pkg/thunkgen"
)

// TestKytyWatch builds and runs tests/kyty_seam/watch_test.c: the page watch table of the NPS5_GPU=kyty seam
// (4 KiB tracker pages on 16 KiB host pages) over real pages and real faults: unit cases, a model-checked fuzz, a
// concurrency invariant and micro-benchmarks. docs/m3/kyty-seam.md.
func TestKytyWatch(t *testing.T) {
	if runtime.GOARCH != "arm64" || runtime.GOOS != "darwin" {
		t.Skip("16 KiB host pages: arm64 macOS")
	}
	clang, err := exec.LookPath("clang")
	if err != nil {
		t.Skip("clang is required")
	}
	bin := filepath.Join(t.TempDir(), "watch_test")
	out, err := exec.Command(clang, "-g", "-O1", "-Wall", "-Wextra", "-Werror", "-Imodules/libSceAnyPS5", "../../tests/kyty_seam/watch_test.c",
		"modules/libSceAnyPS5/nps5_kyty_watch.c", "modules/libSceAnyPS5/nps5_kyty_emu.c", "-o", bin).CombinedOutput()
	if err != nil {
		t.Fatalf("build: %v\n%s", err, out)
	}
	out = runWithTimeout(t, exec.Command(bin), 180*time.Second)
	if !strings.Contains(string(out), "WATCH OK") {
		t.Fatalf("watch test:\n%s", out)
	}
	t.Logf("%s", out)
}

// TestKytyEmu builds and runs tests/kyty_seam/emu_test.c: the trapped-access emulator against the CPU, one load or
// store instruction per snippet (GPR and SIMD, immediate / register / pre / post index, pairs, DC ZVA): registers,
// writeback and memory must match the CPU's; exclusives and atomics must be declined untouched. The Advanced SIMD
// structure class (LD1-4/ST1-4, lanes, LD1R-4R, post-index) is enumerated exhaustively against the CPU.
func TestKytyEmu(t *testing.T) {
	if runtime.GOARCH != "arm64" || runtime.GOOS != "darwin" {
		t.Skip("arm64 macOS")
	}
	clang, err := exec.LookPath("clang")
	if err != nil {
		t.Skip("clang is required")
	}
	bin := filepath.Join(t.TempDir(), "emu_test")
	out, err := exec.Command(clang, "-g", "-O1", "-Wall", "-Wextra", "-Werror", "-Imodules/libSceAnyPS5", "../../tests/kyty_seam/emu_test.c",
		"modules/libSceAnyPS5/nps5_kyty_emu.c", "-o", bin).CombinedOutput()
	if err != nil {
		t.Fatalf("build: %v\n%s", err, out)
	}
	out = runWithTimeout(t, exec.Command(bin), 60*time.Second)
	if !strings.Contains(string(out), "EMU OK 130/135") || !strings.Contains(string(out), "EMU SIMD 1890 defined 6302 undefined") {
		t.Fatalf("emulator test:\n%s", out)
	}
	t.Logf("%s", out)
}

func runWithTimeout(t *testing.T, cmd *exec.Cmd, d time.Duration) []byte {
	t.Helper()
	done := make(chan struct{})
	var out []byte
	go func() { out, _ = cmd.CombinedOutput(); close(done) }()
	select {
	case <-done:
	case <-time.After(d):
		_ = cmd.Process.Kill()
		t.Fatalf("%s timed out", cmd.Path)
	}
	return out
}

// fakeAnyPS5 renders C definitions of AnyPS5 exports that only record their call: `indirect` selects the ones the
// thunks reach through dlsym (libnps5_gpu) or the directly linked ones.
func fakeAnyPS5(t *testing.T, state string, indirect bool) string {
	t.Helper()
	descs, err := thunkgen.AnyPS5Descs()
	if err != nil {
		t.Fatal(err)
	}
	var b strings.Builder
	b.WriteString("#include <stdint.h>\ntypedef struct FakeLast { int id; const char *name; uint64_t a[8]; double f0; uint64_t calls; uint64_t ret; } FakeLast;\n")
	fmt.Fprintf(&b, "__attribute__((visibility(\"default\"))) FakeLast %s;\n", state)
	fmt.Fprintf(&b, "static uint64_t rec(int id, const char *name, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {\n"+
		"  FakeLast *s = &%s; s->id = id; s->name = name; s->a[0] = a0; s->a[1] = a1; s->a[2] = a2; s->a[3] = a3; s->a[4] = a4; s->a[5] = a5; s->a[6] = a6; s->a[7] = a7; s->calls++;\n"+
		"  const uint64_t r = s->ret ? s->ret : 0x7000u + (uint64_t)id; s->ret = 0; return r;\n}\n", state)
	b.WriteString("#define F(id, name) __attribute__((visibility(\"default\"))) uint64_t name(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) { return rec(id, #name, a0, a1, a2, a3, a4, a5, a6, a7); }\n")
	seen := map[string]bool{}
	n := 0
	for _, d := range descs {
		name := d.HLE
		if d.Indirect {
			name = strings.TrimPrefix(name, "aps5_fp_")
		} else if d.RouteTarget != "" {
			name = d.RouteTarget
		}
		if d.Indirect != indirect || seen[name] {
			continue
		}
		seen[name] = true
		fmt.Fprintf(&b, "F(%d, %s)\n", n, name)
		n++
	}
	if n < 100 {
		t.Fatalf("only %d fake exports (indirect=%v)", n, indirect)
	}
	return b.String()
}

const fakeKytyHost = `
#include <stdint.h>
typedef struct FakeHost { int adds, deletes, delivers, pads, exits, script; int64_t eq, ident; uint32_t kind; uint64_t data; void *udata; int code; } FakeHost;
#define X __attribute__((visibility("default")))
X FakeHost fake_host_state;
X int Nps5KytyHostAddGpuEvent(int64_t eq, uint32_t kind, int64_t ident, void *udata) { fake_host_state.adds++; fake_host_state.eq = eq; fake_host_state.kind = kind; fake_host_state.ident = ident; fake_host_state.udata = udata; return 0; }
X int Nps5KytyHostDeleteGpuEvent(int64_t eq, uint32_t kind, int64_t ident) { (void)eq; (void)kind; (void)ident; fake_host_state.deletes++; return 0; }
X void Nps5KytyHostDeliverEvent(uint32_t kind, int64_t ident, uint64_t data) { fake_host_state.delivers++; fake_host_state.kind = kind; fake_host_state.ident = ident; fake_host_state.data = data; }
X void Nps5KytyHostPublishPad(int port, const void *state) { (void)port; (void)state; fake_host_state.pads++; }
X int Nps5KytyHostStartPadScript(void) { fake_host_state.script++; return 1; }
X void Nps5KytyHostRequestExit(int code) { fake_host_state.exits++; fake_host_state.code = code; }
`

// TestKytySeam builds the generated AnyPS5 thunks with the NPS5_GPU=kyty seam against a fake of the AnyPS5 libraries
// and runs tests/kyty_seam/route_test.c with the stub kyty_gpu library (tests/kyty_seam/kyty_stub.c):
//   - switch off (default): every one of the 84 routable thunks still calls its AnyPS5 function, with the same
//     marshalling; nothing is loaded, hooked or registered;
//   - switch on: all 84 reach the library (guest-pointer and host-pointer modes), stack-passed and float arguments
//     and pointer results cross correctly, the other exports stay with AnyPS5; guest memory hand-off, alias views,
//     16 KiB page watches through real faults, host callbacks;
//   - a NID the library lacks stops the process only when the game calls it; a library of an older ABI is refused.
func TestKytySeam(t *testing.T) {
	if runtime.GOARCH != "arm64" || runtime.GOOS != "darwin" {
		t.Skip("the seam is defined for arm64 macOS")
	}
	clang, err := exec.LookPath("clang")
	if err != nil {
		t.Skip("clang is required")
	}
	dir := t.TempDir()
	write := func(name, text string) string {
		p := filepath.Join(dir, name)
		if err := os.WriteFile(p, []byte(text), 0o644); err != nil {
			t.Fatal(err)
		}
		return p
	}
	fakeDirect := write("fake_anyps5.c", fakeAnyPS5(t, "fake_direct", false))
	fakeGpu := write("fake_gpu.c", fakeAnyPS5(t, "fake_gpu_state", true)+fakeKytyHost)
	build := func(args ...string) {
		t.Helper()
		if out, err := exec.Command(clang, args...).CombinedOutput(); err != nil {
			t.Fatalf("clang %s: %v\n%s", strings.Join(args, " "), err, out)
		}
	}
	gpuLib := filepath.Join(dir, "libfake_gpu.dylib")
	build("-O1", "-w", "-dynamiclib", fakeGpu, "-o", gpuLib)
	stubLib := filepath.Join(dir, "kyty_gpu.dylib")
	build("-O1", "-Wall", "-Wextra", "-Werror", "-dynamiclib", "-Imodules/libSceAnyPS5", "../../tests/kyty_seam/kyty_stub.c", "-o", stubLib)
	var inc []string
	for _, d := range []string{"core", "kernel/memory", "kernel/syscalls", "kernel/vfs", "kernel/threading", "modules/libkernel", "modules/libSceAnyPS5", "../../tests/kyty_seam"} {
		inc = append(inc, "-I"+d)
	}
	bin := filepath.Join(dir, "route_test")
	args := append([]string{"-g", "-O1", "-w", "-DNPS5_HAVE_ANYPS5_AGC=1"}, inc...)
	args = append(args, "../../tests/kyty_seam/route_test.c", "modules/libSceAnyPS5/nps5_anyps5_thunks.c", "modules/libSceAnyPS5/nps5_kyty.c",
		"modules/libSceAnyPS5/nps5_kyty_watch.c", "modules/libSceAnyPS5/nps5_kyty_emu.c", "../../tests/kyty_seam/ue5_stub.c", fakeDirect, "-o", bin)
	build(args...)
	// The seam's own sources compile without warnings.
	build(append(append([]string{"-fsyntax-only", "-Wall", "-Wextra", "-Werror", "-DNPS5_HAVE_ANYPS5_AGC=1"}, inc...), "modules/libSceAnyPS5/nps5_kyty.c", "modules/libSceAnyPS5/nps5_kyty_watch.c", "modules/libSceAnyPS5/nps5_kyty_emu.c")...)

	run := func(mode string, env ...string) (string, error) {
		cmd := exec.Command(bin, mode)
		cmd.Env = append(os.Environ(), "FAKE_GPU_LIB="+gpuLib)
		cmd.Env = append(cmd.Env, env...)
		done := make(chan struct{})
		var out []byte
		var err error
		go func() { out, err = cmd.CombinedOutput(); close(done) }()
		select {
		case <-done:
		case <-time.After(120 * time.Second):
			_ = cmd.Process.Kill()
			t.Fatalf("route_test %s timed out", mode)
		}
		return string(out), err
	}
	kyty := []string{"NPS5_GPU=kyty", "NPS5_KYTY_GPU_LIB=" + stubLib}

	// Off: unset, empty, and any other value (the library path may even be set: it must not be opened).
	for _, env := range [][]string{{"NPS5_GPU="}, {"NPS5_GPU=anyps5", "NPS5_KYTY_GPU_LIB=" + stubLib}, {"NPS5_GPU=KYTY", "NPS5_KYTY_GPU_LIB=" + stubLib}} {
		if out, err := run("default", env...); err != nil || !strings.Contains(out, "ROUTE OK default") {
			t.Fatalf("default path %v: %v\n%s", env, err, out)
		}
	}
	if out, err := run("kyty", kyty...); err != nil || !strings.Contains(out, "ROUTE OK kyty ") {
		t.Fatalf("kyty: %v\n%s", err, out)
	} else {
		t.Logf("%s", out)
	}
	if out, err := run("kyty", append(kyty, "NPS5_KYTY_TRACE=1")...); err != nil || !strings.Contains(out, "register 0x") {
		t.Fatalf("kyty with trace: %v\n%s", err, out)
	}
	if out, err := run("kyty-host", append(kyty, "KYTY_STUB_POINTERS=host")...); err != nil || !strings.Contains(out, "ROUTE OK kyty-host") {
		t.Fatalf("kyty-host: %v\n%s", err, out)
	}
	if out, err := run("noentry", append(kyty, "KYTY_STUB_NO_NID_ENTRY=1")...); err != nil || !strings.Contains(out, "ROUTE OK noentry") {
		t.Fatalf("noentry: %v\n%s", err, out)
	}
	// ABI 1.8 guest_ready: called once with extract_ue5_shader_groups usable inside; not called for a library whose guest_ready
	// is null or whose struct ends at 1.7 (the host still accepts it).
	if out, err := run("ready", kyty...); err != nil || !strings.Contains(out, "ROUTE OK ready") {
		t.Fatalf("ready: %v\n%s", err, out)
	}
	if out, err := run("noready", append(kyty, "KYTY_STUB_NO_GUEST_READY=1")...); err != nil || !strings.Contains(out, "ROUTE OK noready") {
		t.Fatalf("noready: %v\n%s", err, out)
	}
	if out, err := run("ready17", append(kyty, "KYTY_STUB_API17=1")...); err != nil || !strings.Contains(out, "ROUTE OK ready17") {
		t.Fatalf("ready17: %v\n%s", err, out)
	}
	// A missing NID: start-up succeeds and says so; the call aborts and names the export.
	out, err := run("missing", append(kyty, "KYTY_STUB_MISSING=Yw0jKSqop+E")...)
	if err == nil || !strings.Contains(out, "calling the missing export") || !strings.Contains(out, "sceAgcDcbDrawIndexAuto (NID Yw0jKSqop+E") ||
		!strings.Contains(out, "does not implement it") || strings.Contains(out, "FAIL") {
		t.Fatalf("missing NID: %v\n%s", err, out)
	}
	// An ABI 1.5 library (no hosted memory) and an unloadable library are refused at start-up, loudly.
	if out, err := run("kyty", append(kyty, "KYTY_STUB_ABI=5")...); err == nil || !strings.Contains(out, "not an ABI 1.6 library") {
		t.Fatalf("old ABI: %v\n%s", err, out)
	}
	if out, err := run("kyty", "NPS5_GPU=kyty", "NPS5_KYTY_GPU_LIB="+filepath.Join(dir, "nope.dylib")); err == nil || !strings.Contains(out, "FATAL: NPS5_KYTY_GPU_LIB=") {
		t.Fatalf("unloadable library: %v\n%s", err, out)
	}
}

// TestKytySeamVM builds tests/kyty_seam/vm_seam_test.c: the seam on the real VM layer with ASTRO BOT's memory shape
// (2 GiB at guest 0x300000000, 3.2 GB of direct memory at guest 0x1000000000, bias 2^40). The guest's memset over
// registered read-write memory must succeed before, under and after the library's page watches, across guest mprotect,
// unmap and re-map; a store to an address nothing maps is declined by the seam (a title crash, reported as SIGBUS by
// macOS because the reservation is PROT_NONE; it is not a watch fault).
func TestKytySeamVM(t *testing.T) {
	if runtime.GOARCH != "arm64" || runtime.GOOS != "darwin" {
		t.Skip("the guest arena is defined for arm64 macOS")
	}
	clang, err := exec.LookPath("clang")
	if err != nil {
		t.Skip("clang is required")
	}
	dir := t.TempDir()
	stubLib := filepath.Join(dir, "kyty_gpu.dylib")
	if out, err := exec.Command(clang, "-O1", "-dynamiclib", "-Imodules/libSceAnyPS5", "../../tests/kyty_seam/kyty_stub.c", "-o", stubLib).CombinedOutput(); err != nil {
		t.Fatalf("stub: %v\n%s", err, out)
	}
	var inc []string
	for _, d := range []string{"core", "kernel/memory", "kernel/syscalls", "kernel/vfs", "kernel/threading", "modules/libkernel", "modules/libSceAnyPS5", "../../tests/kyty_seam"} {
		inc = append(inc, "-I"+d)
	}
	bin := filepath.Join(dir, "vm_seam_test")
	args := append([]string{"-g", "-O1", "-w", "-DNPS5_HAVE_ANYPS5_AGC=1"}, inc...)
	args = append(args, "../../tests/kyty_seam/vm_seam_test.c", "../../tests/memory_vm_test/stubs.c", "core/nps5_vm.c", "core/nps5_mem.c",
		"kernel/memory/ps4_direct_mem.c", "modules/libSceAnyPS5/nps5_kyty.c", "modules/libSceAnyPS5/nps5_kyty_watch.c", "modules/libSceAnyPS5/nps5_kyty_emu.c", "../../tests/kyty_seam/ue5_stub.c", "-Wl,-undefined,dynamic_lookup", "-o", bin)
	if out, err := exec.Command(clang, args...).CombinedOutput(); err != nil {
		t.Fatalf("build: %v\n%s", err, out)
	}
	cmd := exec.Command(bin)
	cmd.Env = append(os.Environ(), "NPS5_GPU=kyty", "NPS5_KYTY_GPU_LIB="+stubLib, "NPS5_KYTY_STATS=1")
	out := runWithTimeout(t, cmd, 180*time.Second)
	if !strings.Contains(string(out), "VM SEAM OK") {
		t.Fatalf("vm seam test:\n%s", out)
	}
	t.Logf("%s", out)
}

// TestKytyRouteTable: the routable set is exactly the 84 graphics NIDs of AstroHybrid's kyty_gpu export table, each
// with one AnyPS5 export; only those thunks changed shape.
func TestKytyRouteTable(t *testing.T) {
	routes, err := thunkgen.KytyRoutes()
	if err != nil {
		t.Fatal(err)
	}
	libs := map[string]int{}
	for _, r := range routes {
		libs[r.Lib]++
	}
	if len(routes) != 121 || libs["libSceAgc.prx"] != 86 || libs["libSceAgcDriver.prx"] != 10 || libs["libSceVideoOut.prx"] != 25 {
		t.Fatalf("routes: %d %v", len(routes), libs)
	}
	descs, err := thunkgen.AnyPS5Descs()
	if err != nil {
		t.Fatal(err)
	}
	routed, direct := 0, 0
	for _, d := range descs {
		if d.RouteNID == "" {
			if strings.HasPrefix(d.HLE, "aps5_rt_") {
				t.Errorf("%s calls through a route pointer but is not routed", d.Shim)
			}
			continue
		}
		routed++
		if d.RouteTarget != "" {
			direct++
			if d.HLE != "aps5_rt_"+d.RouteTarget {
				t.Errorf("%s: pointer %s for %s", d.Shim, d.HLE, d.RouteTarget)
			}
		}
	}
	if routed != 121 || direct != 96 {
		t.Fatalf("routed %d (direct %d), want 121 (96)", routed, direct)
	}
	src, err := os.ReadFile("modules/libSceAnyPS5/nps5_anyps5_thunks.c")
	if err != nil {
		t.Fatal(err)
	}
	if n := strings.Count(string(src), "(void *)kyty_raw_"); n != 121 {
		t.Fatalf("generated table has %d routes: run go run ./pkg/thunkgen/gen", n)
	}
}
