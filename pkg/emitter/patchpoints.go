package emitter

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
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
// An operation that rewrites data flow needs its own lowering and is refused for now.
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
}

// PatchSite is one patched instruction.
type PatchSite struct {
	Addr  string `json:"addr"`  // link-time address, hex
	Bytes string `json:"bytes"` // the instruction this build has there, hex
	Op    string `json:"op"`    // branch-always | branch-never
	Note  string `json:"note,omitempty"`
	// Identity of the function that contains the site, as in the -hle-replace tables: link-time
	// address, byte size and hex SHA-256 of the image bytes [func, func+func_size).
	Func       string `json:"func"`
	FuncSize   uint64 `json:"func_size"`
	FuncSHA256 string `json:"func_sha256"`
}

// Patch site operations.
const (
	PatchBranchAlways = "branch-always"
	PatchBranchNever  = "branch-never"
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
	guest  uint64
	slot   int
	always bool
}

// patchState is what an applied profile leaves on the emitter. It is shared by the worker copies
// of the emitter; seen is the only part written during emission.
type patchState struct {
	profile string // "PPSA21564 01.007"
	patches []Patch
	sites   map[uint64]patchSiteBound
	guests  [][]uint64 // per patch, biased site addresses in profile order

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
		if p.Title == "" || p.On == "" || p.Off == "" {
			return nil, fmt.Errorf("patch %s: title, on and off texts are required", p.Key)
		}
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
			inst, err := x86asm.Decode(want, 64)
			if err != nil || inst.Len != len(want) {
				return nil, fmt.Errorf("patch %s: bytes %x at %s are not exactly one instruction", p.Key, want, s.Addr)
			}
			var always bool
			switch s.Op {
			case PatchBranchAlways:
				always = true
			case PatchBranchNever:
			default:
				return nil, fmt.Errorf("patch %s: operation %q at %s (supported: %s, %s)", p.Key, s.Op, s.Addr, PatchBranchAlways, PatchBranchNever)
			}
			if !isRelCondBranch(inst) {
				return nil, fmt.Errorf("patch %s: %s at %s is not a relative conditional branch (%s)", p.Key, s.Op, s.Addr, inst.String())
			}
			if _, dup := st.sites[guest]; dup {
				return nil, fmt.Errorf("patch %s: site %s is listed twice", p.Key, s.Addr)
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
	for a, s := range st.sites {
		ls[a] = lifter.PatchSite{Slot: s.slot, Always: s.always}
	}
	e.lifter.SetPatchSites(ls)
	rep := &PatchReport{Profile: st.profile, Sites: len(st.sites)}
	for _, pt := range st.patches {
		rep.Keys = append(rep.Keys, pt.Key)
	}
	return rep, nil
}

// patchFact is what a site adds to the facts hash of its chunk.
func (s patchSiteBound) fact() []byte {
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
		fmt.Fprintf(&b, "static const uint64_t sites_%d[] = {", i)
		for k, a := range g {
			if k > 0 {
				b.WriteString(", ")
			}
			fmt.Fprintf(&b, "0x%xull", a)
		}
		b.WriteString("};\n")
	}
	b.WriteString("static const Nps5PatchDesc descs[] = {\n")
	for i, p := range s.patches {
		def := 0
		if p.Default {
			def = 1
		}
		fmt.Fprintf(&b, "  {%s, %s, %s, %s, %s, %s, %d, %d, %d, sites_%d},\n", cString(p.Key), cString(p.Title), cString(p.On), cString(p.Off),
			cString(p.Description), cString(p.Env), i, def, len(s.guests[i]), i)
	}
	b.WriteString("};\n")
	fmt.Fprintf(&b, "__attribute__((constructor)) static void nps5_patches_ctor(void) { nps5_patches_register(1, %s, descs, %d, nps5_guest_patch_flags, %d); }\n",
		cString(s.profile), len(s.patches), MaxPatches)
	return b.String()
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
