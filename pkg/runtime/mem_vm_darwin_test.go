// SPDX-License-Identifier: GPL-2.0-or-later

package recompruntime

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"
)

// TestGuestVM builds and runs tests/memory_vm_test/vm_test.c: the VM extent manager
// (core/nps5_vm.c) and the direct-memory, flexible-memory and pool entry points
// (kernel/memory/ps4_direct_mem.c) over the real arena. It is the regression suite for the
// review findings fixed in docs/m1/memory-fixes-1.md.
func TestGuestVM(t *testing.T) {
	if runtime.GOARCH != "arm64" || runtime.GOOS != "darwin" {
		t.Skip("the guest arena is defined for arm64 macOS")
	}
	clang, err := exec.LookPath("clang")
	if err != nil {
		t.Skip("clang is required")
	}
	var inc []string
	for _, d := range []string{"core", "kernel/memory", "kernel/syscalls", "kernel/vfs", "kernel/threading", "modules/libkernel"} {
		inc = append(inc, "-I"+d)
	}
	bin := filepath.Join(t.TempDir(), "vm_test")
	args := append([]string{"-g", "-O1", "-w"}, inc...)
	args = append(args, "../../tests/memory_vm_test/vm_test.c", "../../tests/memory_vm_test/stubs.c",
		"core/nps5_vm.c", "core/nps5_mem.c", "kernel/memory/ps4_direct_mem.c", "-Wl,-undefined,dynamic_lookup", "-o", bin)
	if out, err := exec.Command(clang, args...).CombinedOutput(); err != nil {
		t.Fatalf("build: %v\n%s", err, out)
	}
	cmd := exec.Command(bin)
	done := make(chan struct{})
	var out []byte
	go func() { out, err = cmd.CombinedOutput(); close(done) }()
	select {
	case <-done:
	case <-time.After(120 * time.Second):
		_ = cmd.Process.Kill()
		t.Fatal("vm test timed out")
	}
	if err != nil || !strings.Contains(string(out), "VM OK") {
		t.Fatalf("vm test: %v\n%s", err, out)
	}
}

// TestAstroMaps builds and runs tests/memory_vm_test/astro_maps.c: the memory shape of ASTRO BOT (PPSA21564) on arm64
// macOS in base address mode. 2 GiB at the fixed guest address 0x300000000, 3.2 GB of direct memory at the fixed
// guest address 0x1000000000, and flexible_memory_size 343,932,928 bytes.
func TestAstroMaps(t *testing.T) {
	if runtime.GOARCH != "arm64" || runtime.GOOS != "darwin" {
		t.Skip("the guest arena is defined for arm64 macOS")
	}
	clang, err := exec.LookPath("clang")
	if err != nil {
		t.Skip("clang is required")
	}
	var inc []string
	for _, d := range []string{"core", "kernel/memory", "kernel/syscalls", "kernel/vfs", "kernel/threading", "modules/libkernel"} {
		inc = append(inc, "-I"+d)
	}
	bin := filepath.Join(t.TempDir(), "astro_maps")
	args := append([]string{"-g", "-O1", "-w"}, inc...)
	args = append(args, "../../tests/memory_vm_test/astro_maps.c", "../../tests/memory_vm_test/stubs.c",
		"core/nps5_vm.c", "core/nps5_mem.c", "kernel/memory/ps4_direct_mem.c", "-Wl,-undefined,dynamic_lookup", "-o", bin)
	if out, err := exec.Command(clang, args...).CombinedOutput(); err != nil {
		t.Fatalf("build: %v\n%s", err, out)
	}
	out, err := exec.Command(bin).CombinedOutput()
	if err != nil || !strings.Contains(string(out), "ASTRO MAPS OK") {
		t.Fatalf("astro maps test: %v\n%s", err, out)
	}
}

// TestDiagFaultResolver: a fault a registered resolver claims (the AGC driver's write watch) is not a crash; an
// unclaimed one still is (docs/m3/gpu-followups.md, task 3).
func TestDiagFaultResolver(t *testing.T) {
	if runtime.GOARCH != "arm64" || runtime.GOOS != "darwin" {
		t.Skip("the guest arena is defined for arm64 macOS")
	}
	clang, err := exec.LookPath("clang")
	if err != nil {
		t.Skip("clang is required")
	}
	bin := filepath.Join(t.TempDir(), "resolver_test")
	args := []string{"-g", "-O1", "-w", "-Icore", "-Ikernel/memory", "-Ikernel/syscalls", "-Ikernel/vfs", "-Ikernel/threading",
		"../../tests/memory_vm_test/resolver_test.c", "core/nps5_diag.c", "-Wl,-undefined,dynamic_lookup", "-o", bin}
	if out, err := exec.Command(clang, args...).CombinedOutput(); err != nil {
		t.Fatalf("build: %v\n%s", err, out)
	}
	env := append(os.Environ(), "NPS5_NO_BUNDLE=1", "NPS5_LOG=off")
	cmd := exec.Command(bin)
	cmd.Env = env
	if out, err := cmd.CombinedOutput(); err != nil || !strings.Contains(string(out), "RESOLVER OK") {
		t.Fatalf("claimed faults: %v\n%s", err, out)
	}
	cmd = exec.Command(bin, "wild")
	cmd.Env = env
	out, err := cmd.CombinedOutput()
	if err == nil || strings.Contains(string(out), "FAIL") {
		t.Fatalf("an unclaimed fault must still kill the process: %v\n%s", err, out)
	}
}

// TestHostAccessHooks: what the runtime owes a library that write-protects or read-traps guest pages (the AGC write
// watch, docs/m3/writewatch.md): faulting stores from many threads, loads that wait in the handler for another thread,
// system calls into protected pages, and that a fault inside the resolver or an unclaimed fault still ends the process.
func TestHostAccessHooks(t *testing.T) {
	if runtime.GOARCH != "arm64" || runtime.GOOS != "darwin" {
		t.Skip("the guest arena is defined for arm64 macOS")
	}
	clang, err := exec.LookPath("clang")
	if err != nil {
		t.Skip("clang is required")
	}
	bin := filepath.Join(t.TempDir(), "watch_test")
	args := []string{"-g", "-O1", "-w", "-Icore", "-Ikernel/memory", "-Ikernel/syscalls", "-Ikernel/vfs", "-Ikernel/threading",
		"../../tests/memory_vm_test/watch_test.c", "core/nps5_diag.c", "core/nps5_mem.c", "-Wl,-undefined,dynamic_lookup", "-o", bin}
	if out, err := exec.Command(clang, args...).CombinedOutput(); err != nil {
		t.Fatalf("build: %v\n%s", err, out)
	}
	env := append(os.Environ(), "NPS5_NO_BUNDLE=1", "NPS5_LOG=off")
	run := func(arg ...string) (string, error, time.Duration) {
		cmd := exec.Command(bin, arg...)
		cmd.Env = env
		start := time.Now()
		out, err := cmd.CombinedOutput()
		return string(out), err, time.Since(start)
	}
	for i := 0; i < 5; i++ {
		if out, err, _ := run(); err != nil || !strings.Contains(out, "WATCH OK") {
			t.Fatalf("run %d: %v\n%s", i, err, out)
		}
	}
	for _, mode := range []string{"wild", "nested", "livelock"} {
		out, err, took := run(mode)
		if mode == "livelock" && !strings.Contains(out, "resolver livelock") {
			t.Fatalf("livelock: no diagnosis in the output:\n%s", out)
		}
		if err == nil || strings.Contains(out, "FAIL") {
			t.Fatalf("%s: the process must die: %v\n%s", mode, err, out)
		}
		if took > 10*time.Second {
			t.Fatalf("%s: took %v to die", mode, took)
		}
	}
}

// TestCheckedPointerTranslation: NPS5_CHECKED_PTR=1 turns a wild guest pointer in the HLE
// translation into a diagnosed stop; the default build stays an unchecked add (m5 of review 1).
func TestCheckedPointerTranslation(t *testing.T) {
	if runtime.GOARCH != "arm64" || runtime.GOOS != "darwin" {
		t.Skip("the guest arena is defined for arm64 macOS")
	}
	clang, err := exec.LookPath("clang")
	if err != nil {
		t.Skip("clang is required")
	}
	build := func(name string, extra ...string) string {
		bin := filepath.Join(t.TempDir(), name)
		args := append([]string{"-O1", "-w", "-Icore"}, extra...)
		args = append(args, "../../tests/memory_vm_test/ptrcheck.c", "core/nps5_mem.c", "-o", bin)
		if out, err := exec.Command(clang, args...).CombinedOutput(); err != nil {
			t.Fatalf("build %s: %v\n%s", name, err, out)
		}
		return bin
	}
	run := func(bin, what string) (string, error) {
		out, err := exec.Command(bin, what).CombinedOutput()
		return string(out), err
	}
	checked, plain := build("checked", "-DNPS5_CHECKED_PTR=1"), build("plain")
	for _, bin := range []string{checked, plain} {
		if out, err := run(bin, "good"); err != nil {
			t.Fatalf("valid pointer and NULL through %s: %v\n%s", filepath.Base(bin), err, out)
		}
	}
	for _, what := range []string{"wild", "neg"} {
		out, err := run(checked, what)
		if err == nil || !strings.Contains(out, "outside the guest window") {
			t.Errorf("checked build, %s pointer: err=%v out=%q", what, err, out)
		}
		if out, err := run(plain, what); err != nil || !strings.Contains(out, "returned") {
			t.Errorf("default build, %s pointer must stay unchecked: err=%v out=%q", what, err, out)
		}
	}
}
