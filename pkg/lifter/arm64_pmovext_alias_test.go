package lifter

import (
	"strings"
	"testing"
)

// VPMOVZX/VPMOVSX ymm, xmm with the same register number (vpmovzxdq ymm4, xmm4): the upper half of
// the result is made from the upper lanes of the source, so it has to be computed before the low
// half overwrites the source. (UE's GC struct-array batch pairs array pointers with element
// counts this way; with the halves in the wrong order the counts of entries 2 and 3 were wrong.)
func TestPmovExtendYmmSameRegister(t *testing.T) {
	for _, c := range []struct{ name, hex string }{
		{"vpmovzxbw ymm4, xmm4", "c4e27d30e4"},
		{"vpmovzxwd ymm4, xmm4", "c4e27d33e4"},
		{"vpmovzxdq ymm4, xmm4", "c4e27d35e4"},
		{"vpmovsxbw ymm4, xmm4", "c4e27d20e4"},
		{"vpmovsxwd ymm4, xmm4", "c4e27d23e4"},
		{"vpmovsxdq ymm4, xmm4", "c4e27d25e4"},
		{"vpmovzxbd ymm4, xmm4", "c4e27d31e4"},
		{"vpmovzxwq ymm4, xmm4", "c4e27d34e4"},
		{"vpmovzxbq ymm4, xmm4", "c4e27d32e4"},
	} {
		lines := liftBlock(t, c.hex)[0]
		lowWrite, lastSrcRead := -1, -1
		for i, ln := range lines {
			f := strings.Fields(strings.ReplaceAll(ln, ",", " "))
			if len(f) < 3 || !(strings.HasPrefix(f[0], "uxtl") || strings.HasPrefix(f[0], "sxtl")) {
				continue
			}
			if strings.HasPrefix(f[2], "v4.") {
				lastSrcRead = i
			}
			if strings.HasPrefix(f[1], "v4.") && lowWrite < 0 {
				lowWrite = i
			}
		}
		if lowWrite < 0 || lastSrcRead < 0 {
			t.Fatalf("%s: lowering not recognised:\n%s", c.name, strings.Join(lines, "\n"))
		}
		if lastSrcRead > lowWrite {
			t.Errorf("%s: the source (v4) is read after the low half overwrote it:\n%s", c.name, strings.Join(lines, "\n"))
		}
	}
}
