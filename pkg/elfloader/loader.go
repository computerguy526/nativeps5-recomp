package elfloader

import (
	"bytes"
	"debug/elf"
	"encoding/binary"
	"fmt"
	"io"
	"log"
	"os"
	"slices"
	"sort"
)

const (
	PT_SCE_DYNLIBDATA   elf.ProgType = 0x61000000
	PT_SCE_PROCPARAM    elf.ProgType = 0x61000001
	PT_SCE_MODULE_PARAM elf.ProgType = 0x61000002
	PT_SCE_RELRO        elf.ProgType = 0x61000010
	PT_GNU_RELRO        elf.ProgType = 0x6474e552

	R_X86_64_JUMP_SLOT = 7
	R_X86_64_RELATIVE  = 8
	R_X86_64_GLOB_DAT  = 6
	R_X86_64_64        = 1

	DT_SCE_NEEDED_MODULE int64 = 0x6100000f
	DT_SCE_EXPORT_LIB    int64 = 0x61000013
	DT_SCE_IMPORT_LIB    int64 = 0x61000015
	DT_SCE_JMPREL        int64 = 0x61000029
	DT_SCE_PLTRELSZ      int64 = 0x6100002d
	DT_SCE_RELA          int64 = 0x6100002f
	DT_SCE_RELASZ        int64 = 0x61000031
	DT_SCE_STRTAB        int64 = 0x61000035
	DT_SCE_STRSZ         int64 = 0x61000037
	DT_SCE_SYMTAB        int64 = 0x61000039
	DT_SCE_SYMENT        int64 = 0x6100003b
	DT_SCE_SYMTABSZ      int64 = 0x6100003f

	DT_INIT            = 12
	DT_FINI            = 13
	DT_INIT_ARRAY      = 25
	DT_INIT_ARRAYSZ    = 27
	DT_PREINIT_ARRAY   = 32
	DT_PREINIT_ARRAYSZ = 33
)

// AddrRange is a half-open [Start, End) guest virtual address interval.
type AddrRange struct {
	Start uint64
	End   uint64
}

// Segment represents a loaded memory segment.
type Segment struct {
	Vaddr  uint64
	Memsz  uint64
	Filesz uint64
	Flags  elf.ProgFlag
	Data   []byte
}

// Relocation represents a dynamic or static relocation record.
type Relocation struct {
	Offset  uint64
	Type    uint32
	SymIdx  uint32
	SymName string
	Addend  int64
	PltAddr uint64
}

// Symbol represents an exported or local symbol.
type Symbol struct {
	Name    string
	Address uint64
	Size    uint64
	Type    elf.SymType
	Section elf.SectionIndex
	// Local is set for STB_LOCAL symbols of the static symbol table. They name things for
	// diagnostics and seeding but never satisfy an import (ResolveModuleRelocations).
	Local bool
}

// LoadedELF holds the parsed ELF metadata and initial memory layout.
type LoadedELF struct {
	Path         string
	FileName     string
	EntryPoint   uint64
	Segments     []*Segment
	Sections     map[string]*elf.Section
	Symbols      []Symbol
	SymbolByAddr map[uint64]Symbol
	SymbolByName map[string]Symbol
	DynSymbols   []Symbol
	Relocations  []Relocation
	InitArray    []uint64
	// InitWalked lists the InitArray entries that the image's DT_INIT function calls itself (a
	// Sony PRX's module_start loops over its preinit/init array): the runtime must not call them
	// a second time. They stay in InitArray as function seeds.
	InitWalked  []uint64
	DynamicTags map[int64]uint64
	NeededLibs  []string
	// ImportLibs maps the 16-bit SCE library ID (DT_SCE_IMPORT_LIB LID)
	// onto the plaintext library name in the SCE string table.
	ImportLibs map[uint16]string
	// LibIDSecond is set for PS5-style dynamic tables, where the library index of a dynsym
	// name "NID#a#b" is the second id (b); on PS4 it is the first (a).
	LibIDSecond bool
	// ExcludedImports maps the NID of every symbol exported by a module left out of the
	// translation (-exclude-module) to that module's file name. Imports of these NIDs are never
	// satisfied by a shim or an unrelated export: they bind to the "excluded-module" stub.
	ExcludedImports map[string]string
	ExecRanges      []AddrRange
	// ROAddrRanges: guest address ranges the loader never writes after it has finished and that
	// the guest never writes: PT_GNU_RELRO / PT_SCE_RELRO (relocated once, then read-only) and
	// the non-executable PT_LOADs without PF_W. The lifter (-ro-loads) emits constant-address
	// loads from them without acquire ordering (docs/m3/lift2.md). Sorted and merged.
	ROAddrRanges []AddrRange
	DynlibData   []byte

	MinVAddr uint64
	MaxVAddr uint64
	// Bias is the module's displacement from its linked addresses (sum of ApplyBias deltas):
	// guest address = linked virtual address + Bias.
	Bias uint64
	// OrigMinVAddr and OrigMaxVAddr track the unmerged extent of this specific ELF image.
	OrigMinVAddr uint64
	OrigMaxVAddr uint64

	// ProcParamAddr and ProcParamSize store the guest virtual address and extent of PT_SCE_PROCPARAM.
	ProcParamAddr uint64
	ProcParamSize uint64
	// CanaryAddr tracks the guest virtual address allocated for the stack canary value.
	CanaryAddr uint64

	// TLSVaddr, TLSFilesz, TLSMemsz, and TLSAlign store this image's own PT_TLS template.
	TLSVaddr  uint64
	TLSFilesz uint64
	TLSMemsz  uint64
	TLSAlign  uint64
	// TLSModules is the process's static TLS: the executable's and every linked module's
	// PT_TLS, by TLS module index (AddTLSModule, LayoutStaticTLS).
	TLSModules []TLSModule
	// tlsTPOFF holds the TPOFF64 slots AddTLSModule queued for ApplyTLSOffsets.
	tlsTPOFF []pendingTPOFF

	// MemoryImage is the sparse guest memory image (relative relocs applied), addressed by final
	// guest virtual address. One region per module; ApplyBias moves regions without copying.
	MemoryImage *Image
	// local is this module's dense load buffer indexed by *linked* virtual address. It is what
	// LoadELFBytes and the .eh_frame parser work on before the module is placed; its bytes are
	// shared with the module's MemoryImage region. After ApplyBias its indices are no longer
	// guest addresses, so nothing outside load-time parsing may use it.
	local []byte

	// FuncBounds are half-open function extents from .eh_frame, sorted by Start.
	FuncBounds []AddrRange
	// LSDABounds are half-open function extents from .eh_frame that contain an LSDA
	// (Language Specific Data Area) with landing pads for exception handling.
	LSDABounds []AddrRange
	// UnwindRanges are executable ranges covered by unwind information.
	UnwindRanges []AddrRange
	// DataRanges are half-open non-code intervals inside the image (.eh_frame
	// and .eh_frame_hdr), sorted and non-overlapping. PS4 RX segments often
	// contain both.
	DataRanges []AddrRange

	// EHFrameHdrAddr and EHFrameHdrSize store the guest virtual address and extent of .eh_frame_hdr.
	EHFrameHdrAddr uint64
	EHFrameHdrSize uint64
	// EHFrameAddr and EHFrameSize store the guest virtual address and extent of .eh_frame.
	EHFrameAddr uint64
	EHFrameSize uint64
	// Seg0Addr and Seg0Size store the start virtual address and extent of segment 0 (the code segment).
	Seg0Addr uint64
	Seg0Size uint64
}

// FunctionHasLSDA reports whether any function extent in [fnStart, fnEnd) has an LSDA
// (exception landing pads for C++ catch/cleanup).
func (l *LoadedELF) FunctionHasLSDA(fnStart, fnEnd uint64) bool {
	if l == nil || len(l.LSDABounds) == 0 {
		return false
	}
	low, high := 0, len(l.LSDABounds)
	for low < high {
		mid := int(uint(low+high) >> 1)
		if l.LSDABounds[mid].End <= fnStart {
			low = mid + 1
		} else {
			high = mid
		}
	}
	return low < len(l.LSDABounds) && l.LSDABounds[low].Start < fnEnd
}

// LoadELF reads and parses a 64-bit ELF binary. PS4 SELF/FSELF containers are unwrapped first.
func LoadELF(path string) (*LoadedELF, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("failed to read '%s': %w", path, err)
	}
	if IsSELF(data) {
		data, err = ExtractELFFromSELF(data)
		if err != nil {
			return nil, fmt.Errorf("failed to extract ELF from SELF '%s': %w", path, err)
		}
	}
	loaded, err := LoadELFBytes(data)
	if err != nil {
		return nil, err
	}
	loaded.Path = path
	loaded.FileName = basename(path)
	return loaded, nil
}

// LoadELFBytes parses a 64-bit x86-64 ELF image from memory.
func LoadELFBytes(data []byte) (*LoadedELF, error) {
	file, err := elf.NewFile(sectionlessReader(data))
	if err != nil {
		return nil, fmt.Errorf("failed to parse elf: %w", err)
	}
	defer func() { _ = file.Close() }()

	if file.Class != elf.ELFCLASS64 || file.Machine != elf.EM_X86_64 {
		return nil, fmt.Errorf("unsupported ELF: must be 64-bit x86-64")
	}

	loaded := &LoadedELF{
		EntryPoint:   file.Entry,
		Sections:     make(map[string]*elf.Section),
		SymbolByAddr: make(map[uint64]Symbol),
		SymbolByName: make(map[string]Symbol),
		MinVAddr:     ^uint64(0),
		MaxVAddr:     0,
	}

	type pendingSeg struct {
		prog *elf.Prog
		off  uint64
		fsz  uint64
	}
	var pending []pendingSeg

	for _, prog := range file.Progs {
		if prog.Type == PT_SCE_DYNLIBDATA && prog.Filesz > 0 && prog.Off+prog.Filesz <= uint64(len(data)) {
			loaded.DynlibData = data[prog.Off : prog.Off+prog.Filesz]
		}
		if prog.Type == PT_SCE_PROCPARAM {
			loaded.ProcParamAddr = prog.Vaddr
			loaded.ProcParamSize = prog.Memsz
		}
		if prog.Type == elf.PT_TLS {
			loaded.TLSVaddr = prog.Vaddr
			loaded.TLSFilesz = prog.Filesz
			loaded.TLSMemsz = prog.Memsz
			loaded.TLSAlign = prog.Align
		}
		if prog.Type == PT_GNU_RELRO || prog.Type == PT_SCE_RELRO {
			// ld.so protects whole pages: the range ends at the last page boundary at or below the end.
			if end := (prog.Vaddr + prog.Memsz) &^ (relroPage - 1); end > prog.Vaddr {
				loaded.ROAddrRanges = append(loaded.ROAddrRanges, AddrRange{Start: prog.Vaddr, End: end})
			}
		}
		if prog.Type == elf.PT_LOAD && prog.Flags&(elf.PF_R|elf.PF_W|elf.PF_X) == elf.PF_R && prog.Memsz > 0 {
			loaded.ROAddrRanges = append(loaded.ROAddrRanges, AddrRange{Start: prog.Vaddr, End: prog.Vaddr + prog.Memsz})
		}
		if !isMappedProg(prog.Type) {
			continue
		}
		if prog.Filesz > prog.Memsz { // also catches memsz == 0 with file data: not silently ignored
			return nil, fmt.Errorf("segment at 0x%x: filesz 0x%x exceeds memsz 0x%x", prog.Vaddr, prog.Filesz, prog.Memsz)
		}
		if prog.Memsz == 0 {
			continue
		}
		if prog.Vaddr+prog.Memsz < prog.Vaddr || prog.Vaddr+prog.Memsz > maxImageSize {
			return nil, fmt.Errorf("segment vaddr 0x%x memsz 0x%x exceeds the %d GiB image limit", prog.Vaddr, prog.Memsz, maxImageSize>>30)
		}
		if prog.Filesz > prog.Memsz {
			return nil, fmt.Errorf("segment at 0x%x: filesz 0x%x exceeds memsz 0x%x", prog.Vaddr, prog.Filesz, prog.Memsz)
		}
		if prog.Filesz > 0 && (prog.Off > uint64(len(data)) || prog.Filesz > uint64(len(data))-prog.Off) {
			return nil, fmt.Errorf("segment at 0x%x: file range [0x%x,+0x%x) exceeds file size 0x%x", prog.Vaddr, prog.Off, prog.Filesz, len(data))
		}
		fsz := prog.Filesz
		pending = append(pending, pendingSeg{prog: prog, off: prog.Off, fsz: fsz})
		if prog.Vaddr < loaded.MinVAddr {
			loaded.MinVAddr = prog.Vaddr
		}
		end := prog.Vaddr + prog.Memsz
		if end > loaded.MaxVAddr {
			loaded.MaxVAddr = end
		}
		if prog.Flags&elf.PF_X != 0 {
			loaded.ExecRanges = append(loaded.ExecRanges, AddrRange{Start: prog.Vaddr, End: end})
		}
	}

	if len(pending) == 0 {
		return nil, fmt.Errorf("no PT_LOAD segments found in ELF")
	}
	// PT_LOAD segments must not overlap: the later one would silently overwrite the earlier.
	{
		loads := make([]*elf.Prog, 0, len(pending))
		for _, p := range pending {
			if p.prog.Type == elf.PT_LOAD {
				loads = append(loads, p.prog)
			}
		}
		sort.Slice(loads, func(i, j int) bool { return loads[i].Vaddr < loads[j].Vaddr })
		for i := 1; i < len(loads); i++ {
			if prev := loads[i-1]; loads[i].Vaddr < prev.Vaddr+prev.Memsz {
				return nil, fmt.Errorf("PT_LOAD segments overlap: [0x%x,+0x%x) and [0x%x,+0x%x)", prev.Vaddr, prev.Memsz, loads[i].Vaddr, loads[i].Memsz)
			}
		}
	}

	pageSize := uint64(4096)
	totalMemSize := (loaded.MaxVAddr + pageSize - 1) &^ (pageSize - 1)
	loaded.local = make([]byte, totalMemSize)

	for _, p := range pending {
		if p.fsz > 0 {
			end := p.prog.Vaddr + p.fsz
			if end > uint64(len(loaded.local)) {
				return nil, fmt.Errorf("segment at 0x%x exceeds memory image", p.prog.Vaddr)
			}
			copy(loaded.local[p.prog.Vaddr:end], data[p.off:p.off+p.fsz])
		}
		seg := &Segment{
			Vaddr:  p.prog.Vaddr,
			Memsz:  p.prog.Memsz,
			Filesz: p.fsz,
			Flags:  p.prog.Flags,
		}
		memEnd := p.prog.Vaddr + p.prog.Memsz
		if memEnd > uint64(len(loaded.local)) {
			memEnd = uint64(len(loaded.local))
		}
		seg.Data = loaded.local[p.prog.Vaddr:memEnd]
		loaded.Segments = append(loaded.Segments, seg)
	}

	if len(loaded.Segments) > 0 {
		loaded.Seg0Addr = loaded.Segments[0].Vaddr
		loaded.Seg0Size = loaded.Segments[0].Memsz
	}

	for _, sec := range file.Sections {
		loaded.Sections[sec.Name] = sec
	}

	if syms, err := file.Symbols(); err == nil {
		for _, s := range syms {
			addSymbol(loaded, Symbol{
				Name:    s.Name,
				Address: s.Value,
				Size:    s.Size,
				Type:    elf.ST_TYPE(s.Info),
				Section: s.Section,
				Local:   elf.ST_BIND(s.Info) == elf.STB_LOCAL,
			})
		}
	}

	if dynSyms, err := file.DynamicSymbols(); err == nil {
		loaded.DynSymbols = append(loaded.DynSymbols, Symbol{})
		for _, s := range dynSyms {
			sym := Symbol{
				Name:    s.Name,
				Address: s.Value,
				Size:    s.Size,
				Type:    elf.ST_TYPE(s.Info),
				Section: s.Section,
			}
			loaded.DynSymbols = append(loaded.DynSymbols, sym)
			if sym.Address != 0 {
				addSymbol(loaded, sym)
			}
		}
	}

	if err := parseSCEDynamic(loaded, data, file); err != nil {
		return nil, fmt.Errorf("dynamic section: %w", err)
	}
	if len(loaded.NeededLibs) == 0 {
		if needed, err := file.DynString(elf.DT_NEEDED); err == nil {
			loaded.NeededLibs = needed
		}
	}
	if err := parseRelocations(loaded, file); err != nil {
		return nil, err
	}
	parseInitArray(loaded)

	if len(loaded.ExecRanges) == 0 {
		for _, seg := range loaded.Segments {
			if seg.Flags&elf.PF_X != 0 {
				loaded.ExecRanges = append(loaded.ExecRanges, AddrRange{Start: seg.Vaddr, End: seg.Vaddr + seg.Memsz})
			}
		}
	}
	loaded.ROAddrRanges = mergeRanges(loaded.ROAddrRanges)
	loaded.OrigMinVAddr = loaded.MinVAddr
	loaded.OrigMaxVAddr = loaded.MaxVAddr
	attachUnwind(loaded, file)
	loaded.publishImage()
	return loaded, nil
}

// relroPage is the granularity ld.so protects PT_GNU_RELRO with (the PS5 page size, the larger of the two).
const relroPage = 0x4000

func isMappedProg(t elf.ProgType) bool {
	return t == elf.PT_LOAD || t == PT_SCE_RELRO
}

func addSymbol(loaded *LoadedELF, sym Symbol) {
	loaded.Symbols = append(loaded.Symbols, sym)
	if sym.Name != "" {
		if existing, ok := loaded.SymbolByName[sym.Name]; !ok || existing.Address == 0 {
			loaded.SymbolByName[sym.Name] = sym
		}
	}
	if sym.Address != 0 {
		if existing, ok := loaded.SymbolByAddr[sym.Address]; !ok || existing.Name == "" {
			loaded.SymbolByAddr[sym.Address] = sym
		}
	}
}

func parseRelocations(loaded *LoadedELF, file *elf.File) error {
	relSecNames := []string{".rela.dyn", ".rela.plt"}
	for _, name := range relSecNames {
		sec, ok := loaded.Sections[name]
		if !ok || sec.Size == 0 {
			continue
		}
		relData, err := sec.Data()
		if err != nil {
			continue
		}
		if err := applyRelaTable(loaded, relData, name == ".rela.plt"); err != nil {
			return fmt.Errorf("%s: %w", name, err)
		}
	}

	resolvePltAddresses(loaded)
	return nil
}

func resolvePltAddresses(loaded *LoadedELF) {
	var pltBase uint64
	if pltSec, ok := loaded.Sections[".plt"]; ok {
		pltBase = pltSec.Addr
	} else {
		// Section headers stripped (standard for PS4 retail ELFs).
		// Find the PLT base by scanning executable segments for the trampoline of the first R_X86_64_JUMP_SLOT.
		var firstJumpSlotGot uint64
		for _, rel := range loaded.Relocations {
			if rel.Type == R_X86_64_JUMP_SLOT {
				firstJumpSlotGot = rel.Offset
				break
			}
		}
		if firstJumpSlotGot != 0 {
			for _, seg := range loaded.Segments {
				if seg.Flags&elf.PF_X == 0 {
					continue
				}
				data := seg.Data
				for i := 0; i+6 <= len(data); i++ {
					// x86_64 PLT stub: jmpq *(%rip + disp32) -> ff 25 [disp32]
					if data[i] == 0xff && data[i+1] == 0x25 {
						disp := int32(binary.LittleEndian.Uint32(data[i+2 : i+6]))
						pc := seg.Vaddr + uint64(i)
						targetGot := uint64(int64(pc) + 6 + int64(disp))
						if targetGot == firstJumpSlotGot {
							if pc >= 16 {
								pltBase = pc - 16
							}
							break
						}
					}
				}
				if pltBase != 0 {
					break
				}
			}
		}
	}

	if pltBase != 0 {
		entryIdx := 0
		for i := range loaded.Relocations {
			rel := &loaded.Relocations[i]
			if rel.Type == R_X86_64_JUMP_SLOT && rel.PltAddr == 0 {
				rel.PltAddr = pltBase + 16*uint64(entryIdx+1)
				entryIdx++
				if rel.Offset+8 <= uint64(len(loaded.local)) {
					binary.LittleEndian.PutUint64(loaded.local[rel.Offset:rel.Offset+8], rel.PltAddr)
				}
			}
		}
	}
}

// relocSlotInImage reports whether the 8-byte word at offset lies wholly inside one loaded segment
// (written without any addition that could wrap).
func relocSlotInImage(loaded *LoadedELF, offset uint64) bool {
	for _, seg := range loaded.Segments {
		if offset >= seg.Vaddr && seg.Memsz >= 8 && offset-seg.Vaddr <= seg.Memsz-8 {
			return true
		}
	}
	return false
}

func applyRelaTable(loaded *LoadedELF, relData []byte, isPlt bool) error {
	const entrySize = 24
	if len(relData)/entrySize > maxRelocEntries {
		return fmt.Errorf("relocation table too large (%d bytes)", len(relData))
	}
	for i := 0; i+entrySize <= len(relData); i += entrySize {
		offset := binary.LittleEndian.Uint64(relData[i : i+8])
		info := binary.LittleEndian.Uint64(relData[i+8 : i+16])
		addend := int64(binary.LittleEndian.Uint64(relData[i+16 : i+24]))
		relType := uint32(info & 0xffffffff)
		symIdx := uint32(info >> 32)

		if !supportedRelocType(relType) {
			return &UnsupportedRelocError{Type: relType, Offset: offset}
		}
		if !relocSlotInImage(loaded, offset) {
			return fmt.Errorf("relocation at 0x%x (type %d) is outside every loaded segment", offset, relType)
		}
		if symIdx != 0 && int(symIdx) >= len(loaded.DynSymbols) && len(loaded.DynSymbols) > 0 {
			return fmt.Errorf("relocation at 0x%x references symbol index %d beyond symbol table (%d entries)", offset, symIdx, len(loaded.DynSymbols))
		}

		var symName string
		if int(symIdx) < len(loaded.DynSymbols) {
			symName = loaded.DynSymbols[symIdx].Name
		}

		rel := Relocation{
			Offset:  offset,
			Type:    relType,
			SymIdx:  symIdx,
			SymName: symName,
			Addend:  addend,
		}

		if relType == R_X86_64_JUMP_SLOT && isPlt {
			if pltSec, ok := loaded.Sections[".plt"]; ok {
				entryIdx := i / entrySize
				rel.PltAddr = pltSec.Addr + 16*uint64(entryIdx+1)
				if offset+8 <= uint64(len(loaded.local)) {
					binary.LittleEndian.PutUint64(loaded.local[offset:offset+8], rel.PltAddr)
				}
			}
		}

		if relType == R_X86_64_RELATIVE {
			if offset+8 <= uint64(len(loaded.local)) {
				binary.LittleEndian.PutUint64(loaded.local[offset:offset+8], uint64(addend))
			}
		}

		canonSym, _ := ResolveNID(symName)
		if (relType == R_X86_64_GLOB_DAT || relType == R_X86_64_64) && (symName == "__stack_chk_guard" || canonSym == "__stack_chk_guard") {
			applyStackCanary(loaded, offset)
		}

		if relType == R_X86_64_64 && int(symIdx) < len(loaded.DynSymbols) {
			sym := loaded.DynSymbols[symIdx]
			if sym.Address != 0 && offset+8 <= uint64(len(loaded.local)) {
				binary.LittleEndian.PutUint64(loaded.local[offset:offset+8], sym.Address+uint64(addend))
			}
		}

		loaded.Relocations = append(loaded.Relocations, rel)
	}
	return nil
}

func applyStackCanary(loaded *LoadedELF, gotOffset uint64) {
	guardAddr := loaded.CanaryAddr
	if guardAddr == 0 {
		guardAddr = (loaded.MaxVAddr + 4095) &^ 4095
		if uint64(len(loaded.local)) < guardAddr+4096 {
			newImg := make([]byte, guardAddr+4096)
			copy(newImg, loaded.local)
			loaded.local = newImg
			loaded.MaxVAddr = guardAddr + 4096
			for _, seg := range loaded.Segments {
				end := seg.Vaddr + seg.Memsz
				if end > uint64(len(loaded.local)) {
					end = uint64(len(loaded.local))
				}
				if seg.Vaddr < end {
					seg.Data = loaded.local[seg.Vaddr:end]
				}
			}
		}
		const canaryValue = uint64(0x595e9fbd94fda766)
		binary.LittleEndian.PutUint64(loaded.local[guardAddr:guardAddr+8], canaryValue)
		loaded.CanaryAddr = guardAddr
	}
	if gotOffset+8 <= uint64(len(loaded.local)) {
		binary.LittleEndian.PutUint64(loaded.local[gotOffset:gotOffset+8], guardAddr)
	}
}

func parseInitArray(loaded *LoadedELF) {
	// 1. Check .init_array section from ELF section headers
	if initSec, ok := loaded.Sections[".init_array"]; ok && initSec.Size > 0 {
		for i := uint64(0); i+8 <= initSec.Size; i += 8 {
			addr := initSec.Addr + i
			if addr+8 <= uint64(len(loaded.local)) {
				fn := binary.LittleEndian.Uint64(loaded.local[addr : addr+8])
				if fn != 0 {
					loaded.InitArray = append(loaded.InitArray, fn)
				}
			}
		}
	}

	// 2. Check .ctors section from ELF section headers
	if ctorsSec, ok := loaded.Sections[".ctors"]; ok && ctorsSec.Size > 0 {
		for i := ctorsSec.Size; i >= 8; i -= 8 {
			addr := ctorsSec.Addr + i - 8
			if addr+8 <= uint64(len(loaded.local)) {
				fn := binary.LittleEndian.Uint64(loaded.local[addr : addr+8])
				if fn != 0 && fn != ^uint64(0) && loaded.InExecutable(fn) {
					loaded.InitArray = append(loaded.InitArray, fn)
				}
			}
		}
	}

	// 3. Check DT_PREINIT_ARRAY and DT_INIT_ARRAY from dynamic tags (used when ELF section headers are stripped, e.g. PS4 PRX/SELF)
	if loaded.DynamicTags != nil {
		if preinitAddr, ok := loaded.DynamicTags[DT_PREINIT_ARRAY]; ok && preinitAddr != 0 {
			sz := loaded.DynamicTags[DT_PREINIT_ARRAYSZ]
			for i := uint64(0); i+8 <= sz; i += 8 {
				addr := preinitAddr + i
				if addr+8 <= uint64(len(loaded.local)) {
					fn := binary.LittleEndian.Uint64(loaded.local[addr : addr+8])
					if fn != 0 {
						loaded.InitArray = append(loaded.InitArray, fn)
					}
				}
			}
		}

		if initArrayAddr, ok := loaded.DynamicTags[DT_INIT_ARRAY]; ok && initArrayAddr != 0 {
			sz := loaded.DynamicTags[DT_INIT_ARRAYSZ]
			for i := uint64(0); i+8 <= sz; i += 8 {
				addr := initArrayAddr + i
				if addr+8 <= uint64(len(loaded.local)) {
					fn := binary.LittleEndian.Uint64(loaded.local[addr : addr+8])
					if fn != 0 {
						loaded.InitArray = append(loaded.InitArray, fn)
					}
				}
			}
			// In stripped ELFs (e.g. PS4 SELF / Orbis PRX), legacy .ctors precedes DT_INIT_ARRAY
			// and is terminated by (uint64)-1 at __CTOR_END__, preceded by function pointers
			// back to __CTOR_LIST__ = (uint64)-1.
			if initArrayAddr >= 16 {
				var ctorEnd uint64
				for off := uint64(8); off <= 64; off += 8 {
					if initArrayAddr < off {
						break
					}
					pos := initArrayAddr - off
					if pos+8 <= uint64(len(loaded.local)) {
						val := binary.LittleEndian.Uint64(loaded.local[pos : pos+8])
						if val == ^uint64(0) {
							ctorEnd = pos
							break
						}
					}
				}
				if ctorEnd >= 8 {
					for pos := ctorEnd - 8; pos >= 8; pos -= 8 {
						if pos+8 > uint64(len(loaded.local)) {
							break
						}
						fn := binary.LittleEndian.Uint64(loaded.local[pos : pos+8])
						if fn == ^uint64(0) {
							break
						}
						if fn != 0 && loaded.InExecutable(fn) {
							loaded.InitArray = append(loaded.InitArray, fn)
						}
					}
				}
			}
		}

		// 4. Scan DT_INIT function for references to legacy .ctors table (standard in PS4 Clang CRT).
		// Not when the dynamic table already names the arrays: a PS5 libc's DT_INIT loops over
		// the preinit array, and the word after the array's end looks like one more constructor.
		hasArrays := loaded.DynamicTags[DT_PREINIT_ARRAY] != 0 || loaded.DynamicTags[DT_INIT_ARRAY] != 0
		if initAddr, ok := loaded.DynamicTags[DT_INIT]; ok && !hasArrays && initAddr != 0 && initAddr < uint64(len(loaded.local)) {
			end := initAddr + 64
			if end > uint64(len(loaded.local)) {
				end = uint64(len(loaded.local))
			}
			code := loaded.local[initAddr:end]
			for i := 0; i+7 <= len(code); i++ {
				// LEA r64, [rip + disp32] (REX.W 0x48..0x4f, 0x8d, ModRM with mod=00, rm=101)
				if (code[i] >= 0x48 && code[i] <= 0x4f) && code[i+1] == 0x8d && (code[i+2]&0xc7) == 0x05 {
					disp := int32(binary.LittleEndian.Uint32(code[i+3 : i+7]))
					pc := initAddr + uint64(i)
					target := uint64(int64(pc+7) + int64(disp))
					if target+8 <= uint64(len(loaded.local)) {
						var ctors []uint64
						for pos := target; pos >= 8; pos -= 8 {
							if pos+8 > uint64(len(loaded.local)) {
								break
							}
							val := binary.LittleEndian.Uint64(loaded.local[pos : pos+8])
							if val == ^uint64(0) {
								ctors = append(ctors, val)
								break
							}
							if val != 0 && loaded.InExecutable(val) {
								ctors = append(ctors, val)
							} else if val == 0 {
								continue
							} else {
								ctors = nil
								break
							}
						}
						if len(ctors) > 1 && ctors[len(ctors)-1] == ^uint64(0) {
							// Found valid backwards ctors list terminated by __CTOR_LIST__ sentinel
							for _, fn := range ctors[:len(ctors)-1] {
								loaded.InitArray = append(loaded.InitArray, fn)
							}
						}
					}
				}
			}
		}
	}

	markInitWalked(loaded)

	// Deduplicate entries while preserving execution order
	if len(loaded.InitArray) > 1 {
		seen := make(map[uint64]struct{}, len(loaded.InitArray))
		var uniqueInits []uint64
		for _, fn := range loaded.InitArray {
			if _, ok := seen[fn]; !ok {
				seen[fn] = struct{}{}
				uniqueInits = append(uniqueInits, fn)
			}
		}
		loaded.InitArray = uniqueInits
	}
}

// markInitWalked fills InitWalked: when the DT_INIT function of a section-less image (PRX/SELF)
// takes the address of the DT_PREINIT_ARRAY / DT_INIT_ARRAY start (LEA r64,[rip+disp] in its
// first bytes), it is the Sony CRT module_start that calls the array's entries itself. Calling
// them again from the module's init list runs every initialiser twice (ASTRO BOT libc.prx: the
// heap set-up ran twice and allocated its 1944 MiB of direct memory twice).
func markInitWalked(loaded *LoadedELF) {
	if loaded.DynamicTags == nil || len(loaded.Sections) != 0 {
		return
	}
	initAddr := loaded.DynamicTags[DT_INIT]
	if initAddr == 0 || initAddr >= uint64(len(loaded.local)) {
		return
	}
	end := min(initAddr+64, uint64(len(loaded.local)))
	code := loaded.local[initAddr:end]
	for _, arr := range [...][2]int64{{DT_PREINIT_ARRAY, DT_PREINIT_ARRAYSZ}, {DT_INIT_ARRAY, DT_INIT_ARRAYSZ}} {
		start, sz := loaded.DynamicTags[arr[0]], loaded.DynamicTags[arr[1]]
		if start == 0 || sz == 0 {
			continue
		}
		walked := false
		for i := 0; i+7 <= len(code); i++ {
			if code[i] >= 0x48 && code[i] <= 0x4f && code[i+1] == 0x8d && code[i+2]&0xc7 == 0x05 {
				disp := int32(binary.LittleEndian.Uint32(code[i+3 : i+7]))
				if uint64(int64(initAddr+uint64(i)+7)+int64(disp)) == start {
					walked = true
					break
				}
			}
		}
		if !walked {
			continue
		}
		for i := uint64(0); i+8 <= sz && start+i+8 <= uint64(len(loaded.local)); i += 8 {
			if fn := binary.LittleEndian.Uint64(loaded.local[start+i : start+i+8]); fn != 0 && fn != initAddr {
				loaded.InitWalked = append(loaded.InitWalked, fn)
			}
		}
	}
}

// InitRunList returns the init functions the runtime calls for this image: InitArray without
// the entries its DT_INIT calls itself.
func (l *LoadedELF) InitRunList() []uint64 {
	out := make([]uint64, 0, len(l.InitArray))
	for _, fn := range l.InitArray {
		if !slices.Contains(l.InitWalked, fn) {
			out = append(out, fn)
		}
	}
	return out
}

// Standard and PS5-era dynamic tags.
const (
	dtNeeded   int64 = 1
	dtPLTRELSZ int64 = 2
	dtStrtab   int64 = 5
	dtSymtab   int64 = 6
	dtRela     int64 = 7
	dtRelaSz   int64 = 8
	dtStrSz    int64 = 10
	dtSymEnt   int64 = 11
	dtJmpRel   int64 = 23

	// PS5 shifts the SCE library tags by +0x30 relative to PS4.
	DT_SCE_EXPORT_LIB_PS5 int64 = 0x61000043
	DT_SCE_IMPORT_LIB_PS5 int64 = 0x61000045

	maxRelocEntries = 1 << 24
	maxImageSize    = uint64(4) << 30
)

// UnsupportedRelocError reports a relocation type the loader cannot honour.
type UnsupportedRelocError struct {
	Type   uint32
	Offset uint64
}

func (e *UnsupportedRelocError) Error() string {
	return fmt.Sprintf("unsupported relocation type %d at offset 0x%x", e.Type, e.Offset)
}

// supportedRelocType lists the x86-64 relocation types the loader and the
// emitter understand. Everything else is rejected rather than silently dropped.
func supportedRelocType(t uint32) bool {
	switch t {
	case 0, // R_X86_64_NONE
		R_X86_64_64, 2, // PC32
		R_X86_64_GLOB_DAT, R_X86_64_JUMP_SLOT, R_X86_64_RELATIVE,
		R_X86_64_DTPMOD64, R_X86_64_DTPOFF64, R_X86_64_TPOFF64:
		return true
	}
	return false
}

// vaSlice returns img[va:va+size] when it lies fully inside img.
func vaSlice(img []byte, va, size uint64) ([]byte, bool) {
	if va > uint64(len(img)) || size > uint64(len(img))-va {
		return nil, false
	}
	return img[va : va+size], true
}

func parseSCEDynamic(loaded *LoadedELF, data []byte, file *elf.File) error {
	var dynOff, dynSz uint64
	for _, prog := range file.Progs {
		if prog.Type == elf.PT_DYNAMIC {
			dynOff = prog.Off
			dynSz = prog.Filesz
			break
		}
	}
	if dynSz < 16 {
		return nil
	}
	if dynOff > uint64(len(data)) || dynSz > uint64(len(data))-dynOff {
		return fmt.Errorf("PT_DYNAMIC range [0x%x,+0x%x) exceeds file size 0x%x", dynOff, dynSz, len(data))
	}
	dyn := data[dynOff : dynOff+dynSz]

	tags := make(map[int64]uint64, 32)
	for i := 0; i+16 <= len(dyn); i += 16 {
		tag := int64(binary.LittleEndian.Uint64(dyn[i : i+8]))
		val := binary.LittleEndian.Uint64(dyn[i+8 : i+16])
		if tag == 0 {
			break
		}
		tags[tag] = val
	}
	loaded.DynamicTags = tags

	// Resolve table regions. PS4 keeps them in the PT_SCE_DYNLIBDATA blob and
	// names them by offset (0x6100003x tags). PS5 and standard linkers name
	// them by guest virtual address (DT_SYMTAB, DT_RELA, ...), the tables
	// living in an ordinary mapped segment.
	var strtab, symBytes []byte
	var relaTab, jmpTab []byte
	syment := tags[DT_SCE_SYMENT]
	base := loaded.DynlibData
	_, sceSym := tags[DT_SCE_SYMTAB]
	if len(base) > 0 && sceSym {
		strtabOff, strtabSz := tags[DT_SCE_STRTAB], tags[DT_SCE_STRSZ]
		symtabOff, symtabSz := tags[DT_SCE_SYMTAB], tags[DT_SCE_SYMTABSZ]
		if strtabOff >= uint64(len(base)) || symtabOff >= uint64(len(base)) {
			return nil
		}
		strtab = base[strtabOff:]
		if strtabSz > 0 && strtabOff+strtabSz <= uint64(len(base)) {
			strtab = base[strtabOff : strtabOff+strtabSz]
		}
		symBytes = base[symtabOff:]
		if symtabSz > 0 && symtabOff+symtabSz <= uint64(len(base)) {
			symBytes = base[symtabOff : symtabOff+symtabSz]
		}
		region := func(offTag, szTag int64) []byte {
			off, ok := tags[offTag]
			if !ok || off >= uint64(len(base)) {
				return nil
			}
			end := off + tags[szTag]
			if tags[szTag] == 0 || end > uint64(len(base)) {
				end = uint64(len(base))
			}
			return base[off:end]
		}
		relaTab = region(DT_SCE_RELA, DT_SCE_RELASZ)
		jmpTab = region(DT_SCE_JMPREL, DT_SCE_PLTRELSZ)
	} else if symVA, ok := tags[dtSymtab]; ok {
		syment = tags[dtSymEnt]
		img := loaded.local
		if sva, ok := tags[dtStrtab]; ok {
			sz := tags[dtStrSz]
			if sz == 0 || sz > uint64(len(img)) {
				sz = uint64(len(img)) - min(sva, uint64(len(img)))
			}
			var ok bool
			if strtab, ok = vaSlice(img, sva, sz); !ok {
				return fmt.Errorf("DT_STRTAB 0x%x size 0x%x outside image", sva, sz)
			}
		}
		symSz := tags[DT_SCE_SYMTABSZ]
		if symSz == 0 {
			if sva, ok := tags[dtStrtab]; ok && sva > symVA {
				symSz = sva - symVA
			}
		}
		var ok2 bool
		if symBytes, ok2 = vaSlice(img, symVA, symSz); !ok2 {
			return fmt.Errorf("DT_SYMTAB 0x%x size 0x%x outside image", symVA, symSz)
		}
		if rva, ok := tags[dtRela]; ok && tags[dtRelaSz] > 0 {
			if relaTab, ok = vaSlice(img, rva, tags[dtRelaSz]); !ok {
				return fmt.Errorf("DT_RELA 0x%x size 0x%x outside image", rva, tags[dtRelaSz])
			}
		}
		if jva, ok := tags[dtJmpRel]; ok && tags[dtPLTRELSZ] > 0 {
			if jmpTab, ok = vaSlice(img, jva, tags[dtPLTRELSZ]); !ok {
				return fmt.Errorf("DT_JMPREL 0x%x size 0x%x outside image", jva, tags[dtPLTRELSZ])
			}
		}
	} else {
		return nil
	}
	if syment == 0 {
		syment = 24
	}
	if syment < 24 {
		return fmt.Errorf("invalid DT_SYMENT %d", syment)
	}

	// SCE SYMTAB already includes the STN_UNDEF entry at index 0. Prepending
	// another dummy shifts every JUMP_SLOT onto the previous import.
	if len(loaded.DynSymbols) == 0 {
		for i := 0; i+int(syment) <= len(symBytes); i += int(syment) {
			raw := symBytes[i : i+int(syment)]
			nameOff := binary.LittleEndian.Uint32(raw[0:4])
			info := raw[4]
			shndx := binary.LittleEndian.Uint16(raw[6:8])
			value := binary.LittleEndian.Uint64(raw[8:16])
			size := binary.LittleEndian.Uint64(raw[16:24])
			name := cstringAt(strtab, int(nameOff))
			sym := Symbol{
				Name:    name,
				Address: value,
				Size:    size,
				Type:    elf.ST_TYPE(info),
				Section: elf.SectionIndex(shndx),
			}
			loaded.DynSymbols = append(loaded.DynSymbols, sym)
			if value != 0 && name != "" {
				addSymbol(loaded, sym)
			}
		}
	}

	_, hasRelaDyn := loaded.Sections[".rela.dyn"]
	_, hasRelaPlt := loaded.Sections[".rela.plt"]
	if !hasRelaDyn && len(relaTab) > 0 {
		if err := applyRelaTable(loaded, relaTab, false); err != nil {
			return err
		}
	}
	if initAddr, ok := tags[DT_INIT]; ok && initAddr != 0 && len(loaded.Sections) == 0 {
		loaded.InitArray = append(loaded.InitArray, initAddr)
	}

	if loaded.ImportLibs == nil {
		loaded.ImportLibs = make(map[uint16]string)
	}
	for i := 0; i+16 <= len(dyn); i += 16 {
		tag := int64(binary.LittleEndian.Uint64(dyn[i : i+8]))
		val := binary.LittleEndian.Uint64(dyn[i+8 : i+16])
		if tag == 0 {
			break
		}
		switch tag {
		case dtNeeded:
			name := cstringAt(strtab, int(val))
			if name != "" {
				loaded.NeededLibs = append(loaded.NeededLibs, name)
			}
		case DT_SCE_IMPORT_LIB, DT_SCE_EXPORT_LIB, DT_SCE_IMPORT_LIB_PS5, DT_SCE_EXPORT_LIB_PS5:
			id := uint16(val >> 48)
			if tag == DT_SCE_IMPORT_LIB_PS5 || tag == DT_SCE_EXPORT_LIB_PS5 {
				loaded.LibIDSecond = true
			}
			name := cstringAt(strtab, int(val&0xffffffff))
			if name == "" {
				name = cstringAt(strtab, int(val&0xfff))
			}
			if name != "" {
				loaded.ImportLibs[id] = name
			}
		}
	}

	if !hasRelaPlt && len(jmpTab) > 0 {
		if err := applyRelaTable(loaded, jmpTab, true); err != nil {
			return err
		}
	}
	return nil
}

func cstringAt(buf []byte, off int) string {
	if off < 0 || off >= len(buf) {
		return ""
	}
	end := off
	for end < len(buf) && buf[end] != 0 {
		end++
	}
	return string(buf[off:end])
}

func basename(path string) string {
	for i := len(path) - 1; i >= 0; i-- {
		if path[i] == '/' || path[i] == '\\' {
			return path[i+1:]
		}
	}
	return path
}

// InExecutable reports whether addr falls in an executable mapped range.
func (l *LoadedELF) InExecutable(addr uint64) bool {
	if l == nil {
		return false
	}
	for _, r := range l.ExecRanges {
		if addr >= r.Start && addr < r.End {
			return true
		}
	}
	return false
}

// ContainsVA reports whether addr is inside the loaded memory image.
func (l *LoadedELF) ContainsVA(addr uint64) bool {
	return l != nil && l.MemoryImage.Contains(addr)
}

// publishImage exposes the load buffer as the module's sparse image. The region starts at the
// page floor of the lowest mapped address, so a fixed-address executable linked at 0x400000 does
// not carry 4 MiB of leading zeros.
func (l *LoadedELF) publishImage() {
	lo := l.MinVAddr &^ 0x3fff
	if lo > uint64(len(l.local)) {
		lo = uint64(len(l.local))
	}
	l.MemoryImage = NewImage(lo, l.local[lo:])
}

// sectionlessReader returns a reader over data. PS5 dumps strip the section headers but keep
// e_shoff/e_shnum in the ELF header, so debug/elf would fail with EOF. When the section header
// table does not fit inside the file, the three section fields of the header are zeroed in an
// overlay (the input is not copied or modified) and the image loads from program headers and
// the dynamic segment alone.
func sectionlessReader(data []byte) io.ReaderAt {
	if len(data) < 64 || string(data[:4]) != "\x7fELF" || data[4] != 2 {
		return bytes.NewReader(data)
	}
	var bo binary.ByteOrder = binary.LittleEndian
	if data[5] == 2 {
		bo = binary.BigEndian
	}
	shoff := bo.Uint64(data[40:])
	shentsize := uint64(bo.Uint16(data[58:]))
	shnum := uint64(bo.Uint16(data[60:]))
	if shoff == 0 && shnum == 0 {
		return bytes.NewReader(data)
	}
	if shoff <= uint64(len(data)) && shentsize*shnum <= uint64(len(data))-shoff {
		return bytes.NewReader(data)
	}
	log.Printf("elfloader: section header table at 0x%x (%d entries) is outside the %d-byte file; loading from program headers", shoff, shnum, len(data))
	hdr := make([]byte, 64)
	copy(hdr, data[:64])
	for i := 40; i < 48; i++ {
		hdr[i] = 0 // e_shoff
	}
	hdr[60], hdr[61], hdr[62], hdr[63] = 0, 0, 0, 0 // e_shnum, e_shstrndx
	return &overlayReader{base: bytes.NewReader(data), hdr: hdr}
}

type overlayReader struct {
	base *bytes.Reader
	hdr  []byte
}

func (o *overlayReader) ReadAt(p []byte, off int64) (int, error) {
	n, err := o.base.ReadAt(p, off)
	if off < int64(len(o.hdr)) {
		for i := 0; i < n; i++ {
			if pos := off + int64(i); pos < int64(len(o.hdr)) {
				p[i] = o.hdr[pos]
			}
		}
	}
	return n, err
}
