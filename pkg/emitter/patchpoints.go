package emitter

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"

	"ps4-recomp/pkg/lifter"

	"golang.org/x/arch/x86/x86asm"
)

// Game patches as run-time settings ("patch points").
//
// A patch profile (-patches <file>, e.g. profiles/patches/PPSA21564.json) lists, per build of a
// title, named patches: each is one user setting and a set of guest instruction sites. A site
// gives the link-time address, the instruction bytes this build must have there and the address,
// size and SHA-256 of the function that contains it (all asserted against the loaded image: a
// profile never applies to a build it was not written for; a site without the function identity
// is refused) and what the patch does to that instruction. The translator does not bake the patched instruction in.
// It lowers the site so that it tests one byte of nps5_guest_patch_flags (the setting's slot) and
// behaves as the patched or as the original instruction: the code pages never change after the
// link, and the same binary serves both values of the setting.
//
// Next to the code the translator writes nps5_patches.c: the flag bytes (initialised to the
// profile defaults, so a binary linked with a runtime that knows nothing of patches still runs
// with them) and a table (key, texts, default, environment variable, sites) registered with the
// runtime before main. The runtime (core/nps5_patches.c, libSceSystemService/ps4_settings.m)
// resolves each setting once before the guest starts: profile default, then "patch.<key>=0|1" of
// the per-game settings file (and its dialog), then the environment (the profile's own variable,
// or NPS5_PATCH_<KEY>).
//
// Site operations: "branch-always" and "branch-never" on a relative conditional branch (the
// classic "jcc -> jmp" and "jcc -> nop" byte patches). Both keep the function's control-flow
// graph a subset of the one the analysis saw, so flag liveness and block structure stay valid.
// The only data-flow operations are the constant loads of a choice setting (below); anything
// else is refused.
//
// A CHOICE setting (a patch with "options") picks one of several constants instead: its sites are
// instructions that load a constant into a register ("value": mov r64/r32, imm; "value-f32": a
// scalar single load from a rip-relative constant) and list one value per option. The lowering is
// the ordinary one followed by a load of the register from a 64-bit slot of
// nps5_guest_patch_values (lifter/arm64_lifter_patchvalue.go); the slot starts as the default
// option's value and the runtime writes the chosen one before the guest starts. One option's
// value must be the instruction's own constant (that option is the game's behaviour). An option
// may also name host environment variables it implies ("setenv": applied by the runtime unless
// the user's environment already has the variable), so that one setting can switch a guest
// constant and a host feature together (ASTRO BOT "Upscaling": render size + MetalFX).
//
// LIVE choice sites (operations "override" and "data32") are for a setting the runtime changes
// while the game runs (ASTRO BOT "Render scale": nps5_rscale.c picks one of the game's own
// dynamic-resolution pipeline sets every frame). An override site is a 32-bit register move
// ("mov r32, r32" / "mov r32, m32"); after its ordinary lowering the register is replaced by the
// slot's low 32 bits unless the slot is "keep" (bit 63 set; profile value "keep") or, with
// keep_negative, the instruction's own result is negative. The code reads the slot every time it
// runs, so the runtime may rewrite it at any moment. A data32 site is a 32-bit word of the image
// (a constant table that must agree with the setting): no code changes; the runtime writes the
// option's value into the guest word when the setting first needs it (the slot holds that value)
// and the profile asserts the word's bytes and the SHA-256 of a range of the image around it
// ("func"/"func_size"/"func_sha256" name that range). Each option of a live setting must have the
// game's behaviour at one option at least: "keep" (or the build's own word) for these sites.
//
// Only the chunks that hold a site are lifted again when a profile is added, changed or
// removed: the site enters the facts hash of its chunk, nothing else.

// PatchProfile is the content of a patch profile file.
type PatchProfile struct {
	TitleID string       `json:"title_id"`
	Name    string       `json:"name,omitempty"`
	Builds  []PatchBuild `json:"builds"`
}

// PatchBuild is the patch set of one build (content version) of the title.
type PatchBuild struct {
	Version string  `json:"version"` // required; for people and logs, the asserted function hashes select the build
	Patches []Patch `json:"patches"`
}

// Patch is one setting.
type Patch struct {
	Key         string      `json:"key"`           // settings key: [a-z0-9_]+ ("patch.<key>" in the settings file)
	Title       string      `json:"title"`         // what the setting is about ("Lighting")
	On          string      `json:"on"`            // text of the patched choice ("Fast (non-tiled)")
	Off         string      `json:"off"`           // text of the game's own behaviour ("Full quality (tiled)")
	Description string      `json:"description"`   // one or two sentences for the settings UI and the log
	Default     bool        `json:"default"`       // patched by default
	Env         string      `json:"env,omitempty"` // environment override (0|1); NPS5_PATCH_<KEY> always works
	Sites       []PatchSite `json:"sites"`
	// A choice setting: two to MaxPatchOptions options, value sites only. On, Off and Default
	// stay empty; DefaultOption names the default (the first option when empty).
	Options       []PatchOption `json:"options,omitempty"`
	DefaultOption string        `json:"default_option,omitempty"`
}

// PatchOption is one option of a choice setting.
type PatchOption struct {
	Name   string            `json:"name"`             // value in the settings file and the environment: [a-z0-9_]+
	Label  string            `json:"label"`            // text in the settings UI and the log
	Setenv map[string]string `json:"setenv,omitempty"` // host environment the option implies (the user's environment wins)
}

// PatchSite is one patched instruction.
type PatchSite struct {
	Addr  string `json:"addr"`  // link-time address, hex
	Bytes string `json:"bytes"` // the instruction this build has there, hex
	Op    string `json:"op"`    // branch-always | branch-never | value | value-f32
	Note  string `json:"note,omitempty"`
	// Identity of the function that contains the site, as in the -hle-replace tables: link-time
	// address, byte size and hex SHA-256 of the image bytes [func, func+func_size).
	Func       string `json:"func"`
	FuncSize   uint64 `json:"func_size"`
	FuncSHA256 string `json:"func_sha256"`
	// value / value-f32: the constant per option, in option order. value: an integer (decimal or
	// 0x hex); value-f32: a decimal float. override / data32: "keep" or a 32-bit unsigned integer.
	Values []string `json:"values,omitempty"`
	// override: the instruction's own result stays when it is negative (bit 31 set).
	KeepNegative bool `json:"keep_negative,omitempty"`
}

// Patch site operations.
const (
	PatchBranchAlways = "branch-always"
	PatchBranchNever  = "branch-never"
	PatchValue        = "value"
	PatchValueF32     = "value-f32"
	PatchOverride     = "override"
	PatchData32       = "data32"
)

// MaxPatchOptions is the number of options of one choice setting, MaxPatchValues the number of
// value slots of one title (NPS5_CHOICE_OPTIONS_MAX / NPS5_PATCH_VALUES_MAX in core/nps5_patches.h).
const (
	MaxPatchOptions = 8
	MaxPatchValues  = 64
)

// MaxPatches is the number of flag bytes (settings) one title may have; the runtime's table has
// the same bound (NPS5_PATCH_MAX in core/nps5_patches.h).
const MaxPatches = 32

var patchKeyRE = regexp.MustCompile(`^[a-z0-9_]{1,40}$`)
var patchEnvRE = regexp.MustCompile(`^[A-Z][A-Z0-9_]{0,63}$`)

// LoadPatchProfile reads a patch profile file.
func LoadPatchProfile(path string) (*PatchProfile, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var p PatchProfile
	if err := decodeStrictJSON(b, &p); err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	if strings.TrimSpace(p.TitleID) == "" {
		return nil, fmt.Errorf("%s: title_id is required", path)
	}
	if len(p.Builds) == 0 {
		return nil, fmt.Errorf("%s: no builds", path)
	}
	return &p, nil
}

// decodeStrictJSON decodes exactly one JSON value into v: unknown fields and any data after the
// value are errors.
func decodeStrictJSON(raw []byte, v any) error {
	dec := json.NewDecoder(bytes.NewReader(raw))
	dec.DisallowUnknownFields()
	if err := dec.Decode(v); err != nil {
		return err
	}
	if _, err := dec.Token(); err != io.EOF {
		return fmt.Errorf("unexpected data after the JSON value")
	}
	return nil
}

// maxLinkAddr bounds the addresses a profile may name (the canonical 48-bit user space).
const maxLinkAddr = uint64(1) << 48

// biasedAddr parses a link-time hex address and returns it with the module bias added; an address
// outside the 48-bit space or a sum that wraps is an error.
func biasedAddr(s string, bias uint64) (link, guest uint64, err error) {
	link, err = parseHexAddr(s)
	if err != nil {
		return 0, 0, err
	}
	if link >= maxLinkAddr || link+bias < link || link+bias >= maxLinkAddr {
		return 0, 0, fmt.Errorf("address %s is outside the 48-bit address space", s)
	}
	return link, link + bias, nil
}

// patchSiteBound is a site resolved against the loaded image.
type patchSiteBound struct {
	guest   uint64
	slot    int // branch site: flag byte; value site: value slot
	always  bool
	value   bool // a value site
	kind    lifter.PatchValueKind
	vals    []uint64 // value site: the slot's content per option
	keepNeg bool     // override site: keep a negative own result
	own     uint64   // data32 site: the build's word (the runtime checks it before writing)
}

// symbol is the host symbol the lowering of the site must mention.
func (s patchSiteBound) symbol() string {
	if s.value {
		return lifter.PatchValuesSymbol
	}
	return lifter.PatchFlagsSymbol
}

// patchState is what an applied profile leaves on the emitter. It is shared by the worker copies
// of the emitter; seen is the only part written during emission.
type patchState struct {
	profile string // "PPSA21564 01.007"
	patches []Patch
	sites   map[uint64]patchSiteBound
	guests  [][]uint64 // per patch, biased site addresses in profile order
	defOpt  []int      // per patch: the default option of a choice setting (0 otherwise)
	nvalues int        // value slots in use

	mu   sync.Mutex
	seen map[uint64]bool
}

func (s *patchState) markSeen(addr uint64) {
	s.mu.Lock()
	s.seen[addr] = true
	s.mu.Unlock()
}

// PatchReport is what ApplyPatchProfile did, for the log.
type PatchReport struct {
	Profile string
	Keys    []string
	Sites   int
}

func parseHexAddr(s string) (uint64, error) {
	return strconv.ParseUint(strings.TrimPrefix(strings.ToLower(strings.TrimSpace(s)), "0x"), 16, 64)
}

// bindPatchBuild checks one build of a profile against the image and resolves its sites. The
// error says which assertion failed.
func (e *AsmEmitter) bindPatchBuild(b *PatchBuild) (*patchState, error) {
	if strings.TrimSpace(b.Version) == "" {
		return nil, fmt.Errorf("version is required")
	}
	if len(b.Patches) == 0 {
		return nil, fmt.Errorf("no patches")
	}
	if len(b.Patches) > MaxPatches {
		return nil, fmt.Errorf("%d patches, at most %d", len(b.Patches), MaxPatches)
	}
	st := &patchState{patches: b.Patches, sites: map[uint64]patchSiteBound{}, seen: map[uint64]bool{}}
	keys := map[string]bool{}
	for slot, p := range b.Patches {
		if !patchKeyRE.MatchString(p.Key) {
			return nil, fmt.Errorf("patch key %q: want [a-z0-9_]{1,40}", p.Key)
		}
		if keys[p.Key] {
			return nil, fmt.Errorf("patch key %q twice", p.Key)
		}
		keys[p.Key] = true
		if p.Env != "" && !patchEnvRE.MatchString(p.Env) {
			return nil, fmt.Errorf("patch %s: environment variable %q", p.Key, p.Env)
		}
		choice := len(p.Options) != 0
		defOpt := 0
		if choice {
			var err error
			if defOpt, err = checkPatchOptions(&p); err != nil {
				return nil, fmt.Errorf("patch %s: %v", p.Key, err)
			}
		} else if p.Title == "" || p.On == "" || p.Off == "" {
			return nil, fmt.Errorf("patch %s: title, on and off texts are required", p.Key)
		} else if p.DefaultOption != "" {
			return nil, fmt.Errorf("patch %s: default_option without options", p.Key)
		}
		st.defOpt = append(st.defOpt, defOpt)
		if len(p.Sites) == 0 {
			return nil, fmt.Errorf("patch %s: no sites", p.Key)
		}
		var guests []uint64
		for _, s := range p.Sites {
			_, guest, err := biasedAddr(s.Addr, e.elf.Bias)
			if err != nil {
				return nil, fmt.Errorf("patch %s: address %q: %v", p.Key, s.Addr, err)
			}
			if err := e.checkSiteFunction(&s, guest); err != nil {
				return nil, fmt.Errorf("patch %s: %w", p.Key, err)
			}
			want, err := hex.DecodeString(strings.ReplaceAll(s.Bytes, " ", ""))
			if err != nil || len(want) == 0 {
				return nil, fmt.Errorf("patch %s: bytes %q at %s", p.Key, s.Bytes, s.Addr)
			}
			got := e.elf.MemoryImage.Bytes(guest, uint64(len(want)))
			if got == nil || !bytes.Equal(got, want) {
				return nil, fmt.Errorf("patch %s: the image has %x at %s, the profile expects %x", p.Key, got, s.Addr, want)
			}
			if _, dup := st.sites[guest]; dup {
				return nil, fmt.Errorf("patch %s: site %s is listed twice", p.Key, s.Addr)
			}
			if s.KeepNegative && s.Op != PatchOverride {
				return nil, fmt.Errorf("patch %s: keep_negative at %s is for an override site", p.Key, s.Addr)
			}
			if s.Op == PatchData32 {
				if !choice {
					return nil, fmt.Errorf("patch %s: %s at %s needs a choice setting (options)", p.Key, s.Op, s.Addr)
				}
				if st.nvalues >= MaxPatchValues {
					return nil, fmt.Errorf("patch %s: more than %d value sites", p.Key, MaxPatchValues)
				}
				if len(want) != 4 || guest%4 != 0 {
					return nil, fmt.Errorf("patch %s: data32 at %s: want 4 bytes at a 4-aligned address", p.Key, s.Addr)
				}
				own := uint64(want[0]) | uint64(want[1])<<8 | uint64(want[2])<<16 | uint64(want[3])<<24
				vals, err := liveValues(&p, s, own)
				if err != nil {
					return nil, fmt.Errorf("patch %s: %s at %s: %v", p.Key, s.Op, s.Addr, err)
				}
				st.sites[guest] = patchSiteBound{guest: guest, value: true, kind: lifter.PatchValueData32, vals: vals, slot: st.nvalues, own: own}
				st.seen[guest] = true // no instruction: nothing to lower
				st.nvalues++
				guests = append(guests, guest)
				continue
			}
			inst, err := x86asm.Decode(want, 64)
			if err != nil || inst.Len != len(want) {
				return nil, fmt.Errorf("patch %s: bytes %x at %s are not exactly one instruction", p.Key, want, s.Addr)
			}
			if s.Op == PatchValue || s.Op == PatchValueF32 || s.Op == PatchOverride {
				if !choice {
					return nil, fmt.Errorf("patch %s: %s at %s needs a choice setting (options)", p.Key, s.Op, s.Addr)
				}
				if st.nvalues >= MaxPatchValues {
					return nil, fmt.Errorf("patch %s: more than %d value sites", p.Key, MaxPatchValues)
				}
				b, err := e.bindValueSite(&p, s, inst, guest)
				if err != nil {
					return nil, fmt.Errorf("patch %s: %s at %s: %v", p.Key, s.Op, s.Addr, err)
				}
				b.slot = st.nvalues
				st.nvalues++
				st.sites[guest] = b
				guests = append(guests, guest)
				continue
			}
			var always bool
			switch s.Op {
			case PatchBranchAlways:
				always = true
			case PatchBranchNever:
			default:
				return nil, fmt.Errorf("patch %s: operation %q at %s (supported: %s, %s, %s, %s, %s, %s)", p.Key, s.Op, s.Addr, PatchBranchAlways, PatchBranchNever,
					PatchValue, PatchValueF32, PatchOverride, PatchData32)
			}
			if choice {
				return nil, fmt.Errorf("patch %s: %s at %s: a choice setting takes value sites only", p.Key, s.Op, s.Addr)
			}
			if len(s.Values) != 0 {
				return nil, fmt.Errorf("patch %s: %s at %s takes no values", p.Key, s.Op, s.Addr)
			}
			if !isRelCondBranch(inst) {
				return nil, fmt.Errorf("patch %s: %s at %s is not a relative conditional branch (%s)", p.Key, s.Op, s.Addr, inst.String())
			}
			st.sites[guest] = patchSiteBound{guest: guest, slot: slot, always: always}
			guests = append(guests, guest)
		}
		st.guests = append(st.guests, guests)
	}
	return st, nil
}

// checkSiteFunction verifies the identity of the function that contains the site at guest: it
// starts at s.Func, spans s.FuncSize bytes, includes the site, and its image bytes have the
// SHA-256 s.FuncSHA256. Without that identity a profile could bind to another build that happens
// to have the same instruction at the same address.
func (e *AsmEmitter) checkSiteFunction(s *PatchSite, guest uint64) error {
	if s.Func == "" || s.FuncSize == 0 || s.FuncSHA256 == "" {
		return fmt.Errorf("site %s: func, func_size and func_sha256 are required", s.Addr)
	}
	want, err := hex.DecodeString(s.FuncSHA256)
	if err != nil || len(want) != sha256.Size {
		return fmt.Errorf("site %s: func_sha256 %q is not a hex SHA-256", s.Addr, s.FuncSHA256)
	}
	_, fn, err := biasedAddr(s.Func, e.elf.Bias)
	if err != nil {
		return fmt.Errorf("site %s: func %q: %v", s.Addr, s.Func, err)
	}
	if s.FuncSize >= maxLinkAddr || fn+s.FuncSize < fn {
		return fmt.Errorf("site %s: func_size %d is out of range", s.Addr, s.FuncSize)
	}
	if guest < fn || guest >= fn+s.FuncSize {
		return fmt.Errorf("site %s is not inside the function %s (+%d)", s.Addr, s.Func, s.FuncSize)
	}
	b := e.elf.MemoryImage.Bytes(fn, s.FuncSize)
	if b == nil {
		return fmt.Errorf("site %s: the function %s (+%d) is not in the image", s.Addr, s.Func, s.FuncSize)
	}
	if got := sha256.Sum256(b); !bytes.Equal(got[:], want) {
		return fmt.Errorf("site %s: the function at %s is not the one this profile was made for (sha256 %x, want %.12s): a different build of the title", s.Addr, s.Func, got[:6], s.FuncSHA256)
	}
	return nil
}

var patchSetenvValRE = regexp.MustCompile(`^[ -~]{0,200}$`)

// checkPatchOptions validates the options of a choice setting and returns its default option.
func checkPatchOptions(p *Patch) (int, error) {
	if p.Title == "" {
		return 0, fmt.Errorf("a title is required")
	}
	if p.On != "" || p.Off != "" || p.Default {
		return 0, fmt.Errorf("a choice setting has options, not on/off/default (use default_option)")
	}
	if len(p.Options) < 2 || len(p.Options) > MaxPatchOptions {
		return 0, fmt.Errorf("%d options, want 2 to %d", len(p.Options), MaxPatchOptions)
	}
	def, names := -1, map[string]bool{}
	for i, o := range p.Options {
		if !patchKeyRE.MatchString(o.Name) {
			return 0, fmt.Errorf("option name %q: want [a-z0-9_]{1,40}", o.Name)
		}
		if _, err := strconv.Atoi(o.Name); err == nil {
			return 0, fmt.Errorf("option name %q is a number (numbers select an option by position)", o.Name)
		}
		if names[o.Name] {
			return 0, fmt.Errorf("option %q twice", o.Name)
		}
		names[o.Name] = true
		if o.Label == "" {
			return 0, fmt.Errorf("option %s: a label is required", o.Name)
		}
		for k, v := range o.Setenv {
			if !patchEnvRE.MatchString(k) || !patchSetenvValRE.MatchString(v) {
				return 0, fmt.Errorf("option %s: setenv %q=%q", o.Name, k, v)
			}
		}
		if o.Name == p.DefaultOption {
			def = i
		}
	}
	if p.DefaultOption == "" {
		def = 0
	}
	if def < 0 {
		return 0, fmt.Errorf("default_option %q is not an option", p.DefaultOption)
	}
	return def, nil
}

// bindValueSite resolves a value site: the instruction shape, one value per option, and one of
// them equal to the constant the instruction has in this build.
func (e *AsmEmitter) bindValueSite(p *Patch, s PatchSite, inst x86asm.Inst, guest uint64) (patchSiteBound, error) {
	b := patchSiteBound{guest: guest, value: true}
	if len(s.Values) != len(p.Options) {
		return b, fmt.Errorf("%d values for %d options", len(s.Values), len(p.Options))
	}
	var own uint64
	if s.Op == PatchOverride {
		b.kind = lifter.PatchValueOverride
		b.keepNeg = s.KeepNegative
		if _, _, err := lifter.PatchValueTarget(inst, b.kind); err != nil {
			return b, err
		}
		vals, err := liveValues(p, s, lifter.PatchOverrideKeep)
		if err != nil {
			return b, err
		}
		b.vals = vals
		return b, nil
	}
	if s.Op == PatchValueF32 {
		b.kind = lifter.PatchValueF32
		if _, _, err := lifter.PatchValueTarget(inst, b.kind); err != nil {
			return b, err
		}
		mem := inst.Args[1].(x86asm.Mem)
		// rip-relative: Base RIP, or (x86asm's VEX decoder) no base, no index and no SIB scale,
		// as lifter.computeEffAddr reads it.
		ripRel := mem.Base == x86asm.RIP || (mem.Base == 0 && mem.Scale == 0)
		if !ripRel || mem.Index != 0 || mem.Segment != 0 {
			return b, fmt.Errorf("the constant is not rip-relative (%s)", inst.String())
		}
		at := uint64(int64(guest) + int64(inst.Len) + int64(int32(mem.Disp)))
		c := e.elf.MemoryImage.Bytes(at, 4)
		if c == nil {
			return b, fmt.Errorf("the constant at 0x%x is not in the image", at)
		}
		own = uint64(c[0]) | uint64(c[1])<<8 | uint64(c[2])<<16 | uint64(c[3])<<24
		for _, v := range s.Values {
			f, err := strconv.ParseFloat(strings.TrimSpace(v), 32)
			if err != nil || math.IsNaN(f) || math.IsInf(f, 0) {
				return b, fmt.Errorf("value %q is not a float", v)
			}
			b.vals = append(b.vals, uint64(math.Float32bits(float32(f))))
		}
	} else {
		b.kind = lifter.PatchValueImm
		reg, imm, err := lifter.PatchValueTarget(inst, b.kind)
		if err != nil {
			return b, err
		}
		own = imm
		for _, v := range s.Values {
			n, err := strconv.ParseUint(strings.TrimSpace(v), 0, 64)
			if err != nil {
				return b, fmt.Errorf("value %q is not an unsigned integer", v)
			}
			if strings.HasPrefix(reg, "w") && n > math.MaxUint32 {
				return b, fmt.Errorf("value %q does not fit the 32-bit destination", v)
			}
			b.vals = append(b.vals, n)
		}
	}
	for _, v := range b.vals {
		if v == own {
			return b, nil
		}
	}
	return b, fmt.Errorf("no option has the instruction's own constant 0x%x (%s)", own, inst.String())
}

// liveValues parses the per-option values of an override or data32 site: "keep" (the slot gets
// lifter.PatchOverrideKeep) or a 32-bit unsigned integer. One option must keep the game's
// behaviour: "keep", or (data32) the build's own word own.
func liveValues(p *Patch, s PatchSite, own uint64) ([]uint64, error) {
	if len(s.Values) != len(p.Options) {
		return nil, fmt.Errorf("%d values for %d options", len(s.Values), len(p.Options))
	}
	var vals []uint64
	game := false
	for _, v := range s.Values {
		v = strings.TrimSpace(v)
		if v == "keep" {
			vals = append(vals, lifter.PatchOverrideKeep)
			game = true
			continue
		}
		n, err := strconv.ParseUint(v, 0, 64)
		if err != nil || n > math.MaxUint32 {
			return nil, fmt.Errorf("value %q is neither \"keep\" nor a 32-bit unsigned integer", v)
		}
		game = game || n == own
		vals = append(vals, n)
	}
	if !game {
		return nil, fmt.Errorf("no option keeps the game's value (\"keep\")")
	}
	return vals, nil
}

func isRelCondBranch(inst x86asm.Inst) bool {
	switch inst.Op {
	case x86asm.JA, x86asm.JAE, x86asm.JB, x86asm.JBE, x86asm.JE, x86asm.JG, x86asm.JGE, x86asm.JL, x86asm.JLE,
		x86asm.JNE, x86asm.JNO, x86asm.JNP, x86asm.JNS, x86asm.JO, x86asm.JP, x86asm.JS:
		_, ok := inst.Args[0].(x86asm.Rel)
		return ok
	}
	return false
}

// ApplyPatchProfile binds the build of p that matches the loaded image: every site of every patch
// of that build has the asserted bytes. No matching build is an error (the user asked for the
// patches; a title translated without them is not the one they meant to run), and so are two.
func (e *AsmEmitter) ApplyPatchProfile(p *PatchProfile) (*PatchReport, error) {
	if e.elf == nil {
		return nil, fmt.Errorf("patch profile: no image")
	}
	if strings.TrimSpace(p.TitleID) == "" {
		return nil, fmt.Errorf("patch profile: title_id is required")
	}
	var st *patchState
	var why []string
	for i := range p.Builds {
		b := &p.Builds[i]
		s, err := e.bindPatchBuild(b)
		if err != nil {
			why = append(why, fmt.Sprintf("build %q: %v", b.Version, err))
			continue
		}
		if st != nil {
			return nil, fmt.Errorf("patch profile %s: builds %q and %q both match the image", p.TitleID, st.profile, b.Version)
		}
		st = s
		st.profile = strings.TrimSpace(p.TitleID + " " + b.Version)
	}
	if st == nil {
		return nil, fmt.Errorf("patch profile %s matches no build of this image: %s", p.TitleID, strings.Join(why, "; "))
	}
	e.patch = st
	ls := make(map[uint64]lifter.PatchSite, len(st.sites))
	vs := make(map[uint64]lifter.PatchValueSite)
	for a, s := range st.sites {
		if s.value && s.kind == lifter.PatchValueData32 {
			continue // data, not code
		}
		if s.value {
			vs[a] = lifter.PatchValueSite{Slot: s.slot, Kind: s.kind, KeepNegative: s.keepNeg}
		} else {
			ls[a] = lifter.PatchSite{Slot: s.slot, Always: s.always}
		}
	}
	e.lifter.SetPatchSites(ls)
	e.lifter.SetPatchValueSites(vs)
	rep := &PatchReport{Profile: st.profile, Sites: len(st.sites)}
	for _, pt := range st.patches {
		rep.Keys = append(rep.Keys, pt.Key)
	}
	return rep, nil
}

// patchFact is what a site adds to the facts hash of its chunk.
func (s patchSiteBound) fact() []byte {
	if s.value && s.keepNeg {
		return []byte(fmt.Sprintf("PV%d,%d,n;", s.slot, s.kind))
	}
	if s.value {
		return []byte(fmt.Sprintf("PV%d,%d;", s.slot, s.kind))
	}
	return []byte(fmt.Sprintf("PP%d,%t;", s.slot, s.always))
}

// CheckPatchSites reports the sites no translated function contains as an instruction: such a
// patch would silently do nothing.
func (e *AsmEmitter) CheckPatchSites() error {
	if e.patch == nil {
		return nil
	}
	var missing []string
	for a := range e.patch.sites {
		if !e.patch.seen[a] {
			missing = append(missing, fmt.Sprintf("0x%x", a))
		}
	}
	if len(missing) == 0 {
		return nil
	}
	sort.Strings(missing)
	return fmt.Errorf("patch profile %s: no translated function has an instruction at guest %s", e.patch.profile, strings.Join(missing, ", "))
}

// PatchTableFile is the generated C file that carries the flags and the table.
const PatchTableFile = "nps5_patches.c"

// WritePatchTable writes nps5_patches.c into outDir and returns its path, or removes a stale one
// (and its object) and returns "" when no profile is applied.
func (e *AsmEmitter) WritePatchTable(outDir string) (string, error) {
	path := filepath.Join(outDir, PatchTableFile)
	if e.patch == nil {
		for _, p := range []string{path, strings.TrimSuffix(path, ".c") + ".o", strings.TrimSuffix(path, ".c") + ".o.flags"} {
			_ = os.Remove(p)
		}
		return "", nil
	}
	return path, writeFileIfChanged(path, []byte(e.patch.tableC()))
}

// tableC renders nps5_patches.c. The struct is spelled out (the file is compiled without the
// runtime's include path being a given) and must match Nps5PatchDesc in core/nps5_patches.h;
// the abi number passed to the registration guards that.
func (s *patchState) tableC() string {
	var b strings.Builder
	fmt.Fprintf(&b, "// Generated by ps4-recomp: game patches as run-time settings (pkg/emitter/patchpoints.go). Profile %s.\n", s.profile)
	b.WriteString("#include <stdint.h>\n")
	b.WriteString("typedef struct { const char *key, *title, *on, *off, *description, *env; uint32_t slot, def, nsites; const uint64_t *sites; } Nps5PatchDesc;\n")
	b.WriteString("void nps5_patches_register(int abi, const char *profile, const Nps5PatchDesc *descs, int n, uint8_t *flags, int nflags);\n")
	// The translated code reads these bytes; they start as the profile defaults.
	fmt.Fprintf(&b, "uint8_t nps5_guest_patch_flags[%d] = {", MaxPatches)
	for i, p := range s.patches {
		if i > 0 {
			b.WriteString(", ")
		}
		if p.Default {
			b.WriteString("1")
		} else {
			b.WriteString("0")
		}
	}
	b.WriteString("};\n")
	for i, g := range s.guests {
		if len(s.patches[i].Options) != 0 {
			continue // a choice setting's sites are in its value table
		}
		fmt.Fprintf(&b, "static const uint64_t sites_%d[] = {", i)
		for k, a := range g {
			if k > 0 {
				b.WriteString(", ")
			}
			fmt.Fprintf(&b, "0x%xull", a)
		}
		b.WriteString("};\n")
	}
	nbool, nchoice := 0, 0
	b.WriteString("static const Nps5PatchDesc descs[] = {\n")
	for i, p := range s.patches {
		if len(p.Options) != 0 {
			nchoice++
			continue
		}
		nbool++
		def := 0
		if p.Default {
			def = 1
		}
		fmt.Fprintf(&b, "  {%s, %s, %s, %s, %s, %s, %d, %d, %d, sites_%d},\n", cString(p.Key), cString(p.Title), cString(p.On), cString(p.Off),
			cString(p.Description), cString(p.Env), i, def, len(s.guests[i]), i)
	}
	if nbool == 0 {
		b.WriteString("  {0}\n")
	}
	b.WriteString("};\n")
	if nchoice != 0 {
		s.choicesC(&b)
	}
	fmt.Fprintf(&b, "__attribute__((constructor)) static void nps5_patches_ctor(void) {\n")
	if nbool != 0 {
		fmt.Fprintf(&b, "  nps5_patches_register(1, %s, descs, %d, nps5_guest_patch_flags, %d);\n", cString(s.profile), nbool, MaxPatches)
	} else {
		b.WriteString("  (void)descs;\n")
	}
	if nchoice != 0 {
		fmt.Fprintf(&b, "  nps5_patch_choices_register(1, %s, choices, %d, nps5_guest_patch_values, %d);\n", cString(s.profile), nchoice, MaxPatchValues)
	}
	b.WriteString("}\n")
	return b.String()
}

// choicesC renders the choice settings: the value slots (initialised to the default options, so
// the code runs with the defaults whatever the runtime does) and their table. The structs must
// match Nps5ChoiceValue / Nps5ChoiceDesc in core/nps5_patches.h (abi 1).
func (s *patchState) choicesC(b *strings.Builder) {
	b.WriteString("typedef struct { uint32_t slot, f32; uint64_t site; const uint64_t *values; } Nps5ChoiceValue;\n")
	b.WriteString("typedef struct { const char *key, *title, *description, *env; uint32_t noptions, def, nvalues; const char *const *names, *const *labels, *const *setenv; const Nps5ChoiceValue *values; } Nps5ChoiceDesc;\n")
	b.WriteString("void nps5_patch_choices_register(int abi, const char *profile, const Nps5ChoiceDesc *descs, int n, uint64_t *values, int nvalues);\n")
	slots := make([]uint64, s.nvalues)
	for i, p := range s.patches {
		if len(p.Options) == 0 {
			continue
		}
		list := func(name string, get func(o PatchOption) string) {
			fmt.Fprintf(b, "static const char *const %s_%d[] = {", name, i)
			for k, o := range p.Options {
				if k > 0 {
					b.WriteString(", ")
				}
				b.WriteString(cString(get(o)))
			}
			b.WriteString("};\n")
		}
		list("names", func(o PatchOption) string { return o.Name })
		list("labels", func(o PatchOption) string { return o.Label })
		list("setenv", func(o PatchOption) string {
			var kv []string
			for k, v := range o.Setenv {
				kv = append(kv, k+"="+v)
			}
			sort.Strings(kv)
			return strings.Join(kv, "\n")
		})
		for k, g := range s.guests[i] {
			site := s.sites[g]
			slots[site.slot] = site.vals[s.defOpt[i]]
			fmt.Fprintf(b, "static const uint64_t vals_%d_%d[] = {", i, k)
			for n, v := range site.vals {
				if n > 0 {
					b.WriteString(", ")
				}
				fmt.Fprintf(b, "0x%xull", v)
			}
			if site.kind == lifter.PatchValueData32 {
				fmt.Fprintf(b, ", 0x%xull", site.own) // after the options: the build's word
			}
			b.WriteString("};\n")
		}
		fmt.Fprintf(b, "static const Nps5ChoiceValue values_%d[] = {", i)
		for k, g := range s.guests[i] {
			site := s.sites[g]
			// "f32" is the site kind: 0 integer, 1 float, 2 override (live), 3 data32 (live), 4 override keep_negative.
			f32 := 0
			switch {
			case site.kind == lifter.PatchValueF32:
				f32 = 1
			case site.kind == lifter.PatchValueOverride && site.keepNeg:
				f32 = 4
			case site.kind == lifter.PatchValueOverride:
				f32 = 2
			case site.kind == lifter.PatchValueData32:
				f32 = 3
			}
			if k > 0 {
				b.WriteString(", ")
			}
			fmt.Fprintf(b, "{%d, %d, 0x%xull, vals_%d_%d}", site.slot, f32, g, i, k)
		}
		b.WriteString("};\n")
	}
	// The translated code reads these slots.
	fmt.Fprintf(b, "uint64_t nps5_guest_patch_values[%d] = {", MaxPatchValues)
	for i, v := range slots {
		if i > 0 {
			b.WriteString(", ")
		}
		fmt.Fprintf(b, "0x%xull", v)
	}
	b.WriteString("};\n")
	b.WriteString("static const Nps5ChoiceDesc choices[] = {\n")
	for i, p := range s.patches {
		if len(p.Options) == 0 {
			continue
		}
		fmt.Fprintf(b, "  {%s, %s, %s, %s, %d, %d, %d, names_%d, labels_%d, setenv_%d, values_%d},\n", cString(p.Key), cString(p.Title), cString(p.Description),
			cString(p.Env), len(p.Options), s.defOpt[i], len(s.guests[i]), i, i, i, i)
	}
	b.WriteString("};\n")
}

// cString is a C string literal of s (UTF-8 bytes above 0x7e and control characters as octal).
func cString(s string) string {
	var b strings.Builder
	b.WriteByte('"')
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c == '"' || c == '\\':
			b.WriteByte('\\')
			b.WriteByte(c)
		case c < 0x20 || c > 0x7e:
			fmt.Fprintf(&b, "\\%03o", c)
		default:
			b.WriteByte(c)
		}
	}
	b.WriteByte('"')
	return b.String()
}

func linesMention(lines []string, sym string) bool {
	for _, l := range lines {
		if strings.Contains(l, sym) {
			return true
		}
	}
	return false
}
