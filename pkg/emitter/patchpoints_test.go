package emitter

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"ps4-recomp/pkg/lifter"
)

// je +0x8ed at link 0x1003 (after test r14b,r14b), jne +2 at 0x2000, mov eax,1 at 0x3000.
func patchTestEmitter() *AsmEmitter {
	e := replTestEmitter(map[uint64][]byte{
		0x1000: {0x45, 0x84, 0xf6, 0x0f, 0x84, 0xed, 0x08, 0x00, 0x00},
		0x2000: {0x75, 0x02},
		0x3000: {0xb8, 0x01, 0x00, 0x00, 0x00},
	}, nil, nil)
	e.lifter = lifter.NewArm64LifterMode(nil, lifter.AddrBase)
	return e
}

// The functions of the test image that contain the sites.
var (
	patchFn1 = []byte{0x45, 0x84, 0xf6, 0x0f, 0x84, 0xed, 0x08, 0x00, 0x00} // 0x1000
	patchFn2 = []byte{0x75, 0x02}                                           // 0x2000
)

// withFn adds the identity of the function fb at link address fn to a site.
func withFn(s PatchSite, fn uint64, fb []byte) PatchSite {
	s.Func, s.FuncSize, s.FuncSHA256 = fmt.Sprintf("0x%x", fn), uint64(len(fb)), hexSHA(fb)
	return s
}

func patchTestProfile() *PatchProfile {
	return &PatchProfile{TitleID: "TEST00001", Builds: []PatchBuild{
		{Version: "01.000", Patches: []Patch{{Key: "x", Title: "X", On: "a", Off: "b", Sites: []PatchSite{
			withFn(PatchSite{Addr: "0x1003", Bytes: "0f84ee080000", Op: PatchBranchAlways}, 0x1000, []byte("another build"))}}}},
		{Version: "01.007", Patches: []Patch{
			{Key: "fast_lighting", Title: "Lighting", On: "Fast (non-tiled)", Off: "Full quality (tiled)", Description: "d \"q\" \\ é", Default: true,
				Env: "NPS5_TEST_FAST_LIGHTING", Sites: []PatchSite{withFn(PatchSite{Addr: "0x1003", Bytes: "0f 84 ed 08 00 00", Op: PatchBranchAlways}, 0x1000, patchFn1)}},
			{Key: "other", Title: "Other", On: "On", Off: "Off", Sites: []PatchSite{withFn(PatchSite{Addr: "2000", Bytes: "7502", Op: PatchBranchNever}, 0x2000, patchFn2)}},
		}},
	}}
}

func TestPatchProfileBindsTheMatchingBuild(t *testing.T) {
	e := patchTestEmitter()
	rep, err := e.ApplyPatchProfile(patchTestProfile())
	if err != nil {
		t.Fatal(err)
	}
	if rep.Profile != "TEST00001 01.007" || rep.Sites != 2 || strings.Join(rep.Keys, ",") != "fast_lighting,other" {
		t.Errorf("report %+v", rep)
	}
	a, b := e.patch.sites[0x1003+replBias], e.patch.sites[0x2000+replBias]
	if a.slot != 0 || !a.always || b.slot != 1 || b.always {
		t.Errorf("sites %+v %+v", a, b)
	}
	// The facts of a chunk change with the slot and the operation only.
	if string(a.fact()) == string(b.fact()) || string(a.fact()) != "PP0,true;" {
		t.Errorf("facts %q %q", a.fact(), b.fact())
	}
	// Sites nobody translated are an error; seen sites are not.
	if err := e.CheckPatchSites(); err == nil || !strings.Contains(err.Error(), "0x401003") || !strings.Contains(err.Error(), "0x402000") {
		t.Errorf("unseen sites: %v", err)
	}
	e.patch.markSeen(0x1003 + replBias)
	e.patch.markSeen(0x2000 + replBias)
	if err := e.CheckPatchSites(); err != nil {
		t.Error(err)
	}
}

func TestPatchProfileRefusals(t *testing.T) {
	mut := func(f func(p *Patch)) *PatchProfile {
		p := patchTestProfile()
		p.Builds = p.Builds[1:]
		f(&p.Builds[0].Patches[0])
		return p
	}
	cases := []struct {
		name string
		p    *PatchProfile
		want string
	}{
		{"other build's bytes", mut(func(p *Patch) { p.Sites[0].Bytes = "0f84ee080000" }), "the image has 0f84ed080000"},
		{"not a branch", mut(func(p *Patch) {
			p.Sites[0] = withFn(PatchSite{Addr: "0x3000", Bytes: "b801000000", Op: PatchBranchAlways}, 0x3000, []byte{0xb8, 1, 0, 0, 0})
		}), "not a relative conditional branch"},
		{"half an instruction", mut(func(p *Patch) { p.Sites[0].Bytes = "0f84ed08" }), "not exactly one instruction"},
		{"two instructions", mut(func(p *Patch) {
			p.Sites[0] = withFn(PatchSite{Addr: "0x1000", Bytes: "4584f60f84ed080000", Op: PatchBranchAlways}, 0x1000, patchFn1)
		}), "not exactly one instruction"},
		{"odd-length hex", mut(func(p *Patch) { p.Sites[0].Bytes = "0f84ed08000" }), "bytes"},
		{"malformed hex", mut(func(p *Patch) { p.Sites[0].Bytes = "0f84zz080000" }), "bytes"},
		{"empty bytes", mut(func(p *Patch) { p.Sites[0].Bytes = "" }), "bytes"},
		{"no function address", mut(func(p *Patch) { p.Sites[0].Func = "" }), "func, func_size and func_sha256 are required"},
		{"no function size", mut(func(p *Patch) { p.Sites[0].FuncSize = 0 }), "func, func_size and func_sha256 are required"},
		{"no function hash", mut(func(p *Patch) { p.Sites[0].FuncSHA256 = "" }), "func, func_size and func_sha256 are required"},
		{"function hash not hex", mut(func(p *Patch) { p.Sites[0].FuncSHA256 = "xyz" }), "not a hex SHA-256"},
		{"function hash too short", mut(func(p *Patch) { p.Sites[0].FuncSHA256 = "abcd" }), "not a hex SHA-256"},
		{"other build's function", mut(func(p *Patch) { p.Sites[0].FuncSHA256 = hexSHA([]byte("another build")) }), "not the one this profile was made for"},
		{"site outside its function", mut(func(p *Patch) { p.Sites[0] = withFn(p.Sites[0], 0x2000, patchFn2) }), "not inside the function"},
		{"function not in the image", mut(func(p *Patch) { p.Sites[0].Func, p.Sites[0].FuncSize = "0x1003", 0x10000000 }), "not in the image"},
		{"function size wraps", mut(func(p *Patch) { p.Sites[0].FuncSize = ^uint64(0) }), "out of range"},
		{"site address above 48 bits", mut(func(p *Patch) { p.Sites[0].Addr = "0x1000000000000" }), "48-bit"},
		{"site address wraps with the bias", mut(func(p *Patch) { p.Sites[0].Addr = "0xffffffffffffffff" }), "48-bit"},
		{"function address above 48 bits", mut(func(p *Patch) { p.Sites[0].Func = "0x1000000000000" }), "48-bit"},
		{"no version", func() *PatchProfile {
			p := patchTestProfile()
			p.Builds = p.Builds[1:]
			p.Builds[0].Version = " "
			return p
		}(), "version is required"},
		{"no title_id", func() *PatchProfile { p := patchTestProfile(); p.TitleID = ""; return p }(), "title_id is required"},
		{"too many patches", func() *PatchProfile {
			p := patchTestProfile()
			p.Builds = p.Builds[1:]
			pt := p.Builds[0].Patches[0]
			p.Builds[0].Patches = nil
			for i := 0; i <= MaxPatches; i++ {
				pt.Key = fmt.Sprintf("k%d", i)
				p.Builds[0].Patches = append(p.Builds[0].Patches, pt)
			}
			return p
		}(), "at most"},
		{"unknown operation", mut(func(p *Patch) { p.Sites[0].Op = "replace" }), "operation \"replace\""},
		{"bad key", mut(func(p *Patch) { p.Key = "Fast Lighting" }), "patch key"},
		{"duplicate key", mut(func(p *Patch) { p.Key = "other" }), "twice"},
		{"bad env", mut(func(p *Patch) { p.Env = "a=b" }), "environment variable"},
		{"no texts", mut(func(p *Patch) { p.On = "" }), "texts are required"},
		{"no sites", mut(func(p *Patch) { p.Sites = nil }), "no sites"},
		{"site twice", mut(func(p *Patch) { p.Sites = append(p.Sites, p.Sites[0]) }), "listed twice"},
		{"outside the image", mut(func(p *Patch) { p.Sites[0].Addr, p.Sites[0].Func, p.Sites[0].FuncSize = "0xfffffff0", "0xfffffff0", 4 }), "not in the image"},
	}
	for _, c := range cases {
		e := patchTestEmitter()
		_, err := e.ApplyPatchProfile(c.p)
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: error %v, want %q", c.name, err, c.want)
		}
		if e.patch != nil {
			t.Errorf("%s: a refused profile left patches bound", c.name)
		}
	}
	// Two builds that both match are ambiguous.
	p := patchTestProfile()
	p.Builds[0] = p.Builds[1]
	if _, err := patchTestEmitter().ApplyPatchProfile(p); err == nil || !strings.Contains(err.Error(), "both match") {
		t.Errorf("ambiguous builds: %v", err)
	}
}

func TestPatchProfileFileAndStaleTable(t *testing.T) {
	// The shipped profile parses and has what the runtime shows.
	p, err := LoadPatchProfile(filepath.Join("..", "..", "profiles", "patches", "PPSA21564.json"))
	if err != nil {
		t.Fatal(err)
	}
	pt := p.Builds[0].Patches[0]
	if p.TitleID != "PPSA21564" || pt.Key != "fast_lighting" || !pt.Default || pt.Env != "NPS5_ASTRO_FAST_LIGHTING" || pt.Sites[0].Op != PatchBranchAlways {
		t.Errorf("profile %+v", p)
	}
	if s := pt.Sites[0]; s.Func == "" || s.FuncSize == 0 || len(s.FuncSHA256) != 64 || p.Builds[0].Version == "" {
		t.Errorf("the shipped profile lacks the function identity: %+v", s)
	}
	dir := t.TempDir()
	for name, body := range map[string]string{
		"trailing.json": `{"title_id":"X","builds":[{"version":"1","patches":[]}]} {"x":1}`,
		"garbage.json":  `{"title_id":"X","builds":[{"version":"1","patches":[]}]} x`,
		"notitle.json":  `{"title_id":" ","builds":[{"version":"1","patches":[]}]}`,
		"nobuilds.json": `{"title_id":"X","builds":[]}`,
	} {
		f := filepath.Join(dir, name)
		os.WriteFile(f, []byte(body), 0o644)
		if _, err := LoadPatchProfile(f); err == nil {
			t.Errorf("%s accepted", name)
		}
	}
	ok := filepath.Join(dir, "ok.json")
	os.WriteFile(ok, []byte(`{"title_id":"X","builds":[{"version":"1","patches":[]}]}`+"\n \n"), 0o644)
	if _, err := LoadPatchProfile(ok); err != nil {
		t.Errorf("trailing whitespace: %v", err)
	}
	bad := filepath.Join(dir, "bad.json")
	os.WriteFile(bad, []byte(`{"title_id":"X","builds":[{"version":"1","patches":[],"typo":1}]}`), 0o644)
	if _, err := LoadPatchProfile(bad); err == nil {
		t.Error("unknown field accepted")
	}
	// Without a profile a table left by an earlier translation is removed.
	e := patchTestEmitter()
	for _, n := range []string{"nps5_patches.c", "nps5_patches.o", "nps5_patches.o.flags"} {
		os.WriteFile(filepath.Join(dir, n), []byte("x"), 0o644)
	}
	if path, err := e.WritePatchTable(dir); err != nil || path != "" {
		t.Fatalf("%q %v", path, err)
	}
	if ents, _ := filepath.Glob(filepath.Join(dir, "nps5_patches.*")); len(ents) != 0 {
		t.Errorf("stale table kept: %v", ents)
	}
}

// The generated table against the runtime: defaults in the flag bytes before anything runs, then
// settings file < environment, the profile's own variable over the generic one, one flag per slot.
func TestPatchTableWithRuntime(t *testing.T) {
	if _, err := exec.LookPath("clang"); err != nil {
		t.Skip("clang not found")
	}
	e := patchTestEmitter()
	if _, err := e.ApplyPatchProfile(patchTestProfile()); err != nil {
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
#define CHECK(c) do { if (!(c)) { printf("line %d: %s\n", __LINE__, #c); return 1; } checks++; } while (0)
int main(int argc, char **argv) {
  int checks = 0, mode = atoi(argv[1]);
  /* registered by the constructor; the flags hold the profile defaults already */
  CHECK(nps5_patches_count() == 2);
  CHECK(!strcmp(nps5_patches_profile(), "TEST00001 01.007"));
  CHECK(nps5_guest_patch_flags[0] == 1 && nps5_guest_patch_flags[1] == 0 && nps5_guest_patch_flags[2] == 0);
  const Nps5PatchDesc *d = nps5_patches_desc(0);
  CHECK(d && !strcmp(d->key, "fast_lighting") && !strcmp(d->title, "Lighting") && !strcmp(d->on, "Fast (non-tiled)") && !strcmp(d->off, "Full quality (tiled)"));
  CHECK(!strcmp(d->description, "d \"q\" \\ \xc3\xa9") && !strcmp(d->env, "NPS5_TEST_FAST_LIGHTING") && d->def == 1 && d->slot == 0);
  CHECK(d->nsites == 1 && d->sites[0] == 0x401003ull);
  CHECK(nps5_patches_desc(1)->slot == 1 && nps5_patches_desc(1)->sites[0] == 0x402000ull && nps5_patches_desc(2) == NULL && nps5_patches_desc(-1) == NULL);
  CHECK(nps5_patches_get(0) == 1 && nps5_patches_get(1) == 0 && nps5_patches_get(7) == 0);
  /* settings file lines */
  CHECK(nps5_patches_cfg_line("lang", 3) == 0 && nps5_patches_cfg_line("patch.nope", 1) == 0 && nps5_patches_cfg_line("patch.", 1) == 0);
  if (mode >= 1) { CHECK(nps5_patches_cfg_line("patch.fast_lighting", 0) == 1); CHECK(nps5_patches_cfg_line("patch.other", 5) == 1); }
  CHECK(nps5_guest_patch_flags[0] == 1); /* nothing reaches the code before resolve */
  FILE *f = fopen(argv[2], "w"); nps5_patches_cfg_write(f); fclose(f);
  nps5_patches_resolve();
  int want0 = 1, want1 = 0;
  if (mode >= 1) { want0 = 0; want1 = 1; }  /* settings file */
  if (mode == 2) want0 = 1;                 /* NPS5_TEST_FAST_LIGHTING=1 */
  if (mode == 3) { want0 = 0; want1 = 0; }  /* generic 1 but own variable 0; NPS5_PATCH_OTHER=0 */
  if (mode == 4) want0 = 1;                 /* generic NPS5_PATCH_FAST_LIGHTING=yes */
  CHECK(nps5_guest_patch_flags[0] == want0 && nps5_guest_patch_flags[1] == want1 && nps5_guest_patch_flags[2] == 0);
  CHECK(nps5_patches_get(0) == want0 && nps5_patches_get(1) == want1);
  /* resolved once: later changes do nothing */
  nps5_patches_set(0, !want0); setenv("NPS5_TEST_FAST_LIGHTING", want0 ? "0" : "1", 1); nps5_patches_resolve();
  CHECK(nps5_guest_patch_flags[0] == want0 && nps5_patches_get(0) == want0);
  /* a table of another abi, or with a slot outside the flags, is ignored */
  nps5_patches_register(2, "x", d, 1, nps5_guest_patch_flags, 32); CHECK(nps5_patches_count() == 2);
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
		logWant string
	}{
		{nil, "patch.fast_lighting=1\npatch.other=0\n", "patch fast_lighting: Lighting = Fast (non-tiled) (default; 1 site, first guest 0x401003)"},
		{nil, "patch.fast_lighting=0\npatch.other=1\n", "patch fast_lighting: Lighting = Full quality (tiled) (settings;"},
		{[]string{"NPS5_TEST_FAST_LIGHTING=1"}, "", "Lighting = Fast (non-tiled) (NPS5_TEST_FAST_LIGHTING;"},
		{[]string{"NPS5_PATCH_FAST_LIGHTING=1", "NPS5_TEST_FAST_LIGHTING=0", "NPS5_PATCH_OTHER=0"}, "", "patch other: Other = Off (NPS5_PATCH_OTHER;"},
		{[]string{"NPS5_PATCH_FAST_LIGHTING=yes"}, "", "Lighting = Fast (non-tiled) (NPS5_PATCH_FAST_LIGHTING;"},
	}
	total := 0
	for m, c := range modes {
		cfg := filepath.Join(dir, "cfg")
		cmd := exec.Command(bin, string(rune('0'+m)), cfg)
		cmd.Env = append([]string{"PATH=" + os.Getenv("PATH")}, c.env...)
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("mode %d: %v\n%s", m, err, out)
		}
		if !strings.Contains(string(out), c.logWant) {
			t.Errorf("mode %d: log lacks %q:\n%s", m, c.logWant, out)
		}
		if c.cfg != "" {
			if b, _ := os.ReadFile(cfg); string(b) != c.cfg {
				t.Errorf("mode %d: settings lines %q, want %q", m, b, c.cfg)
			}
		}
		total++
	}
	t.Logf("%d runtime modes", total)
}

// nps5_patches.c on its own: a table that is not entirely consistent registers nothing and logs a
// refusal, and the environment spellings of a setting.
func TestPatchRuntimeRefusalsAndEnvSpellings(t *testing.T) {
	if _, err := exec.LookPath("clang"); err != nil {
		t.Skip("clang not found")
	}
	dir := t.TempDir()
	code := `#include "nps5_patches.h"
#include <stdlib.h>
#include <string.h>
static const uint64_t sites[] = {0x401003ull};
#define D(k, slot, nsites, sp) {k, "T", "on", "off", "", "NPS5_T_" k, slot, 0, nsites, sp}
static int refuse(const char *name, int abi, const Nps5PatchDesc *d, int n, uint8_t *fl, int nfl) {
  nps5_patches_register(abi, "p", d, n, fl, nfl);
  if (nps5_patches_count() != 0) { printf("%s: registered\n", name); return 1; }
  return 0;
}
int main(int argc, char **argv) {
  uint8_t fl[40] = {0};
  if (!strcmp(argv[1], "refuse")) {
    Nps5PatchDesc ok[2] = {D("a", 0, 1, sites), D("b", 1, 1, sites)};
    Nps5PatchDesc big[2] = {D("a", 0x80000000u, 1, sites), D("b", 1, 1, sites)};
    Nps5PatchDesc huge[2] = {D("a", 0xffffffffu, 1, sites), D("b", 1, 1, sites)};
    Nps5PatchDesc past[2] = {D("a", 0, 1, sites), D("b", 2, 1, sites)};
    Nps5PatchDesc dup[2] = {D("a", 1, 1, sites), D("b", 1, 1, sites)};
    Nps5PatchDesc nosites[2] = {D("a", 0, 1, NULL), D("b", 1, 1, sites)};
    Nps5PatchDesc nokey[2] = {D("a", 0, 1, sites), D("b", 1, 1, sites)};
    Nps5PatchDesc many[40];
    for (int i = 0; i < 40; i++) many[i] = ok[0], many[i].slot = i % 32;
    nokey[1].key = NULL;
    int bad = 0;
    bad += refuse("abi", 2, ok, 2, fl, 32);
    bad += refuse("slot 2^31", 1, big, 2, fl, 32);
    bad += refuse("slot 2^32-1", 1, huge, 2, fl, 32);
    bad += refuse("slot == nflags", 1, past, 2, fl, 2);
    bad += refuse("n > nflags", 1, ok, 2, fl, 1);
    bad += refuse("duplicate slots", 1, dup, 2, fl, 32);
    bad += refuse("sites NULL", 1, nosites, 2, fl, 32);
    bad += refuse("key NULL", 1, nokey, 2, fl, 32);
    bad += refuse("n == 0", 1, ok, 0, fl, 32);
    bad += refuse("n < 0", 1, ok, -1, fl, 32);
    bad += refuse("n > max", 1, many, 40, fl, 40);
    bad += refuse("no flags", 1, ok, 2, NULL, 32);
    bad += refuse("no table", 1, NULL, 2, fl, 32);
    bad += refuse("nflags == 0", 1, ok, 2, fl, 0);
    bad += refuse("nflags < 0", 1, ok, 2, fl, -1);
    if (bad) return 1;
    nps5_patches_register(1, "p", ok, 2, fl, 32); /* a consistent table still registers after refusals */
    if (nps5_patches_count() != 2) { printf("valid table refused\n"); return 1; }
    printf("ok\n");
    return 0;
  }
  Nps5PatchDesc d[2] = {D("alpha", 0, 1, sites), D("beta", 1, 1, sites)};
  d[0].def = 1;
  nps5_patches_register(1, "p", d, 2, fl, 32);
  nps5_patches_resolve();
  printf("a=%d b=%d\n", fl[0], fl[1]);
  return 0;
}`
	src := filepath.Join(dir, "m.c")
	if err := os.WriteFile(src, []byte(code), 0o644); err != nil {
		t.Fatal(err)
	}
	core := filepath.Join("..", "runtime", "core")
	bin := filepath.Join(dir, "t")
	if out, err := exec.Command("clang", "-Wall", "-Werror", "-I"+core, src, filepath.Join(core, "nps5_patches.c"), "-o", bin).CombinedOutput(); err != nil {
		t.Fatalf("clang: %v\n%s", err, out)
	}
	run := func(env []string, arg string) string {
		cmd := exec.Command(bin, arg)
		cmd.Env = append([]string{"PATH=" + os.Getenv("PATH")}, env...)
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("%v: %v\n%s", env, err, out)
		}
		return string(out)
	}
	out := run(nil, "refuse")
	if n := strings.Count(out, "patch table refused"); n != 15 || !strings.Contains(out, "ok\n") {
		t.Errorf("want one refusal line for each of 15 bad tables:\n%s", out)
	}
	for _, c := range []struct {
		env  []string
		want string
		warn bool
	}{
		{nil, "a=1 b=0", false},
		{[]string{"NPS5_PATCH_ALPHA=0"}, "a=0 b=0", false},
		{[]string{"NPS5_PATCH_ALPHA=false"}, "a=0 b=0", false},
		{[]string{"NPS5_PATCH_ALPHA=Off"}, "a=0 b=0", false},
		{[]string{"NPS5_PATCH_ALPHA=NO"}, "a=0 b=0", false},
		{[]string{"NPS5_PATCH_BETA=1"}, "a=1 b=1", false},
		{[]string{"NPS5_PATCH_BETA=true"}, "a=1 b=1", false},
		{[]string{"NPS5_PATCH_BETA=ON"}, "a=1 b=1", false},
		{[]string{"NPS5_PATCH_BETA=Yes"}, "a=1 b=1", false},
		{[]string{"NPS5_T_alpha=off", "NPS5_PATCH_ALPHA=1"}, "a=0 b=0", false}, // own variable wins
		{[]string{"NPS5_PATCH_ALPHA=maybe"}, "a=1 b=0", true},                  // unrecognised: the default stays
		{[]string{"NPS5_PATCH_BETA=2"}, "a=1 b=0", true},
		{[]string{"NPS5_PATCH_BETA= "}, "a=1 b=0", true},
	} {
		out := run(c.env, "env")
		if !strings.Contains(out, c.want+"\n") {
			t.Errorf("%v: %q, want %q", c.env, out, c.want)
		}
		if got := strings.Count(out, "ignored"); (got == 1) != c.warn || got > 1 {
			t.Errorf("%v: %d warning lines:\n%s", c.env, got, out)
		}
	}
}
