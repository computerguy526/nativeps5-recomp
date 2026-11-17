package elfloader

import (
	"debug/elf"
	"os"
	"testing"
)

func TestPRXJumpSlotMatchesELF(t *testing.T) {
	prxPath := "../../tools/OpenOrbis/PS4Toolchain/samples/using_library/sce_module/libExample.prx"
	elfPath := "../../tools/OpenOrbis/PS4Toolchain/samples/library_example/library_example/x64/Debug/library_example.elf"
	if _, err := os.Stat(prxPath); err != nil {
		t.Skip("libExample.prx not present")
	}
	if _, err := os.Stat(elfPath); err != nil {
		t.Skip("library_example.elf not present")
	}

	prx, err := LoadELF(prxPath)
	if err != nil {
		t.Fatal(err)
	}
	plain, err := LoadELF(elfPath)
	if err != nil {
		t.Fatal(err)
	}

	elfByOff := map[uint64]string{}
	for _, rel := range plain.Relocations {
		if rel.Type == R_X86_64_JUMP_SLOT {
			elfByOff[rel.Offset] = rel.SymName
		}
	}
	prxByOff := map[uint64]string{}
	for _, rel := range prx.Relocations {
		if rel.Type == R_X86_64_JUMP_SLOT {
			prxByOff[rel.Offset] = NIDPrefix(rel.SymName)
		}
	}

	lockOff := uint64(0x1100b0)
	if elfByOff[lockOff] != "pthread_mutex_lock" {
		t.Fatalf("ELF GOT 0x%x = %q", lockOff, elfByOff[lockOff])
	}
	wantNID := CalculateNID("pthread_mutex_lock")
	if prxByOff[lockOff] != wantNID {
		t.Fatalf("PRX GOT 0x%x = %q want mutex_lock NID %s", lockOff, prxByOff[lockOff], wantNID)
	}
}

func TestResolveModuleRelocations(t *testing.T) {
	main := &LoadedELF{
		MemoryImage: NewImage(0, make([]byte, 0x2000)),
		Relocations: []Relocation{
			{Offset: 0x1000, Type: R_X86_64_JUMP_SLOT, SymName: "sample_fn"},
			{Offset: 0x1010, Type: R_X86_64_GLOB_DAT, SymName: "unresolved_fn"},
		},
	}
	exports := []Symbol{
		{Name: "sample_fn", Address: 0x1500},
	}
	if err := ResolveModuleRelocations(main, exports); err != nil {
		t.Fatalf("ResolveModuleRelocations failed: %v", err)
	}

	gotAddr := imgU64(main.MemoryImage, 0x1000) & 0xffffff
	if gotAddr != 0x1500 {
		t.Fatalf("expected resolved address 0x1500 at 0x1000, got 0x%x", gotAddr)
	}

	gotUnresolved := imgU64(main.MemoryImage, 0x1010) & 0xffffff
	if gotUnresolved != 0x1010 {
		t.Fatalf("expected fallback target 0x1010 at 0x1010, got 0x%x", gotUnresolved)
	}
}

// TestResolveImportsByKind: an import binds only to a definition of its own kind, and never to
// a local symbol. The payload SDK's startup objects define local data pointers named after libc
// functions ("strlen", "malloc", ...); binding the function import to one made the first call
// jump into .bss ("Unresolved indirect jump/call to 0x148d8").
func TestResolveImportsByKind(t *testing.T) {
	const (
		slotStrlen  = 0x1000 // GLOB_DAT, function import; only a local OBJECT "strlen" exists
		slotMalloc  = 0x1008 // GLOB_DAT, function import; a global OBJECT "malloc" exists
		slotHelper  = 0x1010 // GLOB_DAT, function import; a global FUNC "helper" exists
		slotEnviron = 0x1018 // GLOB_DAT, data import; a global OBJECT "environ" exists
		slotPlt     = 0x1020 // JUMP_SLOT without a typed dynsym; global OBJECT "memcpy" exists
		slotLocalFn = 0x1028 // GLOB_DAT, function import; only a local FUNC "hidden" exists
	)
	main := &LoadedELF{
		MemoryImage: NewImage(0, make([]byte, 0x3000)),
		MinVAddr:    0,
		MaxVAddr:    0x3000,
		DynSymbols: []Symbol{
			{},
			{Name: "strlen", Type: elf.STT_FUNC},
			{Name: "malloc", Type: elf.STT_FUNC},
			{Name: "helper", Type: elf.STT_FUNC},
			{Name: "environ", Type: elf.STT_OBJECT},
			{Name: "memcpy", Type: elf.STT_NOTYPE},
			{Name: "hidden", Type: elf.STT_FUNC},
		},
		Symbols: []Symbol{
			{Name: "strlen", Address: 0x2800, Size: 8, Type: elf.STT_OBJECT, Local: true},
			{Name: "malloc", Address: 0x2808, Size: 8, Type: elf.STT_OBJECT},
			{Name: "helper", Address: 0x1500, Size: 16, Type: elf.STT_FUNC},
			{Name: "environ", Address: 0x2810, Size: 8, Type: elf.STT_OBJECT},
			{Name: "memcpy", Address: 0x2818, Size: 8, Type: elf.STT_OBJECT},
			{Name: "hidden", Address: 0x1600, Size: 16, Type: elf.STT_FUNC, Local: true},
		},
		Relocations: []Relocation{
			{Offset: slotStrlen, Type: R_X86_64_GLOB_DAT, SymIdx: 1, SymName: "strlen"},
			{Offset: slotMalloc, Type: R_X86_64_GLOB_DAT, SymIdx: 2, SymName: "malloc"},
			{Offset: slotHelper, Type: R_X86_64_GLOB_DAT, SymIdx: 3, SymName: "helper"},
			{Offset: slotEnviron, Type: R_X86_64_GLOB_DAT, SymIdx: 4, SymName: "environ"},
			{Offset: slotPlt, Type: R_X86_64_JUMP_SLOT, SymIdx: 5, SymName: "memcpy"},
			{Offset: slotLocalFn, Type: R_X86_64_GLOB_DAT, SymIdx: 6, SymName: "hidden"},
		},
	}
	if err := ResolveModuleRelocations(main, nil); err != nil {
		t.Fatal(err)
	}
	for _, c := range []struct {
		name string
		slot uint64
		want uint64
	}{
		{"function import vs local data symbol: left for the shim", slotStrlen, slotStrlen},
		{"function import vs global data symbol: left for the shim", slotMalloc, slotMalloc},
		{"function import vs global function", slotHelper, 0x1500},
		{"data import vs global data symbol", slotEnviron, 0x2810},
		{"PLT slot vs global data symbol: left for the shim", slotPlt, slotPlt},
		{"function import vs local function: left for the shim", slotLocalFn, slotLocalFn},
	} {
		if got := imgU64(main.MemoryImage, c.slot); got != c.want {
			t.Errorf("%s: slot 0x%x = 0x%x, want 0x%x", c.name, c.slot, got, c.want)
		}
	}

	// A companion module's exports obey the same rule, and locals are not exported at all.
	mod := &LoadedELF{MemoryImage: NewImage(0x10000, make([]byte, 0x1000)), Symbols: []Symbol{
		{Name: "strlen", Address: 0x10100, Type: elf.STT_OBJECT},
		{Name: "malloc", Address: 0x10200, Type: elf.STT_FUNC},
		{Name: "hidden", Address: 0x10300, Type: elf.STT_FUNC, Local: true},
	}}
	exports := mod.ExportedFunctions()
	for _, e := range exports {
		if e.Name == "hidden" {
			t.Error("a local symbol was exported")
		}
	}
	if err := ResolveModuleRelocations(main, exports); err != nil {
		t.Fatal(err)
	}
	if got := imgU64(main.MemoryImage, slotStrlen); got != slotStrlen {
		t.Errorf("function import bound to a companion's data export: 0x%x", got)
	}
	if got := imgU64(main.MemoryImage, slotMalloc); got != 0x10200 {
		t.Errorf("function import not bound to the companion's function export: 0x%x", got)
	}
}
