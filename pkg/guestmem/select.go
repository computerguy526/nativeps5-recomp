// SPDX-License-Identifier: GPL-2.0-or-later

package guestmem

import (
	"debug/elf"
	"encoding/binary"
	"fmt"
	"sort"

	"ps4-recomp/pkg/elfloader"
)

// AddrConst is one address-like constant the scan found.
type AddrConst struct {
	Value uint64 `json:"value"`
	// Where is the guest address of the instruction or data word holding it.
	Where uint64 `json:"where"`
	// Kind is "code" (immediate of a movabs) or "data" (unrelocated 8-byte word).
	Kind string `json:"kind"`
}

// Scan is the evidence the per-title mode selection is made from.
type Scan struct {
	// Relocatable is false when the main executable is linked at a fixed address.
	Relocatable bool `json:"relocatable"`
	// FixedMapImports are imported mapping calls that accept a caller-chosen address.
	FixedMapImports []string `json:"fixed_map_imports,omitempty"`
	// CarveoutConsts are page-aligned constants inside the range identity mapping cannot serve.
	CarveoutConsts []AddrConst `json:"carveout_consts,omitempty"`
	// CarveoutConstCount is the full count (CarveoutConsts is capped).
	CarveoutConstCount int `json:"carveout_const_count"`
}

const maxReportedConsts = 32

// fixedMapAPIs are the libkernel entry points through which a title can ask for memory at an
// address of its choosing. Importing one is not proof (most callers pass 0), so it only
// qualifies the constant scan.
var fixedMapAPIs = map[string]struct{}{
	"mmap": {}, "sceKernelMmap": {}, "sceKernelMapDirectMemory": {}, "sceKernelMapDirectMemory2": {},
	"sceKernelMapNamedDirectMemory": {}, "sceKernelMapFlexibleMemory": {}, "sceKernelMapNamedFlexibleMemory": {},
	"sceKernelReserveVirtualRange": {}, "sceKernelMemoryPoolReserve": {}, "sceKernelMapNamedSystemFlexibleMemory": {},
}

// addrLike reports whether v looks like a hardcoded guest address that identity mapping cannot
// serve: page aligned and inside [CarveoutLo, CarveoutHi) relative to a console layout.
func addrLike(v uint64) bool {
	return v >= CarveoutLo && v < CarveoutHi && v&(PageSize-1) == 0
}

// ScanTitle inspects the loaded title (main image with its modules merged) before any image is
// placed. It looks at: whether the executable can be moved at all; imported fixed-map APIs; 64-bit
// immediates in executable segments (movabs r64, imm64: REX.W B8+r); and unrelocated 8-byte data
// words. Constants below CarveoutLo (6 GiB) are not considered: they are indistinguishable from sizes, so a
// hardcoded fixed address in [4 MiB, 6 GiB) passes this scan and is refused at run time (memory.md §4).
// The scan is conservative in one direction only: a false positive selects base mode, which is
// always correct.
func ScanTitle(l *elfloader.LoadedELF, relocatable bool) Scan {
	s := Scan{Relocatable: relocatable}
	if l == nil {
		return s
	}
	seenAPI := make(map[string]struct{})
	relocated := make(map[uint64]struct{}, len(l.Relocations))
	for _, rel := range l.Relocations {
		relocated[rel.Offset] = struct{}{}
		if rel.SymName == "" {
			continue
		}
		name := rel.SymName
		if canon, ok := elfloader.ResolveNID(name); ok && canon != "" {
			name = canon
		}
		if _, ok := fixedMapAPIs[name]; ok {
			seenAPI[name] = struct{}{}
		}
	}
	for name := range seenAPI {
		s.FixedMapImports = append(s.FixedMapImports, name)
	}
	sort.Strings(s.FixedMapImports)

	add := func(v, where uint64, kind string) {
		s.CarveoutConstCount++
		if len(s.CarveoutConsts) < maxReportedConsts {
			s.CarveoutConsts = append(s.CarveoutConsts, AddrConst{Value: v, Where: where, Kind: kind})
		}
	}
	// An unrelocated data word is evidence only if code uses it: some executable segment holds a
	// RIP-relative operand that addresses exactly that word (the load of a hardcoded address from a
	// constant). A word nothing refers to is far more likely a size, a hash or a table cell indexed
	// elsewhere than an address the title maps memory at; counting them selected base mode for
	// every title that happened to carry a 64-bit constant in the carveout range. A table of
	// addresses reached through a base pointer is not seen here (the runtime refuses an address it
	// cannot serve, memory.md section 4).
	ripTargets := ripRelativeTargets(l)
	for _, seg := range l.Segments {
		data := seg.Data
		if uint64(len(data)) > seg.Filesz {
			data = data[:seg.Filesz] // .bss holds no constants
		}
		if seg.Flags&elf.PF_X != 0 {
			// movabs r64, imm64 = REX.W(+B) B8+r imm64. A byte scan can hit inside another
			// instruction; that only adds false positives.
			for i := 0; i+10 <= len(data); i++ {
				if (data[i] == 0x48 || data[i] == 0x49) && data[i+1]&0xf8 == 0xb8 {
					if v := binary.LittleEndian.Uint64(data[i+2:]); addrLike(v) {
						add(v, seg.Vaddr+uint64(i), "code")
					}
				}
				// mov al/rax,[moffs64] and mov [moffs64],al/rax: REX.W A0-A3 imm64 address a hardcoded
				// 64-bit location exactly as a movabs does (the only instructions with an absolute
				// 64-bit address operand).
				if data[i] == 0x48 && data[i+1]&0xfc == 0xa0 {
					if v := binary.LittleEndian.Uint64(data[i+2:]); addrLike(v) {
						add(v, seg.Vaddr+uint64(i), "code")
					}
				}
			}
			continue
		}
		start := (8 - seg.Vaddr%8) % 8
		for off := start; off+8 <= uint64(len(data)); off += 8 {
			v := binary.LittleEndian.Uint64(data[off:])
			if !addrLike(v) {
				continue
			}
			if _, ok := relocated[seg.Vaddr+off]; ok {
				continue // an image pointer the loader rebases
			}
			if _, ok := ripTargets[seg.Vaddr+off]; !ok {
				continue // nothing loads it as an address
			}
			add(v, seg.Vaddr+off, "data")
		}
	}
	return s
}

// ripRelativeTargets returns every guest address that some executable byte sequence could address
// with a RIP-relative operand (ModRM mod=00 rm=101 + disp32). Instruction boundaries are not
// decoded: the operand's end is tried with no immediate and with an 8- and 32-bit one. A byte
// that is not an instruction only adds addresses, which costs a false positive (base mode,
// always correct), never a miss.
func ripRelativeTargets(l *elfloader.LoadedELF) map[uint64]struct{} {
	t := make(map[uint64]struct{})
	for _, seg := range l.Segments {
		if seg.Flags&elf.PF_X == 0 {
			continue
		}
		data := seg.Data
		if uint64(len(data)) > seg.Filesz {
			data = data[:seg.Filesz]
		}
		for i := 0; i+5 <= len(data); i++ {
			if data[i]&0xc7 != 0x05 {
				continue
			}
			disp := int64(int32(binary.LittleEndian.Uint32(data[i+1:])))
			end := seg.Vaddr + uint64(i) + 5
			for _, imm := range [...]uint64{0, 1, 4} {
				t[uint64(int64(end+imm)+disp)] = struct{}{}
			}
		}
	}
	return t
}

// Merge folds another module's evidence into s.
func (s *Scan) Merge(o Scan) {
	seen := make(map[string]struct{}, len(s.FixedMapImports))
	for _, n := range s.FixedMapImports {
		seen[n] = struct{}{}
	}
	for _, n := range o.FixedMapImports {
		if _, ok := seen[n]; !ok {
			s.FixedMapImports = append(s.FixedMapImports, n)
		}
	}
	sort.Strings(s.FixedMapImports)
	s.CarveoutConstCount += o.CarveoutConstCount
	for _, c := range o.CarveoutConsts {
		if len(s.CarveoutConsts) < maxReportedConsts {
			s.CarveoutConsts = append(s.CarveoutConsts, c)
		}
	}
}

// NeedsBase reports whether the evidence rules identity mode out, and why.
func (s Scan) NeedsBase() (bool, string) {
	if !s.Relocatable {
		return true, "the executable is linked at a fixed address"
	}
	if s.CarveoutConstCount > 0 {
		if len(s.CarveoutConsts) == 0 { // a hand-built or hand-edited record without examples
			return true, fmt.Sprintf("%d page-aligned constant(s) inside 0x%x-0x%x, which identity mapping cannot serve",
				s.CarveoutConstCount, uint64(CarveoutLo), uint64(CarveoutHi))
		}
		c := s.CarveoutConsts[0]
		return true, fmt.Sprintf("%d page-aligned constant(s) inside 0x%x-0x%x, which identity mapping cannot serve "+
			"(first: 0x%x in %s at 0x%x)", s.CarveoutConstCount, uint64(CarveoutLo), uint64(CarveoutHi), c.Value, c.Kind, c.Where)
	}
	return false, ""
}

// Select resolves the requested mode ("base", "identity" or "auto") against the scan.
//   - base: always granted.
//   - auto: identity when nothing rules it out, else base.
//   - identity: granted only when nothing rules it out; otherwise an error naming the evidence
//     ("title needs base addressing"), never a silently broken translation.
func Select(requested string, s Scan) (Mode, string, error) {
	needs, why := s.NeedsBase()
	switch requested {
	case "", "base":
		return Base, "requested", nil
	case "auto":
		if needs {
			return Base, "auto: " + why, nil
		}
		return Identity, "auto: no fixed-address evidence", nil
	case "identity":
		if needs {
			return Base, "", fmt.Errorf("title needs base addressing: %s (use -addr-mode base or auto)", why)
		}
		return Identity, "requested", nil
	}
	return Base, "", fmt.Errorf("unknown address mode %q (want base, identity or auto)", requested)
}
