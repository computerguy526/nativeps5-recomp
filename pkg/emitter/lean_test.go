package emitter

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"ps4-recomp/pkg/lifter"
	"ps4-recomp/pkg/thunkgen"
)

// registerLeanTestShims marks the test shims of testdata/boundary/harness.c lean (their lean entries
// are defined there), with the number of argument registers each reads.
func registerLeanTestShims() {
	for shim, n := range map[string]int{
		"shim_test_lean_mix": 6, "shim_test_lean_wait": 0, "shim_test_lean_bad_x19": 2, "shim_test_lean_bad_x28": 2,
		"shim_test_lean_bad_x29": 2, "shim_test_lean_bad_d9": 2, "shim_test_lean_bad_callback": 0, "shim_test_lean_fault": 1,
	} {
		leanTable[shim] = thunkgen.LeanShim{Shim: shim, Lean: thunkgen.LeanName(shim), Args: n}
	}
}

func withFlat(t *testing.T) {
	t.Helper()
	old := lifter.GetCallModel()
	lifter.SetCallModel(lifter.CallFlat)
	t.Cleanup(func() { lifter.SetCallModel(old) })
}

// A lean stub is the call entry and jump entry of any flat stub, then the lean path, then the full
// bridge as the fallback the runtime switch takes.
func TestLeanShimStub(t *testing.T) {
	withFlat(t)
	var b strings.Builder
	writeShimStub(&b, "_fn_0x1000", "shim_memcpy", 0x1000)
	out := b.String()
	lines := strings.Split(strings.TrimSpace(out), "\n")
	want := append([]string{".globl _fn_0x1000", "_fn_0x1000:"}, lifter.FlatCallEntry()...)
	for i, w := range want {
		if i >= len(lines) || lines[i] != w {
			t.Fatalf("stub line %d = %q, want %q\n%s", i, lines[i], w, out)
		}
	}
	for _, need := range []string{"_g_nps5_lean_mode@PAGE", "cbz  w7, 1f", "mov  x0, x24", "mov  x1, x23", "mov  x2, x22",
		"stp  x19, x21, [x16, #0x00]", "_lean_memcpy@PAGE", "b    _recomp_call_lean", "adrp x0, _shim_memcpy@PAGE", "b    _recomp_call_shim"} {
		if !strings.Contains(out, need) {
			t.Errorf("lean stub lacks %q:\n%s", need, out)
		}
	}
	if strings.Contains(out, "mov  x3,") {
		t.Errorf("memcpy reads three arguments, the stub moves more:\n%s", out)
	}
}

// The nested call model has no lean bridge: a shim that is lean in the flat model gets the plain stub.
func TestLeanNeedsFlatModel(t *testing.T) {
	old := lifter.GetCallModel()
	lifter.SetCallModel(lifter.CallNested)
	defer lifter.SetCallModel(old)
	var b strings.Builder
	writeShimStub(&b, "_fn_0x1000", "shim_memcpy", 0x1000)
	if strings.Contains(b.String(), "lean") {
		t.Errorf("nested model stub uses the lean bridge:\n%s", b.String())
	}
}

// Every lean shim names a thunk or shim the runtime defines, with at most six register arguments.
func TestLeanTableWellFormed(t *testing.T) {
	for _, l := range LeanShimNames() {
		if l.Args < 0 || l.Args > 6 || !strings.HasPrefix(l.Shim, "shim_") || l.Lean != "lean_"+strings.TrimPrefix(l.Shim, "shim_") {
			t.Errorf("malformed lean entry %+v", l)
		}
	}
}

// The weak lean_* fallback (a runtime built without the lean module) used to run the full shim and return rax, silently losing
// writes to every other register. It must set the argument registers, undo the shim's return pop, and refuse a shim that
// changed registers the lean return does not carry. The text must also be valid C against the real context header.
func TestLeanWeakFallback(t *testing.T) {
	c := leanWeakFallback("lean_memcpy", "shim_memcpy")
	for _, need := range []string{"ctx->rdi = a0", "ctx->r9 = a5", "shim_memcpy(ctx);", "ctx->rsp = sp;", "ctx->rdx != rdx", "ctx->rbx != rbx",
		"ctx->r15 != r15", "ctx->xmm[0].u64[0] != x0", "recomp_unimplemented_shim", "return ctx->rax;"} {
		if !strings.Contains(c, need) {
			t.Errorf("fallback lacks %q:\n%s", need, c)
		}
	}
	clang, err := exec.LookPath("clang")
	if err != nil {
		return
	}
	src := "#include <stdint.h>\n#include \"recomp_context.h\"\nvoid recomp_unimplemented_shim(GuestContext *c, const char *n);\nvoid shim_memcpy(GuestContext *c);\n" + c
	f := filepath.Join(t.TempDir(), "fb.c")
	if err := os.WriteFile(f, []byte(src), 0o644); err != nil {
		t.Fatal(err)
	}
	out, err := exec.Command(clang, "-fsyntax-only", "-I../runtime/core", f).CombinedOutput()
	if err != nil {
		t.Fatalf("fallback does not compile: %v\n%s", err, out)
	}
}
