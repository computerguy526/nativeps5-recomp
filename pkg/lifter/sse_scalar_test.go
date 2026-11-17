package lifter

import (
	"strings"
	"testing"

	"golang.org/x/arch/x86/x86asm"
	"ps4-recomp/pkg/disasm"
)

// Legacy SSE MINSS/MAXSS/MINSD/MAXSD/ANDNPS/ANDNPD and CMPSD (xmm) were unsupported; Calculator3D uses them.
func TestSseScalarMinMaxAndnCmp(t *testing.T) {
	l := NewArm64Lifter(map[uint64]bool{0x1000: true})
	l.SetTSO(false)
	cases := []struct {
		name  string
		bytes []byte
		want  []string
	}{
		{"maxss xmm5, xmm0", []byte{0xf3, 0x0f, 0x5f, 0xe8}, []string{"fcmgt s30", "bsl v30.16b", "mov v5.s[0], v30.s[0]"}},
		{"minss xmm4, xmm2", []byte{0xf3, 0x0f, 0x5d, 0xe2}, []string{"fcmgt s30", "mov v4.s[0], v30.s[0]"}},
		{"minsd xmm1, xmm0", []byte{0xf2, 0x0f, 0x5d, 0xc8}, []string{"fcmgt d30", "mov v1.d[0], v30.d[0]"}},
		{"andnps xmm6, xmm14", []byte{0x41, 0x0f, 0x55, 0xf6}, []string{"bic v6.16b"}},
		{"andnpd xmm1, xmm0", []byte{0x66, 0x0f, 0x55, 0xc8}, []string{"bic v1.16b"}},
		{"cmpeqsd xmm1, xmm0", []byte{0xf2, 0x0f, 0xc2, 0xc8, 0x00}, nil},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			inst, err := x86asm.Decode(tc.bytes, 64)
			if err != nil {
				t.Fatal(err)
			}
			lines, err := l.LiftInstructionToBuf(disasm.Instruction{Address: 0x1000, Inst: inst}, 0x1000+uint64(inst.Len), nil, nil)
			if err != nil {
				t.Fatal(err)
			}
			j := strings.Join(lines, "\n")
			if strings.Contains(j, "unsupported") || strings.Contains(j, "recomp_unsupported") || j == "" {
				t.Fatalf("unsupported:\n%s", j)
			}
			for _, w := range tc.want {
				if !strings.Contains(j, w) {
					t.Errorf("missing %q in\n%s", w, j)
				}
			}
			// legacy SSE leaves the upper YMM half alone: no clearing of v(16+n)
			if strings.Contains(j, "movi v2") || strings.Contains(j, "movi v3") {
				t.Errorf("clears the YMM upper half:\n%s", j)
			}
		})
	}
}
