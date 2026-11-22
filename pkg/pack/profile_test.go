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
