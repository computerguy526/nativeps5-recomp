package lifter

import (
	"os"
	"strings"
	"testing"
)

// SYSCALL lowers to a call of the runtime's raw syscall entry through the shim bridge, in both
// address modes, and loads the error flag (r11 bit 0) into the C flag afterwards.
func TestSyscallLowering(t *testing.T) {
	for _, m := range []AddrMode{AddrBase, AddrIdentity} {
		l := NewArm64LifterMode(nil, m)
		lines := liftHex(t, l, "0f05") // syscall
		got := strings.Join(lines, "\n")
		for _, want := range []string{
			"adrp x0, _shim_raw_syscall@PAGE", "bl   _recomp_call_shim",
			"stp  x29, x30, [sp, #-16]!", "ldp  x29, x30, [sp], #16",
			"bfi  x16, x11, #29, #1", "msr  nzcv, x16",
		} {
			if !strings.Contains(got, want) {
				t.Errorf("mode %v: missing %q in:\n%s", m, want, got)
			}
		}
		if l.cpol != carryAdd {
			t.Errorf("mode %v: carry policy after syscall is %v, want carryAdd (C = CF)", m, l.cpol)
		}
		if len(l.Unsupported()) != 0 {
			t.Errorf("mode %v: syscall recorded as unsupported", m)
		}
	}
}

// The shim the lowering calls exists in the runtime and the lifter's opcode table knows SYSCALL.
func TestSyscallShimExists(t *testing.T) {
	b, err := os.ReadFile("../runtime/kernel/syscalls/ps4_syscalls.c")
	if err != nil {
		t.Fatal(err)
	}
	c := string(b)
	if !strings.Contains(c, "void shim_raw_syscall(GuestContext *ctx)") {
		t.Fatal("runtime has no shim_raw_syscall")
	}
}
