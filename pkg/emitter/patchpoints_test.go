package emitter

import (
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

func patchTestProfile() *PatchProfile {
	return &PatchProfile{TitleID: "TEST00001", Builds: []PatchBuild{
		{Version: "01.000", Patches: []Patch{{Key: "x", Title: "X", On: "a", Off: "b", Sites: []PatchSite{{Addr: "0x1003", Bytes: "0f84ee080000", Op: PatchBranchAlways}}}}},
		{Version: "01.007", Patches: []Patch{
			{Key: "fast_lighting", Title: "Lighting", On: "Fast (non-tiled)", Off: "Full quality (tiled)", Description: "d \"q\" \\ é", Default: true,
				Env: "NPS5_TEST_FAST_LIGHTING", Sites: []PatchSite{{Addr: "0x1003", Bytes: "0f 84 ed 08 00 00", Op: PatchBranchAlways}}},
			{Key: "other", Title: "Other", On: "On", Off: "Off", Sites: []PatchSite{{Addr: "2000", Bytes: "7502", Op: PatchBranchNever}}},
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
		{"not a branch", mut(func(p *Patch) { p.Sites[0] = PatchSite{Addr: "0x3000", Bytes: "b801000000", Op: PatchBranchAlways} }), "not a relative conditional branch"},
		{"half an instruction", mut(func(p *Patch) { p.Sites[0].Bytes = "0f84ed08" }), "not exactly one instruction"},
		{"two instructions", mut(func(p *Patch) { p.Sites[0] = PatchSite{Addr: "0x1000", Bytes: "4584f60f84ed080000", Op: PatchBranchAlways} }), "not exactly one instruction"},
		{"unknown operation", mut(func(p *Patch) { p.Sites[0].Op = "replace" }), "operation \"replace\""},
		{"bad key", mut(func(p *Patch) { p.Key = "Fast Lighting" }), "patch key"},
		{"duplicate key", mut(func(p *Patch) { p.Key = "other" }), "twice"},
		{"bad env", mut(func(p *Patch) { p.Env = "a=b" }), "environment variable"},
		{"no texts", mut(func(p *Patch) { p.On = "" }), "texts are required"},
		{"no sites", mut(func(p *Patch) { p.Sites = nil }), "no sites"},
		{"site twice", mut(func(p *Patch) { p.Sites = append(p.Sites, p.Sites[0]) }), "listed twice"},
		{"outside the image", mut(func(p *Patch) { p.Sites[0].Addr = "0xfffffff0" }), "the image has"},
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
	dir := t.TempDir()
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
