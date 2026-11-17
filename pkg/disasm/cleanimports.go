package disasm

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"slices"

	"ps4-recomp/pkg/elfloader"
)

// Imports that do not keep pointers (docs/m3/tsoelide.md, section 10).
//
// A frame address passed to one of these is not an escape: the callee reads or writes through
// it during the call and keeps no copy of it, so no other thread can come to hold it. The
// list is deliberately short and conservative:
//   - nothing that stores a pointer into memory it was given (strtol and strtod write an end
//     pointer through their second argument, strtok keeps one, sscanf with %n is not
//     considered, getline, strsep): the pass tracks registers only, so a derived frame
//     address in a frame slot would be invisible to it;
//   - nothing that calls back (qsort, bsearch, atexit, signal) or hands the address to
//     another thread (pthread_*, std::thread glue);
//   - nothing that retains its argument (setvbuf, setbuf, fopen's name is copied but the
//     list stays to the C core: only data-processing functions).
//
// A function that returns a pointer derived from its argument (memcpy and strchr return or
// point into their first argument) is handled at the call: RAX is tainted when an argument
// register is (spState.step).
var cleanImportNames = []string{
	// memory and strings
	"memcpy", "memmove", "memset", "memcmp", "memchr", "memrchr", "bcopy", "bzero", "bcmp",
	"strlen", "strnlen", "strcmp", "strncmp", "strcasecmp", "strncasecmp", "strchr", "strrchr", "strstr",
	"strcpy", "strncpy", "strcat", "strncat", "strspn", "strcspn", "strpbrk", "strcoll",
	// formatted output (the format arguments are read, never kept)
	"printf", "fprintf", "sprintf", "snprintf", "vprintf", "vfprintf", "vsprintf", "vsnprintf",
	"puts", "fputs", "fputc", "putchar", "fwrite", "fread",
	// math on values; sincos, modf and frexp write numbers through their pointers
	"sin", "cos", "tan", "asin", "acos", "atan", "atan2", "sinh", "cosh", "tanh", "exp", "exp2", "log",
	"log2", "log10", "pow", "sqrt", "cbrt", "fabs", "floor", "ceil", "round", "trunc", "fmod", "hypot",
	"ldexp", "frexp", "modf", "sincos",
	"sinf", "cosf", "tanf", "asinf", "acosf", "atanf", "atan2f", "sinhf", "coshf", "tanhf", "expf", "exp2f",
	"logf", "log2f", "log10f", "powf", "sqrtf", "cbrtf", "fabsf", "floorf", "ceilf", "roundf", "truncf",
	"fmodf", "hypotf", "ldexpf", "frexpf", "modff", "sincosf",
	"abs", "labs", "llabs",
}

var cleanImportNIDs = func() map[string]bool {
	m := make(map[string]bool, len(cleanImportNames))
	for _, n := range cleanImportNames {
		m[elfloader.CalculateNID(n)] = true
	}
	return m
}()

// collectCleanImportAddrs returns the PLT stub and GOT slot addresses of the clean imports
// the image binds.
func collectCleanImportAddrs(loaded *elfloader.LoadedELF) map[uint64]bool {
	if loaded == nil {
		return nil
	}
	out := make(map[uint64]bool)
	for _, rel := range loaded.Relocations {
		if rel.SymName == "" || !cleanImportNIDs[elfloader.NIDPrefix(rel.SymName)] && !cleanImportNames2[rel.SymName] {
			continue
		}
		if rel.PltAddr != 0 {
			out[rel.PltAddr] = true
		}
		if rel.Offset != 0 {
			out[rel.Offset] = true
		}
	}
	return out
}

// cleanImportNames2 matches images that carry plain symbol names instead of NIDs.
var cleanImportNames2 = func() map[string]bool {
	m := make(map[string]bool, len(cleanImportNames))
	for _, n := range cleanImportNames {
		m[n] = true
	}
	return m
}()

// isCleanImport reports whether addr is the PLT stub or the GOT slot of a clean import.
func (d *Disassembler) isCleanImport(addr uint64) bool {
	return d != nil && d.cleanImports[addr]
}

// ImportCleanFn is the StackPrivOpts.ImportClean of this image (nil when it binds none).
func (d *Disassembler) ImportCleanFn() func(uint64) bool {
	if d == nil || len(d.cleanImports) == 0 {
		return nil
	}
	return d.isCleanImport
}

// addCleanStubs adds the PLT stubs that jump through a clean GOT slot: "jmp [rip+disp]"
// (ff 25), optionally preceded by endbr64 or a bnd prefix, as the SDK's linker emits them. A
// byte pattern that is not a real stub can only matter if a call targets exactly its start.
func addCleanStubs(loaded *elfloader.LoadedELF, start, end uint64, set map[uint64]bool) {
	if loaded == nil || len(set) == 0 {
		return
	}
	slots := make(map[uint64]bool, len(set))
	for a := range set {
		slots[a] = true
	}
	for pc := start; pc < end; {
		b := loaded.MemoryImage.From(pc)
		if len(b) == 0 {
			pc += 0x1000
			continue
		}
		if uint64(len(b)) > end-pc {
			b = b[:end-pc]
		}
		for i := 0; i+6 <= len(b); i++ {
			if b[i] != 0xff || b[i+1] != 0x25 {
				continue
			}
			at := pc + uint64(i)
			disp := int32(uint32(b[i+2]) | uint32(b[i+3])<<8 | uint32(b[i+4])<<16 | uint32(b[i+5])<<24)
			if !slots[uint64(int64(at)+6+int64(disp))] {
				continue
			}
			set[at] = true
			if i >= 4 && b[i-4] == 0xf3 && b[i-3] == 0x0f && b[i-2] == 0x1e && b[i-1] == 0xfa {
				set[at-4] = true
			}
			if i >= 1 && b[i-1] == 0xf2 {
				set[at-1] = true
			}
		}
		pc += uint64(len(b))
	}
}

// stackPrivRulesVersion is bumped when the privacy pass (stackpriv.go) changes what it proves.
const stackPrivRulesVersion = 2

// StackPrivRulesID names the rules the stack privacy bits are a function of: the pass version
// and the clean import table. It is part of the lifter's ConfigKey (chunk cache key).
func StackPrivRulesID() string { return stackPrivRulesIDFor(stackPrivRulesVersion, cleanImportNames) }

func stackPrivRulesIDFor(version int, names []string) string {
	sorted := append([]string(nil), names...)
	slices.Sort(sorted)
	h := sha256.New()
	fmt.Fprintf(h, "v%d\n", version)
	for _, n := range sorted {
		h.Write([]byte(n + "\n"))
	}
	return hex.EncodeToString(h.Sum(nil))[:12]
}
