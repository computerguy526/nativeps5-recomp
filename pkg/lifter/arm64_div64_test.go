package lifter

import (
	"fmt"
	"math/big"
	"math/bits"
	"math/rand"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// DIV/IDIV r64 divide the 128-bit RDX:RAX and raise #DE (the guest exception path, `brk #0`) for a zero
// divisor or a quotient that does not fit. The lifted sequence is assembled into a small arm64 function (RAX
// x19, RCX x21, RDX x22 as the lifter pins them), run natively, and compared with math/big on edge cases and
// random vectors. Only arm64 code runs; no x86 is executed.
func TestDiv64Semantics(t *testing.T) {
	if runtime.GOARCH != "arm64" {
		t.Skip("needs an arm64 host")
	}
	if _, err := exec.LookPath("clang"); err != nil {
		t.Skip("no clang")
	}
	dir := t.TempDir()
	var asm strings.Builder
	asm.WriteString(".text\n.p2align 2\n")
	for _, c := range []struct{ name, hex string }{{"div64", "48f7f1"}, {"idiv64", "48f7f9"}} {
		fmt.Fprintf(&asm, ".globl _%s\n_%s:\n    stp x19, x21, [sp, #-48]!\n    str x22, [sp, #16]\n    str x20, [sp, #24]\n    mov x20, x0\n", c.name, c.name)
		asm.WriteString("    ldr x19, [x20]\n    ldr x22, [x20, #8]\n    ldr x21, [x20, #16]\n")
		for _, ln := range liftBlock(t, c.hex)[0] {
			asm.WriteString(strings.ReplaceAll(ln, "L_div64_", "L_"+c.name+"_") + "\n") // each liftBlock restarts its label counter
		}
		asm.WriteString("    str x19, [x20]\n    str x22, [x20, #8]\n    mov x0, #0\n    ldr x22, [sp, #16]\n    ldr x20, [sp, #24]\n    ldp x19, x21, [sp], #48\n    ret\n")
	}
	cc := `#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <setjmp.h>
extern int div64(uint64_t *), idiv64(uint64_t *);
static sigjmp_buf jb;
static void h(int s) { siglongjmp(jb, 1); }
int main(void) {
  struct sigaction sa = {0}; sa.sa_handler = h; sigaction(SIGTRAP, &sa, 0);
  unsigned long long a, d, c; int sg;
  while (scanf("%d %llx %llx %llx", &sg, &a, &d, &c) == 4) {
    uint64_t st[3] = {a, d, c};
    if (sigsetjmp(jb, 1)) { printf("trap\n"); continue; }
    if (sg) idiv64(st); else div64(st);
    printf("%llx %llx\n", (unsigned long long)st[0], (unsigned long long)st[1]);
  }
  return 0;
}
`
	if err := os.WriteFile(filepath.Join(dir, "m.c"), []byte(cc), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "d.s"), []byte(asm.String()), 0o644); err != nil {
		t.Fatal(err)
	}
	bin := filepath.Join(dir, "t")
	if out, err := exec.Command("clang", "-o", bin, filepath.Join(dir, "m.c"), filepath.Join(dir, "d.s")).CombinedOutput(); err != nil {
		t.Fatalf("build: %v\n%s", err, out)
	}

	type vec struct {
		sg      int
		a, d, c uint64
	}
	var vs []vec
	edge := []uint64{0, 1, 2, 3, 7, 0xffffffff, 0x100000000, 0x7fffffffffffffff, 0x8000000000000000, 0x8000000000000001, 0xffffffffffffffff, 0xfffffffffffffffe, 0x123456789abcdef0}
	for _, a := range edge {
		for _, d := range edge {
			for _, c := range edge {
				vs = append(vs, vec{0, a, d, c}, vec{1, a, d, c})
			}
		}
	}
	rng := rand.New(rand.NewSource(1))
	for i := 0; i < 4000; i++ {
		a, d, c := rng.Uint64(), rng.Uint64(), rng.Uint64()
		switch i % 4 {
		case 0:
			d = rng.Uint64() >> uint(rng.Intn(64))
		case 1:
			c >>= uint(rng.Intn(64))
			d %= c | 1
		case 2:
			d = uint64(int64(a) >> 63) // sign extension of RAX
		}
		vs = append(vs, vec{i & 1, a, d, c})
	}
	var in strings.Builder
	for _, v := range vs {
		fmt.Fprintf(&in, "%d %x %x %x\n", v.sg, v.a, v.d, v.c)
	}
	cmd := exec.Command(bin)
	cmd.Stdin = strings.NewReader(in.String())
	out, err := cmd.Output()
	if err != nil {
		t.Fatalf("run: %v", err)
	}
	got := strings.Split(strings.TrimSpace(string(out)), "\n")
	if len(got) != len(vs) {
		t.Fatalf("got %d results for %d vectors", len(got), len(vs))
	}
	bad := 0
	for i, v := range vs {
		var want string
		if v.sg == 0 {
			if v.c == 0 || v.d >= v.c {
				want = "trap"
			} else {
				q, r := bits.Div64(v.d, v.a, v.c)
				want = fmt.Sprintf("%x %x", q, r)
			}
		} else {
			n := new(big.Int).SetInt64(int64(v.d))
			n.Lsh(n, 64)
			n.Add(n, new(big.Int).SetUint64(v.a))
			dv := big.NewInt(int64(v.c))
			if dv.Sign() == 0 {
				want = "trap"
			} else {
				q, r := new(big.Int).QuoRem(n, dv, new(big.Int)) // truncated, as x86
				if !q.IsInt64() {
					want = "trap"
				} else {
					want = fmt.Sprintf("%x %x", uint64(q.Int64()), uint64(r.Int64()))
				}
			}
		}
		if got[i] != want {
			if bad++; bad <= 10 {
				t.Errorf("sg=%d rdx:rax=%x:%x / %x: got %q want %q", v.sg, v.d, v.a, v.c, got[i], want)
			}
		}
	}
	if bad > 0 {
		t.Errorf("%d of %d vectors differ", bad, len(vs))
	}
}
