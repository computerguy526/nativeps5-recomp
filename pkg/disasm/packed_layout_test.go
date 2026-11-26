// SPDX-License-Identifier: GPL-2.0-or-later

package disasm

import (
	"testing"

	"ps4-recomp/pkg/elfloader"
)

// restorePacked puts the package switches back so other tests see the default (environment) state.
func restorePacked(t *testing.T) {
	s, k := seedAfterUD2, keepFallInto
	t.Cleanup(func() { seedAfterUD2, keepFallInto = s, k })
}

// A function packed right behind the previous one's ud2 / jmp, at an address that is not 16-aligned
// and starts with a non-prologue instruction (xor eax,eax; ret): an entry only with the packed rules.
func packedImage(term []byte) (*elfloader.Image, uint64) {
	const base = 0x1000
	code := []byte{0x55, 0x48, 0x89, 0xe5} // push rbp; mov rbp,rsp (the previous function)
	code = append(code, term...)
	entry := base + uint64(len(code))
	code = append(code, 0x31, 0xc0, 0xc3) // xor eax,eax; ret
	for len(code) < 64 {
		code = append(code, 0xcc)
	}
	return elfloader.NewImage(base, code), entry
}

func TestPackedLayoutFlagSeedsBehindTerminators(t *testing.T) {
	restorePacked(t)
	seedAfterUD2, keepFallInto = false, false
	terms := map[string][]byte{
		"ud2":       {0x0f, 0x0b},
		"jmp short": {0xeb, 0xf0},
		"jmp rel32": {0xe9, 0x00, 0xff, 0xff, 0xff},
	}
	for name, term := range terms {
		img, entry := packedImage(term)
		if entry&15 == 0 {
			t.Fatalf("%s: test entry must be unaligned", name)
		}
		if gapEntryOK(img, entry) {
			t.Fatalf("%s: entry accepted with the packed rules off (default discovery must not change)", name)
		}
	}
	SetPackedLayout(false) // off never turns anything on
	if PackedLayout() || keepFallInto {
		t.Fatal("SetPackedLayout(false) turned the packed rules on")
	}
	SetPackedLayout(true)
	if !PackedLayout() || !keepFallInto {
		t.Fatal("SetPackedLayout(true) must turn on both the ud2/jmp seeding and keepFallInto")
	}
	for name, term := range terms {
		img, entry := packedImage(term)
		if !gapEntryOK(img, entry) {
			t.Fatalf("%s: entry rejected with -packed-layout", name)
		}
	}
	// Padding itself is still not an entry.
	img, entry := packedImage([]byte{0x0f, 0x0b})
	if gapEntryOK(img, entry+3) {
		t.Fatal("an int3 pad byte was accepted as an entry")
	}
}

// SetPackedLayout(false) must not clear an environment opt-in (NPS5_SEED_AFTER_UD2=1 keeps working).
func TestPackedLayoutOffKeepsEnvOptIn(t *testing.T) {
	restorePacked(t)
	seedAfterUD2, keepFallInto = true, true
	SetPackedLayout(false)
	if !PackedLayout() || !keepFallInto {
		t.Fatal("SetPackedLayout(false) cleared the environment opt-in")
	}
}
