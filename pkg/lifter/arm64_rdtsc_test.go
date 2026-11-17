package lifter

import (
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"strings"
	"testing"
)

// RDTSC returned the raw host counter (24 MHz on Apple silicon) while sceKernelGetTscFrequency
// reports RECOMP_TSC_FREQ (2.13 GHz) and sceKernelReadTsc scales to it: guest code timing with
// RDTSC saw time run ~89 times slower (TLOU: fades and the boot flow never advanced, black
// screen). The lowering must count in the same units as the runtime's recomp_rdtsc().
func TestRdtscMatchesRuntimeTsc(t *testing.T) {
	hdr, err := os.ReadFile("../runtime/core/recomp_context.h")
	if err != nil {
		t.Fatal(err)
	}
	m := regexp.MustCompile(`#define RECOMP_TSC_FREQ (\d+)ULL`).FindSubmatch(hdr)
	if m == nil {
		t.Fatal("RECOMP_TSC_FREQ not found in recomp_context.h")
	}
	if f, _ := strconv.ParseUint(string(m[1]), 10, 64); f != guestTscFreq {
		t.Fatalf("guestTscFreq %d, RECOMP_TSC_FREQ %d", guestTscFreq, f)
	}
	l := liftBlock(t, "0f31")[0] // rdtsc
	forbid(t, "rdtsc", l, "msr nzcv...", "subs ...", "adds ...", "cmp ...")

	if runtime.GOOS != "darwin" || runtime.GOARCH != "arm64" {
		t.Skip("execution part needs an arm64 Mac")
	}
	if _, err := exec.LookPath("clang"); err != nil {
		t.Skip("clang not found")
	}
	dir := t.TempDir()
	var asm strings.Builder
	asm.WriteString(".text\n.globl _lifted_rdtsc\n.p2align 2\n_lifted_rdtsc:\n")
	asm.WriteString("    stp x19, x22, [sp, #-16]!\n")
	for _, ln := range l {
		asm.WriteString(ln + "\n")
	}
	asm.WriteString("    mov w0, w19\n    orr x0, x0, x22, lsl #32\n    ldp x19, x22, [sp], #16\n    ret\n")
	driver := `
#include <stdint.h>
#include <stdio.h>
#include "recomp_context.h"
uint64_t lifted_rdtsc(void);
int main(void) {
    for (int i = 0; i < 100000; i++) {
        uint64_t a = recomp_rdtsc(), b = lifted_rdtsc(), c = recomp_rdtsc();
        if (b < a || b > c) {
            printf("lifted %llu not within runtime [%llu, %llu]\n", (unsigned long long)b,
                   (unsigned long long)a, (unsigned long long)c);
            return 1;
        }
    }
    return 0;
}
`
	if err := os.WriteFile(filepath.Join(dir, "rdtsc.s"), []byte(asm.String()), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "main.c"), []byte(driver), 0o644); err != nil {
		t.Fatal(err)
	}
	core, _ := filepath.Abs("../runtime/core")
	bin := filepath.Join(dir, "rdtsc")
	if o, err := exec.Command("clang", "-O1", "-I", core, "-o", bin, filepath.Join(dir, "main.c"),
		filepath.Join(dir, "rdtsc.s")).CombinedOutput(); err != nil {
		t.Fatalf("clang: %v\n%s", err, o)
	}
	if o, err := exec.Command(bin).CombinedOutput(); err != nil {
		t.Fatalf("%v: %s", err, o)
	}
}

func TestRdtscpLifting(t *testing.T) {
	// 0f 01 f9 = rdtscp
	l := liftBlock(t, "0f01f9")[0]
	forbid(t, "rdtscp", l, "msr nzcv...", "subs ...", "adds ...", "cmp ...")

	foundTsc := false
	foundAux := false
	for _, ln := range l {
		if strings.Contains(ln, "cntvct_el0") {
			foundTsc = true
		}
		if strings.Contains(ln, "mov w21, wzr") {
			foundAux = true
		}
	}
	if !foundTsc || !foundAux {
		t.Fatalf("expected cntvct_el0 and mov w21, wzr in lifted rdtscp, got:\n%s", strings.Join(l, "\n"))
	}
}

// TestRdtscScaleExact runs the lowering with chosen (counter, frequency) pairs in place of the two MRS reads and
// compares with the exact 128-bit reference floor(v * 2.13e9 / frq): the 24 MHz fast path (355/4) and the divmod
// fallback, at small, large and overflow-prone counter values (uptimes of years).
func TestRdtscScaleExact(t *testing.T) {
	if runtime.GOOS != "darwin" || runtime.GOARCH != "arm64" {
		t.Skip("execution part needs an arm64 Mac")
	}
	if _, err := exec.LookPath("clang"); err != nil {
		t.Skip("clang not found")
	}
	l := liftBlock(t, "0f31")[0]
	var asm strings.Builder
	asm.WriteString(".text\n.globl _lifted_scale\n.p2align 2\n_lifted_scale:\n")
	asm.WriteString("    stp x19, x22, [sp, #-16]!\n    mov x3, x0\n    mov x4, x1\n")
	for _, ln := range l {
		ln = strings.Replace(ln, "mrs x16, cntvct_el0", "mov x16, x3", 1)
		ln = strings.Replace(ln, "mrs x17, cntfrq_el0", "mov x17, x4", 1)
		asm.WriteString(ln + "\n")
	}
	asm.WriteString("    mov w0, w19\n    orr x0, x0, x22, lsl #32\n    ldp x19, x22, [sp], #16\n    ret\n")
	driver := `
#include <stdint.h>
#include <stdio.h>
#include "recomp_context.h"
uint64_t lifted_scale(uint64_t v, uint64_t frq);
int main(void) {
    const uint64_t freqs[] = {24000000ULL, 25000000ULL, 1000000000ULL, 100000000ULL, 3000000000ULL};
    const uint64_t vals[] = {0, 1, 2, 3, 23999999ULL, 24000000ULL, 24000001ULL, 86400ULL * 24000000ULL,
                             3650ULL * 86400 * 24000000ULL, 25ULL * 365 * 86400 * 24000000ULL, 1ULL << 55};
    uint64_t x = 0x9e3779b97f4a7c15ULL;
    for (int f = 0; f < 5; f++)
        for (int i = 0; i < 20000; i++) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            uint64_t v = i < 11 ? vals[i] : (x >> (i % 24));
            unsigned __int128 ref = (unsigned __int128)v * RECOMP_TSC_FREQ / freqs[f];
            if ((ref >> 64) != 0) continue; // result beyond 64 bits: not representable in EDX:EAX either
            if (freqs[f] > 4000000000ULL) continue;
            uint64_t got = lifted_scale(v, freqs[f]), rt = recomp_cnt_to_tsc(v, freqs[f]);
            if (got != (uint64_t)ref || rt != (uint64_t)ref) {
                printf("v=%llu frq=%llu ref=%llu lifted=%llu runtime=%llu\n", (unsigned long long)v,
                       (unsigned long long)freqs[f], (unsigned long long)ref, (unsigned long long)got, (unsigned long long)rt);
                return 1;
            }
        }
    return 0;
}
`
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "s.s"), []byte(asm.String()), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "main.c"), []byte(driver), 0o644); err != nil {
		t.Fatal(err)
	}
	core, _ := filepath.Abs("../runtime/core")
	bin := filepath.Join(dir, "scale")
	if o, err := exec.Command("clang", "-O1", "-I", core, "-o", bin, filepath.Join(dir, "main.c"),
		filepath.Join(dir, "s.s")).CombinedOutput(); err != nil {
		t.Fatalf("clang: %v\n%s", err, o)
	}
	if o, err := exec.Command(bin).CombinedOutput(); err != nil {
		t.Fatalf("%v: %s", err, o)
	}
}
