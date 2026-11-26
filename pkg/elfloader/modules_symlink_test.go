// SPDX-License-Identifier: GPL-2.0-or-later

package elfloader

import (
	"os"
	"path/filepath"
	"sort"
	"testing"
)

func writeStub(t *testing.T, path string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte("stub"), 0o644); err != nil {
		t.Fatal(err)
	}
}

func refNames(refs []ModuleRef) []string {
	var out []string
	for _, r := range refs {
		out = append(out, filepath.Base(r.Path))
	}
	sort.Strings(out)
	return out
}

// A folder of links to a dump (the documented way to leave a dumper's artefacts out): the module
// folders are symbolic links to directories. filepath.WalkDir does not enter them, which left every
// companion module (the title's libc.prx among them) unlinked.
func TestDiscoverThroughSymlinkedDirs(t *testing.T) {
	dump := t.TempDir()
	writeStub(t, filepath.Join(dump, "sce_module", "libSceFoo.prx"))
	writeStub(t, filepath.Join(dump, "prx", "plugin_a.prx"))
	writeStub(t, filepath.Join(dump, "prx", "nested", "plugin_b.sprx"))
	writeStub(t, filepath.Join(dump, "eboot.bin"))

	view := t.TempDir()
	for _, n := range []string{"sce_module", "prx", "eboot.bin"} {
		if err := os.Symlink(filepath.Join(dump, n), filepath.Join(view, n)); err != nil {
			t.Fatal(err)
		}
	}
	// A loop and a dangling link must not hang or fail the walk.
	if err := os.Symlink(view, filepath.Join(view, "loop")); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(filepath.Join(dump, "missing"), filepath.Join(view, "dangling")); err != nil {
		t.Fatal(err)
	}

	got := refNames(DiscoverCompanionModules("", view, nil))
	want := []string{"libSceFoo.prx", "plugin_a.prx", "plugin_b.sprx"}
	if len(got) != len(want) {
		t.Fatalf("modules through links = %v, want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("modules through links = %v, want %v", got, want)
		}
	}
	// Each module once, by its real path.
	for _, r := range DiscoverCompanionModules("", view, nil) {
		if real, _ := filepath.EvalSymlinks(r.Path); real != r.Path {
			t.Fatalf("ref %s is not the real path (%s)", r.Path, real)
		}
	}
}

// A dumper's fakelib/ folder (its own libSceAmpr.sprx) is not one of the game's modules: linked,
// it would replace the Ampr HLE. It is skipped whether it is a real folder or a link.
func TestDiscoverSkipsDumperFakelib(t *testing.T) {
	dump := t.TempDir()
	writeStub(t, filepath.Join(dump, "fakelib", "libSceAmpr.sprx"))
	writeStub(t, filepath.Join(dump, "FakeLib2", "keep.prx")) // only the exact artefact name is skipped
	writeStub(t, filepath.Join(dump, "sce_module", "libSceFoo.prx"))
	writeStub(t, filepath.Join(dump, "build", "out.prx")) // build output, as before
	writeStub(t, filepath.Join(dump, "ampr_emu.index"))

	got := refNames(DiscoverCompanionModules("", dump, nil))
	if len(got) != 2 || got[0] != "keep.prx" || got[1] != "libSceFoo.prx" {
		t.Fatalf("modules = %v, want [keep.prx libSceFoo.prx]", got)
	}

	view := t.TempDir()
	if err := os.Symlink(filepath.Join(dump, "fakelib"), filepath.Join(view, "fakelib")); err != nil {
		t.Fatal(err)
	}
	if got := refNames(DiscoverCompanionModules("", view, nil)); len(got) != 0 {
		t.Fatalf("linked fakelib/ was walked: %v", got)
	}
}
