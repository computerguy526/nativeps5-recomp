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

// A choice setting (core/nps5_patches.h, e.g. ASTRO BOT "Upscaling") through the real
// ps4_settings.m: "patch.<key>=<option name>" of the per-game settings file next to the integer
// keys and a boolean patch, the environment over the file, the value slots the translated code
// reads, and the host environment an option implies (set before the GPU backend starts).
func TestSettingsResolveChoicePatch(t *testing.T) {
	if runtime.GOOS != "darwin" {
		t.Skip("Cocoa settings module")
	}
	dir := t.TempDir()
	src := filepath.Join(dir, "t.m")
	code := `#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "nps5_patches.h"
void ps4_host_settings_init(void);
const char *ps4_vfs_get_app_root(void) { return "/games/PPSA21564"; }
uint8_t nps5_guest_patch_flags[32] = {1};
static const uint64_t sites_0[] = {0x77eb996ull};
static const Nps5PatchDesc descs[] = {{"fast_lighting", "Lighting", "Fast (non-tiled)", "Full quality (tiled)", "d", "NPS5_ASTRO_FAST_LIGHTING", 0, 1, 1, sites_0}};
uint64_t nps5_guest_patch_values[64] = {0x44f00000ull, 0x43800000780ull};
static const char *const names[] = {"off", "metalfx900", "metalfx720"};
static const char *const labels[] = {"Off (1080p, game upscaler)", "MetalFX 900p", "MetalFX 720p"};
static const char *const envs[] = {"", "KYTY_METALFX=spatial", "KYTY_METALFX=spatial"};
static const uint64_t v0[] = {0x44f00000ull, 0x44c80000ull, 0x44a00000ull}, v1[] = {0x43800000780ull, 0x38400000640ull, 0x2d000000500ull};
static const Nps5ChoiceValue vals[] = {{0, 1, 0x77be6f6ull, v0}, {1, 0, 0x77be705ull, v1}};
static const Nps5ChoiceDesc choices[] = {{"upscaling", "Upscaling", "d", "NPS5_ASTRO_UPSCALING", 3, 0, 2, names, labels, envs, vals}};
__attribute__((constructor)) static void ctor(void) {
  nps5_patches_register(1, "PPSA21564 01.007", descs, 1, nps5_guest_patch_flags, 32);
  nps5_patch_choices_register(1, "PPSA21564 01.007", choices, 1, nps5_guest_patch_values, 64);
}
int main(void) { ps4_host_settings_init(); const char *m = getenv("KYTY_METALFX");
  printf("flag=%d w=%llx size=%llx metalfx=%s\n", nps5_guest_patch_flags[0], (unsigned long long)nps5_guest_patch_values[0],
         (unsigned long long)nps5_guest_patch_values[1], m ? m : "-"); return 0; }`
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
		cfg, env string
		want     []string
	}{
		{"", "", []string{"flag=1 w=44f00000 size=43800000780 metalfx=-", "patch upscaling: Upscaling = Off (1080p, game upscaler) (default; 2 value sites, first guest 0x77be6f6)"}},
		{"lang=1\npro=1\nupscale=0\nhud=1\npatch.fast_lighting=0\npatch.upscaling=metalfx900\n", "",
			[]string{"flag=0 w=44c80000 size=38400000640 metalfx=spatial", "Upscaling = MetalFX 900p (settings; 2 value sites, first guest 0x77be6f6; env KYTY_METALFX=spatial)", "Lighting = Full quality (tiled) (settings;"}},
		{"patch.upscaling=metalfx720\r\npatch.fast_lighting=1\n", "", []string{"flag=1 w=44a00000 size=2d000000500 metalfx=spatial", "Upscaling = MetalFX 720p (settings;"}},
		{"patch.upscaling=2\n", "", []string{"w=44a00000 size=2d000000500", "Upscaling = MetalFX 720p (settings;"}},
		{"patch.upscaling=fsr3\nlang=1\n", "", []string{"w=44f00000 size=43800000780 metalfx=-", "Upscaling = Off (1080p, game upscaler) (default;"}},
		{"patch.upscaling=metalfx900\n", "NPS5_ASTRO_UPSCALING=off", []string{"w=44f00000 size=43800000780 metalfx=-", "Upscaling = Off (1080p, game upscaler) (NPS5_ASTRO_UPSCALING;"}},
		{"", "NPS5_ASTRO_UPSCALING=metalfx720", []string{"w=44a00000 size=2d000000500 metalfx=spatial", "Upscaling = MetalFX 720p (NPS5_ASTRO_UPSCALING;"}},
		{"", "NPS5_PATCH_UPSCALING=METALFX900", []string{"w=44c80000 size=38400000640", "Upscaling = MetalFX 900p (NPS5_PATCH_UPSCALING;"}},
		{"patch.upscaling=metalfx900\n", "KYTY_METALFX=temporal", []string{"w=44c80000 size=38400000640 metalfx=temporal", "env KYTY_METALFX kept from the environment)"}},
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
		for _, w := range c.want {
			if !strings.Contains(string(out), w) {
				t.Errorf("case %d (cfg %q env %q): want %q in:\n%s", i, c.cfg, c.env, w, out)
			}
		}
	}
}
