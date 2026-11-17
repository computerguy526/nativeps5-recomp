package disasm

import (
	"os"
	"testing"
)

// helloELF returns the sample ELF the upstream PS4Recomp tests load, or skips the test.
// The file is not part of this tree (no prebuilt binaries are committed): take hello_world.elf
// from https://github.com/wie-project/PS4Recomp and set NPS5_TEST_HELLO_ELF, or place it in the repository root.
func helloELF(tb testing.TB) string {
	p := os.Getenv("NPS5_TEST_HELLO_ELF")
	if p == "" {
		p = "../../hello_world.elf"
	}
	if _, err := os.Stat(p); err != nil {
		tb.Skipf("sample ELF %s not present (set NPS5_TEST_HELLO_ELF)", p)
	}
	return p
}
