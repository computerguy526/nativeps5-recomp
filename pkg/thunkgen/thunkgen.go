// SPDX-License-Identifier: GPL-2.0-or-later

// Package thunkgen generates the guest-to-host call thunks of HLE functions from per-function
// marshalling descriptors (SPEC §4.2, docs/m1/memory.md "HLE pointer translation").
//
// A thunk is entered from recompiled code through _recomp_call_shim with the complete guest
// context saved. It reads the System V x86-64 arguments from the saved context (six integer
// registers, then the guest stack), translates them to what a native AAPCS64 function expects,
// calls the HLE function, and writes the result back as the guest ABI defines.
//
// Pointer translation is the part that depends on the memory model: a guest pointer is
// host = guest + bias in base mode and host == guest in identity mode. Every translation goes
// through NPS5_G2H/NPS5_H2G (nps5_thunk.h), which are plain casts when the runtime is compiled
// with NPS5_ADDR_MODE_IDENTITY, so an identity build pays nothing: no shadow copies are made
// either, the guest structure is passed as it is.
//
// Pointers nested in memory cannot be translated by looking at registers. The descriptor names
// them: a PtrSlot is a pointer to a pointer the callee reads and may replace (void **addr); a
// StructArray is an array of structures whose pointer fields are listed by offset. In base
// mode the thunk passes a translated shadow copy and copies results back.
package thunkgen

import (
	"fmt"
	"sort"
	"strings"
)

// ArgKind says how one argument crosses the boundary.
type ArgKind int

const (
	// Ctx is not a guest argument: the HLE function receives the calling thread's GuestContext.
	// Only this runtime's own kernel functions use it (guest errno and thread identity live
	// there); it consumes no guest argument slot.
	Ctx ArgKind = iota
	// Scalar is an integer passed by value: converted to CType (truncation and sign extension
	// as the C conversion from uint64_t defines).
	Scalar
	// Ptr is a guest pointer to memory without nested pointers: translated, NULL preserved.
	Ptr
	// GuestAddr is an address that stays a guest value ("guest-address-as-value"): guest code
	// addresses (thread entry points, callbacks) and opaque guest words the HLE stores and hands
	// back. It is never translated, whatever CType says. Use it for every parameter of an
	// implementation that keys its own tables by guest address or translates through
	// GuestAddr/AgcSetGuestBias itself (AnyPS5's AGC driver: GPU command buffers, Packet::addr,
	// Shader*, docs/m1/memory.md "AnyPS5 integration"); a translated pointer there would be a host
	// address the driver cannot look up.
	GuestAddr
	// PtrSlot is a pointer to one guest pointer that the callee reads and may overwrite.
	PtrSlot
	// StructArray is a pointer to Count elements of Struct; pointer fields are translated in a
	// shadow copy. Read-only for the callee. Fields not listed in PtrFields are copied as guest
	// values (a guest address stored in such a field stays a guest address). The array is read
	// with nps5_guest_safe_read: an unreadable one reaches the callee as NPS5_BAD_GUEST_ARRAY
	// (EFAULT), an out-of-range count as NULL (EINVAL); neither crashes the host.
	StructArray
	// Float is a float or double passed by value in an xmm register (System V: the n-th
	// floating-point argument is xmm n, independent of the integer argument index). CType must be
	// "float" or "double".
	Float
)

// Struct describes a guest structure that holds pointers. Guest and host layouts are equal
// (both LP64 little-endian with the same alignment rules for the types used here); thunkgen
// rejects descriptors whose fields do not fit.
type Struct struct {
	CType     string // host C type, e.g. "struct iovec"
	Size      int    // sizeof, bytes
	PtrFields []int  // offsets of pointer fields
}

// Arg is one parameter of the HLE function, in declaration order.
type Arg struct {
	Kind  ArgKind
	CType string  // host parameter type
	Name  string  // parameter name, for the generated prototype and trace
	Elem  *Struct // StructArray
	Count int     // StructArray: index (in Args) of the element count
	Max   int     // StructArray: largest count the thunk marshals
}

// RetKind says how the result reaches RAX.
type RetKind int

const (
	RetVoid RetKind = iota
	// RetInt is a signed integer result, sign-extended to 64 bits.
	RetInt
	// RetPtr is a host pointer into guest memory, translated back; NULL stays 0.
	RetPtr
	// RetGuestAddr is a guest-valued address or opaque guest word the HLE hands back as it is
	// (a guest handle, a guest-address-valued out of a table keyed by guest addresses): no
	// translation, the bits go to RAX unchanged.
	RetGuestAddr
	// RetPtrOrFail is RetPtr where the all-ones value (MAP_FAILED) passes through untranslated.
	RetPtrOrFail
	// RetNoReturn marks a function that does not return to the guest.
	RetNoReturn
)

// Desc describes one HLE function.
type Desc struct {
	// Group names the generated file the thunk goes to: one file per implementation file, so a
	// host test that links part of the kernel links only the matching thunks.
	Group string
	Shim  string // thunk symbol the import table binds (emitter/shims.go), e.g. "shim_write"
	HLE   string // native function, e.g. "hle_write"
	CRet  string // host return type
	Ret   RetKind
	Args  []Arg
	Notes string // one line for the generated header
	// Pre is a C statement emitted first in the thunk (before any argument is read), e.g. a call that
	// brings a host library's view of guest memory up to date. Empty for the libkernel subset.
	Pre string
	// Post is a C statement emitted after the result is stored in RAX, before the thunk returns.
	Post string
	// Indirect: HLE names a function POINTER variable (`ret (*HLE)(params)`) the runtime fills with dlsym at
	// start-up, instead of a function the linker resolves (IndirectDefs). Used where the runtime defines C
	// functions of the same names as the library it routes to.
	Indirect bool
	// RouteNID marks an export that NPS5_GPU=kyty reroutes (kyty.go): the thunk calls through the function pointer
	// HLE. RouteTarget is the function that pointer is statically initialised with (empty for an Indirect
	// descriptor, whose pointer dlsym fills); RouteLib is the prx the game imports the NID from.
	RouteNID, RouteTarget, RouteLib string
	// Lean marks a leaf HLE function the guest may reach through the lean bridge (docs/m3/leanshim.md):
	// besides the thunk, a lean entry `lean_<shim>(g0..g5, ctx)` is generated that takes the six System V
	// integer argument registers and returns RAX. Only Ctx, Scalar, GuestAddr and Ptr arguments (at most six
	// guest arguments), no Pre/Post, and a result that is void or one integer/pointer qualify. The HLE function
	// must never enter guest code or switch execution context, and must not depend on the guest FPCR.
	Lean bool
}

// unmarshalable reports why a C type passed or returned by value cannot cross the boundary with
// the integer-register marshalling this generator has: floating point travels in xmm registers
// and aggregates by memory or register pairs, none of which the generated `(T)ctx->reg` reads.
// Pointers are fine whatever they point to. Until the descriptor language has FP and aggregate
// kinds (SPEC §4.2) such a descriptor is a translate-time error, not a silently wrong thunk.
func unmarshalable(ctype string) string {
	if strings.Contains(ctype, "*") {
		return ""
	}
	if strings.Contains(ctype, "...") {
		return "variadic"
	}
	for _, tok := range strings.FieldsFunc(ctype, func(r rune) bool {
		return !(r == '_' || r >= 'a' && r <= 'z' || r >= 'A' && r <= 'Z' || r >= '0' && r <= '9')
	}) {
		switch tok {
		case "float", "double", "_Float16", "__fp16", "_Complex", "__m64", "__m128", "__m128i", "__m128d", "__m256",
			"__m256i", "__m256d", "va_list", "__builtin_va_list":
			return "floating point or vector"
		case "__int128", "__int128_t", "__uint128_t", "_Float128", "__float128":
			return "128-bit integer (two registers)"
		case "struct", "union":
			return "aggregate by value"
		}
	}
	return ""
}

func (d *Desc) check() error {
	if d.Shim == "" || d.HLE == "" || d.Group == "" {
		return fmt.Errorf("descriptor without symbol names")
	}
	guest := 0
	for i, a := range d.Args {
		if a.CType == "" || a.Name == "" {
			return fmt.Errorf("%s: argument %d lacks a type or name", d.HLE, i)
		}
		if a.Kind != Ctx && a.Kind != Float {
			guest++
		}
		if (a.Kind == Ptr || a.Kind == PtrSlot) && strings.Contains(a.CType, "(*") {
			// A callback is guest code: the host cannot call a translated pointer to it. It crosses as
			// a guest value (GuestAddr) and is called through the runtime's guest-call path.
			return fmt.Errorf("%s: argument %q has function pointer type %q: a callback is a guest code address, declare it GuestAddr", d.HLE, a.Name, a.CType)
		}
		if a.Kind == Scalar || a.Kind == GuestAddr {
			if why := unmarshalable(a.CType); why != "" {
				return fmt.Errorf("%s: argument %q has type %q (%s): thunkgen cannot marshal it", d.HLE, a.Name, a.CType, why)
			}
		}
		if a.Kind == Float && a.CType != "float" && a.CType != "double" {
			return fmt.Errorf("%s: floating-point argument %q has type %q (float or double only)", d.HLE, a.Name, a.CType)
		}
		if a.Kind == Ctx && i != 0 {
			return fmt.Errorf("%s: the context argument must come first", d.HLE)
		}
		if a.Kind == StructArray {
			if a.Elem == nil || a.Max <= 0 || a.Count <= i || a.Count >= len(d.Args) || d.Args[a.Count].Kind != Scalar {
				return fmt.Errorf("%s: array argument %q needs an element type, a bound and a later scalar count", d.HLE, a.Name)
			}
			if a.Elem.Size <= 0 || a.Elem.Size%8 != 0 {
				return fmt.Errorf("%s: %s size %d is not a multiple of 8", d.HLE, a.Elem.CType, a.Elem.Size)
			}
			for _, off := range a.Elem.PtrFields {
				if off < 0 || off%8 != 0 || off+8 > a.Elem.Size {
					return fmt.Errorf("%s: %s pointer field at %d does not fit", d.HLE, a.Elem.CType, off)
				}
			}
		}
	}
	if d.Ret != RetVoid && d.Ret != RetNoReturn {
		if why := unmarshalable(d.CRet); why != "" {
			return fmt.Errorf("%s: return type %q (%s): thunkgen cannot marshal it", d.HLE, d.CRet, why)
		}
	}
	if (d.Ret == RetVoid || d.Ret == RetNoReturn) != (d.CRet == "void") {
		return fmt.Errorf("%s: return type %q does not match its return kind", d.HLE, d.CRet)
	}
	if d.Lean {
		if err := d.checkLean(guest); err != nil {
			return err
		}
	}
	return nil
}

// guestArg is the C expression of the i-th integer-class guest argument (System V x86-64).
func guestArg(i int) string {
	regs := [...]string{"rdi", "rsi", "rdx", "rcx", "r8", "r9"}
	if i < len(regs) {
		return "ctx->" + regs[i]
	}
	// [rsp] is the return address; stack arguments follow.
	return fmt.Sprintf("NPS5_GUEST_STACK_ARG(ctx, %d)", i-len(regs))
}

func prototype(d *Desc) string {
	var ps []string
	for _, a := range d.Args {
		sep := " "
		if strings.HasSuffix(a.CType, "*") {
			sep = ""
		}
		ps = append(ps, a.CType+sep+a.Name)
	}
	if len(ps) == 0 {
		ps = []string{"void"}
	}
	attr := ""
	if d.Ret == RetNoReturn {
		attr = "__attribute__((noreturn)) "
	}
	if d.Indirect {
		return fmt.Sprintf("extern %s (*%s)(%s);", d.CRet, d.HLE, strings.Join(ps, ", "))
	}
	if d.RouteTarget != "" {
		return fmt.Sprintf("%s %s(%s);\nextern %s (*%s)(%s);", d.CRet, d.RouteTarget, strings.Join(ps, ", "), d.CRet, d.HLE, strings.Join(ps, ", "))
	}
	return fmt.Sprintf("%s%s %s(%s);", attr, d.CRet, d.HLE, strings.Join(ps, ", "))
}

// IndirectDefs renders the definitions of the function-pointer variables of the Indirect descriptors and
// the function that fills them: bind(handle) = dlsym(handle, <symbol>) for each, returning the number of
// symbols that were not found (each is reported on stderr).
func IndirectDefs(bindName string, descs []Desc) string {
	var b strings.Builder
	sorted := append([]Desc(nil), descs...)
	sort.Slice(sorted, func(i, j int) bool { return sorted[i].HLE < sorted[j].HLE })
	var syms []string
	for i := range sorted {
		d := &sorted[i]
		if !d.Indirect {
			continue
		}
		var ps []string
		for _, a := range d.Args {
			if a.Kind != Ctx {
				ps = append(ps, a.CType)
			}
		}
		if len(ps) == 0 {
			ps = []string{"void"}
		}
		fmt.Fprintf(&b, "%s (*%s)(%s);\n", d.CRet, d.HLE, strings.Join(ps, ", "))
		syms = append(syms, d.HLE)
	}
	fmt.Fprintf(&b, "\nint %s(void *handle) {\n  int missing = 0;\n", bindName)
	for _, hle := range syms {
		sym := strings.TrimPrefix(hle, "aps5_fp_")
		fmt.Fprintf(&b, "  if (!(*(void **)&%s = dlsym(handle, \"%s\"))) { fprintf(stderr, \"[anyps5] missing export %s\\n\"); missing++; }\n", hle, sym, sym)
	}
	b.WriteString("  return missing;\n}\n")
	return b.String()
}

const banner = "// Code generated by thunkgen (go generate ./pkg/thunkgen/...). DO NOT EDIT.\n" +
	"// SPDX-License-Identifier: GPL-2.0-or-later\n"

// Header renders the prototypes of the HLE functions. The file implementing them includes it,
// so a descriptor that disagrees with the implementation is a compile error.
func Header(guard string, descs []Desc) (string, error) {
	var b strings.Builder
	b.WriteString(banner)
	fmt.Fprintf(&b, "//\n// Native (AAPCS64, host pointers) prototypes of the HLE functions reached through generated\n// thunks. Pointer parameters are host pointers into guest memory unless noted.\n\n")
	fmt.Fprintf(&b, "#ifndef %s\n#define %s\n\n#include <stddef.h>\n#include <stdint.h>\n#include <sys/types.h>\n#include <sys/uio.h>\n\n#include \"recomp_context.h\"\n\n", guard, guard)
	sorted := append([]Desc(nil), descs...)
	sort.Slice(sorted, func(i, j int) bool { return sorted[i].HLE < sorted[j].HLE })
	seen := map[string]bool{}
	for i := range sorted {
		d := &sorted[i]
		if err := d.check(); err != nil {
			return "", err
		}
		if seen[d.HLE] {
			continue
		}
		seen[d.HLE] = true
		if d.Notes != "" {
			fmt.Fprintf(&b, "// %s\n", d.Notes)
		}
		b.WriteString(prototype(d) + "\n")
	}
	fmt.Fprintf(&b, "\n#endif\n")
	return b.String(), nil
}

// Thunks renders the thunk functions.
func Thunks(headerName string, descs []Desc) (string, error) {
	var b strings.Builder
	b.WriteString(banner)
	fmt.Fprintf(&b, "//\n// SysV x86-64 guest call -> native HLE call. See pkg/thunkgen and docs/m1/memory.md.\n\n")
	fmt.Fprintf(&b, "#include \"nps5_thunk.h\"\n#include \"%s\"\n", headerName)
	sorted := append([]Desc(nil), descs...)
	sort.Slice(sorted, func(i, j int) bool { return sorted[i].Shim < sorted[j].Shim })
	seen := map[string]bool{}
	for i := range sorted {
		d := &sorted[i]
		if err := d.check(); err != nil {
			return "", err
		}
		if seen[d.Shim] {
			return "", fmt.Errorf("duplicate thunk %s", d.Shim)
		}
		seen[d.Shim] = true
		writeThunk(&b, d)
		if d.Lean {
			writeLeanThunk(&b, d)
		}
	}
	return b.String(), nil
}

func writeThunk(b *strings.Builder, d *Desc) {
	proto, _, _ := strings.Cut(prototype(d), "\n") // a routed descriptor has a second line (its pointer)
	fmt.Fprintf(b, "\n// %s\nvoid %s(GuestContext *ctx) {\n", strings.TrimSuffix(proto, ";"), d.Shim)
	if d.Pre != "" {
		fmt.Fprintf(b, "  %s\n", d.Pre)
	}
	var call []string
	var post []string // statements after the call (copy-back), base mode only where guarded
	g := 0            // guest integer-class argument index
	fp := 0           // guest floating-point argument index (xmm register)
	for i, a := range d.Args {
		v := fmt.Sprintf("a%d", i)
		switch a.Kind {
		case Ctx:
			call = append(call, "ctx")
			continue
		case Scalar:
			fmt.Fprintf(b, "  %s %s = (%s)%s;\n", a.CType, v, a.CType, guestArg(g))
		case GuestAddr:
			fmt.Fprintf(b, "  %s %s = (%s)%s;\n", a.CType, v, a.CType, guestArg(g))
		case Float:
			lane := "f32"
			if a.CType == "double" {
				lane = "f64"
			}
			fmt.Fprintf(b, "  %s %s = ctx->xmm[%d].%s[0];\n", a.CType, v, fp, lane)
			fp++
			call = append(call, v)
			continue
		case Ptr:
			fmt.Fprintf(b, "  %s%s = (%s)NPS5_G2H(ctx, %s);\n", ptrDecl(a.CType), v, a.CType, guestArg(g))
		case PtrSlot:
			// Base mode: the callee sees a host pointer in a host temporary; the guest slot is
			// rewritten with the translated result. Identity mode: the guest slot itself.
			fmt.Fprintf(b, "#if NPS5_ADDR_MODE_IDENTITY\n  %s%s = (%s)NPS5_G2H(ctx, %s);\n#else\n", ptrDecl(a.CType), v, a.CType, guestArg(g))
			fmt.Fprintf(b, "  uint64_t *g%d = (uint64_t *)NPS5_G2H(ctx, %s);\n", i, guestArg(g))
			fmt.Fprintf(b, "  void *t%d = g%d ? NPS5_G2H(ctx, *g%d) : NULL;\n  void *const to%d = t%d;\n", i, i, i, i, i)
			fmt.Fprintf(b, "  %s%s = g%d ? (%s)&t%d : NULL;\n#endif\n", ptrDecl(a.CType), v, i, a.CType, i)
			post = append(post, fmt.Sprintf("#if !NPS5_ADDR_MODE_IDENTITY\n  if (g%d && t%d != to%d) *g%d = NPS5_H2G(ctx, t%d); // only what the callee changed: an untouched slot keeps its guest value\n#endif\n", i, i, i, i, i))
		case StructArray:
			cnt := fmt.Sprintf("a%d", a.Count)
			fmt.Fprintf(b, "#if NPS5_ADDR_MODE_IDENTITY\n  %s%s = (%s)NPS5_G2H(ctx, %s);\n#else\n", ptrDecl(a.CType), v, a.CType, guestArg(g))
			// The count is declared later in the argument list; read it here for the copy.
			cg := guestIndexOf(d, a.Count)
			fmt.Fprintf(b, "  uint64_t ga%d = %s;\n", i, guestArg(g))
			fmt.Fprintf(b, "  long n%d = (long)(%s)%s;\n", i, d.Args[a.Count].CType, guestArg(cg))
			fmt.Fprintf(b, "  %s%s = NULL; // NULL: out-of-range count, the callee rejects it\n", ptrDecl(a.CType), v)
			fmt.Fprintf(b, "  uint64_t s%d[(ga%d && n%d > 0 && n%d <= %d ? n%d : 1) * %d];\n", i, i, i, i, a.Max, i, a.Elem.Size/8)
			fmt.Fprintf(b, "  if (ga%d && n%d > 0 && n%d <= %d) {\n", i, i, i, a.Max)
			// The array is copied with a non-faulting read: a guest array that is not readable is the
			// guest's error (the callee sees NPS5_BAD_GUEST_ARRAY and fails with EFAULT), not a host crash.
			fmt.Fprintf(b, "    if (!nps5_guest_safe_read(ga%d, s%d, (size_t)n%d * %d)) {\n      %s = (%s)NPS5_BAD_GUEST_ARRAY;\n    } else {\n", i, i, i, a.Elem.Size, v, a.CType)
			fmt.Fprintf(b, "    for (long k = 0; k < n%d; k++) {\n", i)
			for _, off := range a.Elem.PtrFields {
				fmt.Fprintf(b, "      s%d[k * %d + %d] = (uint64_t)(uintptr_t)NPS5_G2H(ctx, s%d[k * %d + %d]);\n", i, a.Elem.Size/8, off/8, i, a.Elem.Size/8, off/8)
			}
			fmt.Fprintf(b, "    }\n    %s = (%s)s%d;\n    }\n  }\n#endif\n", v, a.CType, i)
			_ = cnt
		}
		call = append(call, v)
		g++
	}
	invoke := fmt.Sprintf("%s(%s)", d.HLE, strings.Join(call, ", "))
	switch d.Ret {
	case RetNoReturn:
		fmt.Fprintf(b, "  %s;\n}\n", invoke)
		return
	case RetVoid:
		fmt.Fprintf(b, "  %s;\n", invoke)
	default:
		fmt.Fprintf(b, "  %s r = %s;\n", d.CRet, invoke)
	}
	for _, p := range post {
		b.WriteString(p)
	}
	switch d.Ret {
	case RetInt:
		b.WriteString("  ctx->rax = (uint64_t)(int64_t)r;\n")
	case RetGuestAddr:
		b.WriteString("  ctx->rax = (uint64_t)r;\n")
	case RetPtr:
		b.WriteString("  ctx->rax = NPS5_H2G(ctx, r);\n")
	case RetPtrOrFail:
		b.WriteString("  ctx->rax = (r == (void *)-1) ? UINT64_MAX : NPS5_H2G(ctx, r);\n")
	}
	if d.Post != "" {
		fmt.Fprintf(b, "  %s\n", d.Post)
	}
	b.WriteString("  SHIM_RETURN();\n}\n")
}

// ptrDecl renders a declaration prefix for a pointer type ("const void *" -> "const void *").
func ptrDecl(ctype string) string {
	if strings.HasSuffix(ctype, "*") {
		return ctype
	}
	return ctype + " "
}

// guestIndexOf returns the guest argument index of Args[i] (context arguments take no slot).
func guestIndexOf(d *Desc, i int) int {
	g := 0
	for k := 0; k < i; k++ {
		if d.Args[k].Kind != Ctx && d.Args[k].Kind != Float {
			g++
		}
	}
	return g
}

// checkLean rejects a Lean descriptor the lean bridge cannot serve: it passes six integer registers and
// returns RAX, and a lean function must be a plain leaf.
func (d *Desc) checkLean(guestArgs int) error {
	if d.Indirect || d.RouteNID != "" || d.Pre != "" || d.Post != "" {
		return fmt.Errorf("%s: a lean function cannot be Indirect and has no Pre/Post statements", d.HLE)
	}
	if guestArgs > 6 {
		return fmt.Errorf("%s: a lean function takes at most six guest arguments (the register arguments), has %d", d.HLE, guestArgs)
	}
	for _, a := range d.Args {
		switch a.Kind {
		case Ctx, Scalar, GuestAddr, Ptr:
		default:
			return fmt.Errorf("%s: lean function argument %q is not an integer or pointer (kind %d)", d.HLE, a.Name, a.Kind)
		}
	}
	if d.Ret == RetNoReturn {
		return fmt.Errorf("%s: a function that does not return cannot be lean", d.HLE)
	}
	return nil
}

// LeanName is the symbol of the lean entry of a shim: shim_foo -> lean_foo.
func LeanName(shim string) string { return "lean_" + strings.TrimPrefix(shim, "shim_") }

// LeanShim is one lean-marked shim: the emitter binds its stub to Lean instead of the full bridge and
// moves Args argument registers.
type LeanShim struct {
	Shim string
	Lean string
	Args int // guest integer arguments the function reads (0..6)
}

// LeanShims lists the lean descriptors of descs.
func LeanShims(descs []Desc) []LeanShim {
	var out []LeanShim
	for i := range descs {
		d := &descs[i]
		if !d.Lean {
			continue
		}
		out = append(out, LeanShim{Shim: d.Shim, Lean: LeanName(d.Shim), Args: guestIndexOf(d, len(d.Args))})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Shim < out[j].Shim })
	return out
}

// writeLeanThunk renders the lean entry of d: the same marshalling as writeThunk for the argument
// kinds a lean function may have, but reading its arguments from parameters instead of the saved context.
func writeLeanThunk(b *strings.Builder, d *Desc) {
	fmt.Fprintf(b, "\n// lean entry of %s\nuint64_t %s(uint64_t g0, uint64_t g1, uint64_t g2, uint64_t g3, uint64_t g4, uint64_t g5, GuestContext *ctx) {\n",
		d.Shim, LeanName(d.Shim))
	var call []string
	g := 0
	for i, a := range d.Args {
		v := fmt.Sprintf("a%d", i)
		src := fmt.Sprintf("g%d", g)
		switch a.Kind {
		case Ctx:
			call = append(call, "ctx")
			continue
		case Scalar, GuestAddr:
			fmt.Fprintf(b, "  %s %s = (%s)%s;\n", a.CType, v, a.CType, src)
		case Ptr:
			fmt.Fprintf(b, "  %s%s = (%s)NPS5_G2H(ctx, %s);\n", ptrDecl(a.CType), v, a.CType, src)
		}
		call = append(call, v)
		g++
	}
	for k := g; k < 6; k++ {
		fmt.Fprintf(b, "  (void)g%d;\n", k)
	}
	invoke := fmt.Sprintf("%s(%s)", d.HLE, strings.Join(call, ", "))
	switch d.Ret {
	case RetVoid:
		fmt.Fprintf(b, "  %s;\n  return ctx->rax; // a void function leaves RAX as it was\n}\n", invoke)
		return
	default:
		fmt.Fprintf(b, "  %s r = %s;\n", d.CRet, invoke)
	}
	switch d.Ret {
	case RetInt:
		b.WriteString("  return (uint64_t)(int64_t)r;\n")
	case RetGuestAddr:
		b.WriteString("  return (uint64_t)r;\n")
	case RetPtr:
		b.WriteString("  return NPS5_H2G(ctx, r);\n")
	case RetPtrOrFail:
		b.WriteString("  return (r == (void *)-1) ? UINT64_MAX : NPS5_H2G(ctx, r);\n")
	}
	b.WriteString("}\n")
}
