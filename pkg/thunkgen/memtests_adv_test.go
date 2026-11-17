// SPDX-License-Identifier: GPL-2.0-or-later

package thunkgen

// Adversarial tests of the thunk layer, written from SPEC 4.2 and docs/m1/memory.md section 7.
// KNOWN-FAIL policy (docs/m1/memtests.md): a test whose expectation the current implementation is
// known to violate calls knownFail after the violation is observed; that skips (main CI stays
// green) with the reason. MEMTESTS_STRICT=1 turns every knownFail into a failure.

import (
	"bufio"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// knownFail was the KNOWN-FAIL hook (docs/m1/memtests.md); every defect it covered is fixed
// (docs/m1/memory-fixes-2.md), so a violation is now always a failure.
func knownFail(t *testing.T, format string, args ...any) {
	t.Helper()
	t.Fatalf("violated: %s", fmt.Sprintf(format, args...))
}

var (
	ctxA   = Arg{Kind: Ctx, CType: "GuestContext *", Name: "ctx"}
	scalar = func(ct, n string) Arg { return Arg{Kind: Scalar, CType: ct, Name: n} }
	ptrA   = func(ct, n string) Arg { return Arg{Kind: Ptr, CType: ct, Name: n} }
)

func testDescs() []Desc {
	iov := &Struct{CType: "struct iovec", Size: 16, PtrFields: []int{0}}
	return []Desc{
		{Group: "t", Shim: "shim_ints", HLE: "hle_ints", CRet: "long", Ret: RetInt, Args: []Arg{ctxA,
			scalar("long", "a1"), scalar("int", "a2"), scalar("unsigned", "a3"), scalar("size_t", "a4"), scalar("uint16_t", "a5"),
			scalar("long", "a6"), scalar("int", "a7"), scalar("long", "a8"), scalar("unsigned char", "a9")}},
		{Group: "t", Shim: "shim_ptrs", HLE: "hle_ptrs", CRet: "int", Ret: RetInt, Args: []Arg{ctxA,
			ptrA("void *", "p0"), ptrA("const char *", "p1"), ptrA("void *", "p2"), ptrA("void *", "p3"), ptrA("void *", "p4"),
			ptrA("void *", "p5"), ptrA("void *", "p6"), ptrA("void *", "p7")}},
		{Group: "t", Shim: "shim_retptr", HLE: "hle_retptr", CRet: "void *", Ret: RetPtr, Args: []Arg{ctxA}},
		{Group: "t", Shim: "shim_retfail", HLE: "hle_retfail", CRet: "void *", Ret: RetPtrOrFail, Args: []Arg{ctxA}},
		{Group: "t", Shim: "shim_slot", HLE: "hle_slot", CRet: "int", Ret: RetInt, Args: []Arg{ctxA, {Kind: PtrSlot, CType: "void **", Name: "slot"}}},
		{Group: "t", Shim: "shim_iov", HLE: "hle_iov", CRet: "ssize_t", Ret: RetInt, Args: []Arg{ctxA,
			{Kind: StructArray, CType: "const struct iovec *", Name: "iov", Elem: iov, Count: 2, Max: 1024}, scalar("int", "n")}},
	}
}

func repoRuntimeDir(t *testing.T) string {
	t.Helper()
	d, err := filepath.Abs(filepath.Join("..", "runtime", "core"))
	if err != nil {
		t.Fatal(err)
	}
	return d
}

// TestThunkExecution generates thunks for the descriptor set above, links them to a C driver
// with HLE functions that record what they get, and runs it in both addressing modes.
func TestThunkExecution(t *testing.T) {
	if runtime.GOOS != "darwin" || runtime.GOARCH != "arm64" {
		t.Skip("host driver is built for arm64 macOS")
	}
	clang, err := exec.LookPath("clang")
	if err != nil {
		t.Skip("clang required")
	}
	descs := testDescs()
	hdr, err := Header("MT_HLE_H", descs)
	if err != nil {
		t.Fatal(err)
	}
	src, err := Thunks("hle.h", descs)
	if err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	for name, content := range map[string]string{"hle.h": hdr, "thunks.c": src} {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(content), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	for _, mode := range []struct {
		name string
		id   int
	}{{"base", 0}, {"identity", 1}} {
		t.Run(mode.name, func(t *testing.T) {
			bin := filepath.Join(dir, "drv-"+mode.name)
			cmd := exec.Command(clang, "-O1", "-g", "-Wall", "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-parameter",
				fmt.Sprintf("-DNPS5_ADDR_MODE_IDENTITY=%d", mode.id), fmt.Sprintf(`-DMODE="%s"`, mode.name),
				"-I"+dir, "-I"+filepath.Join("..", "..", "tests", "memtests"), "-I"+repoRuntimeDir(t),
				filepath.Join("..", "..", "tests", "memtests", "thunk_driver.c"), "-o", bin)
			if out, err := cmd.CombinedOutput(); err != nil {
				t.Fatalf("build: %v\n%s", err, out)
			}
			out, _ := exec.Command(bin).Output()
			n := 0
			sc := bufio.NewScanner(strings.NewReader(string(out)))
			for sc.Scan() {
				f := strings.SplitN(sc.Text(), "|", 5)
				if len(f) < 5 || f[0] != "RESULT" {
					continue
				}
				n++
				name, status, detail := f[1], f[3], f[4]
				t.Run(strings.TrimPrefix(name, "thunk."+mode.name+"."), func(t *testing.T) {
					switch status {
					case "PASS":
					case "KNOWN-FAIL":
						knownFail(t, "%s", detail)
					default:
						t.Errorf("%s: %s", status, detail)
					}
				})
			}
			if n == 0 {
				t.Fatalf("driver produced no results:\n%s", out)
			}
		})
	}
}

// unsupported marshalling classes must be rejected at generation time (SPEC 4.2: "thunkgen fails
// the build on any export it cannot classify") rather than silently marshalled as integers.
func TestRejectsUnclassifiableSignatures(t *testing.T) {
	cases := []struct {
		name string
		d    Desc
	}{
		{"double_arg", Desc{Group: "t", Shim: "s", HLE: "h", CRet: "int", Ret: RetInt, Args: []Arg{ctxA, scalar("double", "x")}}},
		{"float_arg", Desc{Group: "t", Shim: "s", HLE: "h", CRet: "int", Ret: RetInt, Args: []Arg{ctxA, scalar("float", "x")}}},
		{"double_return", Desc{Group: "t", Shim: "s", HLE: "h", CRet: "double", Ret: RetInt, Args: []Arg{ctxA}}},
		{"float_return", Desc{Group: "t", Shim: "s", HLE: "h", CRet: "float", Ret: RetInt, Args: []Arg{ctxA}}},
		{"long_double_arg", Desc{Group: "t", Shim: "s", HLE: "h", CRet: "int", Ret: RetInt, Args: []Arg{ctxA, scalar("long double", "x")}}},
		{"int128_arg", Desc{Group: "t", Shim: "s", HLE: "h", CRet: "int", Ret: RetInt, Args: []Arg{ctxA, scalar("__int128", "x")}}},
		{"struct_by_value_arg", Desc{Group: "t", Shim: "s", HLE: "h", CRet: "int", Ret: RetInt, Args: []Arg{ctxA, scalar("struct pair", "x")}}},
		{"small_struct_return", Desc{Group: "t", Shim: "s", HLE: "h", CRet: "struct pair", Ret: RetInt, Args: []Arg{ctxA}}},
		{"large_struct_return", Desc{Group: "t", Shim: "s", HLE: "h", CRet: "struct big", Ret: RetInt, Args: []Arg{ctxA}}},
		{"variadic", Desc{Group: "t", Shim: "s", HLE: "h", CRet: "int", Ret: RetInt, Args: []Arg{ctxA, ptrA("const char *", "fmt"), scalar("...", "rest")}}},
		{"va_list_arg", Desc{Group: "t", Shim: "s", HLE: "h", CRet: "int", Ret: RetInt, Args: []Arg{ctxA, ptrA("char *", "buf"), scalar("va_list", "ap")}}},
		{"callback_pointer_as_ptr", Desc{Group: "t", Shim: "s", HLE: "h", CRet: "void", Ret: RetVoid, Args: []Arg{ctxA, ptrA("int (*)(const void *, const void *)", "cmp")}}},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			_, err := Thunks("h.h", []Desc{c.d})
			if err == nil {
				knownFail(t, "thunkgen accepts %s and emits an integer-class marshalling for it (no float/aggregate/variadic/callback classifier exists yet; ADR section 7 'Not done')", c.name)
			}
		})
	}
}

// Integer-class robustness that thunkgen can check without compiling: argument counts beyond the
// registers must read the guest stack at consecutive slots; the hidden-pointer rule shifts them.
func TestStackArgumentIndexing(t *testing.T) {
	var args = []Arg{ctxA}
	for i := 0; i < 12; i++ {
		args = append(args, scalar("long", fmt.Sprintf("a%d", i)))
	}
	src, err := Thunks("h.h", []Desc{{Group: "t", Shim: "s", HLE: "h", CRet: "int", Ret: RetInt, Args: args}})
	if err != nil {
		t.Fatal(err)
	}
	for i := 6; i < 12; i++ {
		want := fmt.Sprintf("NPS5_GUEST_STACK_ARG(ctx, %d)", i-6)
		if !strings.Contains(src, want) {
			t.Errorf("argument %d does not read %s", i+1, want)
		}
	}
	if strings.Count(src, "ctx->r9") != 1 {
		t.Errorf("r9 used %d times", strings.Count(src, "ctx->r9"))
	}
}

// Descriptor validation: structurally wrong descriptors fail loudly.
func TestDescriptorValidation(t *testing.T) {
	iov := &Struct{CType: "struct iovec", Size: 16, PtrFields: []int{0}}
	bad := map[string]Desc{
		"ctx_not_first":       {Group: "g", Shim: "s", HLE: "h", CRet: "int", Ret: RetInt, Args: []Arg{scalar("int", "a"), ctxA}},
		"void_with_int":       {Group: "g", Shim: "s", HLE: "h", CRet: "int", Ret: RetVoid, Args: []Arg{ctxA}},
		"noreturn_with_int":   {Group: "g", Shim: "s", HLE: "h", CRet: "int", Ret: RetNoReturn, Args: []Arg{ctxA}},
		"int_with_void":       {Group: "g", Shim: "s", HLE: "h", CRet: "void", Ret: RetInt, Args: []Arg{ctxA}},
		"array_count_before":  {Group: "g", Shim: "s", HLE: "h", CRet: "int", Ret: RetInt, Args: []Arg{ctxA, scalar("int", "n"), {Kind: StructArray, CType: "const struct iovec *", Name: "iov", Elem: iov, Count: 1, Max: 4}}},
		"array_count_ptr":     {Group: "g", Shim: "s", HLE: "h", CRet: "int", Ret: RetInt, Args: []Arg{ctxA, {Kind: StructArray, CType: "const struct iovec *", Name: "iov", Elem: iov, Count: 2, Max: 4}, ptrA("void *", "n")}},
		"array_no_bound":      {Group: "g", Shim: "s", HLE: "h", CRet: "int", Ret: RetInt, Args: []Arg{ctxA, {Kind: StructArray, CType: "const struct iovec *", Name: "iov", Elem: iov, Count: 2}, scalar("int", "n")}},
		"struct_misaligned":   {Group: "g", Shim: "s", HLE: "h", CRet: "int", Ret: RetInt, Args: []Arg{ctxA, {Kind: StructArray, CType: "const struct x *", Name: "x", Elem: &Struct{CType: "struct x", Size: 12, PtrFields: []int{0}}, Count: 2, Max: 4}, scalar("int", "n")}},
		"ptr_field_outside":   {Group: "g", Shim: "s", HLE: "h", CRet: "int", Ret: RetInt, Args: []Arg{ctxA, {Kind: StructArray, CType: "const struct x *", Name: "x", Elem: &Struct{CType: "struct x", Size: 16, PtrFields: []int{16}}, Count: 2, Max: 4}, scalar("int", "n")}},
		"ptr_field_unaligned": {Group: "g", Shim: "s", HLE: "h", CRet: "int", Ret: RetInt, Args: []Arg{ctxA, {Kind: StructArray, CType: "const struct x *", Name: "x", Elem: &Struct{CType: "struct x", Size: 16, PtrFields: []int{4}}, Count: 2, Max: 4}, scalar("int", "n")}},
		"no_names":            {Args: []Arg{ctxA}},
		"unnamed_arg":         {Group: "g", Shim: "s", HLE: "h", CRet: "int", Ret: RetInt, Args: []Arg{ctxA, {Kind: Scalar, CType: "int"}}},
	}
	for name, d := range bad {
		t.Run(name, func(t *testing.T) {
			if _, err := Thunks("h.h", []Desc{d}); err == nil {
				t.Errorf("accepted")
			}
			if _, err := Header("G", []Desc{d}); err == nil {
				t.Errorf("Header accepted")
			}
		})
	}
	t.Run("duplicate_thunk", func(t *testing.T) {
		d := Desc{Group: "g", Shim: "s", HLE: "h", CRet: "int", Ret: RetInt, Args: []Arg{ctxA}}
		if _, err := Thunks("h.h", []Desc{d, d}); err == nil {
			t.Errorf("duplicate shim accepted")
		}
	})
	t.Run("array_count_scalar_must_be_integer", func(t *testing.T) {
		d := Desc{Group: "g", Shim: "s", HLE: "h", CRet: "int", Ret: RetInt, Args: []Arg{ctxA, {Kind: StructArray, CType: "const struct iovec *", Name: "iov", Elem: iov, Count: 2, Max: 4}, scalar("double", "n")}}
		if _, err := Thunks("h.h", []Desc{d}); err == nil {
			knownFail(t, "a floating-point count is accepted as an array bound")
		}
	})
}
