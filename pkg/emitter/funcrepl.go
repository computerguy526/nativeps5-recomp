package emitter

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
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
	SHA256 string `json:"sha256,omitempty"` // hex digest of the image bytes [Addr, Addr+Size); empty = not verified
	Shim   string `json:"shim"`             // host function: void shim(GuestContext *ctx)
}

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
	Name         string // profile name, e.g. "ue5-binned3/PPSA30552"
	Funcs        []Replacement
	VTableChecks []VTableCheck
	Params       []ReplParam
}

// ReplReport is what ApplyReplacements did, for the log and the manifest.
type ReplReport struct {
	Set      string            `json:"set"`
	Bias     uint64            `json:"bias"`
	Replaced []ReplReportEntry `json:"replaced"`
	Params   map[string]uint64 `json:"params,omitempty"`
	// Unverified counts replacements without a digest (tables loaded from a file).
	Unverified int `json:"unverified,omitempty"`
}

// ReplReportEntry is one replaced function in the manifest (guest addresses, biased).
type ReplReportEntry struct {
	Guest uint64 `json:"guest"`
	Name  string `json:"name"`
	Shim  string `json:"shim"`
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
	if err := set.checkImage(e.elf); err != nil {
		return nil, err
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
		if f.SHA256 == "" {
			rep.Unverified++
		}
		if !fragment && !known[g] {
			return nil, fmt.Errorf("replacement %s: 0x%x is not an entry of a discovered function; its callers would not link", f.Name, g)
		}
		rep.Replaced = append(rep.Replaced, ReplReportEntry{Guest: g, Name: f.Name, Shim: f.Shim})
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
	if len(e.replParams) == 0 {
		return "", nil
	}
	keys := make([]string, 0, len(e.replParams))
	for k := range e.replParams {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	var b bytes.Buffer
	b.WriteString("// Generated by the translator: values for the host side of the function-level replacements.\n")
	b.WriteString("#include \"nps5_funcrepl.h\"\n\nconst Nps5ReplParam nps5_repl_params[] = {\n")
	for _, k := range keys {
		fmt.Fprintf(&b, "    {%q, 0x%xULL},\n", k, e.replParams[k])
	}
	b.WriteString("    {0, 0}\n};\n")
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

func parseAddr(s string) (uint64, error) {
	v, err := strconv.ParseUint(strings.TrimSpace(s), 0, 64)
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
// Addresses are link-time addresses. sha256 is optional; a function without one is not verified
// (the report says how many).
func LoadReplacementFile(path string) (*ReplacementSet, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	dec := json.NewDecoder(bytes.NewReader(raw))
	dec.DisallowUnknownFields()
	var f replFile
	if err := dec.Decode(&f); err != nil {
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
		if err1 != nil || err2 != nil {
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
	return set, nil
}
