// SPDX-License-Identifier: GPL-2.0-or-later

package guestmem

import (
	"debug/elf"
	"encoding/binary"
	"encoding/json"
	"strings"
	"testing"

	"ps4-recomp/pkg/elfloader"
)

func TestLayouts(t *testing.T) {
	b := LayoutFor(Base)
	if b.HostBase != 1<<40 || b.ReserveLo() != 1<<40 || b.ReserveLen() != 1<<40 {
		t.Errorf("base reservation: %+v", b)
	}
	if b.ExeBase != 0x400000 || b.DynLo != 0x2_0000_0000 || b.GuestHi != 0xFC_0000_0000 || b.StackTop != 0x7_EEFF_C000 {
		t.Errorf("base layout: %+v", b)
	}
	i := LayoutFor(Identity)
	if i.HostBase != 0 || i.ReserveLo() != 0x70_0000_0000 || i.ReserveLo()+i.ReserveLen() != 0xFC_0000_0000 {
		t.Errorf("identity reservation: %+v", i)
	}
	// The identity layout is the console layout shifted into the arena, entirely inside it.
	for name, v := range map[string]uint64{"exe": i.ExeBase, "dyn": i.DynLo, "pool": i.PoolLo, "stack": i.StackTop} {
		if v < IdentityLo || v >= IdentityHi {
			t.Errorf("identity %s = %#x is outside the arena", name, v)
		}
	}
	if i.ExeBase-b.ExeBase != IdentityLo || i.DynLo-b.DynLo != IdentityLo || i.StackTop-b.StackTop != IdentityLo {
		t.Errorf("identity layout is not the base layout + %#x", uint64(IdentityLo))
	}
	for _, l := range []Layout{b, i} {
		for name, v := range map[string]uint64{"lo": l.GuestLo, "hi": l.GuestHi, "exe": l.ExeBase, "dyn": l.DynLo, "stack": l.StackTop, "len": l.StackLen} {
			if v%PageSize != 0 {
				t.Errorf("%s %s = %#x is not page aligned", l.Mode, name, v)
			}
		}
	}
}

func TestExeBias(t *testing.T) {
	b, i := LayoutFor(Base), LayoutFor(Identity)
	if d, err := b.ExeBias(0, true); err != nil || d != 0x400000 {
		t.Errorf("base, relocatable at 0: %#x %v", d, err)
	}
	if d, err := i.ExeBias(0, true); err != nil || d != 0x70_0040_0000 {
		t.Errorf("identity, relocatable at 0: %#x %v", d, err)
	}
	if d, err := b.ExeBias(0x400000, false); err != nil || d != 0 {
		t.Errorf("base, fixed at 0x400000: %#x %v", d, err)
	}
	if _, err := i.ExeBias(0x400000, false); err == nil || !strings.Contains(err.Error(), "needs base addressing") {
		t.Errorf("identity accepted a fixed-address executable: %v", err)
	}
	if _, err := b.ExeBias(0x10000, false); err == nil {
		t.Error("fixed-address executable inside the null guard accepted")
	}
}

func scanImage(code, data []byte, relocs []elfloader.Relocation) *elfloader.LoadedELF {
	return &elfloader.LoadedELF{
		Segments: []*elfloader.Segment{
			{Vaddr: 0, Memsz: uint64(len(code)), Filesz: uint64(len(code)), Flags: elf.PF_R | elf.PF_X, Data: code},
			{Vaddr: 0x4000, Memsz: uint64(len(data)), Filesz: uint64(len(data)), Flags: elf.PF_R | elf.PF_W, Data: data},
		},
		Relocations: relocs,
	}
}

func TestScanAndSelect(t *testing.T) {
	// movabs rax, 0x3000200000 (the address 007 First Light hardcodes)
	code := append([]byte{0x90, 0x48, 0xb8}, binary.LittleEndian.AppendUint64(nil, 0x3000200000)...)
	code = append(code, 0xc3)
	s := ScanTitle(scanImage(code, make([]byte, 64), nil), true)
	if s.CarveoutConstCount != 1 || s.CarveoutConsts[0].Value != 0x3000200000 || s.CarveoutConsts[0].Kind != "code" || s.CarveoutConsts[0].Where != 1 {
		t.Fatalf("code scan: %+v", s)
	}
	if m, why, err := Select("auto", s); err != nil || m != Base || !strings.Contains(why, "0x3000200000") {
		t.Errorf("auto with a carveout constant: %v %q %v", m, why, err)
	}
	if _, _, err := Select("identity", s); err == nil || !strings.Contains(err.Error(), "title needs base addressing") {
		t.Errorf("identity with a carveout constant: %v", err)
	}
	if m, _, err := Select("base", s); err != nil || m != Base {
		t.Errorf("base: %v %v", m, err)
	}

	// Data: an unrelocated address-like word counts, a relocated one (an image pointer) and an
	// unaligned or small value do not.
	data := make([]byte, 64)
	binary.LittleEndian.PutUint64(data[0:], 0x200000000)   // counted
	binary.LittleEndian.PutUint64(data[8:], 0x240000000)   // relocated: skipped
	binary.LittleEndian.PutUint64(data[16:], 0x200000123)  // not page aligned
	binary.LittleEndian.PutUint64(data[24:], 0x40000000)   // below the carveout window: a size
	binary.LittleEndian.PutUint64(data[32:], 0x8000000000) // above the carveout: identity can serve it
	// code loads every word (a data word is evidence only when code addresses it: select.go)
	var loads []byte
	for off := uint64(0); off < 40; off += 8 {
		loads = append(loads, ripLoad(uint64(len(loads)), 0x4000+off)...)
	}
	s = ScanTitle(scanImage(append(loads, 0xc3), data, []elfloader.Relocation{{Offset: 0x4008, Type: elfloader.R_X86_64_RELATIVE},
		{Offset: 0x4030, Type: elfloader.R_X86_64_GLOB_DAT, SymName: "sceKernelMapDirectMemory"}}), true)
	if s.CarveoutConstCount != 1 || s.CarveoutConsts[0].Where != 0x4000 || s.CarveoutConsts[0].Kind != "data" {
		t.Fatalf("data scan: %+v", s)
	}
	if len(s.FixedMapImports) != 1 || s.FixedMapImports[0] != "sceKernelMapDirectMemory" {
		t.Errorf("fixed-map imports: %v", s.FixedMapImports)
	}

	clean := ScanTitle(scanImage([]byte{0xc3}, make([]byte, 64), nil), true)
	if m, _, err := Select("auto", clean); err != nil || m != Identity {
		t.Errorf("auto on a clean title: %v %v", m, err)
	}
	if m, _, err := Select("", clean); err != nil || m != Base {
		t.Errorf("default mode is not base: %v %v", m, err)
	}
	fixed := ScanTitle(scanImage([]byte{0xc3}, make([]byte, 64), nil), false)
	if m, why, _ := Select("auto", fixed); m != Base || !strings.Contains(why, "fixed address") {
		t.Errorf("auto on a fixed-address executable: %v %q", m, why)
	}
	if _, _, err := Select("flat", clean); err == nil {
		t.Error("unknown mode accepted")
	}

	var merged Scan
	merged.Relocatable = true
	merged.Merge(s)
	merged.Merge(s)
	if merged.CarveoutConstCount != 2 || len(merged.FixedMapImports) != 1 {
		t.Errorf("merge: %+v", merged)
	}
}

func TestManifest(t *testing.T) {
	l := LayoutFor(Base)
	m := NewManifest(l, "requested", 0x400000, 0x418000, []Region{NewRegion("eboot.bin", 0x400000, 0x15000)}, Scan{Relocatable: true})
	js, err := m.JSON()
	if err != nil {
		t.Fatal(err)
	}
	var back Manifest
	if err := json.Unmarshal(js, &back); err != nil {
		t.Fatalf("%v\n%s", err, js)
	}
	if back.HostBase != 1<<40 || back.GuestHi != 0xFC_0000_0000 || back.Regions[0].Addr != 0x400000 || back.AddrMode != "base" {
		t.Errorf("round trip: %+v", back)
	}
	if !strings.Contains(string(js), `"host_base": "0x10000000000"`) {
		t.Errorf("host_base is not a hex string:\n%s", js)
	}
	c := m.CConfig()
	for _, want := range []string{".addr_mode = NPS5_ADDR_BASE", ".host_base = 0x10000000000ULL", ".image_end = 0x418000ULL", ".dyn_lo = 0x200000000ULL", ".abi = 1"} {
		if !strings.Contains(c, want) {
			t.Errorf("C config lacks %q:\n%s", want, c)
		}
	}
	if ic := NewManifest(LayoutFor(Identity), "auto", 0, 0, nil, Scan{}).CConfig(); !strings.Contains(ic, "NPS5_ADDR_IDENTITY") || !strings.Contains(ic, ".host_base = 0x0ULL") {
		t.Errorf("identity C config:\n%s", ic)
	}
}
