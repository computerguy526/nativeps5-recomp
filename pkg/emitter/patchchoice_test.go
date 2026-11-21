package emitter

import (
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"ps4-recomp/pkg/disasm"
	"ps4-recomp/pkg/elfloader"
	"ps4-recomp/pkg/lifter"

	"golang.org/x/arch/x86/x86asm"
)

// The image of patchTestEmitter plus: movabs rdx, 0x43800000780 at link 0x4000;
// vmovss xmm0, [rip+0xff8] at 0x5000 (the constant 1920.0f at 0x6000); mov ecx, 0x780 at 0x5100;
// vmovss xmm0, [rbx] at 0x5200.
func choiceTestEmitter() *AsmEmitter {
	e := replTestEmitter(choiceImg, nil, nil)
	e.lifter = lifter.NewArm64LifterMode(nil, lifter.AddrBase)
	return e
}

// The functions of choiceTestEmitter's image.
var choiceImg = map[uint64][]byte{
	0x1000: {0x45, 0x84, 0xf6, 0x0f, 0x84, 0xed, 0x08, 0x00, 0x00},
	0x2000: {0x75, 0x02},
	0x3000: {0xb8, 0x01, 0x00, 0x00, 0x00},
	0x4000: {0x48, 0xba, 0x80, 0x07, 0x00, 0x00, 0x38, 0x04, 0x00, 0x00},
	0x5000: {0xc5, 0xfa, 0x10, 0x05, 0xf8, 0x0f, 0x00, 0x00},
	0x5100: {0xb9, 0x80, 0x07, 0x00, 0x00},
	0x5200: {0xc5, 0xfa, 0x10, 0x03},
	0x6000: {0x00, 0x00, 0xf0, 0x44},
}

// applyChoice applies prof after giving every site the identity of the image function that
// contains it (func, func_size, func_sha256; the site checks of these tests come after that one).
func applyChoice(e *AsmEmitter, prof *PatchProfile, img map[uint64][]byte) (*PatchReport, error) {
	for bi := range prof.Builds {
		for pi := range prof.Builds[bi].Patches {
			sites := append([]PatchSite{}, prof.Builds[bi].Patches[pi].Sites...)
			for si, st := range sites {
				a, err := strconv.ParseUint(strings.TrimPrefix(st.Addr, "0x"), 16, 64)
				if err != nil {
					continue
				}
				for fn, fb := range img {
					if a >= fn && a < fn+uint64(len(fb)) {
						sites[si] = withFn(st, fn, fb)
					}
				}
			}
			prof.Builds[bi].Patches[pi].Sites = sites
		}
	}
	return e.ApplyPatchProfile(prof)
}

func choiceTestPatch() Patch {
	return Patch{Key: "upscaling", Title: "Upscaling", Description: "d", Env: "NPS5_TEST_UPSCALING", DefaultOption: "off",
		Options: []PatchOption{
			{Name: "off", Label: "Off (1080p)"},
			{Name: "metalfx900", Label: "MetalFX 900p", Setenv: map[string]string{"NPS5_TEST_METALFX": "spatial", "NPS5_TEST_B": "1 2"}},
			{Name: "metalfx720", Label: "MetalFX 720p", Setenv: map[string]string{"NPS5_TEST_METALFX": "temporal"}},
		},
		Sites: []PatchSite{
			{Addr: "0x5000", Bytes: "c5fa1005f80f0000", Op: PatchValueF32, Values: []string{"1920", "1600", "1280.0"}},
			{Addr: "0x4000", Bytes: "48ba8007000038040000", Op: PatchValue, Values: []string{"0x43800000780", "0x38400000640", "3092376454400"}},
			{Addr: "0x5100", Bytes: "b980070000", Op: PatchValue, Values: []string{"1920", "1600", "1280"}},
		}}
}

func choiceTestProfile(p Patch) *PatchProfile {
	return &PatchProfile{TitleID: "TEST00001", Builds: []PatchBuild{{Version: "01.007", Patches: []Patch{
		{Key: "fast_lighting", Title: "Lighting", On: "Fast", Off: "Full", Default: true, Sites: []PatchSite{{Addr: "0x1003", Bytes: "0f84ed080000", Op: PatchBranchAlways}}},
		p,
	}}}}
}

func TestPatchChoiceBinds(t *testing.T) {
	e := choiceTestEmitter()
	rep, err := applyChoice(e, choiceTestProfile(choiceTestPatch()), choiceImg)
	if err != nil {
		t.Fatal(err)
	}
	if rep.Sites != 4 || strings.Join(rep.Keys, ",") != "fast_lighting,upscaling" {
		t.Errorf("report %+v", rep)
	}
	st := e.patch
	f, m, w, br := st.sites[0x5000+replBias], st.sites[0x4000+replBias], st.sites[0x5100+replBias], st.sites[0x1003+replBias]
	if !f.value || f.kind != lifter.PatchValueF32 || f.slot != 0 || len(f.vals) != 3 || f.vals[0] != uint64(math.Float32bits(1920)) ||
		f.vals[1] != uint64(math.Float32bits(1600)) || f.vals[2] != uint64(math.Float32bits(1280)) {
		t.Errorf("float site %+v", f)
	}
	if !m.value || m.kind != lifter.PatchValueImm || m.slot != 1 || m.vals[0] != 0x43800000780 || m.vals[1] != 0x38400000640 || m.vals[2] != 0x2d000000500 {
		t.Errorf("movabs site %+v", m)
	}
	if !w.value || w.slot != 2 || w.vals[2] != 1280 || st.nvalues != 3 {
		t.Errorf("mov r32 site %+v, %d slots", w, st.nvalues)
	}
	if br.value || br.slot != 0 || !br.always {
		t.Errorf("branch site %+v", br)
	}
	if st.defOpt[0] != 0 || st.defOpt[1] != 0 {
		t.Errorf("default options %v", st.defOpt)
	}
	// Facts: the slot and the kind; a value site and a branch site of the same slot differ.
	if string(f.fact()) != "PV0,1;" || string(m.fact()) != "PV1,0;" || string(br.fact()) != "PP0,true;" {
		t.Errorf("facts %q %q %q", f.fact(), m.fact(), br.fact())
	}
	if f.symbol() != lifter.PatchValuesSymbol || br.symbol() != lifter.PatchFlagsSymbol {
		t.Errorf("symbols %q %q", f.symbol(), br.symbol())
	}
	// The lifter got the value sites: the lowering of the movabs ends with the slot load.
	inst, err := disasm.Decode([]byte{0x48, 0xba, 0x80, 0x07, 0x00, 0x00, 0x38, 0x04, 0x00, 0x00})
	if err != nil {
		t.Fatal(err)
	}
	lines, err := e.lifter.LiftInstructionToBuf(disasm.Instruction{Address: 0x4000 + replBias, Inst: inst}, 0x400a+replBias, nil, nil)
	if err != nil || !linesMention(lines, lifter.PatchValuesSymbol) || lines[len(lines)-1] != "    ldr x22, [x16, #8]" {
		t.Errorf("lowering of the site: %v\n%s", err, strings.Join(lines, "\n"))
	}
	// The default option decides the slots' initial content.
	p := choiceTestPatch()
	p.DefaultOption = "metalfx720"
	e2 := choiceTestEmitter()
	if _, err := applyChoice(e2, choiceTestProfile(p), choiceImg); err != nil {
		t.Fatal(err)
	}
	c := e2.patch.tableC()
	if !strings.Contains(c, "uint64_t nps5_guest_patch_values[64] = {0x44a00000ull, 0x2d000000500ull, 0x500ull};") {
		t.Errorf("initial slots of default metalfx720:\n%s", c)
	}
	// With the first option as the default (no default_option) the slots are the game's constants.
	p.DefaultOption = ""
	e3 := choiceTestEmitter()
	if _, err := applyChoice(e3, choiceTestProfile(p), choiceImg); err != nil {
		t.Fatal(err)
	}
	if c := e3.patch.tableC(); !strings.Contains(c, "nps5_guest_patch_values[64] = {0x44f00000ull, 0x43800000780ull, 0x780ull};") {
		t.Errorf("initial slots of the first option:\n%s", c)
	}
}

func TestPatchChoiceRefusals(t *testing.T) {
	cases := []struct {
		name string
		mut  func(p *Patch)
		want string
	}{
		{"one option", func(p *Patch) { p.Options = p.Options[:1] }, "want 2 to 8"},
		{"nine options", func(p *Patch) {
			for len(p.Options) < 9 {
				p.Options = append(p.Options, PatchOption{Name: "o" + string(rune('a'+len(p.Options))), Label: "x"})
			}
		}, "want 2 to 8"},
		{"option name", func(p *Patch) { p.Options[1].Name = "MetalFX 900" }, "option name"},
		{"numeric option name", func(p *Patch) { p.Options[1].Name = "900" }, "is a number"},
		{"option twice", func(p *Patch) { p.Options[2].Name = "off" }, "twice"},
		{"no label", func(p *Patch) { p.Options[2].Label = "" }, "label is required"},
		{"no title", func(p *Patch) { p.Title = "" }, "title is required"},
		{"on text", func(p *Patch) { p.On = "On" }, "not on/off/default"},
		{"bool default", func(p *Patch) { p.Default = true }, "not on/off/default"},
		{"default option", func(p *Patch) { p.DefaultOption = "metalfx1080" }, "is not an option"},
		{"setenv key", func(p *Patch) { p.Options[1].Setenv = map[string]string{"bad key": "1"} }, "setenv"},
		{"setenv value", func(p *Patch) { p.Options[1].Setenv = map[string]string{"A": "x\ny"} }, "setenv"},
		{"value count", func(p *Patch) { p.Sites[0].Values = p.Sites[0].Values[:2] }, "2 values for 3 options"},
		{"no values", func(p *Patch) { p.Sites[1].Values = nil }, "0 values for 3 options"},
		{"float text", func(p *Patch) { p.Sites[0].Values[1] = "wide" }, "not a float"},
		{"float inf", func(p *Patch) { p.Sites[0].Values[1] = "1e99" }, "not a float"},
		{"integer text", func(p *Patch) { p.Sites[1].Values[1] = "-5" }, "not an unsigned integer"},
		{"own float missing", func(p *Patch) { p.Sites[0].Values[0] = "1919" }, "no option has the instruction's own constant 0x44f00000"},
		{"own integer missing", func(p *Patch) { p.Sites[1].Values[0] = "0x43800000781" }, "no option has the instruction's own constant 0x43800000780"},
		{"32-bit overflow", func(p *Patch) { p.Sites[2].Values[1] = "0x100000000" }, "does not fit the 32-bit destination"},
		{"float op on a mov", func(p *Patch) { p.Sites[1].Op = PatchValueF32 }, "not a scalar single load"},
		{"integer op on a float load", func(p *Patch) { p.Sites[0].Op = PatchValue }, "is not a mov"},
		{"constant not rip-relative", func(p *Patch) { p.Sites[0].Addr, p.Sites[0].Bytes = "0x5200", "c5fa1003" }, "not rip-relative"},
		{"value on a branch", func(p *Patch) { p.Sites[2].Addr, p.Sites[2].Bytes = "0x2000", "7502" }, "is not a mov"},
		{"branch op in a choice", func(p *Patch) {
			p.Sites[2] = PatchSite{Addr: "0x2000", Bytes: "7502", Op: PatchBranchNever}
		}, "takes value sites only"},
		{"bytes differ", func(p *Patch) { p.Sites[1].Bytes = "48ba8007000038040001" }, "the profile expects"},
		{"site twice", func(p *Patch) { p.Sites[2] = p.Sites[1] }, "listed twice"},
		{"no sites", func(p *Patch) { p.Sites = nil }, "no sites"},
		{"unknown op", func(p *Patch) { p.Sites[1].Op = "nop" }, "supported: branch-always, branch-never, value, value-f32"},
	}
	for _, c := range cases {
		p := choiceTestPatch()
		p.Sites = append([]PatchSite{}, p.Sites...)
		for i := range p.Sites {
			p.Sites[i].Values = append([]string{}, p.Sites[i].Values...)
		}
		p.Options = append([]PatchOption{}, p.Options...)
		c.mut(&p)
		if _, err := applyChoice(choiceTestEmitter(), choiceTestProfile(p), choiceImg); err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: %v, want %q", c.name, err, c.want)
		}
	}
	// On a setting without options: value sites, values on a branch site, default_option.
	bools := []struct {
		name string
		mut  func(p *Patch)
		want string
	}{
		{"value site", func(p *Patch) {
			p.Sites = []PatchSite{{Addr: "0x4000", Bytes: "48ba8007000038040000", Op: PatchValue, Values: []string{"1", "2"}}}
		}, "needs a choice setting"},
		{"values on a branch", func(p *Patch) { p.Sites[0].Values = []string{"1"} }, "takes no values"},
		{"default_option", func(p *Patch) { p.DefaultOption = "on" }, "default_option without options"},
	}
	for _, c := range bools {
		prof := choiceTestProfile(choiceTestPatch())
		c.mut(&prof.Builds[0].Patches[0])
		if _, err := applyChoice(choiceTestEmitter(), prof, choiceImg); err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: %v, want %q", c.name, err, c.want)
		}
	}
	// More value sites than slots.
	p := choiceTestPatch()
	img := map[uint64][]byte{}
	p.Sites = nil
	for i := 0; i <= MaxPatchValues; i++ {
		a := uint64(0x10000 + 0x10*i)
		img[a] = []byte{0xb9, 0x80, 0x07, 0x00, 0x00}
		p.Sites = append(p.Sites, PatchSite{Addr: "0x" + strings.ToLower(strings.TrimLeft(strings.ToUpper(hexU(a)), "0")), Bytes: "b980070000", Op: PatchValue, Values: []string{"1920", "1600", "1280"}})
	}
	e := replTestEmitter(img, nil, nil)
	e.lifter = lifter.NewArm64LifterMode(nil, lifter.AddrBase)
	prof := &PatchProfile{TitleID: "T", Builds: []PatchBuild{{Version: "1", Patches: []Patch{p}}}}
	if _, err := applyChoice(e, prof, img); err == nil || !strings.Contains(err.Error(), "more than 64 value sites") {
		t.Errorf("slot overflow: %v", err)
	}
	prof.Builds[0].Patches[0].Sites = p.Sites[:MaxPatchValues]
	if _, err := applyChoice(e, prof, img); err != nil {
		t.Errorf("64 value sites: %v", err)
	}
}

func hexU(v uint64) string {
	const d = "0123456789abcdef"
	b := make([]byte, 16)
	for i := 15; i >= 0; i-- {
		b[i] = d[v&15]
		v >>= 4
	}
	return string(b)
}

// The shipped ASTRO BOT profile: the upscaling choice with its four value sites.
func TestShippedProfileUpscaling(t *testing.T) {
	p, err := LoadPatchProfile(filepath.Join("..", "..", "profiles", "patches", "PPSA21564.json"))
	if err != nil {
		t.Fatal(err)
	}
	var up *Patch
	for i := range p.Builds[0].Patches {
		if p.Builds[0].Patches[i].Key == "upscaling" {
			up = &p.Builds[0].Patches[i]
		}
	}
	if up == nil {
		t.Fatal("no upscaling patch")
	}
	// default: metalfx720 (v1.0.3, user decision 2026-11-18); "off" (native 1080p) stays the first option
	if def, err := checkPatchOptions(up); err != nil || def != 2 || up.Options[2].Setenv["KYTY_METALFX"] != "spatial" || up.Options[1].Setenv["KYTY_METALFX"] != "spatial" {
		t.Fatalf("options: %d %v", def, err)
	}
	if len(up.Options) != 3 || up.Options[0].Name != "off" || up.Options[1].Name != "metalfx900" || up.Options[2].Name != "metalfx720" ||
		up.Env != "NPS5_ASTRO_UPSCALING" || len(up.Options[0].Setenv) != 0 || up.Options[1].Setenv["KYTY_METALFX"] == "" || up.Options[2].Setenv["KYTY_METALFX"] == "" {
		t.Errorf("options %+v", up.Options)
	}
	if len(up.Sites) != 4 {
		t.Fatalf("%d sites", len(up.Sites))
	}
	nf, ni := 0, 0
	for _, s := range up.Sites {
		b, err := hexBytes(s.Bytes)
		if err != nil {
			t.Fatal(err)
		}
		inst, err := x86asm.Decode(b, 64)
		if err != nil || inst.Len != len(b) {
			t.Fatalf("site %s: %v", s.Addr, err)
		}
		switch s.Op {
		case PatchValueF32:
			nf++
			if _, _, err := lifter.PatchValueTarget(inst, lifter.PatchValueF32); err != nil {
				t.Errorf("site %s: %v", s.Addr, err)
			}
			if strings.Join(s.Values, ",") != "1920,1600,1280" {
				t.Errorf("site %s widths %v", s.Addr, s.Values)
			}
		case PatchValue:
			ni++
			_, imm, err := lifter.PatchValueTarget(inst, lifter.PatchValueImm)
			if err != nil || imm != 0x43800000780 {
				t.Errorf("site %s: 0x%x %v", s.Addr, imm, err)
			}
			// {width, height}: 1920x1080, 1600x900, 1280x720
			if strings.Join(s.Values, ",") != "0x43800000780,0x38400000640,0x2d000000500" {
				t.Errorf("site %s sizes %v", s.Addr, s.Values)
			}
		default:
			t.Errorf("site %s op %s", s.Addr, s.Op)
		}
	}
	if nf != 2 || ni != 2 {
		t.Errorf("%d float and %d integer sites", nf, ni)
	}
	// Every site names the function that contains it (the binder refuses sites without it).
	for _, s := range up.Sites {
		a, err1 := strconv.ParseUint(strings.TrimPrefix(s.Addr, "0x"), 16, 64)
		fn, err2 := strconv.ParseUint(strings.TrimPrefix(s.Func, "0x"), 16, 64)
		if err1 != nil || err2 != nil || len(s.FuncSHA256) != 64 || a < fn || a >= fn+s.FuncSize {
			t.Errorf("site %s: function identity %q +%d %q", s.Addr, s.Func, s.FuncSize, s.FuncSHA256)
		}
	}
}

// TestShippedProfileBindsTitle binds the shipped PPSA21564 profile to the title's eboot.bin
// (NPS5_TEST_PPSA21564_EBOOT = path of the user's own dump; skipped without it): every site,
// with its function identity, matches build 01.007.
func TestShippedProfileBindsTitle(t *testing.T) {
	path := os.Getenv("NPS5_TEST_PPSA21564_EBOOT")
	if path == "" {
		t.Skip("NPS5_TEST_PPSA21564_EBOOT not set")
	}
	p, err := LoadPatchProfile(filepath.Join("..", "..", "profiles", "patches", "PPSA21564.json"))
	if err != nil {
		t.Fatal(err)
	}
	l, err := elfloader.LoadELF(path)
	if err != nil {
		t.Fatal(err)
	}
	e := &AsmEmitter{elf: l, shimMap: map[uint64]string{}}
	e.lifter = lifter.NewArm64LifterMode(nil, lifter.AddrBase)
	rep, err := e.ApplyPatchProfile(p)
	if err != nil {
		t.Fatal(err)
	}
	if rep.Sites != 14 || strings.Join(rep.Keys, ",") != "fast_lighting,upscaling,render_scale" {
		t.Errorf("report %+v", rep)
	}
}

// The shipped render_scale setting: auto (default) and p540 build level 1 at 960x540 and open the level selector + the scene
// level getter (override sites) and the level-1 light-tile words (data32); p720 keeps the game everywhere.
func TestShippedProfileRenderScale(t *testing.T) {
	p, err := LoadPatchProfile(filepath.Join("..", "..", "profiles", "patches", "PPSA21564.json"))
	if err != nil {
		t.Fatal(err)
	}
	var rs *Patch
	for i := range p.Builds[0].Patches {
		if p.Builds[0].Patches[i].Key == "render_scale" {
			rs = &p.Builds[0].Patches[i]
		}
	}
	if rs == nil {
		t.Fatal("no render_scale patch")
	}
	if def, err := checkPatchOptions(rs); err != nil || def != 0 || len(rs.Options) != 3 || rs.Options[0].Name != "auto" || rs.Options[1].Name != "p540" ||
		rs.Options[2].Name != "p720" || rs.Env != "" {
		t.Fatalf("options %+v %d %v", rs.Options, def, err)
	}
	n := map[string]int{}
	for _, s := range rs.Sites {
		n[s.Op]++
		if s.Values[2] != "keep" && s.Op != PatchValue && s.Op != PatchValueF32 {
			t.Errorf("site %s: p720 must keep the game (%v)", s.Addr, s.Values)
		}
		if s.Values[0] != s.Values[1] {
			t.Errorf("site %s: auto and p540 differ (%v)", s.Addr, s.Values)
		}
		if s.Op == PatchData32 {
			continue
		}
		b, err := hexBytes(s.Bytes)
		if err != nil {
			t.Fatal(err)
		}
		inst, err := x86asm.Decode(b, 64)
		if err != nil || inst.Len != len(b) {
			t.Fatalf("site %s: %v", s.Addr, err)
		}
		switch s.Op {
		case PatchOverride:
			if _, _, err := lifter.PatchValueTarget(inst, lifter.PatchValueOverride); err != nil || s.Values[0] != "0" {
				t.Errorf("site %s: %v %v", s.Addr, err, s.Values)
			}
		case PatchValueF32:
			if strings.Join(s.Values, ",") != "960,960,2432" {
				t.Errorf("site %s widths %v", s.Addr, s.Values)
			}
		case PatchValue:
			if strings.Join(s.Values, ",") != "0x21c000003c0,0x21c000003c0,0x55800000980" {
				t.Errorf("site %s sizes %v", s.Addr, s.Values)
			}
		}
	}
	if n[PatchOverride] != 3 || n[PatchData32] != 2 || n[PatchValue] != 2 || n[PatchValueF32] != 2 {
		t.Errorf("sites per op %v", n)
	}
}

func hexBytes(s string) ([]byte, error) {
	s = strings.ReplaceAll(s, " ", "")
	out := make([]byte, len(s)/2)
	for i := range out {
		var v byte
		for _, c := range []byte(s[2*i : 2*i+2]) {
			v <<= 4
			switch {
			case c >= '0' && c <= '9':
				v |= c - '0'
			case c >= 'a' && c <= 'f':
				v |= c - 'a' + 10
			default:
				return nil, os.ErrInvalid
			}
		}
		out[i] = v
	}
	return out, nil
}

// The generated table against the runtime: the slots hold the default option before anything
// runs; settings file (option name or position) < environment (generic < the profile's own);
// unknown values are ignored; an option's setenv lines are applied unless the environment has the
// variable; the settings file line is the option's name; everything is fixed after resolve; the
// boolean settings next to it keep working.
func TestPatchChoiceTableWithRuntime(t *testing.T) {
	if _, err := exec.LookPath("clang"); err != nil {
		t.Skip("clang not found")
	}
	e := choiceTestEmitter()
	if _, err := applyChoice(e, choiceTestProfile(choiceTestPatch()), choiceImg); err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	table, err := e.WritePatchTable(dir)
	if err != nil || table == "" {
		t.Fatal(err)
	}
	main := filepath.Join(dir, "main.c")
	code := `#include "nps5_patches.h"
#include <stdlib.h>
#include <string.h>
extern uint8_t nps5_guest_patch_flags[];
extern uint64_t nps5_guest_patch_values[];
#define CHECK(c) do { if (!(c)) { printf("line %d: %s\n", __LINE__, #c); return 1; } checks++; } while (0)
static const uint64_t W[3] = {0x44f00000, 0x44c80000, 0x44a00000}, P[3] = {0x43800000780ull, 0x38400000640ull, 0x2d000000500ull}, I[3] = {1920, 1600, 1280};
int main(int argc, char **argv) {
  int checks = 0, want = atoi(argv[1]);
  const char *cfg = argv[2];
  /* registered by the constructor; the slots hold the default option already */
  CHECK(nps5_patches_count() == 1 && nps5_choices_count() == 1 && !strcmp(nps5_patches_profile(), "TEST00001 01.007"));
  CHECK(nps5_guest_patch_values[0] == W[0] && nps5_guest_patch_values[1] == P[0] && nps5_guest_patch_values[2] == I[0] && nps5_guest_patch_values[3] == 0);
  const Nps5ChoiceDesc *d = nps5_choices_desc(0);
  CHECK(d && nps5_choices_desc(1) == NULL && nps5_choices_desc(-1) == NULL);
  CHECK(!strcmp(d->key, "upscaling") && !strcmp(d->title, "Upscaling") && !strcmp(d->description, "d") && !strcmp(d->env, "NPS5_TEST_UPSCALING"));
  CHECK(d->noptions == 3 && d->def == 0 && d->nvalues == 3);
  CHECK(!strcmp(d->names[0], "off") && !strcmp(d->names[2], "metalfx720") && !strcmp(d->labels[1], "MetalFX 900p"));
  CHECK(!strcmp(d->setenv[0], "") && !strcmp(d->setenv[1], "NPS5_TEST_B=1 2\nNPS5_TEST_METALFX=spatial") && !strcmp(d->setenv[2], "NPS5_TEST_METALFX=temporal"));
  CHECK(d->values[0].slot == 0 && d->values[0].f32 == 1 && d->values[0].site == 0x405000ull && d->values[0].values[2] == W[2]);
  CHECK(d->values[1].slot == 1 && d->values[1].f32 == 0 && d->values[1].site == 0x404000ull && d->values[2].site == 0x405100ull);
  /* option lookup: names in any case, positions, nothing else */
  CHECK(nps5_choices_option(0, "off") == 0 && nps5_choices_option(0, "MetalFX900") == 1 && nps5_choices_option(0, " metalfx720 \n") == 2);
  CHECK(nps5_choices_option(0, "0") == 0 && nps5_choices_option(0, "2") == 2 && nps5_choices_option(0, "3") == -1 && nps5_choices_option(0, "-1") == -1);
  CHECK(nps5_choices_option(0, "") == -1 && nps5_choices_option(0, "metalfx") == -1 && nps5_choices_option(0, "1x") == -1 && nps5_choices_option(0, NULL) == -1);
  CHECK(nps5_choices_option(1, "off") == -1 && nps5_choices_get(0) == 0 && nps5_choices_get(5) == 0);
  /* settings file lines */
  CHECK(nps5_patches_cfg_text("lang", "3") == 0 && nps5_patches_cfg_text("patch.nope", "off") == 0 && nps5_patches_cfg_text("patch.fast_lighting", "0") == 0);
  CHECK(nps5_patches_cfg_text(NULL, "off") == 0 && nps5_patches_cfg_line("patch.upscaling", 1) == 0);
  if (*cfg) { CHECK(nps5_patches_cfg_text("patch.upscaling", cfg) == 1); }
  nps5_choices_set(0, 3); nps5_choices_set(0, -1); nps5_choices_set(4, 1); /* out of range: ignored */
  CHECK(nps5_guest_patch_values[1] == P[0]); /* nothing reaches the code before resolve */
  FILE *f = fopen(argv[3], "w"); nps5_patches_cfg_write(f); fclose(f);
  nps5_patches_resolve();
  CHECK(nps5_choices_get(0) == want);
  CHECK(nps5_guest_patch_values[0] == W[want] && nps5_guest_patch_values[1] == P[want] && nps5_guest_patch_values[2] == I[want] && nps5_guest_patch_values[3] == 0);
  CHECK(nps5_guest_patch_flags[0] == 1 && nps5_patches_get(0) == 1);
  const char *m = getenv("NPS5_TEST_METALFX"), *b = getenv("NPS5_TEST_B");
  printf("env metalfx=%s b=%s\n", m ? m : "-", b ? b : "-");
  /* resolved once: later changes do nothing */
  nps5_choices_set(0, (want + 1) % 3); setenv("NPS5_TEST_UPSCALING", "metalfx720", 1); setenv("NPS5_PATCH_UPSCALING", "off", 1); nps5_patches_resolve();
  CHECK(nps5_choices_get(0) == want && nps5_guest_patch_values[1] == P[want]);
  CHECK(nps5_patches_cfg_text("patch.upscaling", "off") == 1 && nps5_choices_get(0) == want);
  /* a table of another abi, or a malformed one, is ignored */
  nps5_patch_choices_register(2, "x", d, 1, nps5_guest_patch_values, 64); CHECK(nps5_choices_count() == 1);
  printf("ok %d\n", checks);
  return 0; }`
	if err := os.WriteFile(main, []byte(code), 0o644); err != nil {
		t.Fatal(err)
	}
	core := filepath.Join("..", "runtime", "core")
	bin := filepath.Join(dir, "t")
	if out, err := exec.Command("clang", "-Wall", "-Werror", "-I"+core, main, table, filepath.Join(core, "nps5_patches.c"), "-o", bin).CombinedOutput(); err != nil {
		t.Fatalf("clang: %v\n%s", err, out)
	}
	modes := []struct {
		env     []string
		cfg     string
		want    string // option index
		cfgLine string
		log     []string
	}{
		{nil, "", "0", "patch.upscaling=off\n", []string{"patch upscaling: Upscaling = Off (1080p) (default; 3 value sites, first guest 0x405000)", "env metalfx=- b=-"}},
		{nil, "metalfx900", "1", "patch.upscaling=metalfx900\n",
			[]string{"Upscaling = MetalFX 900p (settings; 3 value sites, first guest 0x405000; env NPS5_TEST_B=1 2; env NPS5_TEST_METALFX=spatial)", "env metalfx=spatial b=1 2"}},
		{nil, "2", "2", "patch.upscaling=metalfx720\n", []string{"Upscaling = MetalFX 720p (settings;", "env metalfx=temporal b=-"}},
		{nil, "MetalFX720", "2", "", []string{"Upscaling = MetalFX 720p (settings;"}},
		{nil, "fsr", "0", "patch.upscaling=off\n", []string{"Upscaling = Off (1080p) (default;"}},
		{[]string{"NPS5_TEST_UPSCALING=metalfx720"}, "metalfx900", "2", "", []string{"Upscaling = MetalFX 720p (NPS5_TEST_UPSCALING;", "env metalfx=temporal"}},
		{[]string{"NPS5_PATCH_UPSCALING=1"}, "", "1", "", []string{"Upscaling = MetalFX 900p (NPS5_PATCH_UPSCALING;"}},
		{[]string{"NPS5_PATCH_UPSCALING=metalfx900", "NPS5_TEST_UPSCALING=off"}, "metalfx720", "0", "", []string{"Upscaling = Off (1080p) (NPS5_TEST_UPSCALING;", "env metalfx=- b=-"}},
		{[]string{"NPS5_TEST_UPSCALING=dlss"}, "metalfx900", "1", "", []string{"patch upscaling: NPS5_TEST_UPSCALING=dlss is not an option (ignored)", "Upscaling = MetalFX 900p (settings;"}},
		{[]string{"NPS5_TEST_UPSCALING="}, "", "0", "", []string{"Upscaling = Off (1080p) (default;"}},
		// the user's own environment wins over an option's setenv
		{[]string{"NPS5_TEST_METALFX=off"}, "metalfx900", "1", "",
			[]string{"env NPS5_TEST_B=1 2; env NPS5_TEST_METALFX kept from the environment)", "env metalfx=off b=1 2"}},
		{[]string{"NPS5_PATCH_FAST_LIGHTING=1"}, "metalfx720", "2", "patch.fast_lighting=1\npatch.upscaling=metalfx720\n", []string{"patch fast_lighting: Lighting = Fast", "Upscaling = MetalFX 720p"}},
	}
	for m, c := range modes {
		cfg := filepath.Join(dir, "cfg")
		cmd := exec.Command(bin, c.want, c.cfg, cfg)
		cmd.Env = append([]string{"PATH=" + os.Getenv("PATH")}, c.env...)
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("mode %d: %v\n%s", m, err, out)
		}
		for _, w := range c.log {
			if !strings.Contains(string(out), w) {
				t.Errorf("mode %d: output lacks %q:\n%s", m, w, out)
			}
		}
		if c.cfgLine != "" {
			want := c.cfgLine
			if !strings.HasPrefix(want, "patch.fast_lighting") {
				want = "patch.fast_lighting=1\n" + want
			}
			if b, _ := os.ReadFile(cfg); string(b) != want {
				t.Errorf("mode %d: settings lines %q, want %q", m, b, want)
			}
		}
	}
	t.Logf("%d runtime modes", len(modes))

	// A profile with choice settings only: no boolean table is registered, the choices are.
	e2 := choiceTestEmitter()
	if _, err := applyChoice(e2, &PatchProfile{TitleID: "ONLY", Builds: []PatchBuild{{Version: "1", Patches: []Patch{choiceTestPatch()}}}}, choiceImg); err != nil {
		t.Fatal(err)
	}
	dir2 := t.TempDir()
	table2, err := e2.WritePatchTable(dir2)
	if err != nil {
		t.Fatal(err)
	}
	main2 := filepath.Join(dir2, "main.c")
	os.WriteFile(main2, []byte(`#include "nps5_patches.h"
#include <string.h>
extern uint64_t nps5_guest_patch_values[];
int main(void) { nps5_patches_resolve();
  return !(nps5_patches_count() == 0 && nps5_choices_count() == 1 && !strcmp(nps5_patches_profile(), "ONLY 1") && nps5_guest_patch_values[1] == 0x38400000640ull); }`), 0o644)
	bin2 := filepath.Join(dir2, "t")
	if out, err := exec.Command("clang", "-Wall", "-Werror", "-I"+core, main2, table2, filepath.Join(core, "nps5_patches.c"), "-o", bin2).CombinedOutput(); err != nil {
		t.Fatalf("clang: %v\n%s", err, out)
	}
	cmd := exec.Command(bin2)
	cmd.Env = []string{"NPS5_PATCH_UPSCALING=metalfx900"}
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("choices-only table: %v\n%s", err, out)
	}
}
