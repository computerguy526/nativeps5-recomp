package disasm

import (
	"debug/elf"
	"encoding/binary"
	"os"

	"ps4-recomp/pkg/elfloader"

	"golang.org/x/arch/x86/x86asm"
)

// SeedEntryPoints collects CFG roots from the ELF entry point, .init_array,
// and every STT_FUNC symbol. When the image is stripped (few function symbols),
// it also recovers pointers from relocations and read-only data.
func SeedEntryPoints(loaded *elfloader.LoadedELF, allSymbols bool) []uint64 {
	opts := DefaultSeedOptions()
	opts.AllSymbols = allSymbols
	entries, _ := SeedEntryPointsOpts(loaded, opts)
	return entries
}

// SeedOptions selects seed sources. DefaultSeedOptions is what the recompiler uses; the
// switches exist so each source can be measured on its own (see docs/m1/discovery.md).
type SeedOptions struct {
	AllSymbols bool
	// WritableData scans initialized writable segments (.data.rel.ro, .data, .got,
	// init/fini arrays) for code pointers. PS5 SDK images put .data.rel.ro/.got/.rodata in
	// one RW segment, so a read-only-only scan never sees vtables there.
	WritableData bool
	// LandingPads seeds the landing pads named by .gcc_except_table call-site records.
	LandingPads bool
	// ReturnSites seeds the address after every LSDA call-site region (a throwing call's
	// return address), so a return or unwinder resume there has a dispatch entry.
	ReturnSites bool
}

// DefaultSeedOptions enables every seed source that has been validated.
func DefaultSeedOptions() SeedOptions {
	return SeedOptions{WritableData: true, LandingPads: true}
}

// SeedReport counts the seeds each source contributed that no earlier source had.
type SeedReport struct {
	New map[string]int
	// Rejected counts candidate pointers dropped by decode validation, per source.
	Rejected map[string]int
}

// SeedEntryPointsOpts is SeedEntryPoints with explicit source switches and a per-source report.
func SeedEntryPointsOpts(loaded *elfloader.LoadedELF, opts SeedOptions) ([]uint64, *SeedReport) {
	rep := &SeedReport{New: map[string]int{}, Rejected: map[string]int{}}
	if loaded == nil {
		return nil, rep
	}
	allSymbols := opts.AllSymbols
	src := "entry"
	seen := make(map[uint64]struct{}, 256)
	var entries []uint64
	add := func(addr uint64) {
		if addr == 0 || !loaded.InExecutable(addr) || loaded.InData(addr) {
			return
		}
		if _, ok := seen[addr]; ok {
			return
		}
		seen[addr] = struct{}{}
		entries = append(entries, addr)
		rep.New[src]++
	}
	// Pointer relocations name function entries. When the target module carries unwind
	// extents, the entry has to be one of those starts. A PC-relative reloc
	// addend is not a pointer and is ignored.
	addPtr := func(addr uint64) {
		if loaded.IsFuncEntry(addr) {
			add(addr)
			return
		}
		if loaded.InFuncExtent(addr) {
			// Strictly inside an FDE-covered function: a label or resume address, not an entry.
			return
		}
		if len(loaded.FuncBounds) > 0 {
			// Code the unwind tables do not cover (stripped or hand-written regions): the
			// pointer has to look like a function start (gapEntryOK is the whole test).
			if !gapEntryOK(loaded.MemoryImage, addr) {
				rep.Rejected[src]++
				return
			}
		} else if !looksLikeFuncStart(loaded.MemoryImage, addr) {
			return
		}
		add(addr)
	}

	add(loaded.EntryPoint)
	src = "init_array"
	for _, addr := range loaded.InitArray {
		add(addr)
	}
	if mainSym, ok := loaded.SymbolByName["main"]; ok {
		add(mainSym.Address)
	}

	src = "symbols"
	funcSyms := 0
	for _, sym := range loaded.Symbols {
		if sym.Type != elf.STT_FUNC || sym.Address == 0 {
			continue
		}
		if allSymbols || loaded.InExecutable(sym.Address) {
			add(sym.Address)
			funcSyms++
		}
	}

	// Relocations name function pointers (vtables, init, callbacks) even when
	// the symbol table is complete: static functions have no STT_FUNC dynsym.
	// The applied slot is the pointer. After a module bias the reloc addend
	// itself is stale, and a PC32 addend was never a virtual address.
	src = "relocs"
	for _, rel := range loaded.Relocations {
		switch rel.Type {
		case elfloader.R_X86_64_RELATIVE, elfloader.R_X86_64_64:
		default:
			continue
		}
		if val, ok := loaded.MemoryImage.U64(rel.Offset); ok {
			addPtr(val)
		}
	}

	src = "ro_scan"
	// Dense RO-segment scan recovers function pointer tables (vtables, relro)
	// that are not in the dynamic symbol table or relocation table.

	for _, seg := range loaded.Segments {
		if seg.Flags&elf.PF_X != 0 {
			continue
		}
		// Function-pointer tables live in read-only data (vtables, relro).
		// Writable segments are too noisy: integers collide with code addresses.
		if seg.Flags&elf.PF_W != 0 {
			continue
		}
		if seg.Memsz < 8 {
			continue
		}
		// The segment's bytes as far as the image holds them (Memsz may run past the region).
		data := loaded.MemoryImage.From(seg.Vaddr)
		if uint64(len(data)) > seg.Memsz {
			data = data[:seg.Memsz]
		}
		for off := 0; off+8 <= len(data); off += 8 {
			val := binary.LittleEndian.Uint64(data[off:])
			if val&3 != 0 {
				continue
			}
			addPtr(val)
		}
	}

	if opts.WritableData {
		src = "data_scan"
		seedWritableData(loaded, addPtr, add, rep)
	}

	// Every .eh_frame FDE describes a function extent the compiler emitted,
	// reachable or not (callbacks, virtual methods, tail-call targets, code
	// reached only through indirect calls). Seeding all of them recovers
	// functions the call graph and relocations alone miss.
	src = "fde"
	for _, b := range loaded.FuncBounds {
		add(b.Start)
	}
	src = "fini"

	// DT_FINI and DT_FINI_ARRAY entries run at exit and are called by nothing.
	if loaded.DynamicTags != nil {
		if fini, ok := loaded.DynamicTags[13]; ok {
			add(fini)
		}
		if arr, ok := loaded.DynamicTags[26]; ok && arr != 0 {
			sz := loaded.DynamicTags[28]
			for off := uint64(0); off+8 <= sz; off += 8 {
				fn, ok := loaded.MemoryImage.U64(arr + off)
				if !ok {
					break
				}
				add(fn)
			}
		}
	}

	if opts.LandingPads || opts.ReturnSites {
		eh := ParseLSDA(loaded)
		if opts.LandingPads {
			src = "landing_pad"
			for _, lp := range eh.LandingPads {
				add(lp)
			}
		}
		if opts.ReturnSites {
			src = "return_site"
			for _, rs := range eh.ReturnSites {
				add(rs)
			}
		}
	}

	return entries, rep
}

// seedWritableData scans the file-backed part of every non-executable writable segment for
// 8-aligned words that point into code. Integers collide with code addresses here, so a
// candidate must be a known entry (FDE start) or pass plausibleEntry (16-aligned, and a
// straight-line run of valid non-privileged instructions).
func seedWritableData(loaded *elfloader.LoadedELF, addPtr func(uint64), add func(uint64), rep *SeedReport) {
	img := loaded.MemoryImage
	for _, seg := range loaded.Segments {
		if seg.Flags&elf.PF_X != 0 || seg.Flags&elf.PF_W == 0 {
			continue
		}
		n := seg.Filesz
		if n > seg.Memsz {
			n = seg.Memsz
		}
		data := img.From(seg.Vaddr)
		if uint64(len(data)) > n {
			data = data[:n]
		}
		// 8-aligned guest addresses inside the segment.
		for off := (8 - seg.Vaddr%8) % 8; off+8 <= uint64(len(data)); off += 8 {
			val := binary.LittleEndian.Uint64(data[off:])
			if val == 0 || !loaded.InExecutable(val) || loaded.InData(val) {
				continue
			}
			if loaded.IsFuncEntry(val) {
				add(val)
				continue
			}
			if loaded.InFuncExtent(val) {
				// FDE-covered code: a real entry is an FDE start, anything else is a
				// pointer into a function body (resume address, jump slot) or an integer.
				continue
			}
			if seedAfterUD2 {
				// Packed layout: the same entry test as relocation targets (behind padding or a terminator).
				if !gapEntryOK(img, val) {
					rep.Rejected["data_scan"]++
					continue
				}
			} else if val&15 != 0 || !plausibleEntry(img, val) {
				rep.Rejected["data_scan"]++
				continue
			}
			add(val)
		}
	}
}

// plausibleEntry decodes up to 6 instructions from addr and reports whether they form a
// valid straight-line prologue-like run (no decode error, nothing privileged).
func plausibleEntry(img *elfloader.Image, addr uint64) bool {
	if !looksLikeFuncStart(img, addr) {
		return false
	}
	return cleanRun(img, addr)
}

// cleanRun decodes up to 6 instructions from addr: no decode error and nothing privileged up
// to the first terminator.
func cleanRun(img *elfloader.Image, addr uint64) bool {
	pc := addr
	for i := 0; i < 6; i++ {
		code := img.From(pc)
		if code == nil {
			return false
		}
		inst, err := x86asm.Decode(code, 64)
		if err != nil || inst.Len == 0 || isPrivileged(inst.Op) {
			return false
		}
		switch inst.Op {
		case x86asm.RET, x86asm.JMP, x86asm.UD2, x86asm.INT:
			return true
		}
		pc += uint64(inst.Len)
	}
	return true
}

func looksLikeFuncStart(img *elfloader.Image, addr uint64) bool {
	code := img.From(addr)
	if len(code) == 0 || !prologueByte(code[0]) {
		return false
	}
	inst, err := x86asm.Decode(code, 64)
	if err != nil || inst.Len == 0 || isPrivileged(inst.Op) {
		return false
	}
	return true
}

func prologueByte(b byte) bool {
	switch b {
	case 0x40, 0x41, 0x43, 0x44, 0x45, 0x48, 0x49, 0x4C, 0x4D, // REX prefixes
		0x53, 0x54, 0x55, 0x56, 0x57, // push
		0x89, 0x8B, // mov
		0xC3,             // ret
		0xE8, 0xE9, 0xEB, // call/jmp
		0xF3: // endbr64 / rep
		return true
	default:
		return false
	}
}

// gapEntryOK reports whether addr, in code without unwind coverage, plausibly starts a
// function: it decodes cleanly (plausibleEntry) and is 16-aligned or follows padding or a
// terminator (int3, nop, ret).
//
// A 16-aligned address behind padding is where the compiler puts a function, so there the
// first-byte prologue filter does not apply: only the clean run is required. Small virtual
// methods and getters start with whatever their first statement is (cmp/test on a member, xor
// eax, movzx, a VEX load) and are reached through vtables and callbacks only; filtered out,
// they and everything only they call are missing from the translation.
var seedAfterUD2 = os.Getenv("NPS5_SEED_AFTER_UD2") == "1"

// SetPackedLayout turns the packed-code discovery rules on for this process: entries behind a ud2 or
// an unconditional jmp are seeded (gapEntryOK) and fall-into entries are kept (keepFallInto). It is
// what NPS5_SEED_AFTER_UD2=1 does, as a translator flag (-packed-layout) so a title profile can set
// it; the environment variable keeps working. Call it before NewDisassembler; off never clears an
// environment opt-in.
func SetPackedLayout(on bool) {
	if !on {
		return
	}
	seedAfterUD2 = true
	keepFallInto = true
}

// PackedLayout reports whether the packed-code discovery rules are on (flag or environment).
func PackedLayout() bool { return seedAfterUD2 }

func gapEntryOK(img *elfloader.Image, addr uint64) bool {
	prev := img.Bytes(addr-1, 1)
	afterPad := len(prev) == 1 && (prev[0] == 0xCC || prev[0] == 0x90 || prev[0] == 0xC3)
	// A ud2 (0F 0B) ends the previous function in titles built without function alignment (clang trap
	// after a noreturn call / unreachable; PPSA07631: 257 of 308 tier-1 misses sat right behind one).
	// Opt-in (translate-time env NPS5_SEED_AFTER_UD2=1) so other titles' discovery is unchanged.
	if !afterPad && seedAfterUD2 {
		if p2 := img.Bytes(addr-2, 2); len(p2) == 2 && p2[0] == 0x0F && p2[1] == 0x0B {
			afterPad = true
		}
		// The same packed layout also ends a function with an unconditional jmp (rel8 back edge or rel32 tail
		// jump; PPSA07631 r8: 551 of 598 misses behind `pop rbp; jmp short <back>`). Code right after an
		// unconditional jmp is only reached by branches, so a relocation pointing there is an entry.
		if p2 := img.Bytes(addr-2, 2); !afterPad && len(p2) == 2 && p2[0] == 0xEB && p2[1]&0x80 != 0 {
			afterPad = true
		}
		if p5 := img.Bytes(addr-5, 5); !afterPad && len(p5) == 5 && p5[0] == 0xE9 {
			afterPad = true
		}
	}
	// Packed layout (opt-in): behind any padding or terminator the first-byte prologue filter does not
	// apply either (small methods start with a VEX load, cmp, xor ...): only the clean run is required.
	if afterPad && (addr&15 == 0 || seedAfterUD2) {
		code := img.From(addr)
		// Padding itself is not an entry (a pointer into an int3/nop run).
		return len(code) > 0 && code[0] != 0xCC && cleanRun(img, addr)
	}
	if !plausibleEntry(img, addr) {
		return false
	}
	return addr&15 == 0 || afterPad
}
