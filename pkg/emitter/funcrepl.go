package emitter

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"sort"
	"strconv"
	"strings"

	"ps4-recomp/pkg/elfloader"
)

// Function-level HLE (docs/m3/function-hle.md).
//
// A ReplacementSet says: these guest functions of the title are not translated, the host
// functions named by Shim run in their place. The mechanism is the one that already serves
// imports: the function's symbol becomes a shim stub (writeShimStub), so a direct call, a tail
// jump and an indirect call through a vtable all reach the host function, and the guest return
// address handling stays that of every other call. What a set adds is the evidence: it is bound
// to one exact binary by the SHA-256 of every function it replaces and by the vtable slots that
// point at them, and it is refused as a whole when any of that does not match.

// Replacement binds one guest function to a host shim.
type Replacement struct {
	Name   string `json:"name"`             // what the function is ("FMalloc::Malloc"), for logs
	Addr   uint64 `json:"-"`                // link-time address, before the module's bias
	Size   uint64 `json:"size"`             // bytes covered by SHA256
	SHA256 string `json:"sha256,omitempty"` // hex digest of the image bytes [Addr, Addr+Size); empty = not verified by hash
	Shim   string `json:"shim"`             // host function: void shim(GuestContext *ctx)
	// Wrap keeps the guest function: its translated body is emitted under the symbol
	// _fn_0x<addr>_orig (WrapSymbol) and every caller reaches the shim, which may call the body
	// (any number of times) through the nps5_repl_wraps table of funcrepl_params.c.
	Wrap bool `json:"wrap,omitempty"`
}

// WrapSymbol is the assembly symbol of the kept body of a wrapped function at guest address g
// (the C name is the same without the leading underscore).
func WrapSymbol(g uint64) string { return fmt.Sprintf("_fn_0x%x_orig", g) }

// VTableCheck asserts that vtable slot Slot holds the address of Func (both link-time addresses).
// It is how a set proves that the functions it replaces are what the virtual calls reach.
type VTableCheck struct {
	VTable uint64
	Slot   int
	Func   uint64
}

// ReplParam is a value for the host side (nps5_funcrepl.h): the biased guest address of Addr, or
// Value as is when Addr is zero.
type ReplParam struct {
	Key   string
	Addr  uint64
	Value uint64
}

// ReplacementSet is a per-title table of replacements.
type ReplacementSet struct {
	Name string // profile name, e.g. "ue5-binned3/PPSA30552"
	// AllowUnverified admits a function that has neither a digest nor a vtable check naming it
	// (-hle-replace-unverified). Without it such a function refuses the whole set.
	AllowUnverified bool
	Funcs           []Replacement
	VTableChecks    []VTableCheck
	Params          []ReplParam
}

// ReplReport is what ApplyReplacements did, for the log and the manifest.
type ReplReport struct {
	Set      string            `json:"set"`
	Bias     uint64            `json:"bias"`
	Replaced []ReplReportEntry `json:"replaced"`
	Params   map[string]uint64 `json:"params,omitempty"`
	// Unverified counts replacements with neither a digest nor a vtable check (admitted only by
	// ReplacementSet.AllowUnverified).
	Unverified int `json:"unverified,omitempty"`
}

// ReplReportEntry is one replaced function in the manifest (guest addresses, biased).
type ReplReportEntry struct {
	Guest uint64 `json:"guest"`
	Name  string `json:"name"`
	Shim  string `json:"shim"`
	Wrap  bool   `json:"wrap,omitempty"`
}

// ApplyReplacements verifies set against the loaded image and, only if all of it holds, binds
// every function to its shim. fragment is true for a tier-1 fragment emitter, which has no
// function list of its own: the "is a discovered function" check is skipped there (the
// translation it continues passed it).
func (e *AsmEmitter) ApplyReplacements(set *ReplacementSet, fragment bool) (*ReplReport, error) {
	if set == nil || len(set.Funcs) == 0 {
		return nil, fmt.Errorf("replacement set %q is empty", setName(set))
	}
	if e.elf == nil || e.elf.MemoryImage == nil {
		return nil, fmt.Errorf("replacement set %s: no loaded image", set.Name)
	}
	bias := e.elf.Bias
	known := map[uint64]bool{}
	if !fragment {
		for _, a := range e.functionAddrs() {
			known[a] = true
		}
	}
	if err := set.validate(); err != nil {
		return nil, err
	}
	if err := set.checkImage(e.elf); err != nil {
		return nil, err
	}
	checked := map[uint64]bool{}
	for _, c := range set.VTableChecks {
		checked[c.Func] = true
	}
	seen := map[uint64]bool{}
	rep := &ReplReport{Set: set.Name, Bias: bias}
	for _, f := range set.Funcs {
		g := f.Addr + bias
		if f.Shim == "" {
			return nil, fmt.Errorf("replacement %s (0x%x): no shim", f.Name, f.Addr)
		}
		if seen[g] {
			return nil, fmt.Errorf("replacement %s: address 0x%x appears twice", f.Name, f.Addr)
		}
		seen[g] = true
		if old, ok := e.shimMap[g]; ok {
			return nil, fmt.Errorf("replacement %s: 0x%x is already bound to host shim %s", f.Name, g, old)
		}
		if f.SHA256 == "" && !checked[f.Addr] {
			if !set.AllowUnverified {
				return nil, fmt.Errorf("replacement %s (0x%x): no sha256 and no vtable check, so nothing ties it to this binary (-hle-replace-unverified accepts that)", f.Name, f.Addr)
			}
			rep.Unverified++
		}
		if !fragment && !known[g] {
			return nil, fmt.Errorf("replacement %s: 0x%x is not an entry of a discovered function; its callers would not link", f.Name, g)
		}
		rep.Replaced = append(rep.Replaced, ReplReportEntry{Guest: g, Name: f.Name, Shim: f.Shim, Wrap: f.Wrap})
	}
	params := map[string]uint64{}
	for _, p := range set.Params {
		v := p.Value
		if p.Addr != 0 {
			v = p.Addr + bias
		}
		params[p.Key] = v
	}
	// everything holds: bind
	for _, f := range set.Funcs {
		e.shimMap[f.Addr+bias] = f.Shim
		if f.Wrap {
			if e.wrapMap == nil {
				e.wrapMap = map[uint64]string{}
			}
			e.wrapMap[f.Addr+bias] = f.Shim
		}
	}
	if e.replParams == nil {
		e.replParams = map[string]uint64{}
	}
	for k, v := range params {
		e.replParams[k] = v
	}
	if len(params) > 0 {
		rep.Params = params
	}
	e.replReports = append(e.replReports, rep)
	return rep, nil
}

var shimIdentRE = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_]*$`)

// validate checks the shape of the set: every function has a shim that is a C identifier (it is
// written into generated assembly and C), a nonzero size and a link-time range inside the 48-bit
// address space, and no two ranges overlap.
func (set *ReplacementSet) validate() error {
	type rng struct {
		lo, hi uint64
		name   string
	}
	rs := make([]rng, 0, len(set.Funcs))
	for _, f := range set.Funcs {
		if f.Shim != "" && !shimIdentRE.MatchString(f.Shim) {
			return fmt.Errorf("replacement %s (0x%x): shim %q is not a C identifier", f.Name, f.Addr, f.Shim)
		}
		if f.Size == 0 {
			return fmt.Errorf("replacement %s (0x%x): size is zero", f.Name, f.Addr)
		}
		if f.Addr >= maxLinkAddr || f.Size >= maxLinkAddr {
			return fmt.Errorf("replacement %s: range 0x%x (+%d) is outside the 48-bit address space", f.Name, f.Addr, f.Size)
		}
		rs = append(rs, rng{f.Addr, f.Addr + f.Size, f.Name})
	}
	sort.SliceStable(rs, func(i, j int) bool { return rs[i].lo < rs[j].lo })
	for i := 1; i < len(rs); i++ {
		if rs[i].lo == rs[i-1].lo {
			return fmt.Errorf("replacement %s: address 0x%x appears twice", rs[i].name, rs[i].lo)
		}
		if rs[i].lo < rs[i-1].hi {
			return fmt.Errorf("replacements %s [0x%x,0x%x) and %s [0x%x,0x%x) overlap", rs[i-1].name, rs[i-1].lo, rs[i-1].hi, rs[i].name, rs[i].lo, rs[i].hi)
		}
	}
	return nil
}

// checkImage verifies the digests of the functions and the vtable slots against the loaded image.
func (set *ReplacementSet) checkImage(l *elfloader.LoadedELF) error {
	bias := l.Bias
	for _, f := range set.Funcs {
		if f.SHA256 == "" {
			continue
		}
		g := f.Addr + bias
		b := l.MemoryImage.Bytes(g, f.Size)
		if f.Size == 0 || b == nil {
			return fmt.Errorf("replacement %s: bytes at 0x%x (+%d) are not in the image", f.Name, g, f.Size)
		}
		if got := sha256.Sum256(b); hex.EncodeToString(got[:]) != strings.ToLower(f.SHA256) {
			return fmt.Errorf("replacement %s: the function at 0x%x is not the one this table was made for (sha256 %x, want %.12s): a different build of the title",
				f.Name, g, got[:6], f.SHA256)
		}
	}
	for _, c := range set.VTableChecks {
		if c.Slot < 0 {
			return fmt.Errorf("replacement set %s: vtable 0x%x slot %d is negative", set.Name, c.VTable, c.Slot)
		}
		slot := c.VTable + bias + 8*uint64(c.Slot)
		v, ok := l.MemoryImage.U64(slot)
		if !ok || v != c.Func+bias {
			return fmt.Errorf("replacement set %s: vtable 0x%x slot %d holds 0x%x, expected function 0x%x", set.Name, c.VTable+bias, c.Slot, v, c.Func+bias)
		}
	}
	return nil
}

// MatchesImage reports whether the image is the binary the set was made for.
func (set *ReplacementSet) MatchesImage(l *elfloader.LoadedELF) bool {
	return l != nil && l.MemoryImage != nil && set.checkImage(l) == nil
}

// Entries returns the biased guest addresses of the replaced functions.
func (set *ReplacementSet) Entries(bias uint64) []uint64 {
	out := make([]uint64, 0, len(set.Funcs))
	for _, f := range set.Funcs {
		out = append(out, f.Addr+bias)
	}
	return out
}

// ReplacementSeeds returns the entry points the replacements need in the code discovery: a
// replaced function that only a vtable reaches is not a seed otherwise, and its stub must exist
// (the dispatch table) for the virtual call to find it. Sets that do not match the image
// contribute nothing; ApplyReplacements reports them later.
func ReplacementSeeds(l *elfloader.LoadedELF, hleMalloc, hleReplace string) []uint64 {
	var out []uint64
	if hleMalloc == "ue5-binned3" {
		for _, p := range ue5Binned3Profiles {
			if p.MatchesImage(l) {
				out = append(out, p.Entries(l.Bias)...)
			}
		}
	}
	if hleReplace != "" {
		if set, err := LoadReplacementFile(hleReplace); err == nil && set.MatchesImage(l) {
			out = append(out, set.Entries(l.Bias)...)
		}
	}
	return out
}

func setName(s *ReplacementSet) string {
	if s == nil {
		return ""
	}
	return s.Name
}

// ReplReports returns what ApplyReplacements did, in order.
func (e *AsmEmitter) ReplReports() []*ReplReport { return e.replReports }

// emitReplParams writes funcrepl_params.c (the nps5_repl_params table of nps5_funcrepl.h), or
// nothing when no replacement was applied. It returns the path written, or "".
func (e *AsmEmitter) emitReplParams(outDir string) (string, error) {
	if len(e.replParams) == 0 && len(e.wrapMap) == 0 {
		return "", nil
	}
	keys := make([]string, 0, len(e.replParams))
	for k := range e.replParams {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	var b bytes.Buffer
	b.WriteString("// Generated by the translator: values for the host side of the function-level replacements.\n")
	b.WriteString("#include \"nps5_funcrepl.h\"\n\n")
	if len(e.replParams) > 0 {
		b.WriteString("const Nps5ReplParam nps5_repl_params[] = {\n")
		for _, k := range keys {
			fmt.Fprintf(&b, "    {%q, 0x%xULL},\n", k, e.replParams[k])
		}
		b.WriteString("    {0, 0}\n};\n")
	}
	if len(e.wrapMap) > 0 {
		// The kept bodies of the wrapped functions (Replacement.Wrap): shim name, guest entry, host entry.
		ws := make([]uint64, 0, len(e.wrapMap))
		for g := range e.wrapMap {
			ws = append(ws, g)
		}
		slices.Sort(ws)
		for _, g := range ws {
			fmt.Fprintf(&b, "extern void %s(void *ctx);\n", strings.TrimPrefix(WrapSymbol(g), "_"))
		}
		b.WriteString("const Nps5ReplWrap nps5_repl_wraps[] = {\n")
		for _, g := range ws {
			fmt.Fprintf(&b, "    {%q, 0x%xULL, %s},\n", e.wrapMap[g], g, strings.TrimPrefix(WrapSymbol(g), "_"))
		}
		b.WriteString("    {0, 0, 0}\n};\n")
	}
	path := filepath.Join(outDir, "funcrepl_params.c")
	return path, writeFileIfChanged(path, b.Bytes())
}

// ---- tables from a file (-hle-replace) --------------------------------------------------------

type replFile struct {
	Name         string          `json:"name"`
	Funcs        []replFileFunc  `json:"funcs"`
	VTableChecks []replFileCheck `json:"vtable_checks,omitempty"`
	Params       []replFileParam `json:"params,omitempty"`
}

type replFileFunc struct {
	Replacement
	Addr string `json:"addr"`
}

type replFileCheck struct {
	VTable string `json:"vtable"`
	Slot   int    `json:"slot"`
	Func   string `json:"func"`
}

type replFileParam struct {
	Key   string `json:"key"`
	Addr  string `json:"addr,omitempty"`
	Value string `json:"value,omitempty"`
}

// parseAddr reads "0x<hex>" or decimal; there is no octal ("010" is ten).
func parseAddr(s string) (uint64, error) {
	t := strings.ToLower(strings.TrimSpace(s))
	base := 10
	if h := strings.TrimPrefix(t, "0x"); h != t {
		t, base = h, 16
	}
	v, err := strconv.ParseUint(t, base, 64)
	if err != nil {
		return 0, fmt.Errorf("bad address %q", s)
	}
	return v, nil
}

// LoadReplacementFile reads a JSON replacement table:
//
//	{"name": "...", "funcs": [{"name": "..", "addr": "0x1130", "size": 64, "sha256": "..", "shim": "shim_x"}],
//	 "vtable_checks": [{"vtable": "0x5000", "slot": 6, "func": "0x1130"}],
//	 "params": [{"key": "ue5m.vtable", "addr": "0x5000"}]}
//
// Addresses are link-time addresses, "0x" hex or plain decimal. A function needs a sha256 or a
// vtable check that names it; the set is refused otherwise unless ReplacementSet.AllowUnverified
// is set.
func LoadReplacementFile(path string) (*ReplacementSet, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var f replFile
	if err := decodeStrictJSON(raw, &f); err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	set := &ReplacementSet{Name: f.Name}
	if set.Name == "" {
		set.Name = filepath.Base(path)
	}
	for _, fn := range f.Funcs {
		a, err := parseAddr(fn.Addr)
		if err != nil {
			return nil, fmt.Errorf("%s: %s: %w", path, fn.Name, err)
		}
		r := fn.Replacement
		r.Addr = a
		set.Funcs = append(set.Funcs, r)
	}
	for _, c := range f.VTableChecks {
		vt, err1 := parseAddr(c.VTable)
		fa, err2 := parseAddr(c.Func)
		if err1 != nil || err2 != nil || c.Slot < 0 {
			return nil, fmt.Errorf("%s: bad vtable check %+v", path, c)
		}
		set.VTableChecks = append(set.VTableChecks, VTableCheck{vt, c.Slot, fa})
	}
	for _, p := range f.Params {
		rp := ReplParam{Key: p.Key}
		var err error
		if p.Addr != "" {
			rp.Addr, err = parseAddr(p.Addr)
		} else {
			rp.Value, err = parseAddr(p.Value)
		}
		if err != nil {
			return nil, fmt.Errorf("%s: param %s: %w", path, p.Key, err)
		}
		set.Params = append(set.Params, rp)
	}
	if err := set.validate(); err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	return set, nil
}
