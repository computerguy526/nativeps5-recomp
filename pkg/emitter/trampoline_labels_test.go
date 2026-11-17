package emitter

import (
	"regexp"
	"testing"
)

// A branch from __recomp_guest to a local (L-prefixed) label in __text is mis-resolved by the
// Apple linker once the guest code is larger than the +-128 MB branch range and it needs a
// thunk island (it landed in the objc stub area on a 487 MB title). Cross-section branch
// targets must be real symbols, not assembler-local labels.
func TestTrampolineCrossSectionTargetsAreSymbols(t *testing.T) {
	re := regexp.MustCompile(`(?m)^\s+b\s+(L\w+)\s*$`)
	for _, m := range re.FindAllStringSubmatch(trampolineAsm+X87BridgeAsm+trampolineFlatEnter+trampolineFlatDispatch, -1) {
		lbl := m[1]
		if lbl == "Lenter_guest_ret" || lbl == "Lret_slow" {
			continue
		}
		def := regexp.MustCompile(`(?m)^` + lbl + `:`)
		if def.MatchString(trampolineAsm + X87BridgeAsm + trampolineFlatEnter + trampolineFlatDispatch) {
			t.Errorf("branch to local label %s: rename it to a symbol (_nps5_xs_...)", lbl)
		}
	}
}
