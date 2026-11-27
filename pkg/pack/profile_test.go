// SPDX-License-Identifier: GPL-2.0-or-later

package pack

import (
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

func writeProfile(t *testing.T, dir, name, body string) {
	t.Helper()
	p := filepath.Join(dir, name)
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(p, []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestLoadProfileBase(t *testing.T) {
	repo := t.TempDir()
	dir := filepath.Join(repo, "profiles")
	writeProfile(t, dir, "engines/fam.json", `{"name":"fam","notes":["base note"],
		"translate":{"x87":"f64","exclude_module":["a"],"args":["-x"]},
		"env":{"A":"1","B":"base"}}`)
	writeProfile(t, dir, "TITLE1.json", `{"base":"fam","notes":["title note"],
		"translate":{"call_model":"flat","exclude_module":["b"],"args":["-y"]},
		"env":{"B":"title","C":"3"}}`)
	t.Setenv("NPS5_PROFILES", "")
	p, path, err := LoadProfile("", "TITLE1", repo)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasSuffix(path, "TITLE1.json") {
		t.Fatalf("path %s", path)
	}
	if want := map[string]string{"A": "1", "B": "title", "C": "3"}; !reflect.DeepEqual(p.Env, want) {
		t.Fatalf("env %v, want %v", p.Env, want)
	}
	if p.Translate.X87 != "f64" || p.Translate.CallModel != "flat" || p.Translate.GPU != "anyps5" {
		t.Fatalf("translate %+v", p.Translate)
	}
	if !reflect.DeepEqual(p.Translate.ExcludeModule, []string{"a", "b"}) || !reflect.DeepEqual(p.Translate.Args, []string{"-x", "-y"}) {
		t.Fatalf("lists %v %v", p.Translate.ExcludeModule, p.Translate.Args)
	}
	if !reflect.DeepEqual(p.Notes, []string{"base note", "title note"}) {
		t.Fatalf("notes %v", p.Notes)
	}
	if p.Name != "fam" { // the title file has no name of its own: the base's stays
		t.Fatalf("name %q", p.Name)
	}
}

func TestLoadProfileBaseErrors(t *testing.T) {
	repo := t.TempDir()
	dir := filepath.Join(repo, "profiles")
	writeProfile(t, dir, "MISSING.json", `{"base":"nope"}`)
	writeProfile(t, dir, "engines/x.json", `{"base":"y"}`)
	writeProfile(t, dir, "engines/y.json", `{"base":"x"}`)
	writeProfile(t, dir, "LOOP.json", `{"base":"x"}`)
	writeProfile(t, dir, "BAD.json", `{"_unknown":1}`)
	t.Setenv("NPS5_PROFILES", "")
	for _, tc := range []struct{ id, want string }{
		{"MISSING", `base "nope" not found`},
		{"LOOP", "base chain deeper"},
		{"BAD", "unknown field"},
	} {
		if _, _, err := LoadProfile("", tc.id, repo); err == nil || !strings.Contains(err.Error(), tc.want) {
			t.Errorf("%s: err %v, want %q", tc.id, err, tc.want)
		}
	}
}

// The profiles shipped in the repository must load.
func TestRepoProfilesLoad(t *testing.T) {
	repo, err := filepath.Abs(filepath.Join("..", ".."))
	if err != nil {
		t.Fatal(err)
	}
	t.Setenv("NPS5_PROFILES", "")
	files, err := filepath.Glob(filepath.Join(repo, "profiles", "*.json"))
	if err != nil || len(files) == 0 {
		t.Fatalf("no profiles: %v", err)
	}
	for _, f := range files {
		if _, _, err := LoadProfile(f, "", repo); err != nil {
			t.Errorf("%s: %v", filepath.Base(f), err)
		}
	}
	p, _, err := LoadProfile("", "PPSA28997", repo)
	if err != nil {
		t.Fatal(err)
	}
	if p.Env["NPS5_VQ_RESERVED_UNCOMMITTED"] != "1" || p.Env["NPS5_AVPLAYER_CLOSE_CALLER_CTX"] != "1" {
		t.Fatalf("PPSA28997: base + title env not merged: %v", p.Env)
	}
}

// TestSarosProfileLightFunctionQuality: the SAROS (PPSA07631) profile keeps the approved r.LightFunctionQuality=1 in its UE
// cvar list, next to the Nanite / ray tracing entries it already carried, and no entry is lost or doubled.
func TestSarosProfileLightFunctionQuality(t *testing.T) {
	repo, err := filepath.Abs(filepath.Join("..", ".."))
	if err != nil {
		t.Fatal(err)
	}
	t.Setenv("NPS5_PROFILES", "")
	p, _, err := LoadProfile("", "PPSA07631", repo)
	if err != nil {
		t.Fatal(err)
	}
	seen := map[string]int{}
	for _, kv := range strings.Split(p.Env["NPS5_UECVAR_SET"], ",") {
		seen[kv]++
	}
	for _, want := range []string{"r.LightFunctionQuality=1", "r.RayTracing.Enable=0", "r.Nanite.PersistentThreadsCulling=0",
		"r.Nanite.PrimShaderRasterization=0", "r.Nanite.MeshShaderRasterization=0"} {
		if seen[want] != 1 {
			t.Errorf("NPS5_UECVAR_SET: %q appears %d times, want 1 (%q)", want, seen[want], p.Env["NPS5_UECVAR_SET"])
		}
	}
}

// TestSarosProfileHandShaderStack: the SAROS (PPSA07631) profile carries the three switches the runners used to pass by hand
// (hand shaders, Nanite raw raster parameters, shader-bundle compaction), and no other title's profile sets them.
func TestSarosProfileHandShaderStack(t *testing.T) {
	repo, err := filepath.Abs(filepath.Join("..", ".."))
	if err != nil {
		t.Fatal(err)
	}
	t.Setenv("NPS5_PROFILES", "")
	keys := []string{"KYTY_SAROS_HANDSHADERS", "KYTY_SAROS_NANITE_RAW", "KYTY_BUNDLE_COMPACT"}
	p, _, err := LoadProfile("", "PPSA07631", repo)
	if err != nil {
		t.Fatal(err)
	}
	for _, k := range keys {
		if p.Env[k] != "1" {
			t.Errorf("PPSA07631: %s=%q, want 1", k, p.Env[k])
		}
	}
	for _, id := range []string{"PPSA21564", "PPSA28997"} {
		q, _, err := LoadProfile("", id, repo)
		if err != nil {
			t.Fatal(err)
		}
		for _, k := range keys {
			if _, ok := q.Env[k]; ok {
				t.Errorf("%s must not set %s", id, k)
			}
		}
	}
}

// TestSarosProfileTranslateArgs: SAROS is translated with the packed-code discovery and with the unsupported MMX / moffs
// sites as stubs (the bring-up translation did both; without them `nps5 build` refuses the title or the UE main thread
// loops in dispatch). No other title gets them from its profile.
func TestSarosProfileTranslateArgs(t *testing.T) {
	repo, err := filepath.Abs(filepath.Join("..", ".."))
	if err != nil {
		t.Fatal(err)
	}
	t.Setenv("NPS5_PROFILES", "")
	p, _, err := LoadProfile("", "PPSA07631", repo)
	if err != nil {
		t.Fatal(err)
	}
	args, err := p.Args(func(string) bool { return true })
	if err != nil {
		t.Fatal(err)
	}
	have := map[string]bool{}
	for _, a := range args {
		have[a] = true
	}
	for _, want := range []string{"-packed-layout", "-allow-unsupported"} {
		if !have[want] {
			t.Errorf("PPSA07631 translate args %v lack %s", args, want)
		}
	}
	for _, id := range []string{"PPSA21564", "PPSA28997"} {
		q, _, err := LoadProfile("", id, repo)
		if err != nil {
			t.Fatal(err)
		}
		for _, a := range q.Translate.Args {
			if a == "-packed-layout" {
				t.Errorf("%s must not set -packed-layout", id)
			}
		}
	}
}

// TestAstroProfileSeeds: the ASTRO (PPSA21564) profile hands the translator its tier-1 seed list, the file it names exists
// in the repository (the path is relative to the checkout, like the patch list), and no other title's profile has one.
func TestAstroProfileSeeds(t *testing.T) {
	repo, err := filepath.Abs(filepath.Join("..", ".."))
	if err != nil {
		t.Fatal(err)
	}
	t.Setenv("NPS5_PROFILES", "")
	seedsArg := func(id string) string {
		p, _, err := LoadProfile("", id, repo)
		if err != nil {
			t.Fatal(err)
		}
		args, err := p.Args(func(string) bool { return true })
		if err != nil {
			t.Fatal(err)
		}
		for i, a := range args {
			if a == "-seeds-from" && i+1 < len(args) {
				return args[i+1]
			}
		}
		return ""
	}
	got := seedsArg("PPSA21564")
	if got != "profiles/seeds/PPSA21564.seeds" {
		t.Fatalf("PPSA21564 -seeds-from = %q", got)
	}
	if _, err := os.Stat(filepath.Join(repo, got)); err != nil {
		t.Errorf("the seed list the ASTRO profile names is missing: %v", err)
	}
	for _, id := range []string{"PPSA07631", "PPSA28997", "syobon"} {
		if s := seedsArg(id); s != "" {
			t.Errorf("%s must not carry tier-1 seeds, has %q", id, s)
		}
	}
}
