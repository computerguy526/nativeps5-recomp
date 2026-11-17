// SPDX-License-Identifier: GPL-2.0-or-later

package thunkgen

import (
	"strings"
	"testing"
)

func anyps5Desc(t *testing.T, name string) Desc {
	t.Helper()
	descs, err := AnyPS5Descs()
	if err != nil {
		t.Fatal(err)
	}
	for _, d := range descs {
		if d.HLE == name || d.RouteTarget == name {
			return d
		}
	}
	t.Fatalf("no descriptor for %s", name)
	return Desc{}
}

func TestAnyPS5Descriptors(t *testing.T) {
	descs, err := AnyPS5Descs()
	if err != nil {
		t.Fatal(err)
	}
	if len(descs) < 1000 {
		t.Fatalf("only %d descriptors", len(descs))
	}
	if _, err := Thunks("x.h", descs); err != nil {
		t.Fatal(err)
	}
	if _, err := Header("X_H", descs); err != nil {
		t.Fatal(err)
	}
	// Packet is an HLE structure (translated); Packet::addr inside it stays guest (never touched).
	for _, a := range anyps5Desc(t, "sceAgcDriverSubmitDcb").Args {
		if a.Kind != Ptr {
			t.Errorf("sceAgcDriverSubmitDcb: %+v, want a translated pointer", a)
		}
	}
	// A ring base is a guest address the driver keys by: it must not be translated.
	tf := anyps5Desc(t, "sceAgcDriverSetTFRing")
	if tf.Args[0].Kind != GuestAddr || tf.Args[1].Kind != Scalar {
		t.Errorf("sceAgcDriverSetTFRing: %+v", tf.Args)
	}
	// Command-buffer cursors come back as guest addresses.
	if d := anyps5Desc(t, "sceAgcDcbSetFlip"); d.Ret != RetPtr && d.Ret != RetInt {
		t.Errorf("sceAgcDcbSetFlip result %v", d.Ret)
	}
}

func TestFloatArguments(t *testing.T) {
	d := anyps5Desc(t, "sceAgcGetGsOversubscription")
	src, err := Thunks("x.h", []Desc{d})
	if err != nil {
		t.Fatal(err)
	}
	// the 4th guest argument is an integer one (rcx), the float is the first xmm argument
	for _, want := range []string{"float a3 = ctx->xmm[0].f32[0];", "uint32_t a2 = (uint32_t)ctx->rdx;", "nps5_anyps5_enter(ctx, \"sceAgcGetGsOversubscription\");", "nps5_anyps5_leave(ctx, \"sceAgcGetGsOversubscription\");"} {
		if !strings.Contains(src, want) {
			t.Errorf("thunk lacks %q:\n%s", want, src)
		}
	}
	// two floats and integers interleaved: float index and integer index count separately
	d2 := Desc{Group: "t", Shim: "shim_f", HLE: "hle_f", CRet: "int", Ret: RetInt, Args: []Arg{
		sc("int", "a"), {Kind: Float, CType: "float", Name: "x"}, sc("int", "b"), {Kind: Float, CType: "double", Name: "y"}, sc("int", "c")}}
	src, err = Thunks("x.h", []Desc{d2})
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"int a0 = (int)ctx->rdi;", "float a1 = ctx->xmm[0].f32[0];", "int a2 = (int)ctx->rsi;", "double a3 = ctx->xmm[1].f64[0];", "int a4 = (int)ctx->rdx;"} {
		if !strings.Contains(src, want) {
			t.Errorf("missing %q:\n%s", want, src)
		}
	}
	if _, err := Header("X_H", []Desc{{Group: "t", Shim: "s", HLE: "h", CRet: "int", Ret: RetInt, Args: []Arg{{Kind: Float, CType: "int", Name: "x"}}}}); err == nil {
		t.Error("a Float argument of integer type was accepted")
	}
}
