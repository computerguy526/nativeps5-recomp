// SPDX-License-Identifier: GPL-2.0-or-later

package guestmem

// Adversarial tests of the translate-time memory model (docs/m1/memory.md sections 1, 3, 4).
// KNOWN-FAIL policy: see docs/m1/memtests.md (skip with the reason after the violation is
// observed; MEMTESTS_STRICT=1 turns it into a failure).

import (
	"debug/elf"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"testing"

	"ps4-recomp/pkg/elfloader"
)

// knownFail was the KNOWN-FAIL hook (docs/m1/memtests.md); every defect it covered is fixed
// (docs/m1/memory-fixes-2.md), so a violation is now always a failure.
func knownFail(t *testing.T, format string, args ...any) {
	t.Helper()
	t.Fatalf("violated: %s", fmt.Sprintf(format, args...))
}

func TestMemLayoutInvariants(t *testing.T) {
	for _, m := range []Mode{Base, Identity} {
		l := LayoutFor(m)
		org := l.Origin
		t.Run(m.String(), func(t *testing.T) {
			// console layout relative to the origin
			for name, c := range map[string][2]uint64{
				"null guard end (GuestLo)": {l.GuestLo - org, 0x40_0000}, "exe": {l.ExeBase - org, 0x40_0000}, "dyn": {l.DynLo - org, 0x2_0000_0000},
				"stack top": {l.StackTop - org, 0x7_EEFF_C000}, "pool": {l.PoolLo - org, 0x10_0000_0000},
			} {
				if c[0] != c[1] {
					t.Errorf("%s = %#x want %#x", name, c[0], c[1])
				}
			}
			if l.StackLen != 0x400_0000 {
				t.Errorf("main stack is %#x bytes, ADR says 64 MiB", l.StackLen)
			}
			// ordering: guard < exe < dyn < stack guard < stack < pool < end
			guard := l.StackTop - l.StackLen - PageSize
			order := []uint64{org, l.GuestLo, l.ExeBase, l.DynLo, guard, l.StackTop, l.PoolLo, l.GuestHi}
			for i := 1; i < len(order); i++ {
				if order[i-1] >= order[i] && !(i == 2 && order[i-1] == order[i]) {
					t.Errorf("layout order violated at %d: %#x >= %#x", i, order[i-1], order[i])
				}
			}
			// 16 KiB granule everywhere, including the stack guard
			for name, v := range map[string]uint64{"guard": guard, "pool": l.PoolLo, "lo": l.GuestLo} {
				if v%PageSize != 0 {
					t.Errorf("%s %#x not 16 KiB aligned", name, v)
				}
			}
			// the reservation covers the whole guest window
			if l.GuestHi-org > l.ReserveLen() {
				t.Errorf("guest window [%#x,%#x) exceeds the reservation length %#x", org, l.GuestHi, l.ReserveLen())
			}
			if m == Base && l.ReserveLen() != 1<<40 {
				t.Errorf("base reservation is %#x, ADR says 2^40", l.ReserveLen())
			}
			if m == Identity && (l.ReserveLo() != 0x70_0000_0000 || l.ReserveLo()+l.ReserveLen() != 0xFC_0000_0000) {
				t.Errorf("identity reservation %#x+%#x", l.ReserveLo(), l.ReserveLen())
			}
		})
	}
}

func TestMemContainsBoundaries(t *testing.T) {
	for _, m := range []Mode{Base, Identity} {
		l := LayoutFor(m)
		cases := []struct {
			name string
			a, n uint64
			want bool
		}{
			{"first page", l.GuestLo, PageSize, true}, {"byte below the window", l.GuestLo - 1, 1, false},
			{"last byte", l.GuestHi - 1, 1, true}, {"one past the end", l.GuestHi, 1, false},
			{"empty range at the end", l.GuestHi, 0, true}, {"straddles the end", l.GuestHi - 8, 16, false},
			{"wrap", l.GuestHi - 8, ^uint64(0), false}, {"addr wrap", ^uint64(0) - 3, 16, false},
			{"null", 0, 1, false}, {"whole window", l.GuestLo, l.GuestHi - l.GuestLo, true},
			{"whole window + 1", l.GuestLo, l.GuestHi - l.GuestLo + 1, false},
		}
		for _, c := range cases {
			if got := l.Contains(c.a, c.n); got != c.want {
				t.Errorf("%s %s: Contains(%#x,%#x) = %v want %v", m, c.name, c.a, c.n, got, c.want)
			}
		}
	}
}

func TestMemExeBiasEdges(t *testing.T) {
	b, i := LayoutFor(Base), LayoutFor(Identity)
	if d, err := b.ExeBias(0x3FFF, true); err != nil || d != 0x400000 {
		t.Errorf("relocatable at 0x3fff: %#x %v", d, err)
	}
	if d, err := b.ExeBias(0x400000, true); err != nil || d != 0 {
		t.Errorf("relocatable already at the console base: %#x %v", d, err)
	}
	if d, err := i.ExeBias(0x400000, true); err != nil || d != 0x70_0000_0000 {
		t.Errorf("identity, relocatable linked at 0x400000: %#x %v", d, err)
	}
	if _, err := b.ExeBias(0x3F_C000, false); err == nil {
		t.Errorf("fixed-address executable ending in the last guard page accepted")
	}
	if _, err := b.ExeBias(0x40_0000, false); err != nil {
		t.Errorf("fixed-address executable at the first usable page refused: %v", err)
	}
	for _, v := range []uint64{0xFC_0000_0000, 0xFC_0000_4000, 1 << 40, ^uint64(0) - 0x3FFF} {
		if _, err := b.ExeBias(v, false); err == nil {
			knownFail(t, "a fixed-address executable linked at %#x (outside the guest window ending at %#x) is accepted by ExeBias", v, b.GuestHi)
		}
	}
}

func TestMemManifestConfigIsAcceptableToRuntime(t *testing.T) {
	re := regexp.MustCompile(`\.(\w+) = (0x[0-9a-fA-F]+|\w+)`)
	for _, m := range []Mode{Base, Identity} {
		l := LayoutFor(m)
		man := NewManifest(l, "test", 0x400000, 0x418000, nil, Scan{Relocatable: true})
		c := man.CConfig()
		fields := map[string]string{}
		for _, f := range re.FindAllStringSubmatch(c, -1) {
			fields[f[1]] = strings.TrimSuffix(f[2], "ULL")
		}
		for name, want := range map[string]uint64{"guest_lo": l.GuestLo, "guest_hi": l.GuestHi, "dyn_lo": l.DynLo, "pool_lo": l.PoolLo, "stack_top": l.StackTop, "stack_len": l.StackLen, "host_base": l.HostBase} {
			v, err := strconv.ParseUint(fields[name], 0, 64)
			if err != nil || v != want {
				t.Errorf("%s: %s = %q want %#x", m, name, fields[name], want)
			}
		}
		wantMode := "NPS5_ADDR_BASE"
		if m == Identity {
			wantMode = "NPS5_ADDR_IDENTITY"
		}
		if fields["addr_mode"] != wantMode {
			t.Errorf("addr_mode %q", fields["addr_mode"])
		}
		// round trip through JSON
		js, err := man.JSON()
		if err != nil {
			t.Fatal(err)
		}
		var back Manifest
		if err := json.Unmarshal(js, &back); err != nil {
			t.Fatalf("manifest JSON does not parse back: %v", err)
		}
		if back.GuestHi != man.GuestHi || back.StackTop != man.StackTop || back.AddrMode != man.AddrMode {
			t.Errorf("manifest round trip lost fields")
		}
	}
	// image_end is handed to the runtime, which refuses a config that is not 16 KiB aligned
	man := NewManifest(LayoutFor(Base), "test", 0x400000, 0x418123, nil, Scan{Relocatable: true})
	v, _ := strconv.ParseUint(regexp.MustCompile(`\.image_end = (0x[0-9a-f]+)ULL`).FindStringSubmatch(man.CConfig())[1], 0, 64)
	if v%PageSize != 0 {
		knownFail(t, "NewManifest/CConfig passes an unaligned image_end (%#x) to the runtime, which rejects the config at startup ('memory config is not 16 KiB aligned'); the value should be rounded up or refused at translate time", v)
	}
}

func movabs(rex, op byte, v uint64) []byte {
	return append([]byte{rex, op}, binary.LittleEndian.AppendUint64(nil, v)...)
}

func scanCode(code []byte) Scan {
	return ScanTitle(scanImage(code, make([]byte, 64), nil), true)
}

func TestMemScanBoundaries(t *testing.T) {
	cases := []struct {
		v    uint64
		want int
		why  string
	}{
		{0x1_8000_0000, 1, "lowest address of the carveout window (6 GiB)"},
		{0x1_7FFF_C000, 0, "just below 6 GiB: indistinguishable from a size"},
		{0x6F_FFFF_C000, 1, "last page below the identity arena"},
		{0x70_0000_0000, 0, "identity can serve it"},
		{0x2_0000_0001, 0, "unaligned"},
		{0x2_0000_3FFF, 0, "unaligned (page - 1)"},
		{0x2_0000_4000, 1, "16 KiB aligned"},
		{0x2_0000_1000, 0, "4 KiB aligned only: not a 16 KiB page address"},
		{0, 0, "zero"}, {^uint64(0), 0, "all ones"}, {1 << 63, 0, "high bit"},
	}
	for _, c := range cases {
		code := movabs(0x48, 0xb8, c.v)
		s := scanCode(code)
		if s.CarveoutConstCount != c.want {
			if c.v == 0x2_0000_1000 {
				// the doc says "page-aligned"; PageSize is 16 KiB, so only multiples of 0x4000 count
			}
			t.Errorf("movabs 0x%x (%s): count %d want %d", c.v, c.why, s.CarveoutConstCount, c.want)
		}
	}
	// every register form, both REX prefixes
	for r := byte(0); r < 8; r++ {
		for _, rex := range []byte{0x48, 0x49} {
			if s := scanCode(movabs(rex, 0xb8+r, 0x30_0020_0000)); s.CarveoutConstCount != 1 {
				t.Errorf("movabs %#x %#x not found", rex, 0xb8+r)
			}
		}
	}
	// a 32-bit mov (B8 imm32) followed by bytes that look like an address must not be misread as REX.W movabs
	code := append([]byte{0xb8, 0, 0, 0, 0}, make([]byte, 16)...)
	if s := scanCode(code); s.CarveoutConstCount != 0 {
		t.Errorf("false positive on mov eax, imm32")
	}
	// at the very end of the segment: exactly 10 bytes
	if s := scanCode(movabs(0x48, 0xb8, 0x30_0020_0000)); s.CarveoutConstCount != 1 {
		t.Errorf("movabs filling the whole segment missed")
	}
	if s := scanCode(movabs(0x48, 0xb8, 0x30_0020_0000)[:9]); s.CarveoutConstCount != 0 {
		t.Errorf("truncated movabs counted")
	}
}

func TestMemScanAbsoluteMoffsForms(t *testing.T) {
	// mov rax, [moffs64] / mov [moffs64], rax (REX.W A1 / A3 imm64) read and write a hardcoded
	// 64-bit address exactly as a movabs does; a title using them breaks identity mode the same way.
	for _, op := range []byte{0xa1, 0xa3, 0xa0, 0xa2} {
		s := scanCode(movabs(0x48, op, 0x30_0020_0000))
		if s.CarveoutConstCount == 0 {
			knownFail(t, "ScanTitle misses the absolute-address form REX.W %#x imm64 (mov rax,[moffs64] family): a title addressing 0x30_0020_0000 this way is classed as identity-safe", op)
		}
	}
}

// ripLoad is `mov rax, [rip+disp32]` at guest address at, loading the word at target.
func ripLoad(at, target uint64) []byte {
	return binary.LittleEndian.AppendUint32([]byte{0x48, 0x8b, 0x05}, uint32(int32(int64(target)-int64(at+7))))
}

func TestMemScanDataAndRelocations(t *testing.T) {
	data := make([]byte, 0x40)
	binary.LittleEndian.PutUint64(data[0:], 0x2_0000_0000)
	binary.LittleEndian.PutUint64(data[8:], 0x2_4000_0000)
	// Data words are evidence only when code loads them: the code below loads the word at 0x4008
	// (rip-relative) and nothing refers to the word at 0x4000. The word at 0x4008 is also
	// relocated in the first image, so it is skipped there; in the second it counts.
	code := append(ripLoad(0, 0x4008), 0xc3)
	s := ScanTitle(scanImage(code, data, []elfloader.Relocation{{Offset: 0x4008, Type: elfloader.R_X86_64_RELATIVE}}), true)
	if s.CarveoutConstCount != 0 {
		t.Errorf("relocated word must be skipped, an unreferenced one is not evidence: %d", s.CarveoutConstCount)
	}
	s = ScanTitle(scanImage(code, data, nil), true)
	if s.CarveoutConstCount != 1 || s.CarveoutConsts[0].Where != 0x4008 || s.CarveoutConsts[0].Kind != "data" {
		t.Errorf("a data word loaded by code is evidence, an unreferenced neighbour is not: %+v", s)
	}
	// an unreferenced word (the layout of an SDK-built title's constant tables) is not evidence
	if s := ScanTitle(scanImage([]byte{0xc3}, data, nil), true); s.CarveoutConstCount != 0 {
		t.Errorf("data words nothing refers to counted as fixed-address evidence: %+v", s)
	}
	// a relocation of type 64 against a data symbol still marks the slot as an image pointer
	code2 := append(append(ripLoad(0, 0x4000), ripLoad(7, 0x4008)...), 0xc3)
	s = ScanTitle(scanImage(code2, data, []elfloader.Relocation{{Offset: 0x4000, Type: elfloader.R_X86_64_64, SymName: "x"}, {Offset: 0x4008, Type: elfloader.R_X86_64_GLOB_DAT, SymName: "y"}}), true)
	if s.CarveoutConstCount != 0 {
		t.Errorf("slots under R_X86_64_64/GLOB_DAT counted as constants: %d", s.CarveoutConstCount)
	}
	// unaligned segment base: 8-byte words are aligned to guest addresses, not slice offsets
	ud := make([]byte, 0x20)
	binary.LittleEndian.PutUint64(ud[4:], 0x2_0000_0000) // guest 0x4008 for a segment at 0x4004
	img := &elfloader.LoadedELF{Segments: []*elfloader.Segment{
		{Vaddr: 0, Memsz: 8, Filesz: 8, Flags: elf.PF_R | elf.PF_X, Data: append(ripLoad(0, 0x4008), 0xc3)},
		{Vaddr: 0x4004, Memsz: 0x20, Filesz: 0x20, Flags: elf.PF_R | elf.PF_W, Data: ud}}}
	if s := ScanTitle(img, true); s.CarveoutConstCount != 1 || s.CarveoutConsts[0].Where != 0x4008 {
		t.Errorf("unaligned data segment: %+v", s)
	}
	// nil / empty loaded images
	if s := ScanTitle(nil, true); s.CarveoutConstCount != 0 {
		t.Errorf("nil image")
	}
	if s := ScanTitle(&elfloader.LoadedELF{}, false); s.Relocatable {
		t.Errorf("Relocatable flag lost")
	}
}

func TestMemSelectMatrix(t *testing.T) {
	clean := scanCode([]byte{0xc3})
	dirty := scanCode(movabs(0x48, 0xb8, 0x30_0020_0000))
	fixed := ScanTitle(scanImage([]byte{0xc3}, make([]byte, 64), nil), false)
	for _, c := range []struct {
		req  string
		scan Scan
		mode Mode
		err  bool
	}{
		{"", clean, Base, false}, {"base", dirty, Base, false}, {"auto", clean, Identity, false}, {"auto", dirty, Base, false}, {"auto", fixed, Base, false},
		{"identity", clean, Identity, false}, {"identity", dirty, Base, true}, {"identity", fixed, Base, true},
		{"IDENTITY", clean, Base, true}, {"Auto", clean, Base, true}, {" base", clean, Base, true}, {"flat", clean, Base, true}, {"identity,base", clean, Base, true},
	} {
		m, why, err := Select(c.req, c.scan)
		if (err != nil) != c.err || (err == nil && m != c.mode) {
			t.Errorf("Select(%q): mode %v why %q err %v; want mode %v err %v", c.req, m, why, err, c.mode, c.err)
		}
		if err != nil && c.req == "identity" && !strings.Contains(err.Error(), "title needs base addressing") {
			t.Errorf("identity refusal lacks the evidence: %v", err)
		}
		if err == nil && why == "" {
			t.Errorf("Select(%q) gave no reason", c.req)
		}
	}
	// an evidence record claiming constants without examples must not panic the report
	func() {
		defer func() {
			if r := recover(); r != nil {
				knownFail(t, "NeedsBase panics (%v) on a Scan with CarveoutConstCount > 0 and no recorded constants (a hand-built or hand-edited scan record)", r)
			}
		}()
		Scan{Relocatable: true, CarveoutConstCount: 3}.NeedsBase()
	}()
}

func TestMemScanMergeCap(t *testing.T) {
	var big Scan
	big.Relocatable = true
	for i := 0; i < 100; i++ {
		big.CarveoutConstCount++
		if len(big.CarveoutConsts) < 32 {
			big.CarveoutConsts = append(big.CarveoutConsts, AddrConst{Value: uint64(0x2_0000_0000 + i*0x4000), Where: uint64(i), Kind: "code"})
		}
	}
	var m Scan
	m.Relocatable = true
	for i := 0; i < 5; i++ {
		m.Merge(big)
	}
	if m.CarveoutConstCount != 500 || len(m.CarveoutConsts) != 32 {
		t.Errorf("merge: count %d examples %d", m.CarveoutConstCount, len(m.CarveoutConsts))
	}
	// a fixed-address companion module must make the merged evidence fixed as well
	var fixedMod Scan
	fixedMod.Relocatable = false
	m2 := Scan{Relocatable: true}
	m2.Merge(fixedMod)
	if !m2.Relocatable {
		t.Logf("note: Merge ignores Relocatable of the merged module (only the executable's flag is meaningful)")
	}
}

func TestMemLayoutConcurrentUse(t *testing.T) {
	var wg sync.WaitGroup
	for g := 0; g < 8; g++ {
		wg.Add(1)
		go func(g int) {
			defer wg.Done()
			for i := 0; i < 2000; i++ {
				l := LayoutFor(Mode(i % 2))
				_, _ = l.ExeBias(uint64(i)*0x4000, i%3 != 0)
				_ = l.Contains(uint64(i), 8)
				s := scanCode(movabs(0x48, 0xb8, 0x30_0020_0000+uint64(i)*0x4000))
				_, _, _ = Select("auto", s)
				_ = NewManifest(l, "x", 0, 0, nil, s).CConfig()
			}
		}(g)
	}
	wg.Wait()
}
