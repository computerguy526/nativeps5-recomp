package elfloader

import (
	"debug/elf"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"strings"
)

// System stub modules shipped with the OpenOrbis SDK. Their APIs are provided
// by host shims; they are not AOT-recompiled.
var systemStubModules = map[string]struct{}{
	"libc.prx":         {},
	"libscelibc.prx":   {},
	"libscefios2.prx":  {},
	"libscelibc.sprx":  {},
	"libscefios2.sprx": {},
	"right.sprx":       {},
	"right.prx":        {},
}

// IsSystemStubModule reports whether name/path is an SDK import stub PRX.
// OpenOrbis SDK stub modules are tiny symbol placeholders (< 64 KB).
// Real retail modules provided by games (such as retail libc.prx or libSceFios2.prx)
// contain actual implementations and are much larger.
func IsSystemStubModule(name string) bool {
	base := strings.ToLower(filepath.Base(name))
	if base == "libscenptoolkit2.prx" || base == "libscenptoolkit2.sprx" ||
		base == "libscenpcppwebapi.prx" || base == "libscenpcppwebapi.sprx" {
		return true
	}
	if _, ok := systemStubModules[base]; !ok {
		return false
	}
	if fi, err := os.Stat(name); err == nil && fi.Size() > 64*1024 {
		// A large file is a real implementation unless its code is hollow: some SDKs pad the stub
		// libraries (a PS5 libc.prx of 1.2 MB with 127 non-zero code bytes, every export aliased
		// to one return-zero body). Linking such a module would bind every libc import to that body.
		return hollowModule(name)
	}
	return true
}

// hollowModule reports whether the executable segments of the module are (almost) all zero.
func hollowModule(path string) bool {
	l, err := LoadELF(path)
	if err != nil || l.MemoryImage == nil {
		return false
	}
	var code []byte
	for _, r := range l.ExecRanges {
		code = append(code, l.MemoryImage.Bytes(r.Start, r.End-r.Start)...)
	}
	return hollowCode(code)
}

// hollowCode: fewer than one non-zero byte in a thousand.
func hollowCode(code []byte) bool {
	if len(code) == 0 {
		return false
	}
	n := 0
	for _, b := range code {
		if b != 0 {
			n++
		}
	}
	return n*1000 < len(code)
}

// ModuleRef is a companion PRX/SPRX/ELF to AOT-link into the guest image.
type ModuleRef struct {
	Path    string
	Aliases []string
}

// ReferencedModuleNames extracts .prx/.sprx paths stored as C strings in guest memory.
func ReferencedModuleNames(img *Image) []string {
	seen := make(map[string]struct{})
	var out []string
	for _, r := range img.Regions() {
		out = referencedModuleNames(r.Data, seen, out)
	}
	return out
}

func referencedModuleNames(image []byte, seen map[string]struct{}, out []string) []string {
	for i := 0; i < len(image); {
		if image[i] < 0x20 || image[i] > 0x7e {
			i++
			continue
		}
		j := i
		for j < len(image) && image[j] >= 0x20 && image[j] <= 0x7e {
			j++
		}
		if j-i >= 8 && j < len(image) && image[j] == 0 {
			s := string(image[i:j])
			if isGuestModulePath(s) {
				if _, ok := seen[s]; !ok {
					seen[s] = struct{}{}
					out = append(out, s)
				}
			}
		}
		i = j + 1
	}
	return out
}

func isGuestModulePath(s string) bool {
	lower := strings.ToLower(s)
	if !strings.Contains(lower, ".prx") && !strings.Contains(lower, ".sprx") {
		return false
	}
	return !IsSystemStubModule(filepath.Base(s))
}

// MapGuestPath translates a PS4 guest path onto the host app directory.
// /app0/sce_module/foo.prx → <appDir>/sce_module/foo.prx
// %s/Modules/foo.prx → <appDir>/Modules/foo.prx
func MapGuestPath(guestPath, appDir string) string {
	if appDir == "" || guestPath == "" {
		return ""
	}
	p := strings.ReplaceAll(guestPath, "\\", "/")
	if strings.HasPrefix(p, "/app0/") {
		p = p[len("/app0/"):]
	} else if p == "/app0" {
		return appDir
	} else if strings.HasPrefix(p, "/") {
		return ""
	}
	if strings.HasPrefix(p, "%s/") {
		p = strings.TrimPrefix(p, "%s/")
	} else if p == "%s" {
		return appDir
	}
	return filepath.Join(appDir, p)
}

func fileExists(path string) bool {
	if path == "" {
		return false
	}
	fi, err := os.Stat(path)
	return err == nil && !fi.IsDir()
}

func addModuleRef(out *[]ModuleRef, seen map[string]struct{}, path string, aliases ...string) {
	if !fileExists(path) || IsSystemStubModule(path) {
		return
	}
	if abs, err := filepath.Abs(path); err == nil {
		path = abs
	}
	if realPath, err := filepath.EvalSymlinks(path); err == nil {
		path = realPath
	}
	key := strings.ToLower(path)
	if _, ok := seen[key]; ok {
		for i := range *out {
			if strings.ToLower((*out)[i].Path) == key {
				(*out)[i].Aliases = uniqueNonEmpty(append((*out)[i].Aliases, aliases...))
			}
		}
		return
	}
	seen[key] = struct{}{}
	*out = append(*out, ModuleRef{Path: path, Aliases: uniqueNonEmpty(aliases)})
}

func uniqueNonEmpty(in []string) []string {
	seen := make(map[string]struct{})
	var out []string
	for _, s := range in {
		s = strings.TrimSpace(s)
		if s == "" {
			continue
		}
		key := strings.ToLower(filepath.Base(s))
		if _, ok := seen[key]; ok {
			continue
		}
		seen[key] = struct{}{}
		out = append(out, filepath.Base(s))
	}
	return out
}

func thisAppModuleDirs(elfPath, appDir string) []string {
	seen := make(map[string]struct{})
	var dirs []string
	add := func(dir string) {
		if dir == "" {
			return
		}
		if abs, err := filepath.Abs(dir); err == nil {
			dir = abs
		}
		if realPath, err := filepath.EvalSymlinks(dir); err == nil {
			dir = realPath
		}
		if _, err := os.Stat(dir); err != nil {
			return
		}
		key := strings.ToLower(dir)
		if _, ok := seen[key]; ok {
			return
		}
		seen[key] = struct{}{}
		dirs = append(dirs, dir)
	}

	checkDir := func(base string) {
		if base == "" {
			return
		}
		add(base)
		add(filepath.Join(base, "sce_module"))
		add(filepath.Join(base, "prx"))
		add(filepath.Join(base, "Modules"))
		add(filepath.Join(base, "modules"))
		add(filepath.Join(base, "Media", "Modules"))
		add(filepath.Join(base, "Media", "modules"))
		add(filepath.Join(base, "Plugins"))
		add(filepath.Join(base, "plugins"))
	}

	if appDir != "" {
		checkDir(appDir)
	}
	if elfPath != "" {
		dir := filepath.Dir(elfPath)
		for range 5 {
			checkDir(dir)
			parent := filepath.Dir(dir)
			if parent == dir {
				break
			}
			dir = parent
		}
	}
	return dirs
}

// DiscoverCompanionModules finds PRX/SPRX files that belong to this app:
// files in <appDir>/sce_module, and guest paths such as /app0/sce_module/foo.prx
// resolved through MapGuestPath. It does not search sibling projects.
func DiscoverCompanionModules(elfPath, appDir string, image *Image) []ModuleRef {
	if appDir == "" && elfPath != "" {
		appDir = filepath.Dir(elfPath)
	}
	seen := make(map[string]struct{})
	var out []ModuleRef

	// Walk application roots recursively to find all companion .prx/.sprx files
	var roots []string
	if appDir != "" {
		roots = append(roots, appDir)
	}
	if elfPath != "" {
		dir := filepath.Dir(elfPath)
		if dir != appDir {
			roots = append(roots, dir)
		}
	}

	for _, root := range roots {
		_ = filepath.WalkDir(root, func(path string, d os.DirEntry, err error) error {
			if err != nil {
				return nil
			}
			name := d.Name()
			if strings.HasPrefix(name, ".") {
				if d.IsDir() {
					return filepath.SkipDir
				}
				return nil
			}
			if d.IsDir() {
				// Don't recurse into output/build dirs if they are inside the app dir
				if name == "build" || name == "build_asm" || name == ".git" {
					return filepath.SkipDir
				}
				return nil
			}
			ext := strings.ToLower(filepath.Ext(name))
			if ext == ".prx" || ext == ".sprx" {
				addModuleRef(&out, seen, path, name)
			}
			return nil
		})
	}

	for _, ref := range ReferencedModuleNames(image) {
		mapped := MapGuestPath(ref, appDir)
		if mapped != "" {
			addModuleRef(&out, seen, mapped, filepath.Base(ref))
		}
		cleanRef := strings.TrimPrefix(strings.ReplaceAll(ref, "\\", "/"), "%s/")
		if cleanRef != ref {
			mappedClean := MapGuestPath(cleanRef, appDir)
			if mappedClean != "" {
				addModuleRef(&out, seen, mappedClean, filepath.Base(cleanRef))
			}
		}
		for _, dir := range thisAppModuleDirs(elfPath, appDir) {
			addModuleRef(&out, seen, filepath.Join(dir, filepath.Base(ref)), filepath.Base(ref))
			if cleanRef != ref {
				addModuleRef(&out, seen, filepath.Join(dir, filepath.Base(cleanRef)), filepath.Base(cleanRef))
			}
		}
	}
	return out
}

// Overlaps reports whether two loaded images share any virtual address bytes.
func Overlaps(a, b *LoadedELF) bool {
	if a == nil || b == nil {
		return false
	}
	return a.MinVAddr < b.MaxVAddr && b.MinVAddr < a.MaxVAddr
}

func alignUp(v, a uint64) uint64 {
	if a == 0 {
		return v
	}
	return (v + a - 1) &^ (a - 1)
}

// symDefined reports a symbol defined in a section of the image (not undefined, not absolute or
// common): its value is an address even when that address is 0.
func symDefined(s Symbol) bool { return s.Section != elf.SHN_UNDEF && s.Section < elf.SHN_LORESERVE }

// symMoves reports whether ApplyBias shifts the symbol's value: addresses do, TLS offsets and
// absolute values do not, and an undefined symbol has no address.
func symMoves(s Symbol) bool {
	if s.Type == elf.STT_TLS || s.Section == elf.SHN_ABS {
		return false
	}
	return s.Address != 0 || symDefined(s)
}

// ApplyBias shifts the entire loaded image by delta bytes so it can sit next
// to another module without overlapping. RIP-relative code stays valid because
// the module is moved as a unit.
func (l *LoadedELF) ApplyBias(delta uint64) error {
	if l == nil || delta == 0 {
		return nil
	}
	if l.MaxVAddr+delta < l.MaxVAddr || l.MinVAddr+delta < l.MinVAddr || l.OrigMaxVAddr+delta < l.OrigMaxVAddr {
		return fmt.Errorf("bias 0x%x moves the image [0x%x, 0x%x) past the end of the address space", delta, l.MinVAddr, l.MaxVAddr)
	}
	shift := func(addr uint64) uint64 {
		if addr == 0 {
			return 0
		}
		return addr + delta
	}

	// An entry point of 0 means "none" (SCE executables carry no e_entry and the loader finds
	// the entry elsewhere); it is not moved. The first segment, sections and symbols, by
	// contrast, can really sit at link address 0 (hello_world.elf's .text) and are moved below.
	l.EntryPoint = shift(l.EntryPoint)
	l.MinVAddr += delta
	l.MaxVAddr += delta
	l.OrigMinVAddr += delta
	l.OrigMaxVAddr += delta
	if l.CanaryAddr != 0 {
		l.CanaryAddr += delta
	}
	if len(l.Segments) > 0 {
		l.Seg0Addr += delta // segment 0 may sit at link address 0
	} else {
		l.Seg0Addr = shift(l.Seg0Addr)
	}
	l.TLSVaddr = shift(l.TLSVaddr)
	l.EHFrameHdrAddr = shift(l.EHFrameHdrAddr)
	l.EHFrameAddr = shift(l.EHFrameAddr)
	l.ProcParamAddr = shift(l.ProcParamAddr)
	l.Bias += delta
	// The region moves; its bytes (and every Segment.Data slice into them) stay where they are.
	l.MemoryImage.Shift(delta)

	for _, seg := range l.Segments {
		seg.Vaddr += delta
	}
	for i := range l.LSDABounds {
		l.LSDABounds[i].Start += delta
		l.LSDABounds[i].End += delta
	}
	// Address-valued dynamic tags that are read after load (init/fini seeding).
	for _, tag := range [...]int64{DT_INIT, 13 /* DT_FINI */, DT_INIT_ARRAY, 26 /* DT_FINI_ARRAY */, DT_PREINIT_ARRAY} {
		if v, ok := l.DynamicTags[tag]; ok && v != 0 {
			l.DynamicTags[tag] = v + delta
		}
	}
	// Section headers are consulted by address after load (.text bounds): move copies, the
	// originals belong to the parsed file.
	for name, sec := range l.Sections {
		if sec == nil || (sec.Addr == 0 && sec.Flags&elf.SHF_ALLOC == 0) { // non-allocated sections have no address
			continue
		}
		moved := *sec
		moved.Addr += delta
		l.Sections[name] = &moved
	}
	for i := range l.ExecRanges {
		l.ExecRanges[i].Start += delta
		l.ExecRanges[i].End += delta
	}
	for i := range l.ROAddrRanges {
		l.ROAddrRanges[i].Start += delta
		l.ROAddrRanges[i].End += delta
	}
	for i := range l.FuncBounds {
		l.FuncBounds[i].Start += delta
		l.FuncBounds[i].End += delta
	}
	for i := range l.UnwindRanges {
		l.UnwindRanges[i].Start += delta
		l.UnwindRanges[i].End += delta
	}
	for i := range l.DataRanges {
		l.DataRanges[i].Start += delta
		l.DataRanges[i].End += delta
	}

	// An STT_TLS symbol's value is an offset in the module's TLS block, not an address.
	for i := range l.Symbols {
		if symMoves(l.Symbols[i]) {
			l.Symbols[i].Address += delta
		}
	}
	l.SymbolByAddr = make(map[uint64]Symbol, len(l.Symbols))
	l.SymbolByName = make(map[string]Symbol, len(l.Symbols))
	for _, sym := range l.Symbols {
		if sym.Name != "" {
			l.SymbolByName[sym.Name] = sym
		}
		if sym.Address != 0 || symMoves(sym) {
			l.SymbolByAddr[sym.Address] = sym
		}
	}
	for i := range l.DynSymbols {
		if symMoves(l.DynSymbols[i]) {
			l.DynSymbols[i].Address += delta
		}
	}
	for i := range l.InitArray {
		if l.InitArray[i] != 0 {
			l.InitArray[i] += delta
		}
	}
	for i := range l.InitWalked {
		l.InitWalked[i] += delta
	}

	for i := range l.Relocations {
		rel := &l.Relocations[i]
		rel.Offset += delta
		if rel.PltAddr != 0 {
			rel.PltAddr += delta
		}
		switch rel.Type {
		case R_X86_64_RELATIVE:
			// The addend is link-relative; Bias is the module's total displacement.
			l.MemoryImage.PutU64(rel.Offset, uint64(rel.Addend)+l.Bias)
		case R_X86_64_JUMP_SLOT:
			if rel.PltAddr != 0 {
				l.MemoryImage.PutU64(rel.Offset, rel.PltAddr)
			}
		case R_X86_64_64, R_X86_64_GLOB_DAT:
			canonSym, _ := ResolveNID(rel.SymName)
			if (rel.SymName == "__stack_chk_guard" || canonSym == "__stack_chk_guard") && l.CanaryAddr != 0 {
				l.MemoryImage.PutU64(rel.Offset, l.CanaryAddr)
			} else if int(rel.SymIdx) < len(l.DynSymbols) {
				sym := l.DynSymbols[rel.SymIdx]
				if sym.Address != 0 {
					l.MemoryImage.PutU64(rel.Offset, sym.Address+uint64(rel.Addend))
				}
			}
		}
	}
	return nil
}

// MergeImages copies src's mapped bytes and metadata into dst. The caller must
// relocate src first if the virtual address ranges overlap.
func MergeImages(dst, src *LoadedELF) error {
	if dst == nil || src == nil {
		return fmt.Errorf("merge requires two loaded images")
	}
	if Overlaps(dst, src) {
		return fmt.Errorf("cannot merge overlapping modules %s (0x%x-0x%x) and %s (0x%x-0x%x)",
			dst.FileName, dst.MinVAddr, dst.MaxVAddr, src.FileName, src.MinVAddr, src.MaxVAddr)
	}
	if dst.MemoryImage == nil {
		dst.MemoryImage = &Image{}
	}
	if err := dst.MemoryImage.Merge(src.MemoryImage); err != nil {
		return fmt.Errorf("cannot merge %s into %s: %w", src.FileName, dst.FileName, err)
	}
	if src.MinVAddr < dst.MinVAddr {
		dst.MinVAddr = src.MinVAddr
	}
	if src.MaxVAddr > dst.MaxVAddr {
		dst.MaxVAddr = src.MaxVAddr
	}
	dst.Segments = append(dst.Segments, src.Segments...)
	dst.ExecRanges = append(dst.ExecRanges, src.ExecRanges...)
	dst.ROAddrRanges = mergeRanges(append(dst.ROAddrRanges, src.ROAddrRanges...))
	dst.FuncBounds = append(dst.FuncBounds, src.FuncBounds...)
	slices.SortFunc(dst.FuncBounds, func(a, b AddrRange) int {
		if a.Start < b.Start {
			return -1
		}
		if a.Start > b.Start {
			return 1
		}
		return 0
	})
	dst.UnwindRanges = mergeRanges(append(dst.UnwindRanges, src.UnwindRanges...))
	dst.DataRanges = mergeRanges(append(dst.DataRanges, src.DataRanges...))
	dst.Relocations = append(dst.Relocations, src.Relocations...)

	canaryTarget := dst.CanaryAddr
	if canaryTarget == 0 {
		canaryTarget = src.CanaryAddr
		dst.CanaryAddr = src.CanaryAddr
	}
	if canaryTarget != 0 {
		const canaryValue = uint64(0x595e9fbd94fda766)
		dst.MemoryImage.PutU64(canaryTarget, canaryValue)
		for _, rel := range dst.Relocations {
			canonSym, _ := ResolveNID(rel.SymName)
			if rel.SymName == "__stack_chk_guard" || canonSym == "__stack_chk_guard" {
				dst.MemoryImage.PutU64(rel.Offset, canaryTarget)
			}
		}
	}

	dst.InitArray = append(dst.InitArray, src.InitArray...)
	for _, sym := range src.Symbols {
		// An STT_TLS symbol's value is an offset in the module's TLS block, not a guest
		// address (AddBias leaves it unrelocated for the same reason). It must not enter
		// the address-keyed symbol maps of the merged image: a small TLS offset can name
		// an address that belongs to the main image's code.
		if sym.Type == elf.STT_TLS {
			continue
		}
		addSymbol(dst, sym)
	}
	if len(src.DynSymbols) > 1 {
		if len(dst.DynSymbols) == 0 {
			dst.DynSymbols = append(dst.DynSymbols, Symbol{})
		}
		dst.DynSymbols = append(dst.DynSymbols, src.DynSymbols[1:]...)
	}
	return nil
}

// ExportedFunctions returns named function symbols suitable for sceKernelDlsym.
func (l *LoadedELF) ExportedFunctions() []Symbol {
	if l == nil {
		return nil
	}
	seen := make(map[string]struct{})
	var out []Symbol
	consider := func(sym Symbol) {
		if sym.Name == "" || sym.Address == 0 || sym.Local {
			return
		}
		if _, ok := seen[sym.Name]; ok {
			return
		}
		validAddr := l.MemoryImage.Contains(sym.Address)
		isExport := sym.Type == elf.STT_FUNC || sym.Type == elf.STT_OBJECT || (sym.Type == elf.STT_NOTYPE && validAddr)
		if !isExport {
			return
		}
		seen[sym.Name] = struct{}{}
		out = append(out, sym)
	}
	for _, sym := range l.DynSymbols {
		consider(sym)
	}
	for _, sym := range l.Symbols {
		consider(sym)
	}
	return out
}

// exportTable holds the definitions imports may bind to, split by kind.
type exportTable struct {
	funcs map[string]uint64 // STT_FUNC, STT_GNU_IFUNC and untyped definitions
	data  map[string]uint64 // STT_OBJECT, STT_COMMON and untyped definitions
}

func newExportTable() *exportTable {
	return &exportTable{funcs: make(map[string]uint64), data: make(map[string]uint64)}
}

// sttGNUIFunc is STT_GNU_IFUNC, which debug/elf does not name.
const sttGNUIFunc = elf.SymType(10)

// add records sym under its name, NID prefix and canonical name. Local symbols, undefined
// symbols and kinds that are not addresses (TLS offsets, sections, files) are ignored.
// override lets a later definition replace an earlier one (companion exports, in link order).
func (t *exportTable) add(sym Symbol, override bool) {
	if sym.Name == "" || sym.Address == 0 || sym.Local {
		return
	}
	var into []map[string]uint64
	switch sym.Type {
	case elf.STT_FUNC, sttGNUIFunc:
		into = []map[string]uint64{t.funcs}
	case elf.STT_OBJECT, elf.STT_COMMON:
		into = []map[string]uint64{t.data}
	case elf.STT_NOTYPE:
		into = []map[string]uint64{t.funcs, t.data}
	default:
		return
	}
	put := func(key string) {
		if key == "" {
			return
		}
		for _, m := range into {
			if _, ok := m[key]; !ok || override {
				m[key] = sym.Address
			}
		}
	}
	put(sym.Name)
	put(NIDPrefix(sym.Name))
	if canon, ok := ResolveNID(sym.Name); ok {
		put(canon)
	}
}

// lookup resolves an import of the given kind by name, NID prefix or canonical name. An untyped
// import (SCE NID imports often are) may bind to either kind, functions first.
func (t *exportTable) lookup(name string, kind elf.SymType) (uint64, bool) {
	var from []map[string]uint64
	switch kind {
	case elf.STT_FUNC, sttGNUIFunc:
		from = []map[string]uint64{t.funcs}
	case elf.STT_OBJECT, elf.STT_COMMON:
		from = []map[string]uint64{t.data}
	default:
		from = []map[string]uint64{t.funcs, t.data}
	}
	keys := [3]string{name, NIDPrefix(name), ""}
	if canon, ok := ResolveNID(name); ok {
		keys[2] = canon
	}
	for _, m := range from {
		for _, k := range keys {
			if k == "" {
				continue
			}
			if addr, ok := m[k]; ok && addr != 0 {
				return addr, true
			}
		}
	}
	return 0, false
}

// ResolveModuleRelocations patches all GOT/PLT and data relocations across the main image
// and linked companion modules into main.MemoryImage.
func ResolveModuleRelocations(main *LoadedELF, companionExports []Symbol) error {
	if main == nil {
		return nil
	}

	// 1. Build the lookup tables from companion module exports and main's defined symbols.
	// Imports are resolved by kind: a function import binds only to a function definition and a
	// data import only to a data definition, and a local symbol satisfies nothing. Without this
	// the payload SDK's startup objects break every title: they carry local 8-byte data symbols
	// named malloc, strlen, memcpy, vsnprintf, ... (its own lazily bound pointers), and a GOT
	// slot for the imported function malloc bound to one of those sends the call into .bss.
	// Earlier sources win: companion exports, then main's dynamic symbols, then its static ones.
	exports := newExportTable()
	for _, sym := range companionExports {
		exports.add(sym, true)
	}
	for _, sym := range main.DynSymbols {
		exports.add(sym, false)
	}
	for _, sym := range main.Symbols {
		exports.add(sym, false)
	}

	// 2. Iterate through all relocations in main
	for i := range main.Relocations {
		rel := &main.Relocations[i]
		if !main.MemoryImage.Has(rel.Offset, 8) {
			continue
		}

		switch rel.Type {
		case R_X86_64_JUMP_SLOT, R_X86_64_GLOB_DAT, R_X86_64_64:
			if rel.SymName == "" {
				continue
			}

			canonSym, _ := ResolveNID(rel.SymName)
			if (rel.SymName == "__stack_chk_guard" || canonSym == "__stack_chk_guard") && main.CanaryAddr != 0 {
				main.MemoryImage.PutU64(rel.Offset, main.CanaryAddr)
				continue
			}

			// Look the import up by the kind its own symbol table entry declares; a PLT slot is
			// a function by construction.
			importType := elf.STT_NOTYPE
			if int(rel.SymIdx) < len(main.DynSymbols) && main.DynSymbols[rel.SymIdx].Name == rel.SymName {
				importType = main.DynSymbols[rel.SymIdx].Type
			}
			if rel.Type == R_X86_64_JUMP_SLOT {
				importType = elf.STT_FUNC
			}
			targetAddr, ok := exports.lookup(rel.SymName, importType)

			if ok && targetAddr != 0 {
				finalAddr := targetAddr + uint64(rel.Addend)
				main.MemoryImage.PutU64(rel.Offset, finalAddr)
			} else {
				// For host shims or unresolved symbols: point the GOT slot to rel.Offset (or rel.PltAddr)
				// so indirect call recomp_dispatch(ctx, MEM_U64(rel.Offset)) dispatches to the registered shim/stub!
				target := rel.Offset
				if rel.PltAddr != 0 {
					target = rel.PltAddr
				}
				main.MemoryImage.PutU64(rel.Offset, target)
			}
		}
	}

	return nil
}
