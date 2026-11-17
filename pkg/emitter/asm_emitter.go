package emitter

import (
	"bufio"
	"bytes"
	"cmp"
	"debug/elf"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"slices"
	"strings"

	"ps4-recomp/pkg/diag"
	"ps4-recomp/pkg/disasm"
	"ps4-recomp/pkg/elfloader"
	"ps4-recomp/pkg/guestmem"
	"ps4-recomp/pkg/lifter"
	"ps4-recomp/pkg/tcache"
)

type GuestModule struct {
	FileName       string
	Aliases        []string
	Exports        []elfloader.Symbol
	Init           []uint64
	StartAddr      uint64
	EndAddr        uint64
	Seg0Addr       uint64
	Seg0Size       uint64
	EHFrameHdrAddr uint64
	EHFrameHdrSize uint64
	EHFrameAddr    uint64
	EHFrameSize    uint64
	Segments       []*elfloader.Segment
}

func writeFileIfChanged(path string, data []byte) error {
	existing, err := os.ReadFile(path)
	if err == nil && bytes.Equal(existing, data) {
		return nil
	}
	return os.WriteFile(path, data, 0o644)
}

func uniqueModuleNames(fileName string, aliases []string) []string {
	seen := make(map[string]struct{})
	var out []string
	add := func(s string) {
		s = strings.TrimSpace(s)
		if s == "" {
			return
		}
		key := strings.ToLower(filepath.Base(s))
		if _, ok := seen[key]; ok {
			return
		}
		seen[key] = struct{}{}
		out = append(out, filepath.Base(s))
	}
	add(fileName)
	for _, a := range aliases {
		add(a)
	}
	return out
}

// AsmEmitter emits Apple Silicon ARM64 assembly (.s) files from disassembled functions.
type AsmEmitter struct {
	elf     *elfloader.LoadedELF
	disasm  *disasm.Disassembler
	lifter  *lifter.Arm64Lifter
	shimMap map[uint64]string
	AppDir  string
	// Mem is the guest memory model of the title (nil: the runtime's default base-mode
	// config is used, as in emitter tests).
	Mem          *guestmem.Manifest
	Modules      []GuestModule
	ChunkSize    int
	Funcs        []disasm.ReachableFunc
	HLE          *HLEReport
	linesBuf     []string
	knownFuncSet map[uint64]bool
	bb60fps      bool
	patch        *patchState // applied patch profile (patchpoints.go); nil = none
	// unresolved describes every import slot (PLT stub and GOT slot address) that no host
	// shim and no loaded module satisfies. Each one is bound to shim_unresolved_stub.
	unresolved map[uint64]unresolvedImport
	// FuncInfoOf, when set, extracts the census facts of every function just before it is lowered;
	// FuncInfos collects them in function order.
	FuncInfoOf func(fn *disasm.Function) diag.FuncInfo
	FuncInfos  []diag.FuncInfo
	// Workers is the number of chunks lifted concurrently (each on its own lifter and decoder
	// clone); the output does not depend on it. Cache, when set with a ChunkSink, skips lifting
	// the chunks whose key is unchanged. EmitDrops counts weak function entries that decoding
	// dropped after discovery (always 0 in practice; nonzero would make clones diverge).
	Workers   int
	Cache     *tcache.Index
	EmitDrops int
	// ChunkStats is filled by EmitChunkedAsm.
	ChunkStats ChunkStats
	// UnresolvedFuncImports and UnresolvedDataImports count the unresolved import relocations:
	// JUMP_SLOT ones are calls, GLOB_DAT/R_X86_64_64 ones are address loads (a data import is
	// bound to the same stub, so reading it as data yields the stub's address).
	UnresolvedFuncImports int
	UnresolvedDataImports int
	// UnresolvedDataNames lists the unresolved data imports, sorted, each as its name (the NID
	// when the NID database does not know it) and importing library.
	UnresolvedDataNames []string
	// Warnings collects generation-time problems that do not stop the generation (the caller
	// prints them in its summary).
	Warnings []string

	// Comments keeps the lifter's per-instruction annotations ("// 0xADDR: INST") and the
	// original formatting in the assembly. The default is lean output: no comments, tab
	// indentation, no blanks. Nothing depends on the comments (Facts and the pc map carry the
	// same information in structured form).
	Comments bool
	// PCMap emits the compact host-pc -> guest-pc table (__DATA,__nps5_pcmap) the runtime crash
	// handler reads (see pcmap.go). PCMapStrict turns a mismatch between the table and the
	// assembled code (asserted by the assembler) from a warning into an error.
	PCMap       bool
	PCMapStrict bool
	// ChunkSink, when set, receives each chunk's assembly instead of the emitter writing the
	// .s file: the sink assembles and deletes it (streaming build), so the whole program's
	// assembly text never exists at once.
	ChunkSink ChunkSink
	// Facts is the structured census/addrmap data collected while emitting.
	Facts diag.AsmFacts

	// replParams / replReports: function-level replacements (funcrepl.go).
	replParams  map[string]uint64
	replReports []*ReplReport
}

// AsmChunk is the rendered assembly of one chunk.
type AsmChunk struct {
	Index int
	Path  string // where the .s file belongs (the sink decides whether to write it)
	Data  []byte // the assembly text; valid until Chunk returns; nil when Cached
	Funcs int
	Insts int
	// DataHash is the sha256 of the assembly. Cached: the chunk is unchanged since the object the
	// sink already has (CanReuse said so); Data is nil.
	DataHash string
	Cached   bool
	Bytes    int64
}

// ObjectReuser is implemented by a sink that keeps the objects of earlier chunks: the emitter
// asks whether the object for a chunk whose assembly hashes to DataHash is still there before it
// skips lifting the chunk, and records its identity in the cache index afterwards.
type ObjectReuser interface {
	CanReuse(c AsmChunk) bool
	ObjectInfo(c AsmChunk) (hash string, size, zsize int64)
}

// ChunkStats summarises one EmitChunkedAsm.
type ChunkStats struct {
	Chunks, Lifted, FromCache, Workers int
	LiftCPU                            float64
}

// ChunkSink consumes rendered chunks one at a time, in order. It may block (back-pressure
// from a bounded assembler pool) and may return an error to stop the emission.
type ChunkSink interface {
	Chunk(c AsmChunk) error
}

// maxModuleSegments is the number of segments a module registers with the runtime
// (RecompModuleUnwindInfo.segments, the four of SceKernelModuleInfo).
const maxModuleSegments = 4

// registeredSegments returns the segments of module name that fit the runtime's table, with a
// warning in e.Warnings when some do not.
func (e *AsmEmitter) registeredSegments(name string, segs []*elfloader.Segment) []*elfloader.Segment {
	if len(segs) <= maxModuleSegments {
		return segs
	}
	e.Warnings = append(e.Warnings, fmt.Sprintf("module %s has %d segments; module info reports only the first %d",
		name, len(segs), maxModuleSegments))
	return segs[:maxModuleSegments]
}

// unresolvedImport is what shim_unresolved_stub reports when the guest calls a missing import.
type unresolvedImport struct {
	nid  string // NID portion of the dynsym name
	name string // plaintext symbol name when the NID database knows it
	lib  string // importing library as named by DT_SCE_IMPORT_LIB
}

// unresolvedStubShim is the host function every unresolved import reports through.
const unresolvedStubShim = "shim_unresolved_stub"

// collectUnresolvedImports finds the import slots that remain self-bound after module
// relocation: elfloader.ResolveModuleRelocations leaves a slot pointing at its own PLT stub
// (or at itself when there is no PLT) when neither a module export nor the canary applies, and
// the emitter binds a host shim to the slots it can serve. What is left would jump to itself
// forever, so it is routed to shim_unresolved_stub with its name instead.
func collectUnresolvedImports(loaded *elfloader.LoadedELF, shimMap map[uint64]string) (out map[uint64]unresolvedImport, funcs, data int, dataNames []string) {
	out = make(map[uint64]unresolvedImport)
	seenData := make(map[string]bool)
	for _, rel := range loaded.Relocations {
		switch rel.Type {
		case elfloader.R_X86_64_JUMP_SLOT, elfloader.R_X86_64_GLOB_DAT, elfloader.R_X86_64_64:
		default:
			continue
		}
		if rel.SymName == "" {
			continue
		}
		slot, mapped := loaded.MemoryImage.U64(rel.Offset)
		if !mapped {
			continue
		}
		excludedMod, excluded := loaded.IsExcludedImport(rel.SymName)
		if _, ok := LookupShim(rel.SymName); ok && !excluded {
			continue
		}
		if slot != rel.Offset && (rel.PltAddr == 0 || slot != rel.PltAddr) {
			continue
		}
		imp := unresolvedImport{nid: elfloader.NIDPrefix(rel.SymName), lib: loaded.LibraryForNID(rel.SymName)}
		if canon, ok := elfloader.ResolveNID(rel.SymName); ok {
			imp.name = canon
		}
		if excluded {
			imp.lib = "excluded:" + excludedMod
		}
		if rel.Type == elfloader.R_X86_64_JUMP_SLOT {
			funcs++
		} else {
			data++
			label := imp.name
			if label == "" {
				label = imp.nid
			}
			if imp.lib != "" {
				label += " (" + imp.lib + ")"
			}
			if !seenData[label] {
				seenData[label] = true
				dataNames = append(dataNames, label)
			}
		}
		for _, addr := range []uint64{rel.PltAddr, rel.Offset} {
			if addr == 0 {
				continue
			}
			if _, bound := shimMap[addr]; bound {
				continue
			}
			if _, dup := out[addr]; !dup {
				out[addr] = imp
			}
		}
	}
	slices.Sort(dataNames)
	return out, funcs, data, dataNames
}

// NewAsmEmitter creates a new ARM64 assembly emitter.
func NewAsmEmitter(loaded *elfloader.LoadedELF, d *disasm.Disassembler, l *lifter.Arm64Lifter) *AsmEmitter {
	shimMap := make(map[uint64]string)
	bindAddr := func(name string, addr uint64) {
		if name == "" || addr == 0 {
			return
		}
		if shim, ok := LookupShim(name); ok && !isFallbackShim(shim) {
			shimMap[addr] = shim
		}
	}
	// A fallback (HLE libc) shim only serves an import that nothing else satisfied: the slot
	// still points at itself or at its own PLT stub after module relocation.
	slotSatisfied := func(rel elfloader.Relocation) bool {
		slot, mapped := loaded.MemoryImage.U64(rel.Offset)
		if !mapped {
			return false
		}
		return slot != rel.Offset && (rel.PltAddr == 0 || slot != rel.PltAddr)
	}
	for _, rel := range loaded.Relocations {
		if _, ex := loaded.IsExcludedImport(rel.SymName); ex {
			continue // bound to the excluded-module stub by collectUnresolvedImports
		}
		if rel.SymName != "" {
			if shim, ok := LookupShim(rel.SymName); ok && isFallbackShim(shim) && slotSatisfied(rel) {
				continue
			}
		}
		if rel.Type == elfloader.R_X86_64_JUMP_SLOT && loaded.MemoryImage.Has(rel.Offset, 8) {
			if rel.SymName != "" {
				if shim, ok := LookupShim(rel.SymName); ok {
					if rel.PltAddr != 0 {
						shimMap[rel.PltAddr] = shim
					}
					shimMap[rel.Offset] = shim
					loaded.MemoryImage.PutU64(rel.Offset, rel.Offset)
					continue
				}
			}
			continue
		}
		if rel.SymName == "" {
			continue
		}
		shim, ok := LookupShim(rel.SymName)
		if !ok {
			continue
		}
		if rel.PltAddr != 0 {
			shimMap[rel.PltAddr] = shim
		}
		shimMap[rel.Offset] = shim
	}
	// Only code symbols: the PS5 SDK's libc++ keeps data objects named after the functions they
	// point to (".bss calloc"), and binding those would turn a data address into a shim.
	for _, sym := range loaded.Symbols {
		if sym.Type != elf.STT_OBJECT && sym.Type != elf.STT_TLS {
			bindAddr(sym.Name, sym.Address)
		}
	}
	for _, sym := range loaded.DynSymbols {
		if sym.Type != elf.STT_OBJECT && sym.Type != elf.STT_TLS {
			bindAddr(sym.Name, sym.Address)
		}
	}

	unresolved, unresFuncs, unresData, unresDataNames := collectUnresolvedImports(loaded, shimMap)
	for addr := range unresolved {
		shimMap[addr] = unresolvedStubShim
	}

	return &AsmEmitter{
		elf:        loaded,
		disasm:     d,
		lifter:     l,
		shimMap:    shimMap,
		unresolved: unresolved,
		bb60fps:    isBloodborne109(loaded),

		UnresolvedFuncImports: unresFuncs,
		UnresolvedDataImports: unresData,
		UnresolvedDataNames:   unresDataNames,
	}
}

// ResolveEntryAddress dynamically resolves the primary guest entry point.
func (e *AsmEmitter) ResolveEntryAddress() (uint64, string, error) {
	if e.elf.EntryPoint != 0 {
		return e.elf.EntryPoint, "_start", nil
	}
	if mainSym, ok := e.elf.SymbolByName["main"]; ok && mainSym.Address != 0 {
		return mainSym.Address, "main", nil
	}
	return 0, "", fmt.Errorf("ELF has no entry point and no main symbol")
}

// EmitAll generates all .s assembly files and runtime C files in outDir.
func (e *AsmEmitter) EmitAll(outDir string) ([]string, error) {
	if err := os.MkdirAll(outDir, 0o755); err != nil {
		return nil, err
	}

	var buildFiles []string

	mainPath := filepath.Join(outDir, "main.c")
	if err := e.EmitMainRunner(mainPath); err != nil {
		return nil, fmt.Errorf("failed to emit main.c: %w", err)
	}
	buildFiles = append(buildFiles, mainPath)

	binPath := filepath.Join(outDir, "guest_image.bin")
	if err := e.EmitGuestImage(binPath); err != nil {
		return nil, fmt.Errorf("failed to emit guest_image.bin: %w", err)
	}

	if e.Mem != nil {
		mj, err := e.Mem.JSON()
		if err != nil {
			return nil, err
		}
		if err := writeFileIfChanged(filepath.Join(outDir, "memory_manifest.json"), mj); err != nil {
			return nil, err
		}
	}

	trampPath := filepath.Join(outDir, "trampoline_arm64.s")
	if err := e.EmitTrampoline(trampPath); err != nil {
		return nil, fmt.Errorf("failed to emit trampoline_arm64.s: %w", err)
	}
	buildFiles = append(buildFiles, trampPath)

	codeFiles, err := e.EmitChunkedAsm(outDir, 15000)
	if err != nil {
		return nil, fmt.Errorf("failed to emit chunked assembly: %w", err)
	}
	buildFiles = append(buildFiles, codeFiles...)
	if err := e.checkUnsupported(outDir); err != nil {
		return nil, err
	}
	if err := e.CheckPatchSites(); err != nil {
		return nil, err
	}
	if pt, err := e.WritePatchTable(outDir); err != nil {
		return nil, fmt.Errorf("failed to write %s: %w", PatchTableFile, err)
	} else if pt != "" {
		buildFiles = append(buildFiles, pt)
	}
	if err := e.writeLifterManifest(outDir); err != nil {
		return nil, err
	}

	modPath := filepath.Join(outDir, "guest_modules.c")
	if err := e.EmitGuestModules(modPath); err != nil {
		return nil, fmt.Errorf("failed to emit guest_modules.c: %w", err)
	}
	buildFiles = append(buildFiles, modPath)
	synthPath := strings.TrimSuffix(modPath, ".c") + "_synth.s"
	if _, err := os.Stat(synthPath); err == nil {
		buildFiles = append(buildFiles, synthPath)
	}
	if p, err := e.emitReplParams(outDir); err != nil {
		return nil, err
	} else if p != "" {
		buildFiles = append(buildFiles, p)
	}

	return buildFiles, nil
}

// LifterManifest records the translate-time choices that the generated code depends on; the
// runtime/loader must agree with them (addr_mode "base": x28 = host base of guest memory,
// guest address added at every access; "identity": guest VA == host VA, x28 unused).
type LifterManifest struct {
	AddrMode  string `json:"addr_mode"`
	CallModel string `json:"call_model"`
	TSO       bool   `json:"tso"`
	// TSORelaxed: functions lifted without x86 memory ordering (NPS5_TSO_RELAX_FUNCS), as hex entry addresses.
	TSORelaxed []string `json:"tso_relaxed_functions,omitempty"`
	// TSOElide: which stack accesses are emitted without ordering ("off": every RSP/RBP-based
	// one, "stack": the proven-private ones only; docs/m3/tsoelide.md).
	TSOElide string `json:"tso_elide"`
	// ROLoads, RedZoneLeaf, BMIPeep: the m3-lift2 options (docs/m3/lift2.md).
	ROLoads          bool `json:"ro_loads"`
	RedZoneLeaf      bool `json:"redzone_leaf"`
	BMIPeep          bool `json:"bmi_peep"`
	AllowUnsupported bool `json:"allow_unsupported"`
	UnsupportedSites int  `json:"unsupported_sites"`
	// FuncRepl lists the function-level replacements in effect (funcrepl.go).
	FuncRepl []*ReplReport `json:"func_replace,omitempty"`
}

func hexAddrs(a []uint64) []string {
	var out []string
	for _, v := range a {
		out = append(out, fmt.Sprintf("0x%x", v))
	}
	return out
}

func (e *AsmEmitter) writeLifterManifest(outDir string) error {
	m := LifterManifest{
		AddrMode:         e.lifter.AddrMode().String(),
		CallModel:        lifter.GetCallModel().String(),
		TSO:              e.lifter.TSO(),
		TSORelaxed:       hexAddrs(e.lifter.TSORelaxed()),
		TSOElide:         e.lifter.TSOElide().String(),
		ROLoads:          lifter.GetROLoads(),
		RedZoneLeaf:      lifter.GetRedZoneLeaf(),
		BMIPeep:          lifter.GetBMIPeep(),
		AllowUnsupported: e.lifter.AllowUnsupported(),
		UnsupportedSites: len(e.lifter.Unsupported()),
		FuncRepl:         e.replReports,
	}
	b, err := json.MarshalIndent(m, "", "  ")
	if err != nil {
		return err
	}
	return writeFileIfChanged(filepath.Join(outDir, "lifter_manifest.json"), append(b, '\n'))
}

// checkUnsupported applies the unsupported-instruction policy after the code is emitted: the
// list goes to unsupported.txt, and unless the lifter allows unsupported instructions the
// translation fails with it.
func (e *AsmEmitter) checkUnsupported(outDir string) error {
	sites := e.lifter.Unsupported()
	if len(sites) == 0 {
		return nil
	}
	if err := os.WriteFile(filepath.Join(outDir, "unsupported.txt"), []byte(lifter.FormatUnsupported(sites, 0)), 0o644); err != nil {
		return err
	}
	if !e.lifter.AllowUnsupported() {
		return fmt.Errorf("%s(full list: %s)", lifter.FormatUnsupported(sites, 40), filepath.Join(outDir, "unsupported.txt"))
	}
	e.Warnings = append(e.Warnings, fmt.Sprintf("%d unsupported instruction(s) emitted as diagnostic stubs (see unsupported.txt)", len(sites)))
	return nil
}

// GuestImageMagic opens guest_image.bin (recomp_runtime.c: load_guest_image).
const GuestImageMagic = "NPS5IMG\x01"

// EncodeGuestImage serialises the sparse guest image for the runtime loader:
//
//	magic[8]  u32 nregions  u32 nsegments
//	nregions  x { u64 guest_addr, u64 size, u64 file_offset }
//	nsegments x { u64 guest_addr, u64 memsz, u32 pf_flags, u32 reserved }
//	region bytes, at their file offsets
//
// Regions are the contiguous byte runs to copy to guest addresses; segments carry the ELF
// protection of each mapped segment so the loader can map guest code read-only (it is data to
// the host and is never executed).
func EncodeGuestImage(img *elfloader.Image, segs []*elfloader.Segment) []byte {
	regions := img.Regions()
	hdr := 16 + 24*len(regions) + 24*len(segs)
	total := hdr
	for _, r := range regions {
		total += len(r.Data)
	}
	out := make([]byte, hdr, total)
	copy(out, GuestImageMagic)
	binary.LittleEndian.PutUint32(out[8:], uint32(len(regions)))
	binary.LittleEndian.PutUint32(out[12:], uint32(len(segs)))
	p := 16
	off := uint64(hdr)
	for _, r := range regions {
		binary.LittleEndian.PutUint64(out[p:], r.Addr)
		binary.LittleEndian.PutUint64(out[p+8:], uint64(len(r.Data)))
		binary.LittleEndian.PutUint64(out[p+16:], off)
		off += uint64(len(r.Data))
		p += 24
	}
	for _, s := range segs {
		binary.LittleEndian.PutUint64(out[p:], s.Vaddr)
		binary.LittleEndian.PutUint64(out[p+8:], s.Memsz)
		binary.LittleEndian.PutUint32(out[p+16:], uint32(s.Flags))
		p += 24
	}
	for _, r := range regions {
		out = append(out, r.Data...)
	}
	return out
}

// EmitGuestImage writes the loaded guest memory image (EncodeGuestImage).
func (e *AsmEmitter) EmitGuestImage(path string) error {
	return writeFileIfChanged(path, EncodeGuestImage(e.elf.MemoryImage, e.elf.CodeProtectionSegments()))
}

func (e *AsmEmitter) functionAddrs() []uint64 {
	if len(e.Funcs) > 0 {
		addrs := make([]uint64, len(e.Funcs))
		for i, f := range e.Funcs {
			addrs[i] = f.Addr
		}
		return addrs
	}
	if e.disasm == nil {
		return nil
	}
	addrs := make([]uint64, 0, len(e.disasm.Functions))
	for addr := range e.disasm.Functions {
		addrs = append(addrs, addr)
	}
	slices.Sort(addrs)
	return addrs
}

func (e *AsmEmitter) instCountByAddr() map[uint64]int {
	if len(e.Funcs) > 0 {
		m := make(map[uint64]int, len(e.Funcs))
		for _, f := range e.Funcs {
			m[f.Addr] = f.Insts
		}
		return m
	}
	if e.disasm == nil {
		return nil
	}
	m := make(map[uint64]int, len(e.disasm.Functions))
	for addr, fn := range e.disasm.Functions {
		count := 0
		if fn != nil {
			for _, b := range fn.Blocks {
				count += len(b.Insts)
			}
		}
		m[addr] = count
	}
	return m
}

func (e *AsmEmitter) loadFunction(addr uint64) (*disasm.Function, error) {
	if e.disasm.Functions != nil {
		if fn := e.disasm.Functions[addr]; fn != nil {
			return fn, nil
		}
	}
	fn, _, err := e.disasm.DisasmFunction(addr)
	return fn, err
}

// EmitChunkedAsm partitions recompiled functions across .s assembly files.
func (e *AsmEmitter) EmitChunkedAsm(outDir string, targetBudget int) ([]string, error) {
	fnAddrs := e.functionAddrs()

	if targetBudget <= 0 {
		targetBudget = 15000
	}
	maxFuncsPerChunk := e.ChunkSize
	if maxFuncsPerChunk <= 0 {
		maxFuncsPerChunk = 250
	}

	var chunks [][]uint64
	var currentChunk []uint64
	currentInsts := 0

	insts := e.instCountByAddr()
	for _, addr := range fnAddrs {
		nInsts := insts[addr]
		if len(currentChunk) > 0 && (currentInsts+nInsts > targetBudget || len(currentChunk) >= maxFuncsPerChunk) {
			chunks = append(chunks, currentChunk)
			currentChunk = nil
			currentInsts = 0
		}
		currentChunk = append(currentChunk, addr)
		currentInsts += nInsts
	}
	if len(currentChunk) > 0 {
		chunks = append(chunks, currentChunk)
	}
	if len(chunks) == 0 {
		chunks = append(chunks, []uint64{})
	}

	var generatedFiles []string
	if err := e.emitChunks(outDir, chunks, &generatedFiles); err != nil {
		return nil, err
	}

	dispatchPath := filepath.Join(outDir, "dispatch.c")
	if err := e.EmitDispatch(dispatchPath, len(chunks)); err != nil {
		return nil, fmt.Errorf("failed to emit dispatch.c: %w", err)
	}
	generatedFiles = append(generatedFiles, dispatchPath)

	return generatedFiles, nil
}

// isCallContinuation reports whether the lowering of a CALL ends where the callee returns to:
// with the bl itself or with the local label after a call to a block of the same function.
func isCallContinuation(lines []string) bool {
	for i := len(lines) - 1; i >= 0; i-- {
		t := strings.TrimSpace(lines[i])
		if t == "" || strings.HasPrefix(t, "//") {
			continue
		}
		return strings.HasPrefix(t, "bl ") || t == "1:"
	}
	return false
}

// guestTextSection holds all code that runs with the guest registers pinned in host registers:
// recompiled functions, shim stubs and the indirect-branch entry points of the trampoline. The
// runtime tells guest code from host code by this section (recomp_pc_in_guest_code).
const guestTextSection = ".section __TEXT,__recomp_guest,regular,pure_instructions"

// ShimStubViaGOT makes shim stubs load the shim address through the GOT (tier-1 fragments).
var ShimStubViaGOT bool

// writeShimStub emits the guest-callable symbol sym of a host shim for guest address addr:
// guest code calls it like any recompiled function, and it hands the shim and addr (stored
// to ctx->rip, so diagnostics name the shim a thread is in) to _recomp_call_shim.
func writeShimStub(w io.Writer, sym, shim string, addr uint64) {
	if l, ok := leanInfo(shim); ok && !ShimStubViaGOT { // fragments (GOT-bound) use the full bridge: the lean path needs PC-relative symbols
		writeLeanStub(w, sym, l, shim, addr)
		return
	}
	fmt.Fprintf(w, ".globl %s\n%s:\n", sym, sym)
	if lifter.GetCallModel() == lifter.CallFlat {
		// Call entry, then the jump entry (the bridge returns like a guest RET).
		for _, line := range lifter.FlatCallEntry() {
			fmt.Fprintln(w, line)
		}
		for _, line := range lifter.FlatJumpEntryLabel(sym) {
			fmt.Fprintln(w, line)
		}
	}
	writeFullBridgeCall(w, shim, addr)
}

func (e *AsmEmitter) emitChunkRegistration(path string, chunkIdx int, chunkAddrs []uint64) error {
	var buf bytes.Buffer
	w := bufio.NewWriter(&buf)

	fmt.Fprintf(w, "#include \"recomp_runtime.h\"\n\n")

	for _, addr := range chunkAddrs {
		fmt.Fprintf(w, "extern void fn_0x%x(GuestContext *ctx);\n", addr)
	}

	fmt.Fprintf(w, "\nvoid recomp_init_dispatch_chunk_%d(void) {\n", chunkIdx)
	for _, addr := range chunkAddrs {
		fmt.Fprintf(w, "    recomp_register_fn(0x%xULL, fn_0x%x);\n", addr, addr)
	}
	fmt.Fprintf(w, "}\n")

	if err := w.Flush(); err != nil {
		return err
	}
	return writeFileIfChanged(path, buf.Bytes())
}

// EmitTrampoline writes the ARM64 assembly bridge between the host C runtime and guest
// assembly (trampolineAsm).
func (e *AsmEmitter) EmitTrampoline(path string) error {
	return writeFileIfChanged(path, []byte(e.lifter.RewriteGuestMem(TrampolineSource(lifter.GetCallModel())+X87BridgeSource(lifter.GetCallModel()))))
}

// EmitDispatch ties all chunk registrations together.
func (e *AsmEmitter) EmitDispatch(path string, numChunks int) error {
	var buf bytes.Buffer
	w := bufio.NewWriter(&buf)

	fmt.Fprintf(w, "#include \"recomp_runtime.h\"\n\nvoid recomp_register_guest_modules(void);\n\n")

	for i := range numChunks {
		fmt.Fprintf(w, "void recomp_init_dispatch_chunk_%d(void);\n", i)
	}

	fmt.Fprintf(w, "\nvoid recomp_init_dispatch_table(void) {\n")
	for i := range numChunks {
		fmt.Fprintf(w, "    recomp_init_dispatch_chunk_%d();\n", i)
	}
	fmt.Fprintf(w, "    recomp_register_guest_modules();\n}\n")

	if err := w.Flush(); err != nil {
		return err
	}
	return writeFileIfChanged(path, buf.Bytes())
}

// EmitMainRunner emits the main.c entry point.
func (e *AsmEmitter) EmitMainRunner(path string) error {
	entryAddr, entryName, err := e.ResolveEntryAddress()
	if err != nil {
		return err
	}

	vfsInitArg := "NULL"
	if e.AppDir != "" {
		vfsInitArg = fmt.Sprintf("%q", e.AppDir)
	}

	memConfig := "// Guest memory model: the runtime default (base mode, console layout)."
	if e.Mem != nil {
		memConfig = e.Mem.CConfig()
	}

	content := fmt.Sprintf(`#include "recomp_runtime.h"
#include "ps4_vfs.h"
#include "ps4_sysmodule.h"

extern void recomp_init_dispatch_table(void);
extern void recomp_enter_guest(GuestContext *ctx, uint64_t target_fn);

%s

typedef struct {
    int argc;
    char **argv;
} GuestMainArgs;

// Guest code runs on a secondary host thread so the host main thread stays free
// for AppKit (window creation, event pumping, dialogs).
void nps5_guest_thread_qos(int is_main);
static void *guest_main(void *p) {
    GuestMainArgs *a = (GuestMainArgs *)p;
    nps5_guest_thread_qos(1);
    int argc = a->argc;
    char **argv = a->argv;
    printf("[ps4-recomp] Initializing runtime (ARM64 Assembly Backend)...\n");
    ps4_vfs_init(%s);
    recomp_init_dispatch_table();
    recomp_register_guest_modules();

    const char *prog_name = (argc > 0 && argv[0]) ? argv[0] : "/app0/eboot.bin";
    GuestContext *ctx = recomp_init_runtime_file("guest_image.bin", 0, prog_name);
    if (!ctx) {
        fprintf(stderr, "[ps4-recomp] Failed to allocate guest memory or load guest_image.bin\n");
        exit(1);
    }

    // Static TLS first: rtld allocates the initial TLS before any guest code runs, and
    // recomp_init_process_param runs the game's malloc replacement init.
%s

    recomp_init_process_param(ctx, 0x%xULL);

    recomp_modules_start_preloaded(ctx);

    recomp_setup_entry_args(ctx, argc, argv, 0x%xULL);

    printf("[ps4-recomp] Executing %%s (0x%%x) via ARM64 assembly...\n", %q, (unsigned int)0x%xULL);
    extern void fn_0x%x(void);
    recomp_enter_guest(ctx, (uint64_t)fn_0x%x);

    printf("[ps4-recomp] Execution complete.\n");
    recomp_free_runtime(ctx);
    exit(0);
    return NULL;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    static GuestMainArgs args;
    args.argc = argc;
    args.argv = argv;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024 * 1024);
    // Startup settings (language, PS4 Pro mode, upscaling, FPS HUD) are resolved before the
    // guest starts; the dialog needs the application directory for the language list.
    ps4_vfs_init(%s);
    ps4_host_settings_init();
    pthread_t guest_thread;
    if (pthread_create(&guest_thread, &attr, guest_main, &args) != 0) {
        fprintf(stderr, "[ps4-recomp] Failed to start guest thread\n");
        return 1;
    }
    pthread_attr_destroy(&attr);
    ps4_host_run_main_loop();
    return 0;
}
`, memConfig, vfsInitArg, tlsInitSource(e.elf.TLSModules), e.elf.ProcParamAddr, entryAddr, entryName, entryAddr, entryAddr, entryAddr, vfsInitArg)

	return writeFileIfChanged(path, []byte(content))
}

// tlsInitSource returns the main.c statements that hand the process's static TLS layout
// (elfloader.LayoutStaticTLS) to the runtime.
func tlsInitSource(mods []elfloader.TLSModule) string {
	if len(mods) == 0 {
		return "    recomp_init_tls_modules(ctx, NULL, 0);"
	}
	var b strings.Builder
	b.WriteString("    // Static TLS by module index: id, template vaddr, filesz, memsz, align, offset below FS.\n")
	b.WriteString("    static const RecompTlsModule tls_modules[] = {\n")
	for _, m := range mods {
		fmt.Fprintf(&b, "        {%d, 0x%xULL, 0x%xULL, 0x%xULL, 0x%xULL, 0x%xULL}, // %s\n",
			m.ID, m.Vaddr, m.Filesz, m.Memsz, m.Align, m.Offset, m.Name)
	}
	b.WriteString("    };\n")
	fmt.Fprintf(&b, "    recomp_init_tls_modules(ctx, tls_modules, %d);", len(mods))
	return b.String()
}

// ownSegments returns the segments of the main image itself. MergeImages appends the
// segments of merged companion modules (e.g. libSceFios2) to the main image, but the eboot's
// module info must list only its own mapped segments.
func ownSegments(elf *elfloader.LoadedELF) []*elfloader.Segment {
	end := elf.OrigMaxVAddr
	if end == 0 {
		end = elf.MaxVAddr
	}
	var segs []*elfloader.Segment
	for _, s := range elf.Segments {
		if s.Vaddr >= elf.OrigMinVAddr && s.Vaddr < end {
			segs = append(segs, s)
		}
	}
	return segs
}

// EmitGuestModules writes the runtime export tables, synthetic shims, and unwind registrations.
func (e *AsmEmitter) EmitGuestModules(path string) error {
	var buf bytes.Buffer
	w := bufio.NewWriter(&buf)

	fmt.Fprintf(w, "#include \"recomp_runtime.h\"\n#include \"ps4_sysmodule.h\"\n\n")

	// 1. Sort shimMap keys
	var sortedShimAddrs []uint64
	for addr := range e.shimMap {
		sortedShimAddrs = append(sortedShimAddrs, addr)
	}
	slices.Sort(sortedShimAddrs)

	shimAddr := make(map[string]uint64)
	for _, addr := range sortedShimAddrs {
		shim := e.shimMap[addr]
		for name, target := range CanonicalShims {
			if target == shim {
				if _, exists := shimAddr[name]; !exists {
					shimAddr[name] = addr
				}
			}
		}
	}

	nextSynth := uint64(0x1000)
	if e.elf != nil {
		nextSynth = (e.elf.MaxVAddr + 0xFFF) &^ 0xFFF
		if nextSynth < 0x1000 {
			nextSynth = 0x1000
		}
	}

	type synthEntry struct {
		addr uint64
		shim string
	}
	var synthList []synthEntry
	var synthRegs []string

	shimNames := make([]string, 0, len(CanonicalShims))
	for name := range CanonicalShims {
		shimNames = append(shimNames, name)
	}
	slices.Sort(shimNames)

	for _, name := range shimNames {
		if _, ok := shimAddr[name]; ok {
			continue
		}
		shimAddr[name] = nextSynth
		shimFn := CanonicalShims[name]
		synthList = append(synthList, synthEntry{addr: nextSynth, shim: shimFn})
		synthRegs = append(synthRegs, fmt.Sprintf("    extern void fn_0x%x(GuestContext *ctx);\n    recomp_register_fn(0x%xULL, fn_0x%x);\n", nextSynth, nextSynth, nextSynth))
		nextSynth += 16
	}

	// Forward declarations and weak fallback implementations for host shims
	uniqueShims := make(map[string]bool)
	uniqueShims[unresolvedStubShim] = true
	for _, target := range CanonicalShims {
		uniqueShims[target] = true
	}
	sortedShims := make([]string, 0, len(uniqueShims))
	for s := range uniqueShims {
		sortedShims = append(sortedShims, s)
	}
	slices.Sort(sortedShims)

	w.WriteString("// Forward declarations and weak fallback implementations for host shims\n")
	for _, s := range sortedShims {
		fmt.Fprintf(w, "void %s(GuestContext *ctx);\n", s)
	}
	w.WriteString("\n// Weak fallback implementations\n")
	for _, s := range sortedShims {
		if s == unresolvedStubShim {
			continue
		}
		fmt.Fprintf(w, "__attribute__((weak)) void %s(GuestContext *ctx) { recomp_unimplemented_shim(ctx, %q); }\n", s, s)
	}
	// A lean stub calls lean_<name>; where the runtime does not define it (a build without the module
	// that has the lean variant), the weak fallback runs the full shim on the stored context.
	for _, s := range sortedShims {
		if l, ok := leanInfo(s); ok {
			w.WriteString(leanWeakFallback(l.Lean, s))
		}
	}
	w.WriteString("\n")

	w.WriteString("static const RecompModuleExport recomp_host_shim_exports[] = {\n")
	for _, name := range shimNames {
		fmt.Fprintf(w, "    { %q, 0x%xULL },\n", name, shimAddr[name])
	}
	w.WriteString("    { NULL, 0 }\n};\n\n")

	for i, mod := range e.Modules {
		fmt.Fprintf(w, "static const RecompModuleExport recomp_module_exports_%d[] = {\n", i)
		for _, exp := range mod.Exports {
			fmt.Fprintf(w, "    { %q, 0x%xULL },\n", exp.Name, exp.Address)
		}
		w.WriteString("    { NULL, 0 }\n};\n\n")

		if len(mod.Init) > 0 {
			fmt.Fprintf(w, "static const uint64_t recomp_module_init_%d[] = {\n", i)
			for _, addr := range mod.Init {
				fmt.Fprintf(w, "    0x%xULL,\n", addr)
			}
			w.WriteString("};\n\n")
		}
	}

	w.WriteString("void recomp_register_guest_modules(void) {\n")
	unresolvedAddrs := make([]uint64, 0, len(e.unresolved))
	for addr := range e.unresolved {
		unresolvedAddrs = append(unresolvedAddrs, addr)
	}
	slices.Sort(unresolvedAddrs)
	for _, addr := range unresolvedAddrs {
		imp := e.unresolved[addr]
		fmt.Fprintf(w, "    recomp_register_unresolved(0x%xULL, %q, %q, %q);\n", addr, imp.nid, imp.name, imp.lib)
	}
	// Slots that are not discovered functions (GOT entries) still need a registered target;
	// the discovered ones (PLT stubs) get their stub from the code chunks.
	isFunc := make(map[uint64]bool)
	for _, a := range e.functionAddrs() {
		isFunc[a] = true
	}
	var unresolvedSynth []uint64
	for _, addr := range unresolvedAddrs {
		if isFunc[addr] {
			continue
		}
		unresolvedSynth = append(unresolvedSynth, addr)
		fmt.Fprintf(w, "    extern void fn_0x%x(GuestContext *ctx);\n    recomp_register_fn(0x%xULL, fn_0x%x);\n", addr, addr, addr)
	}
	// GOT-style imports (-fno-plt, `call [rip+GOT]`) bound to a host shim: the slot
	// holds its own address as the stub ID and is never a CFG target, so it
	// needs the same synthesized stub + registration as an unresolved slot.
	type gotShim struct {
		addr uint64
		shim string
	}
	var gotShims []gotShim
	if e.elf != nil {
		seenGOT := make(map[uint64]bool)
		for _, rel := range e.elf.Relocations {
			for _, addr := range []uint64{rel.PltAddr, rel.Offset} {
				shim, ok := e.shimMap[addr]
				if addr == 0 || !ok || shim == unresolvedStubShim || isFunc[addr] || seenGOT[addr] {
					continue
				}
				if _, un := e.unresolved[addr]; un {
					continue
				}
				seenGOT[addr] = true
				gotShims = append(gotShims, gotShim{addr, shim})
			}
		}
		slices.SortFunc(gotShims, func(a, b gotShim) int { return cmp.Compare(a.addr, b.addr) })
	}
	for _, g := range gotShims {
		fmt.Fprintf(w, "    extern void fn_0x%x(GuestContext *ctx);\n    recomp_register_fn(0x%xULL, fn_0x%x);\n", g.addr, g.addr, g.addr)
	}
	for _, line := range synthRegs {
		w.WriteString(line)
	}
	hostNames := []string{"libc.prx", "libSceLibcInternal.prx", "libkernel.prx", "libSceFios2.prx"}
	for _, name := range hostNames {
		fmt.Fprintf(w, "    recomp_module_register(%q, recomp_host_shim_exports);\n", name)
	}

	if e.elf != nil {
		mainName := e.elf.FileName
		if mainName == "" {
			mainName = "eboot.bin"
		}
		startAddr := e.elf.OrigMinVAddr
		endAddr := e.elf.OrigMaxVAddr
		if endAddr == 0 {
			endAddr = e.elf.MaxVAddr
		}
		mainSegs := e.registeredSegments(mainName, ownSegments(e.elf))
		numSegs := len(mainSegs)
		var segsInit strings.Builder
		for si := 0; si < numSegs; si++ {
			s := mainSegs[si]
			prot := 0
			if s.Flags&1 != 0 {
				prot |= 1
			}
			if s.Flags&2 != 0 {
				prot |= 2
			}
			if s.Flags&4 != 0 {
				prot |= 4
			}
			fmt.Fprintf(&segsInit, "            { .addr = 0x%xULL, .size = 0x%xULL, .prot = %d },\n", s.Vaddr, s.Memsz, prot)
		}
		fmt.Fprintf(w, "    static const RecompModuleUnwindInfo eboot_unwind = {\n"+
			"        .name = %q,\n"+
			"        .start_addr = 0x%xULL,\n"+
			"        .end_addr = 0x%xULL,\n"+
			"        .seg0_addr = 0x%xULL,\n"+
			"        .seg0_size = 0x%xULL,\n"+
			"        .eh_frame_hdr_addr = 0x%xULL,\n"+
			"        .eh_frame_hdr_size = 0x%xULL,\n"+
			"        .eh_frame_addr = 0x%xULL,\n"+
			"        .eh_frame_size = 0x%xULL,\n"+
			"        .num_segments = %d,\n"+
			"        .segments = {\n%s"+
			"        },\n"+
			"    };\n"+
			"    recomp_module_register_unwind_info(&eboot_unwind);\n",
			mainName, startAddr, endAddr, e.elf.Seg0Addr, e.elf.Seg0Size,
			e.elf.EHFrameHdrAddr, e.elf.EHFrameHdrSize, e.elf.EHFrameAddr, e.elf.EHFrameSize,
			numSegs, segsInit.String())
	}

	for i, mod := range e.Modules {
		names := uniqueModuleNames(mod.FileName, mod.Aliases)
		for _, name := range names {
			fmt.Fprintf(w, "    recomp_module_register(%q, recomp_module_exports_%d);\n", name, i)
			if len(mod.Init) > 0 {
				fmt.Fprintf(w, "    recomp_module_register_init(%q, recomp_module_init_%d, %d);\n", name, i, len(mod.Init))
			}
		}
		modSegs := e.registeredSegments(mod.FileName, mod.Segments)
		modNumSegs := len(modSegs)
		var modSegsInit strings.Builder
		for si := 0; si < modNumSegs; si++ {
			s := modSegs[si]
			prot := 0
			if s.Flags&1 != 0 {
				prot |= 1
			}
			if s.Flags&2 != 0 {
				prot |= 2
			}
			if s.Flags&4 != 0 {
				prot |= 4
			}
			fmt.Fprintf(&modSegsInit, "            { .addr = 0x%xULL, .size = 0x%xULL, .prot = %d },\n", s.Vaddr, s.Memsz, prot)
		}
		fmt.Fprintf(w, "    static const RecompModuleUnwindInfo mod_%d_unwind = {\n"+
			"        .name = %q,\n"+
			"        .start_addr = 0x%xULL,\n"+
			"        .end_addr = 0x%xULL,\n"+
			"        .seg0_addr = 0x%xULL,\n"+
			"        .seg0_size = 0x%xULL,\n"+
			"        .eh_frame_hdr_addr = 0x%xULL,\n"+
			"        .eh_frame_hdr_size = 0x%xULL,\n"+
			"        .eh_frame_addr = 0x%xULL,\n"+
			"        .eh_frame_size = 0x%xULL,\n"+
			"        .num_segments = %d,\n"+
			"        .segments = {\n%s"+
			"        },\n"+
			"    };\n"+
			"    recomp_module_register_unwind_info(&mod_%d_unwind);\n",
			i, mod.FileName, mod.StartAddr, mod.EndAddr, mod.Seg0Addr, mod.Seg0Size,
			mod.EHFrameHdrAddr, mod.EHFrameHdrSize, mod.EHFrameAddr, mod.EHFrameSize,
			modNumSegs, modSegsInit.String(), i)
	}

	w.WriteString("}\n")
	if err := w.Flush(); err != nil {
		return err
	}
	if err := writeFileIfChanged(path, buf.Bytes()); err != nil {
		return err
	}

	// 2. Write synth_shims.s
	synthPath := strings.TrimSuffix(path, ".c") + "_synth.s"
	var sBuf bytes.Buffer
	sw := bufio.NewWriter(&sBuf)
	sw.WriteString("// Synthetic shims ARM64 bridge\n" + guestTextSection + "\n.p2align 2\n\n")
	stub := func(addr uint64, shim string) {
		writeShimStub(sw, fmt.Sprintf("_fn_0x%x", addr), shim, addr)
		e.Facts.Funcs = append(e.Facts.Funcs, diag.AddrMapFunc{Guest: addr, Symbol: fmt.Sprintf("_fn_0x%x", addr), Chunk: filepath.Base(synthPath)})
	}
	for _, entry := range synthList {
		stub(entry.addr, entry.shim)
	}
	for _, addr := range unresolvedSynth {
		stub(addr, unresolvedStubShim)
	}
	for _, g := range gotShims {
		stub(g.addr, g.shim)
	}
	if err := sw.Flush(); err != nil {
		return err
	}
	return writeFileIfChanged(synthPath, sBuf.Bytes())
}

// leanWeakFallback is the weak C definition of a lean function for runtimes built without its module. The lean return carries
// rax only (the lean bridge reloads rax and r8-r15 from the saved registers), so a full shim run here must not change anything else
// the guest could observe: it runs on the saved context with the argument registers set, its own return-address pop is undone (the
// lean bridge returns itself), and a shim that changed rdx, a callee-saved register or xmm0 is refused loudly rather than silently
// losing the write.
func leanWeakFallback(lean, shim string) string {
	return fmt.Sprintf("__attribute__((weak)) uint64_t %s(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, GuestContext *ctx) {\n"+
		"    ctx->rdi = a0; ctx->rsi = a1; ctx->rdx = a2; ctx->rcx = a3; ctx->r8 = a4; ctx->r9 = a5;\n"+
		"    const uint64_t sp = ctx->rsp, rdx = ctx->rdx, rbx = ctx->rbx, rbp = ctx->rbp, r12 = ctx->r12, r13 = ctx->r13, r14 = ctx->r14, r15 = ctx->r15;\n"+
		"    const uint64_t x0 = ctx->xmm[0].u64[0], x1 = ctx->xmm[0].u64[1];\n"+
		"    %s(ctx);\n    ctx->rsp = sp;\n"+
		"    if (ctx->rdx != rdx || ctx->rbx != rbx || ctx->rbp != rbp || ctx->r12 != r12 || ctx->r13 != r13 || ctx->r14 != r14 || ctx->r15 != r15 ||\n"+
		"        ctx->xmm[0].u64[0] != x0 || ctx->xmm[0].u64[1] != x1) {\n"+
		"        recomp_unimplemented_shim(ctx, \"%s (lean fallback would lose a guest register write; rebuild with the lean module)\");\n    }\n"+
		"    return ctx->rax;\n}\n", lean, shim, lean)
}
