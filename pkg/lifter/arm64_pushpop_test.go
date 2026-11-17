package lifter

import (
	"encoding/hex"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"ps4-recomp/pkg/disasm"
	"runtime"
	"strings"
	"testing"

	"golang.org/x/arch/x86/x86asm"
)

// Push/pop fusion (arm64_lifter_pushpop.go): runs must leave the memory image, the registers and
// rsp exactly as the one-at-a-time sequence does.

var ppRegs = []struct {
	r    x86asm.Reg
	push string
	pop  string
}{
	{x86asm.RBP, "55", "5d"}, {x86asm.R15, "4157", "415f"}, {x86asm.R14, "4156", "415e"},
	{x86asm.R13, "4155", "415d"}, {x86asm.R12, "4154", "415c"}, {x86asm.RBX, "53", "5b"},
	{x86asm.RSI, "56", "5e"}, {x86asm.RDI, "57", "5f"}, {x86asm.RAX, "50", "58"}, {x86asm.RCX, "51", "59"},
	{x86asm.RDX, "52", "5a"}, {x86asm.R8, "4150", "4158"},
}

func ppVal(i int) uint64 { return 0x1122334455667700 + uint64(i)*0x0101010101 + 7 }

func TestPushPopFuseExec(t *testing.T) {
	old := pushPopFuse
	defer func() { pushPopFuse = old }()
	rsp0 := uint64(execRSP)
	for _, on := range []bool{true, false} {
		pushPopFuse = on
		var cases []execCase
		var want []map[int][]byte
		for n := 1; n <= len(ppRegs); n++ {
			// push run of n registers
			c := execCase{name: fmt.Sprintf("fuse=%v push x%d", on, n), cfg: true, block: true, regs: map[x86asm.Reg]uint64{}}
			mem := map[int][]byte{}
			for k := 0; k < n; k++ {
				c.code = append(c.code, ppRegs[k].push)
				c.regs[ppRegs[k].r] = ppVal(k)
				mem[int(rsp0-execMemBase)-8*(k+1)] = le64(ppVal(k))
			}
			c.code = append(c.code, "39c0")
			c.wantRegs = map[x86asm.Reg]uint64{x86asm.RSP: rsp0 - 8*uint64(n)}
			cases, want = append(cases, c), append(want, mem)
			// pop run of n registers from a stack that holds the same image
			p := execCase{name: fmt.Sprintf("fuse=%v pop x%d", on, n), cfg: true, block: true, mem: map[int][]byte{}}
			p.wantRegs = map[x86asm.Reg]uint64{x86asm.RSP: rsp0 + 8*uint64(n)}
			for k := 0; k < n; k++ {
				p.code = append(p.code, ppRegs[k].pop)
				p.mem[int(rsp0-execMemBase)+8*k] = le64(ppVal(k))
				p.wantRegs[ppRegs[k].r] = ppVal(k)
			}
			p.code = append(p.code, "39c0")
			cases, want = append(cases, p), append(want, nil)
		}
		// A mov splits the run; a duplicate pop destination stays sequential (last one wins).
		cases = append(cases, execCase{name: fmt.Sprintf("fuse=%v push mov push", on), cfg: true, block: true,
			code:     []string{"55", "4157", "4889e5", "4156", "4155", "39c0"}, // push rbp; push r15; mov rbp,rsp; push r14; push r13
			regs:     map[x86asm.Reg]uint64{x86asm.RBP: 1, x86asm.R15: 2, x86asm.R14: 3, x86asm.R13: 4},
			wantRegs: map[x86asm.Reg]uint64{x86asm.RSP: rsp0 - 32, x86asm.RBP: rsp0 - 16}})
		want = append(want, map[int][]byte{int(rsp0-execMemBase) - 8: le64(1), int(rsp0-execMemBase) - 16: le64(2),
			int(rsp0-execMemBase) - 24: le64(3), int(rsp0-execMemBase) - 32: le64(4)})
		cases = append(cases, execCase{name: fmt.Sprintf("fuse=%v pop dup dest", on), cfg: true, block: true,
			code:     []string{"58", "58", "5b", "415c", "39c0"}, // pop rax; pop rax; pop rbx; pop r12
			mem:      map[int][]byte{int(rsp0 - execMemBase): append(append(append(le64(10), le64(11)...), le64(12)...), le64(13)...)},
			wantRegs: map[x86asm.Reg]uint64{x86asm.RSP: rsp0 + 32, x86asm.RAX: 11, x86asm.RBX: 12, x86asm.R12: 13}})
		want = append(want, nil)
		// push rsp / pop rsp are not fused (they keep their special semantics).
		cases = append(cases, execCase{name: fmt.Sprintf("fuse=%v push rax; push rsp; pop rcx", on), cfg: true, block: true,
			code: []string{"50", "54", "59", "39c0"}, regs: map[x86asm.Reg]uint64{x86asm.RAX: 9},
			wantRegs: map[x86asm.Reg]uint64{x86asm.RSP: rsp0 - 8, x86asm.RCX: rsp0 - 8}})
		want = append(want, map[int][]byte{int(rsp0-execMemBase) - 8: le64(9)})
		runExecExpect(t, cases, want)
	}
}

func TestPushPopFuseShape(t *testing.T) {
	old := pushPopFuse
	defer func() { pushPopFuse = old }()
	lift := func(mode AddrMode, code []string) string {
		l := NewArm64LifterMode(nil, mode)
		return strings.Join(liftSeqCFG(t, l, execCase{name: "shape", cfg: true, block: true, code: code}, 0x10000), "\n")
	}
	run := []string{"55", "4157", "4156", "53", "50"}
	pushPopFuse = true
	s := lift(AddrIdentity, run)
	for _, w := range []string{"sub x26, x26, #40", "stp ", "str "} {
		if !strings.Contains(s, w) {
			t.Errorf("identity push run lacks %q:\n%s", w, s)
		}
	}
	if strings.Count(s, "sub x26") != 1 {
		t.Errorf("one rsp update expected:\n%s", s)
	}
	pop := lift(AddrBase, []string{"58", "5b", "415c"})
	if !strings.Contains(pop, "ldp ") || !strings.Contains(pop, "add x16, x28, x26") || strings.Count(pop, "add x26, x26") != 1 {
		t.Errorf("base pop run:\n%s", pop)
	}
	pushPopFuse = false
	if s := lift(AddrBase, run); strings.Contains(s, "stp ") || strings.Count(s, "sub x26") != 5 {
		t.Errorf("flag off must keep the per-register lowering:\n%s", s)
	}
}

// Fault semantics at the guard page (documented in arm64_lifter_pushpop.go): a push run whose last
// slot lies in a PROT_NONE page, and a pop run that reads into one, fused and not. Only lifted
// arm64 code runs; the faulting access is caught with SIGSEGV and the registers are read from
// the signal context.
func TestPushPopFuseFaults(t *testing.T) {
	if runtime.GOOS != "darwin" || runtime.GOARCH != "arm64" {
		t.Skip("needs an arm64 Mac")
	}
	if _, err := exec.LookPath("clang"); err != nil {
		t.Skip("clang not found")
	}
	old := pushPopFuse
	defer func() { pushPopFuse = old }()

	run := func(on bool, op string, n int) map[string]int64 {
		pushPopFuse = on
		var code []string
		for k := 0; k < n; k++ {
			if op == "push" {
				code = append(code, ppRegs[k].push)
			} else {
				code = append(code, ppRegs[k].pop)
			}
		}
		code = append(code, "39c0")
		l := NewArm64LifterMode(nil, AddrBase)
		lines := liftSeqCFG(t, l, execCase{name: "fault", cfg: true, block: true, code: code}, 0x10000)
		var s strings.Builder
		s.WriteString(".text\n.globl _run\n.p2align 2\n_run:\n")
		s.WriteString("    stp x29, x30, [sp, #-16]!\n    stp x19, x20, [sp, #-16]!\n    stp x21, x22, [sp, #-16]!\n    stp x23, x24, [sp, #-16]!\n    stp x25, x26, [sp, #-16]!\n    stp x27, x28, [sp, #-16]!\n")
		s.WriteString("    mov x26, x0\n    mov x28, xzr\n")
		var hr []string
		for k := 0; k < n; k++ {
			x := arm64RegMap[ppRegs[k].r].X
			hr = append(hr, x[1:])
			fmt.Fprintf(&s, "    ldr %s, [x1, #%d]\n", x, 8*k)
		}
		for _, ln := range lines {
			s.WriteString(ln + "\n")
		}
		s.WriteString("    adrp x2, _g_regs@PAGE\n    add x2, x2, _g_regs@PAGEOFF\n")
		for i := 0; i < 28; i += 2 {
			fmt.Fprintf(&s, "    stp x%d, x%d, [x2, #%d]\n", i, i+1, 8*i)
		}
		s.WriteString("    str x28, [x2, #224]\n    ldp x27, x28, [sp], #16\n    ldp x25, x26, [sp], #16\n    ldp x23, x24, [sp], #16\n    ldp x21, x22, [sp], #16\n    ldp x19, x20, [sp], #16\n    ldp x29, x30, [sp], #16\n    ret\n")
		c := fmt.Sprintf(`#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <unistd.h>
uint64_t g_regs[29];
extern void run(uint64_t rsp, uint64_t *vals);
static sigjmp_buf jb; static uint64_t faddr; static int faulted;
static void h(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    for (int i = 0; i < 29; i++) g_regs[i] = uc->uc_mcontext->__ss.__x[i];
    faddr = (uint64_t)si->si_addr; faulted = 1; siglongjmp(jb, 1);
}
int main(void) {
    long pg = sysconf(_SC_PAGESIZE);
    uint8_t *m = mmap(0, 3 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = h; sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0);
    uint64_t vals[16]; for (int i = 0; i < 16; i++) vals[i] = 0x1122334455667700ull + i + 7;
    static const int hr[] = {%s};
    int n = %d, push = %d; uint64_t rsp0; uint8_t *edge; // edge: the guard boundary
    if (push) { mprotect(m, pg, PROT_NONE); edge = m + pg; rsp0 = (uint64_t)edge - 8 + 8 * n; }
    else { mprotect(m + pg, pg, PROT_NONE); edge = m + pg; rsp0 = (uint64_t)edge - 8 * (n - 1);
           for (int i = 0; i < n - 1; i++) memcpy((void *)(rsp0 + 8 * i), &vals[i], 8); }
    if (!sigsetjmp(jb, 1)) run(rsp0, vals);
    printf("faulted=%%d\n", faulted);
    printf("faddr_rel=%%lld\n", faulted ? (long long)(faddr - (uint64_t)edge) : 0ll);
    printf("rsp_delta=%%lld\n", (long long)(g_regs[26] - rsp0));
    for (int i = 0; i < n; i++) {
        if (push) { uint64_t v = 0; if (i < n - 1) memcpy(&v, (void *)(rsp0 - 8 * (i + 1)), 8); printf("slot%%d=%%d\n", i, i < n - 1 && v == vals[i]); }
        else printf("reg%%d=%%d\n", i, g_regs[hr[i]] == vals[i] && i < n - 1);
    }
    return 0;
}
`, strings.Join(hr, ", "), n, map[bool]int{true: 1, false: 0}[op == "push"])
		dir := t.TempDir()
		os.WriteFile(filepath.Join(dir, "a.s"), []byte(s.String()), 0o644)
		os.WriteFile(filepath.Join(dir, "m.c"), []byte(c), 0o644)
		bin := filepath.Join(dir, "m")
		if o, err := exec.Command("clang", "-arch", "arm64", "-O1", "-w", "-o", bin, filepath.Join(dir, "m.c"), filepath.Join(dir, "a.s")).CombinedOutput(); err != nil {
			t.Fatalf("clang: %v\n%s", err, o)
		}
		out, err := exec.Command("timeout", "20", bin).CombinedOutput()
		if err != nil {
			t.Fatalf("run: %v\n%s", err, out)
		}
		res := map[string]int64{}
		for _, ln := range strings.Split(strings.TrimSpace(string(out)), "\n") {
			var k string
			var v int64
			if i := strings.Index(ln, "="); i > 0 {
				k = ln[:i]
				fmt.Sscan(ln[i+1:], &v)
				res[k] = v
			}
		}
		t.Logf("fuse=%v %s x%d: %s", on, op, n, strings.ReplaceAll(strings.TrimSpace(string(out)), "\n", " "))
		return res
	}

	for _, n := range []int{3, 4, 5, 6} {
		for _, op := range []string{"push", "pop"} {
			un, fu := run(false, op, n), run(true, op, n)
			if un["faulted"] != 1 || fu["faulted"] != 1 {
				t.Fatalf("%s x%d: expected a fault (unfused %d fused %d)", op, n, un["faulted"], fu["faulted"])
			}
			if op == "push" {
				// The first fault is in the lowest slot, below the guard boundary, in both lowerings.
				if un["faddr_rel"] >= 0 || fu["faddr_rel"] >= 0 {
					t.Errorf("push x%d: fault address not in the guard page: unfused %d fused %d", n, un["faddr_rel"], fu["faddr_rel"])
				}
				if un["rsp_delta"] != int64(-8*n) && un["rsp_delta"] != int64(-8*(n-1)) || fu["rsp_delta"] != int64(-8*n) {
					t.Errorf("push x%d: rsp at fault: unfused %d fused %d", n, un["rsp_delta"], fu["rsp_delta"])
				}
				// Stores issued before the faulting pair are complete in both.
				for k := 0; k < n-2; k++ {
					if un[fmt.Sprintf("slot%d", k)] != 1 || fu[fmt.Sprintf("slot%d", k)] != 1 {
						t.Errorf("push x%d: slot %d missing (unfused %d fused %d)", n, k, un[fmt.Sprintf("slot%d", k)], fu[fmt.Sprintf("slot%d", k)])
					}
				}
			} else {
				if fu["faddr_rel"] < 0 || un["faddr_rel"] < 0 {
					t.Errorf("pop x%d: fault address below the guard boundary: unfused %d fused %d", n, un["faddr_rel"], fu["faddr_rel"])
				}
				if fu["rsp_delta"] != 0 {
					t.Errorf("pop x%d: fused rsp must be unchanged at the fault, delta %d", n, fu["rsp_delta"])
				}
				for k := 0; k < n-2; k++ {
					if fu[fmt.Sprintf("reg%d", k)] != 1 || un[fmt.Sprintf("reg%d", k)] != 1 {
						t.Errorf("pop x%d: reg %d not loaded before the fault", n, k)
					}
				}
			}
		}
	}
}

// A pop run under -tso-elide stack: a pop whose slot is neither private nor a register save slot must stay an acquire load.
// The fused LDP ([x16, #imm]) is never rewritten by applyTSO, so such pops are not fused; private ones still are.
func TestPushPopFuseKeepsOrderedPops(t *testing.T) {
	old := pushPopFuse
	pushPopFuse = true
	defer func() { pushPopFuse = old }()
	const (
		shared  = disasm.StackPrivSeen
		private = disasm.StackPrivSeen | disasm.StackPrivPop
	)
	decode := func(h string, addr uint64, bits uint8) disasm.Instruction {
		b, _ := hex.DecodeString(h)
		in, err := x86asm.Decode(b, 64)
		if err != nil {
			t.Fatal(err)
		}
		return disasm.Instruction{Address: addr, Inst: in, ParityDead: true, AdjustDead: true, StackPriv: bits}
	}
	for _, mode := range []AddrMode{AddrBase, AddrIdentity} {
		for _, c := range []struct {
			name    string
			bits    []uint8 // per pop: rbx, r12, r13
			ordered []bool  // per pop: lowered as an acquire load
			fused   bool
		}{
			{"all private", []uint8{private, private, private}, []bool{false, false, false}, true},
			{"all shared", []uint8{shared, shared, shared}, []bool{true, true, true}, false},
			{"middle shared", []uint8{private, shared, private}, []bool{false, true, false}, false},
		} {
			l := NewArm64LifterMode(nil, mode)
			l.SetTSOElide(TSOElideStack)
			hexes := []string{"5b", "415c", "415d"} // pop rbx; pop r12; pop r13
			blk := &disasm.BasicBlock{StartAddr: 0x1000}
			addr := uint64(0x1000)
			for i, h := range hexes {
				in := decode(h, addr, c.bits[i])
				blk.Insts = append(blk.Insts, in)
				addr += uint64(in.Inst.Len)
			}
			fn := &disasm.Function{EntryAddr: 0x1000, EndAddr: addr, Blocks: map[uint64]*disasm.BasicBlock{0x1000: blk}}
			l.SetBlock(blk)
			var out []string
			for i, in := range blk.Insts {
				next := addr
				if i+1 < len(blk.Insts) {
					next = blk.Insts[i+1].Address
				}
				lines, err := l.LiftInstructionToBuf(in, next, fn, nil)
				if err != nil {
					t.Fatal(err)
				}
				asm := strings.Join(lines, "\n")
				if isOrdered(asm) != c.ordered[i] {
					t.Errorf("%v %s: pop %d ordered=%v, want %v\n%s", mode, c.name, i, isOrdered(asm), c.ordered[i], asm)
				}
				out = append(out, asm)
			}
			all := strings.Join(out, "\n")
			if got := strings.Contains(all, "ldp "); got != c.fused {
				t.Errorf("%v %s: fused=%v, want %v\n%s", mode, c.name, got, c.fused, all)
			}
		}
	}
}
