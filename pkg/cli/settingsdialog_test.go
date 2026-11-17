// SPDX-License-Identifier: GPL-2.0-or-later

package cli

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// The inherited settings dialog is opt-in: by default (first launch, no env) the policy says no.
func TestSettingsDialogOptIn(t *testing.T) {
	dir := t.TempDir()
	src := filepath.Join(dir, "t.c")
	code := `#include "ps4_settings_policy.h"
#include <stdio.h>
int main(void){
  unsetenv("NPS5_SETTINGS_DIALOG"); unsetenv("PS4_NO_DIALOGS"); unsetenv("PS4_LANG");
  if (ps4_settings_dialog_wanted(false, false)) { puts("default shows dialog"); return 1; }
  if (ps4_settings_dialog_wanted(false, true)) { puts("option key shows dialog"); return 2; }
  setenv("NPS5_SETTINGS_DIALOG","1",1);
  if (!ps4_settings_dialog_wanted(false, false)) { puts("opt-in does not show"); return 3; }
  if (ps4_settings_dialog_wanted(true, false)) { puts("shown with stored settings"); return 4; }
  setenv("PS4_NO_DIALOGS","1",1);
  if (ps4_settings_dialog_wanted(false, false)) { puts("NO_DIALOGS ignored"); return 5; }
  return 0; }`
	if err := os.WriteFile(src, []byte(code), 0o644); err != nil {
		t.Fatal(err)
	}
	inc := filepath.Join("..", "runtime", "modules", "libSceSystemService")
	bin := filepath.Join(dir, "t")
	if out, err := exec.Command("clang", "-I"+inc, src, "-o", bin).CombinedOutput(); err != nil {
		t.Skipf("no clang: %v %s", err, out)
	}
	if out, err := exec.Command(bin).CombinedOutput(); err != nil {
		t.Fatalf("%v: %s", err, out)
	}
}

// Game patches are settings (core/nps5_patches.h): the real ps4_settings.m resolves them from the
// per-game settings file and the environment before the guest starts, and logs the result.
func TestSettingsResolveGamePatches(t *testing.T) {
	if runtime.GOOS != "darwin" {
		t.Skip("Cocoa settings module")
	}
	dir := t.TempDir()
	src := filepath.Join(dir, "t.m")
	code := `#include <stdint.h>
#include <stdio.h>
#include "nps5_patches.h"
void ps4_host_settings_init(void);
const char *ps4_vfs_get_app_root(void) { return "/games/PPSA21564"; }
uint8_t nps5_guest_patch_flags[32] = {1};
static const uint64_t sites_0[] = {0x77eb996ull};
static const Nps5PatchDesc descs[] = {{"fast_lighting", "Lighting", "Fast (non-tiled)", "Full quality (tiled)", "d", "NPS5_ASTRO_FAST_LIGHTING", 0, 1, 1, sites_0}};
__attribute__((constructor)) static void ctor(void) { nps5_patches_register(1, "PPSA21564 01.007", descs, 1, nps5_guest_patch_flags, 32); }
int main(void) { ps4_host_settings_init(); printf("flag=%d\n", nps5_guest_patch_flags[0]); return 0; }`
	if err := os.WriteFile(src, []byte(code), 0o644); err != nil {
		t.Fatal(err)
	}
	rt := filepath.Join("..", "runtime")
	bin := filepath.Join(dir, "t")
	args := []string{"-fobjc-arc", "-I" + filepath.Join(rt, "core"), "-I" + filepath.Join(rt, "kernel", "vfs"), "-I" + filepath.Join(rt, "modules", "libSceSystemService"),
		src, filepath.Join(rt, "modules", "libSceSystemService", "ps4_settings.m"), filepath.Join(rt, "core", "nps5_patches.c"), "-framework", "Cocoa", "-o", bin}
	if out, err := exec.Command("clang", args...).CombinedOutput(); err != nil {
		t.Fatalf("clang: %v %s", err, out)
	}
	cases := []struct {
		cfg, env, flag, log string
	}{
		{"", "", "flag=1", "patch fast_lighting: Lighting = Fast (non-tiled) (default; 1 site, first guest 0x77eb996)"},
		{"lang=1\npro=1\nupscale=0\nhud=1\npatch.fast_lighting=0\n", "", "flag=0", "Lighting = Full quality (tiled) (settings;"},
		{"lang=1\npatch.fast_lighting=1\npatch.unknown=1\n", "", "flag=1", "Lighting = Fast (non-tiled) (default;"},
		{"patch.fast_lighting=0\n", "NPS5_ASTRO_FAST_LIGHTING=1", "flag=1", "Lighting = Fast (non-tiled) (NPS5_ASTRO_FAST_LIGHTING;"},
		{"", "NPS5_ASTRO_FAST_LIGHTING=0", "flag=0", "Lighting = Full quality (tiled) (NPS5_ASTRO_FAST_LIGHTING;"},
		{"", "NPS5_PATCH_FAST_LIGHTING=0", "flag=0", "Lighting = Full quality (tiled) (NPS5_PATCH_FAST_LIGHTING;"},
	}
	for i, c := range cases {
		data := filepath.Join(dir, "data", string(rune('a'+i)))
		if c.cfg != "" {
			os.MkdirAll(filepath.Join(data, "settings"), 0o755)
			if err := os.WriteFile(filepath.Join(data, "settings", "PPSA21564.cfg"), []byte(c.cfg), 0o644); err != nil {
				t.Fatal(err)
			}
		}
		cmd := exec.Command(bin)
		cmd.Env = []string{"PATH=" + os.Getenv("PATH"), "HOME=" + dir, "PS4RECOMP_DATA_DIR=" + data, "PS4_NO_DIALOGS=1"}
		if c.env != "" {
			cmd.Env = append(cmd.Env, c.env)
		}
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("case %d: %v: %s", i, err, out)
		}
		if !strings.Contains(string(out), c.flag) || !strings.Contains(string(out), c.log) {
			t.Errorf("case %d (cfg %q env %q): want %q and %q in:\n%s", i, c.cfg, c.env, c.flag, c.log, out)
		}
	}
}
