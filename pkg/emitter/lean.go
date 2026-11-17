package emitter

import (
	"fmt"
	"io"
	"os"

	"ps4-recomp/pkg/lifter"
	"ps4-recomp/pkg/thunkgen"
)

// Lean shims (docs/m3/leanshim.md): leaf host functions that guest code reaches through a short
// bridge instead of _recomp_call_shim. A stub of a lean shim does what the full bridge does up to
// the point where it leaves guest code (the 16 GPRs, ctx->rip and the shadow stack pointer are
// stored), then maps the System V argument registers straight to AAPCS64 argument registers and
// branches to _recomp_call_lean. The function is `uint64_t lean_<name>(a0..a5, GuestContext *ctx)`
// (nps5_lean.h). The runtime switch g_nps5_lean_mode (NPS5_LEAN=0) sends every stub to the full
// bridge instead; so does the nested call model, which has no lean bridge.

// leanHandwritten lists the lean variants of hand-written shims (kernel/syscalls/ps4_syscalls.c,
// core/nps5_lean.c, modules/libc) with the number of integer arguments each reads. The generated
// thunks of libkernel declare theirs with Desc.Lean (thunkgen); both are merged by leanInfo.
// Every entry needs a `lean_<name>` definition in the runtime; the link fails without one.
var leanHandwritten = map[string]int{
	// kernel/syscalls: the memcpy family that every module shares
	"shim_memcpy": 3, "shim_memmove": 3, "shim_memset": 3, "shim_strlen": 1,
	"shim_strcpy": 2, "shim_strncpy": 3, "shim_strcmp": 2,
	// libc heap
	"shim_malloc": 1, "shim_calloc": 2, "shim_realloc": 2, "shim_free": 1,
	"shim_aligned_alloc": 2, "shim_memalign": 2, "shim_malloc_usable_size": 1,
	// UE FMallocBinned3 replacement (modules/libSceUeMalloc, -hle-malloc ue5-binned3); a0 is `this`
	"shim_ue5_malloc_malloc": 3, "shim_ue5_malloc_realloc": 4, "shim_ue5_malloc_free": 2,
	"shim_ue5_malloc_quantize": 3, "shim_ue5_malloc_getsize": 3, "shim_ue5_malloc_nop": 0,
	// clocks (modules/libkernel/ps4_kernel_sys.c, docs/m3/clock.md): a counter read plus a scale
	"shim_sceKernelGetProcessTimeCounter": 0, "shim_sceKernelGetProcessTimeCounterFrequency": 0,
	"shim_sceKernelGetProcessTime": 0, "shim_sceKernelGetTscFrequency": 0, "shim_sceKernelReadTsc": 0,
	"shim_clock_gettime": 2, "shim_sceKernelClockGettime": 2, "shim_gettimeofday": 2, "shim_sceKernelGettimeofday": 1,
	// libc string
	"shim_memchr": 3, "shim_memcmp": 3, "shim_bcmp": 3, "shim_bzero": 2,
	"shim_strchr": 2, "shim_strrchr": 2, "shim_strncmp": 3, "shim_strcasecmp": 2, "shim_strncasecmp": 3,
	"shim_strcat": 2, "shim_strncat": 3, "shim_strnlen": 2, "shim_toupper": 1, "shim_tolower": 1,
}

var leanTable = func() map[string]thunkgen.LeanShim {
	m := map[string]thunkgen.LeanShim{}
	for shim, n := range leanHandwritten {
		m[shim] = thunkgen.LeanShim{Shim: shim, Lean: thunkgen.LeanName(shim), Args: n}
	}
	for _, l := range thunkgen.LeanShims(thunkgen.Libkernel) {
		m[l.Shim] = l
	}
	return m
}()

// LeanShimNames returns every lean-marked shim (tests, docs).
func LeanShimNames() []thunkgen.LeanShim {
	out := make([]thunkgen.LeanShim, 0, len(leanTable))
	for _, l := range leanTable {
		out = append(out, l)
	}
	return out
}

// leanDisabled is set by NPS5_LEAN=0 in the environment of the translator: no lean stubs are
// emitted at all (the runtime switch of the same name only picks the bridge at run time).
var leanDisabled = os.Getenv("NPS5_LEAN") == "0"

// leanInfo reports whether shim has a lean entry that stubs may use.
func leanInfo(shim string) (thunkgen.LeanShim, bool) {
	if leanDisabled || lifter.GetCallModel() != lifter.CallFlat {
		return thunkgen.LeanShim{}, false
	}
	l, ok := leanTable[shim]
	return l, ok
}

// sysvArgRegs are the host registers pinned to rdi, rsi, rdx, rcx, r8, r9.
var sysvArgRegs = [6]string{"x24", "x23", "x22", "x21", "x8", "x9"}

// writeLeanStub emits the stub of a lean shim: the lean path, then the full bridge as the fallback
// the mode switch selects.
func writeLeanStub(w io.Writer, sym string, l thunkgen.LeanShim, shim string, addr uint64) {
	fmt.Fprintf(w, ".globl %s\n%s:\n", sym, sym)
	for _, line := range lifter.FlatCallEntry() {
		fmt.Fprintln(w, line)
	}
	for _, line := range lifter.FlatJumpEntryLabel(sym) {
		fmt.Fprintln(w, line)
	}
	fmt.Fprintf(w, "    adrp x7, _g_nps5_lean_mode@PAGE\n")
	fmt.Fprintf(w, "    ldrb w7, [x7, _g_nps5_lean_mode@PAGEOFF]\n")
	fmt.Fprintf(w, "    cbz  w7, 1f\n")
	fmt.Fprintf(w, "    add  x16, x27, #0x70\n")
	fmt.Fprintf(w, "    ldr  x16, [x28, x16]\n") // ctx: the TCB slot
	for i := 0; i < l.Args && i < len(sysvArgRegs); i++ {
		fmt.Fprintf(w, "    mov  x%d, %s\n", i, sysvArgRegs[i])
	}
	fmt.Fprintf(w, "    mov  x6, x16\n")
	writeMovImm(w, "x17", addr)
	fmt.Fprintf(w, "    str  x17, [x16, #0x80]\n") // rip
	// SAVE_GPRS x16 (asm_trampoline.go)
	for _, s := range [][3]string{{"x19", "x21", "0x00"}, {"x22", "x20", "0x10"}, {"x26", "x25", "0x20"}, {"x23", "x24", "0x30"},
		{"x8", "x9", "0x40"}, {"x10", "x11", "0x50"}, {"x12", "x13", "0x60"}, {"x14", "x15", "0x70"}} {
		fmt.Fprintf(w, "    stp  %s, %s, [x16, #%s]\n", s[0], s[1], s[2])
	}
	fmt.Fprintf(w, "    str  x29, [x16, #0x%x]\n", ctxShadowSP)
	fmt.Fprintf(w, "    adrp x17, _%s@PAGE\n", l.Lean)
	fmt.Fprintf(w, "    add  x17, x17, _%s@PAGEOFF\n", l.Lean)
	fmt.Fprintf(w, "    b    _recomp_call_lean\n")
	fmt.Fprintf(w, "1:\n")
	writeFullBridgeCall(w, shim, addr)
}

func writeMovImm(w io.Writer, reg string, v uint64) {
	fmt.Fprintf(w, "    movz %s, #0x%x\n", reg, v&0xffff)
	for shift := 16; shift < 64; shift += 16 {
		if chunk := (v >> shift) & 0xffff; chunk != 0 {
			fmt.Fprintf(w, "    movk %s, #0x%x, lsl #%d\n", reg, chunk, shift)
		}
	}
}

func writeFullBridgeCall(w io.Writer, shim string, addr uint64) {
	if ShimStubViaGOT { // tier-1 fragment: the shim is bound from the executable at dlopen
		fmt.Fprintf(w, "    adrp x0, _%s@GOTPAGE\n", shim)
		fmt.Fprintf(w, "    ldr  x0, [x0, _%s@GOTPAGEOFF]\n", shim)
	} else {
		fmt.Fprintf(w, "    adrp x0, _%s@PAGE\n", shim)
		fmt.Fprintf(w, "    add  x0, x0, _%s@PAGEOFF\n", shim)
	}
	writeMovImm(w, "x1", addr)
	fmt.Fprintf(w, "    b    _recomp_call_shim\n\n")
}
