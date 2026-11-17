// SPDX-License-Identifier: GPL-2.0-or-later

package thunkgen

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The generated sources are checked in (the runtime's host tests compile them without running
// the translator); this keeps them in step with the descriptors.
func TestGeneratedSourcesAreCurrent(t *testing.T) {
	hdr, err := Header("NPS5_LIBKERNEL_HLE_H", Libkernel)
	if err != nil {
		t.Fatal(err)
	}
	files := map[string]string{LibkernelHeader: hdr}
	groups := Groups(Libkernel)
	if len(groups) != 4 {
		t.Fatalf("groups = %d", len(groups))
	}
	for group, descs := range groups {
		src, err := Thunks(filepath.Base(LibkernelHeader), descs)
		if err != nil {
			t.Fatal(err)
		}
		files[LibkernelThunkFile(group)] = src
	}
	for path, want := range files {
		got, err := os.ReadFile(filepath.Join("..", "runtime", path))
		if err != nil {
			t.Fatal(err)
		}
		if string(got) != want {
			t.Errorf("pkg/runtime/%s is stale: run `go run ./pkg/thunkgen/gen` from the repository root", path)
		}
	}
}

func TestThunkMarshalling(t *testing.T) {
	iov := &Struct{CType: "struct iovec", Size: 16, PtrFields: []int{0}}
	d := []Desc{{
		Group: "g", Shim: "shim_t", HLE: "hle_t", CRet: "void *", Ret: RetPtrOrFail,
		Args: []Arg{
			{Kind: Ctx, CType: "GuestContext *", Name: "ctx"},
			{Kind: Scalar, CType: "int", Name: "fd"},
			{Kind: StructArray, CType: "const struct iovec *", Name: "iov", Elem: iov, Count: 3, Max: 8},
			{Kind: Scalar, CType: "int", Name: "n"},
			{Kind: Ptr, CType: "const char *", Name: "s"},
			{Kind: GuestAddr, CType: "uint64_t", Name: "fn"},
			{Kind: PtrSlot, CType: "void **", Name: "out"},
			{Kind: Scalar, CType: "size_t", Name: "len"},
			{Kind: Scalar, CType: "off_t", Name: "off"},
		},
	}}
	src, err := Thunks("h.h", d)
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{
		"int a1 = (int)ctx->rdi;",                        // first guest argument: the context takes no slot
		"NPS5_G2H(ctx, s2[k * 2 + 0])",                   // iov_base translated in the shadow copy
		"nps5_guest_safe_read(ga2, s2, (size_t)n2 * 16)", // the guest array is read without faulting
		"(const struct iovec *)NPS5_BAD_GUEST_ARRAY",     // unreadable: EFAULT in the callee, not a host crash
		"long n2 = (long)(int)ctx->rdx;",                 // the count read ahead of its own declaration
		"n2 <= 8",                                        // bounded
		"const char *a4 = (const char *)NPS5_G2H(ctx, ctx->rcx);",
		"uint64_t a5 = (uint64_t)ctx->r8;",                  // guest address: untouched
		"if (g6 && t6 != to6) *g6 = NPS5_H2G(ctx, t6);",     // slot written back (only when changed), translated
		"size_t a7 = (size_t)NPS5_GUEST_STACK_ARG(ctx, 0);", // seventh integer argument: guest stack
		"off_t a8 = (off_t)NPS5_GUEST_STACK_ARG(ctx, 1);",
		"(r == (void *)-1) ? UINT64_MAX : NPS5_H2G(ctx, r)",
		"#if NPS5_ADDR_MODE_IDENTITY\n  const struct iovec *a2 = (const struct iovec *)NPS5_G2H(ctx, ctx->rsi);", // identity: no copy
	} {
		if !strings.Contains(src, want) {
			t.Errorf("thunk lacks %q\n%s", want, src)
		}
	}
}

func TestDescriptorChecks(t *testing.T) {
	bad := []Desc{
		{Group: "g", Shim: "s", HLE: "h", CRet: "int", Ret: RetVoid},
		{Group: "g", Shim: "s", HLE: "h", CRet: "void", Ret: RetVoid, Args: []Arg{{Kind: Scalar, CType: "int", Name: "a"}, {Kind: Ctx, CType: "GuestContext *", Name: "ctx"}}},
		{Group: "g", Shim: "s", HLE: "h", CRet: "void", Ret: RetVoid, Args: []Arg{{Kind: StructArray, CType: "const struct iovec *", Name: "v", Elem: &Struct{CType: "x", Size: 12}, Count: 1, Max: 4}, {Kind: Scalar, CType: "int", Name: "n"}}},
		{Group: "g", Shim: "s", HLE: "h", CRet: "void", Ret: RetVoid, Args: []Arg{{Kind: StructArray, CType: "const struct iovec *", Name: "v", Elem: &Struct{CType: "x", Size: 16, PtrFields: []int{12}}, Count: 1, Max: 4}, {Kind: Scalar, CType: "int", Name: "n"}}},
		{Group: "g", Shim: "s", HLE: "h", CRet: "void", Ret: RetVoid, Args: []Arg{{Kind: Scalar, CType: "", Name: "a"}}},
	}
	for i := range bad {
		if _, err := Thunks("h.h", bad[i:i+1]); err == nil {
			t.Errorf("descriptor %d accepted", i)
		}
	}
	if _, err := Thunks("h.h", append([]Desc{}, Libkernel[0], Libkernel[0])); err == nil {
		t.Error("duplicate thunk accepted")
	}
}

// Types the integer-register marshalling cannot carry must be refused at translate time, not
// generated as `(double)ctx->rdi` (review 1, m9).
func TestUnmarshalableTypesRejected(t *testing.T) {
	scalar := func(ct string) Desc {
		return Desc{Group: "g", Shim: "s", HLE: "h", CRet: "void", Ret: RetVoid, Args: []Arg{{Kind: Scalar, CType: ct, Name: "a"}}}
	}
	ret := func(ct string) Desc {
		return Desc{Group: "g", Shim: "s", HLE: "h", CRet: ct, Ret: RetInt}
	}
	for name, d := range map[string]Desc{
		"double arg":       scalar("double"),
		"float arg":        scalar("const float"),
		"long double arg":  scalar("long double"),
		"struct by value":  scalar("struct big"),
		"union by value":   scalar("union u"),
		"vector arg":       scalar("__m128"),
		"va_list arg":      scalar("va_list"),
		"double result":    ret("double"),
		"struct result":    ret("struct big"),
		"guest addr float": {Group: "g", Shim: "s", HLE: "h", CRet: "void", Ret: RetVoid, Args: []Arg{{Kind: GuestAddr, CType: "double", Name: "a"}}},
	} {
		if _, err := Thunks("h.h", []Desc{d}); err == nil {
			t.Errorf("%s accepted", name)
		}
	}
	// Pointers to such types, and plain integers, stay fine.
	for _, ct := range []string{"struct stat *", "const double *", "uint64_t", "unsigned long long", "int"} {
		if _, err := Thunks("h.h", []Desc{scalar(ct)}); err != nil {
			t.Errorf("%q refused: %v", ct, err)
		}
	}
}

// "Guest-address-as-value" (AnyPS5 AGC entry points, docs/m1/memory.md "AnyPS5 integration"): a
// GuestAddr argument, a GuestAddr-typed pointer parameter, an unlisted struct field and a
// RetGuestAddr result are never translated, in either mode; a Ptr next to them still is.
func TestGuestAddrAsValueIsNeverTranslated(t *testing.T) {
	pkt := &Struct{CType: "struct agc_packet", Size: 16, PtrFields: nil} // addr is a guest value: not listed
	d := []Desc{{
		Group: "g", Shim: "shim_agc", HLE: "hle_agc", CRet: "uint64_t", Ret: RetGuestAddr,
		Args: []Arg{
			{Kind: Ctx, CType: "GuestContext *", Name: "ctx"},
			{Kind: GuestAddr, CType: "uint64_t", Name: "cmdbuf"},
			{Kind: GuestAddr, CType: "const void *", Name: "shader"}, // pointer-typed, still a guest value
			{Kind: Ptr, CType: "void *", Name: "scratch"},
			{Kind: StructArray, CType: "const struct agc_packet *", Name: "pk", Elem: pkt, Count: 5, Max: 64},
			{Kind: Scalar, CType: "int", Name: "n"},
		},
	}}
	src, err := Thunks("h.h", d)
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{
		"uint64_t a1 = (uint64_t)ctx->rdi;",
		"const void * a2 = (const void *)ctx->rsi;",
		"void *a3 = (void *)NPS5_G2H(ctx, ctx->rdx);",    // an ordinary pointer next to them is translated
		"nps5_guest_safe_read(ga4, s4, (size_t)n4 * 16)", // unlisted fields arrive as the guest values
		"ctx->rax = (uint64_t)r;",                        // result untranslated
	} {
		if !strings.Contains(src, want) {
			t.Errorf("thunk lacks %q\n%s", want, src)
		}
	}
	for _, bad := range []string{"NPS5_G2H(ctx, ctx->rdi)", "NPS5_G2H(ctx, ctx->rsi)", "NPS5_H2G(ctx, r)", "NPS5_G2H(ctx, s4["} {
		if strings.Contains(src, bad) {
			t.Errorf("thunk translates a guest value: found %q\n%s", bad, src)
		}
	}
	// a callback is a guest address: accepted as GuestAddr, refused as a (translated) Ptr
	cb := "int (*)(const void *, const void *)"
	if _, err := Thunks("h.h", []Desc{{Group: "g", Shim: "s", HLE: "h", CRet: "void", Ret: RetVoid,
		Args: []Arg{{Kind: Ctx, CType: "GuestContext *", Name: "ctx"}, {Kind: GuestAddr, CType: cb, Name: "cmp"}}}}); err != nil {
		t.Errorf("function pointer as GuestAddr refused: %v", err)
	}
}
