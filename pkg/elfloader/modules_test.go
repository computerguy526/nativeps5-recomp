package elfloader

import (
	"debug/elf"
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"
)

func TestMapGuestPath(t *testing.T) {
	app := filepath.Join(t.TempDir(), "game")
	if got := MapGuestPath("/app0/sce_module/libExample.prx", app); got != filepath.Join(app, "sce_module", "libExample.prx") {
		t.Fatalf("got %q", got)
	}
	if got := MapGuestPath("/app0", app); got != app {
		t.Fatalf("got %q", got)
	}
	if got := MapGuestPath("/data/save", app); got != "" {
		t.Fatalf("non-app0 absolute path must not map onto appDir, got %q", got)
	}
	if got := MapGuestPath("sce_module/libExample.prx", app); got != filepath.Join(app, "sce_module", "libExample.prx") {
		t.Fatalf("got %q", got)
	}
	if got := MapGuestPath("%s/Modules/Il2CppUserAssemblies.prx", app); got != filepath.Join(app, "Modules", "Il2CppUserAssemblies.prx") {
		t.Fatalf("got %q", got)
	}
	if got := MapGuestPath("/app0/%s/Modules/PS4Util.prx", app); got != filepath.Join(app, "Modules", "PS4Util.prx") {
		t.Fatalf("got %q", got)
	}
}

func TestDiscoverModulesDirectory(t *testing.T) {
	dir := t.TempDir()
	modDir := filepath.Join(dir, "Modules")
	if err := os.Mkdir(modDir, 0o755); err != nil {
		t.Fatal(err)
	}
	prxPath := filepath.Join(modDir, "Il2CppUserAssemblies.prx")
	if err := os.WriteFile(prxPath, []byte("fake-elf-data"), 0o644); err != nil {
		t.Fatal(err)
	}

	img := append([]byte{0, 0, 0, 0}, []byte("%s/Modules/Il2CppUserAssemblies.prx\x00")...)
	// Discover with empty appDir, but elfPath pointing to eboot.bin in dir
	refs := DiscoverCompanionModules(filepath.Join(dir, "eboot.bin"), "", NewImage(0, img))
	if len(refs) != 1 {
		t.Fatalf("expected 1 module, got %+v", refs)
	}
	if filepath.Base(refs[0].Path) != "Il2CppUserAssemblies.prx" {
		t.Fatalf("unexpected path: %s", refs[0].Path)
	}
}

func TestReferencedModuleNames(t *testing.T) {
	img := make([]byte, 64)
	copy(img[8:], []byte("/app0/sce_module/libExample.prx\x00"))
	copy(img[40:], []byte("libc.prx\x00"))
	names := ReferencedModuleNames(NewImage(0, img))
	if len(names) != 1 || names[0] != "/app0/sce_module/libExample.prx" {
		t.Fatalf("names=%v", names)
	}
}

func TestDiscoverResolvesApp0(t *testing.T) {
	dir := t.TempDir()
	modDir := filepath.Join(dir, "sce_module")
	if err := os.Mkdir(modDir, 0o755); err != nil {
		t.Fatal(err)
	}
	prx := filepath.Join(modDir, "libExample.prx")
	if err := os.WriteFile(prx, []byte("not-an-elf"), 0o644); err != nil {
		t.Fatal(err)
	}
	img := append([]byte{0, 0, 0, 0}, []byte("/app0/sce_module/libExample.prx\x00")...)
	refs := DiscoverCompanionModules("", dir, NewImage(0, img))
	if len(refs) != 1 {
		t.Fatalf("expected 1 module, got %+v", refs)
	}
	if filepath.Base(refs[0].Path) != "libExample.prx" {
		t.Fatalf("path=%s", refs[0].Path)
	}
}

func TestRetailModuleNotStub(t *testing.T) {
	dir := t.TempDir()
	libcStub := filepath.Join(dir, "libc.prx")
	if err := os.WriteFile(libcStub, make([]byte, 1024), 0o644); err != nil {
		t.Fatal(err)
	}
	if !IsSystemStubModule(libcStub) {
		t.Fatalf("expected %s (< 64KB) to be a system stub", libcStub)
	}

	libcPath := filepath.Join(dir, "retail", "libc.prx")
	if err := os.MkdirAll(filepath.Dir(libcPath), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(libcPath, make([]byte, 128*1024), 0o644); err != nil {
		t.Fatal(err)
	}
	if IsSystemStubModule(libcPath) {
		t.Fatalf("expected %s (> 64KB) to NOT be treated as a system stub", libcPath)
	}
}

func TestDiscoverPrxDir(t *testing.T) {
	dir := t.TempDir()
	prxDir := filepath.Join(dir, "prx")
	if err := os.Mkdir(prxDir, 0o755); err != nil {
		t.Fatal(err)
	}
	plugin := filepath.Join(prxDir, "akplugin.prx")
	if err := os.WriteFile(plugin, []byte("stub"), 0o644); err != nil {
		t.Fatal(err)
	}
	refs := DiscoverCompanionModules("", dir, nil)
	found := false
	for _, ref := range refs {
		if filepath.Base(ref.Path) == "akplugin.prx" {
			found = true
			break
		}
	}
	if !found {
		t.Fatalf("expected akplugin.prx from prx directory to be discovered, got %+v", refs)
	}
}

func TestDiscoverRecursivePrx(t *testing.T) {
	dir := t.TempDir()
	nestedDir := filepath.Join(dir, "Media", "Plugins", "Sub")
	if err := os.MkdirAll(nestedDir, 0o755); err != nil {
		t.Fatal(err)
	}
	plugin := filepath.Join(nestedDir, "custom_plugin.prx")
	if err := os.WriteFile(plugin, []byte("stub"), 0o644); err != nil {
		t.Fatal(err)
	}
	refs := DiscoverCompanionModules(filepath.Join(dir, "eboot.bin"), "", nil)
	found := false
	for _, ref := range refs {
		if filepath.Base(ref.Path) == "custom_plugin.prx" {
			found = true
			break
		}
	}
	if !found {
		t.Fatalf("expected custom_plugin.prx in nested directory to be recursively discovered, got %+v", refs)
	}
}

func TestApplyBiasCanary(t *testing.T) {
	img := make([]byte, 0x10000)
	const origCanaryAddr = 0x5000
	const gotOffset = 0x2000
	const canaryVal = uint64(0x595e9fbd94fda766)
	binary.LittleEndian.PutUint64(img[origCanaryAddr:origCanaryAddr+8], canaryVal)
	binary.LittleEndian.PutUint64(img[gotOffset:gotOffset+8], origCanaryAddr)

	loaded := &LoadedELF{
		MinVAddr:    0x1000,
		MaxVAddr:    0x6000,
		CanaryAddr:  origCanaryAddr,
		MemoryImage: NewImage(0, img),
		Relocations: []Relocation{
			{Offset: gotOffset, Type: R_X86_64_GLOB_DAT, SymName: "f7uOxY9mM1U#D#D"},
		},
	}

	const delta = uint64(0x20000)
	if err := loaded.ApplyBias(delta); err != nil {
		t.Fatalf("ApplyBias failed: %v", err)
	}

	if loaded.CanaryAddr != origCanaryAddr+delta {
		t.Fatalf("expected CanaryAddr 0x%x, got 0x%x", origCanaryAddr+delta, loaded.CanaryAddr)
	}

	newGotOffset := gotOffset + delta
	gotPtr := imgU64(loaded.MemoryImage, newGotOffset)
	if gotPtr != loaded.CanaryAddr {
		t.Fatalf("expected GOT entry to point to shifted canary 0x%x, got 0x%x", loaded.CanaryAddr, gotPtr)
	}
}

func TestMergeImagesCanary(t *testing.T) {
	const dstCanaryAddr = 0x8000
	const canaryVal = uint64(0x595e9fbd94fda766)

	dstImg := make([]byte, 0x9000)
	binary.LittleEndian.PutUint64(dstImg[dstCanaryAddr:dstCanaryAddr+8], canaryVal)
	dst := &LoadedELF{
		FileName:    "main",
		MinVAddr:    0x1000,
		MaxVAddr:    0x9000,
		CanaryAddr:  dstCanaryAddr,
		MemoryImage: NewImage(0, dstImg),
	}

	const srcCanaryAddr = 0x15000
	const srcGotOffset = 0x12000
	srcImg := make([]byte, 0x16000)
	binary.LittleEndian.PutUint64(srcImg[srcCanaryAddr:srcCanaryAddr+8], canaryVal)
	binary.LittleEndian.PutUint64(srcImg[srcGotOffset:srcGotOffset+8], srcCanaryAddr)
	src := &LoadedELF{
		FileName:    "companion.prx",
		MinVAddr:    0x10000,
		MaxVAddr:    0x16000,
		CanaryAddr:  srcCanaryAddr,
		MemoryImage: NewImage(0x10000, srcImg[0x10000:]),
		Relocations: []Relocation{
			{Offset: srcGotOffset, Type: R_X86_64_GLOB_DAT, SymName: "__stack_chk_guard"},
		},
	}

	if err := MergeImages(dst, src); err != nil {
		t.Fatalf("MergeImages failed: %v", err)
	}

	if dst.CanaryAddr != dstCanaryAddr {
		t.Fatalf("expected dst CanaryAddr 0x%x, got 0x%x", dstCanaryAddr, dst.CanaryAddr)
	}

	gotPtr := imgU64(dst.MemoryImage, srcGotOffset)
	if gotPtr != dstCanaryAddr {
		t.Fatalf("expected merged GOT entry to point to process canary 0x%x, got 0x%x", dstCanaryAddr, gotPtr)
	}
}

// TestMergeImagesKeepsTLSSymbolsOutOfAddressMaps: an STT_TLS symbol's value is an offset
// in the module's TLS block, not a guest address. Merging libc.prx's TLS symbol 0x1d0
// into the main image's SymbolByAddr made the disassembler treat that eboot code address
// as an 8-byte function.
func TestMergeImagesKeepsTLSSymbolsOutOfAddressMaps(t *testing.T) {
	dst := &LoadedELF{
		MemoryImage:  NewImage(0, make([]byte, 0x1000)),
		MinVAddr:     0,
		MaxVAddr:     0x1000,
		SymbolByAddr: map[uint64]Symbol{},
		SymbolByName: map[string]Symbol{},
	}
	src := &LoadedELF{
		MemoryImage: NewImage(0x2000, make([]byte, 0x1000)),
		MinVAddr:    0x2000,
		MaxVAddr:    0x3000,
		Symbols: []Symbol{
			{Name: "J-oDqE1N8R4", Address: 0x100, Size: 8, Type: elf.STT_TLS},
			{Name: "libc_fn", Address: 0x2000, Size: 0x20, Type: elf.STT_FUNC},
		},
		DynSymbols: []Symbol{
			{Name: "", Address: 0},
			{Name: "J-oDqE1N8R4", Address: 0x100, Size: 8, Type: elf.STT_TLS},
		},
	}
	if err := MergeImages(dst, src); err != nil {
		t.Fatalf("MergeImages failed: %v", err)
	}
	if _, ok := dst.SymbolByAddr[0x100]; ok {
		t.Fatalf("TLS offset 0x100 entered SymbolByAddr: %+v", dst.SymbolByAddr[0x100])
	}
	if _, ok := dst.SymbolByName["J-oDqE1N8R4"]; ok {
		t.Fatalf("TLS symbol name entered SymbolByName")
	}
	if got, ok := dst.SymbolByAddr[0x2000]; !ok || got.Name != "libc_fn" {
		t.Fatalf("function symbol missing after merge: %+v", dst.SymbolByAddr[0x2000])
	}
	if len(src.Symbols) != 2 || src.Symbols[0].Address != 0x100 {
		t.Fatalf("source symbols were modified: %+v", src.Symbols)
	}
	if len(src.DynSymbols) != 2 {
		t.Fatalf("source dynsymbols were modified: %+v", src.DynSymbols)
	}
}

// An image linked at address 0 (the PS5 hello_world.elf) has its .text, entry, first segment and
// some symbols at link address 0; ApplyBias must move them with everything else (review 1, m10).
func TestApplyBiasImageLinkedAtZero(t *testing.T) {
	const delta = uint64(0x400000)
	loaded := &LoadedELF{
		MinVAddr:    0,
		MaxVAddr:    0x8000,
		EntryPoint:  0,
		Seg0Addr:    0,
		Segments:    []*Segment{{Vaddr: 0, Memsz: 0x8000}},
		MemoryImage: NewImage(0, make([]byte, 0x8000)),
		Sections: map[string]*elf.Section{
			".text":    {SectionHeader: elf.SectionHeader{Name: ".text", Addr: 0, Size: 0x100, Flags: elf.SHF_ALLOC | elf.SHF_EXECINSTR}},
			".symtab":  {SectionHeader: elf.SectionHeader{Name: ".symtab", Addr: 0, Size: 0x40}},
			".rodata":  {SectionHeader: elf.SectionHeader{Name: ".rodata", Addr: 0x1000, Size: 0x40, Flags: elf.SHF_ALLOC}},
			".comment": {SectionHeader: elf.SectionHeader{Name: ".comment", Addr: 0, Size: 0x10}},
		},
		Symbols: []Symbol{
			{Name: "__text_start", Address: 0, Type: elf.STT_NOTYPE, Section: 1},
			{Name: "main", Address: 0x40, Type: elf.STT_FUNC, Section: 1},
			{Name: "undef", Address: 0, Type: elf.STT_FUNC, Section: elf.SHN_UNDEF},
			{Name: "abs", Address: 0x1234, Type: elf.STT_NOTYPE, Section: elf.SHN_ABS},
			{Name: "tlsvar", Address: 0, Type: elf.STT_TLS, Section: 4},
		},
	}
	if err := loaded.ApplyBias(delta); err != nil {
		t.Fatal(err)
	}
	if loaded.Seg0Addr != delta {
		t.Errorf("seg0 0x%x: want 0x%x", loaded.Seg0Addr, delta)
	}
	if loaded.EntryPoint != 0 { // 0 is "no entry point", not an address
		t.Errorf("unset entry point became 0x%x", loaded.EntryPoint)
	}
	if got := loaded.Sections[".text"].Addr; got != delta {
		t.Errorf(".text at 0x%x, want 0x%x", got, delta)
	}
	if got := loaded.Sections[".rodata"].Addr; got != 0x1000+delta {
		t.Errorf(".rodata at 0x%x", got)
	}
	for _, n := range []string{".symtab", ".comment"} {
		if got := loaded.Sections[n].Addr; got != 0 {
			t.Errorf("non-allocated %s moved to 0x%x", n, got)
		}
	}
	want := map[string]uint64{"__text_start": delta, "main": 0x40 + delta, "undef": 0, "abs": 0x1234, "tlsvar": 0}
	for _, s := range loaded.Symbols {
		if s.Address != want[s.Name] {
			t.Errorf("symbol %s at 0x%x, want 0x%x", s.Name, s.Address, want[s.Name])
		}
	}
	if s, ok := loaded.SymbolByAddr[delta]; !ok || s.Name != "__text_start" {
		t.Errorf("SymbolByAddr[0x%x] = %+v", delta, s)
	}
	// A shared object (no segment at 0) keeps "0 means none" for an unset entry point.
	dso := &LoadedELF{MinVAddr: 0x1000, MaxVAddr: 0x3000, Segments: []*Segment{{Vaddr: 0x1000}}, MemoryImage: NewImage(0, nil)}
	if err := dso.ApplyBias(delta); err != nil {
		t.Fatal(err)
	}
	if dso.EntryPoint != 0 {
		t.Errorf("unset entry point became 0x%x", dso.EntryPoint)
	}
}

func TestHollowCode(t *testing.T) {
	code := make([]byte, 100000)
	copy(code[0x40:], []byte{0x31, 0xc0, 0xc3})
	if !hollowCode(code) {
		t.Fatal("zero-padded stub code must be hollow")
	}
	for i := range code {
		code[i] = 0x90
	}
	if hollowCode(code) {
		t.Fatal("real code must not be hollow")
	}
	if hollowCode(nil) {
		t.Fatal("no code is not a hollow module")
	}
}
