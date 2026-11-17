// SPDX-License-Identifier: GPL-2.0-or-later

package pack

import (
	"embed"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
)

//go:embed assets/launcher.c assets/launcher_gui.m
var assets embed.FS

// Toolkit is where the prebuilt third-party pieces come from.
type Toolkit struct {
	AnyPS5Libs string // AnyPS5 build-arm64/core/libs/libs/unpatched (the .prx and libnps5_gpu.dylib)
	KKICD      string // KosmicKrisp libvulkan_kosmickrisp.dylib
	Vulkan     string // Vulkan loader
}

// bundleInput is everything assemble needs.
type bundleInput struct {
	Title   *Title
	Profile *Profile
	Out     string // output directory that gets <Name>.app
	Binary  string // linked runtime + translated code
	Image   string // guest_image.bin
	Kit     Toolkit
	Verbose bool
}

func run(name string, args ...string) (string, error) {
	out, err := exec.Command(name, args...).CombinedOutput()
	if err != nil {
		return string(out), fmt.Errorf("%s %s: %v\n%s", name, strings.Join(args, " "), err, out)
	}
	return string(out), nil
}

var depRe = regexp.MustCompile(`(?m)^\s+(\S.*?) \(compatibility version`)

func isSystem(p string) bool {
	return strings.HasPrefix(p, "/System/") || strings.HasPrefix(p, "/usr/lib/") || strings.HasPrefix(p, "/Library/Apple/")
}

// deps lists the install names a Mach-O file loads (skipping its own id line for dylibs).
func deps(path string) ([]string, error) {
	out, err := run("otool", "-L", path)
	if err != nil {
		return nil, err
	}
	var d []string
	for i, m := range depRe.FindAllStringSubmatch(out, -1) {
		if i == 0 && strings.HasSuffix(path, ".dylib") || i == 0 && strings.HasSuffix(path, ".prx") {
			// first entry of a library is its own id
			if id, _ := run("otool", "-D", path); strings.Contains(id, m[1]) {
				continue
			}
		}
		d = append(d, m[1])
	}
	return d, nil
}

func rpaths(path string) []string {
	out, _ := run("otool", "-l", path)
	var r []string
	lines := strings.Split(out, "\n")
	for i, l := range lines {
		if strings.TrimSpace(l) == "cmd LC_RPATH" && i+2 < len(lines) {
			f := strings.Fields(lines[i+2])
			if len(f) >= 2 && f[0] == "path" {
				r = append(r, f[1])
			}
		}
	}
	return r
}

// relocate makes the Mach-O at path load everything from Frameworks: absolute dependencies become
// @rpath/<base>, absolute rpaths are dropped, and the rpath that finds Frameworks is ensured.
func relocate(path, want string, search []string, copied map[string]string, frameworks string, queue *[]string) error {
	ds, err := deps(path)
	if err != nil {
		return err
	}
	for _, d := range ds {
		if isSystem(d) {
			continue
		}
		base := filepath.Base(d)
		if _, ok := copied[base]; !ok {
			src := ""
			if strings.HasPrefix(d, "/") {
				src = d
			} else { // @rpath/x, @loader_path/x
				for _, dir := range search {
					if fileExists(filepath.Join(dir, base)) {
						src = filepath.Join(dir, base)
						break
					}
				}
			}
			if src == "" || !fileExists(src) {
				return fmt.Errorf("%s: cannot find dependency %s (searched %v)", path, d, search)
			}
			if real, err := filepath.EvalSymlinks(src); err == nil {
				src = real
			}
			dst := filepath.Join(frameworks, base)
			if err := copyFile(src, dst); err != nil {
				return err
			}
			copied[base] = dst
			*queue = append(*queue, dst)
			search = append(search, filepath.Dir(src))
		}
		if d != "@rpath/"+base {
			if _, err := run("install_name_tool", "-change", d, "@rpath/"+base, path); err != nil {
				return err
			}
		}
	}
	// rpaths: absolute ones leak the build machine. Drop them first (that frees header room), then make
	// sure the one that finds Frameworks is there.
	for _, r := range rpaths(path) {
		if strings.HasPrefix(r, "/") {
			if _, err := run("install_name_tool", "-delete_rpath", r, path); err != nil {
				return err
			}
		}
	}
	if !contains(rpaths(path), want) {
		if _, err := run("install_name_tool", "-add_rpath", want, path); err != nil {
			return err
		}
	}
	return nil
}

func contains(l []string, s string) bool {
	for _, x := range l {
		if x == s {
			return true
		}
	}
	return false
}

func copyFile(src, dst string) error {
	b, err := os.ReadFile(src)
	if err != nil {
		return err
	}
	_ = os.Remove(dst)
	return os.WriteFile(dst, b, 0o755)
}

func plist(t *Title, p *Profile, exe string, icon bool) string {
	esc := func(s string) string { return strings.NewReplacer("&", "&amp;", "<", "&lt;", ">", "&gt;").Replace(s) }
	var b strings.Builder
	b.WriteString(`<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
`)
	kv := func(k, v string) { fmt.Fprintf(&b, "  <key>%s</key><string>%s</string>\n", k, esc(v)) }
	kv("CFBundleDevelopmentRegion", "en")
	kv("CFBundleExecutable", exe)
	kv("CFBundleIdentifier", "com.nativeps5."+t.ID)
	kv("CFBundleName", t.Name)
	kv("CFBundleDisplayName", t.Name)
	kv("CFBundlePackageType", "APPL")
	kv("CFBundleShortVersionString", t.Version)
	kv("CFBundleVersion", t.Version)
	kv("LSMinimumSystemVersion", "26.0")
	kv("LSApplicationCategoryType", "public.app-category.games")
	kv("NSGameControllerUsageDescription", t.Name+" reads your game controller.")
	// The dump stays where the user keeps it, often Downloads or Documents: macOS asks the first time the
	// bundle reads it (TCC), and shows these strings in the prompt.
	for _, k := range []string{"NSDownloadsFolderUsageDescription", "NSDocumentsFolderUsageDescription", "NSDesktopFolderUsageDescription", "NSRemovableVolumesUsageDescription", "NSNetworkVolumesUsageDescription"} {
		kv(k, t.Name+" reads its game files (read-only) from where you extracted them.")
	}
	if p.Microphone {
		kv("NSMicrophoneUsageDescription", t.Name+" uses the microphone for voice chat and audio input.")
	}
	if icon {
		kv("CFBundleIconFile", "AppIcon")
	}
	b.WriteString("  <key>LSSupportsGameMode</key><true/>\n  <key>NSHighResolutionCapable</key><true/>\n  <key>GCSupportsControllerUserInteraction</key><true/>\n</dict></plist>\n")
	return b.String()
}

// makeIcns converts icon0.png into an .icns with iconutil (full iconset), falling back to sips.
func makeIcns(png, dst, tmp string) error {
	set := filepath.Join(tmp, "AppIcon.iconset")
	_ = os.RemoveAll(set)
	if err := os.MkdirAll(set, 0o755); err != nil {
		return err
	}
	for _, s := range []struct {
		px   int
		name string
	}{{16, "icon_16x16"}, {32, "icon_16x16@2x"}, {32, "icon_32x32"}, {64, "icon_32x32@2x"}, {128, "icon_128x128"},
		{256, "icon_128x128@2x"}, {256, "icon_256x256"}, {512, "icon_256x256@2x"}, {512, "icon_512x512"}, {1024, "icon_512x512@2x"}} {
		if _, err := run("sips", "-z", fmt.Sprint(s.px), fmt.Sprint(s.px), png, "--out", filepath.Join(set, s.name+".png")); err != nil {
			return run1(png, dst)
		}
	}
	if _, err := run("iconutil", "-c", "icns", set, "-o", dst); err != nil {
		return run1(png, dst)
	}
	return nil
}

func run1(png, dst string) error {
	_, err := run("sips", "-s", "format", "icns", png, "--out", dst)
	return err
}

func compile(args ...string) error { _, err := run("clang", args...); return err }

// assemble writes <Out>/<Name>.app and returns its path.
func assemble(in bundleInput) (string, error) {
	t := in.Title
	name := fileName(t.Name)
	app := filepath.Join(in.Out, name+".app")
	if err := os.RemoveAll(app); err != nil {
		return "", err
	}
	contents := filepath.Join(app, "Contents")
	macos, fw, res := filepath.Join(contents, "MacOS"), filepath.Join(contents, "Frameworks"), filepath.Join(contents, "Resources")
	for _, d := range []string{macos, fw, res, filepath.Join(res, "vulkan", "icd.d")} {
		if err := os.MkdirAll(d, 0o755); err != nil {
			return "", err
		}
	}
	tmp, err := os.MkdirTemp("", "nps5-pack")
	if err != nil {
		return "", err
	}
	defer os.RemoveAll(tmp)

	// Executables: the launcher stub is CFBundleExecutable; the translated title sits next to it.
	src := filepath.Join(tmp, "launcher.c")
	b, _ := assets.ReadFile("assets/launcher.c")
	if err := os.WriteFile(src, b, 0o644); err != nil {
		return "", err
	}
	if err := compile("-O2", "-target", "arm64-apple-macos26.0", src, "-o", filepath.Join(macos, name)); err != nil {
		return "", err
	}
	title := filepath.Join(macos, "nps5-title")
	if err := copyFile(in.Binary, title); err != nil {
		return "", err
	}

	// Frameworks: AnyPS5 libraries, KosmicKrisp, the Vulkan loader and, transitively, whatever else
	// they load from outside /System and /usr/lib (SDL2, SPIRV-Tools, glslang, FFmpeg, FreeType, ...).
	copied := map[string]string{}
	var queue []string
	search := []string{in.Kit.AnyPS5Libs, "/opt/homebrew/lib"}
	add := func(p string) error {
		base := filepath.Base(p)
		if real, err := filepath.EvalSymlinks(p); err == nil {
			p = real
		}
		search = append(search, filepath.Dir(p))
		dst := filepath.Join(fw, base)
		copied[base] = dst
		queue = append(queue, dst)
		return copyFile(p, dst)
	}
	for _, l := range []string{"libSceAgc.prx", "libSceAgcDriver.prx", "libkernel.prx", "libc.prx", "libnps5_gpu.dylib"} {
		if err := add(filepath.Join(in.Kit.AnyPS5Libs, l)); err != nil {
			return "", err
		}
	}
	if err := add(in.Kit.KKICD); err != nil {
		return "", err
	}
	// the loader keeps the name the runtime asks for (libvulkan.dylib via SDL_VULKAN_LIBRARY)
	if err := add(in.Kit.Vulkan); err != nil {
		return "", err
	}
	var rel []string
	if err := relocate(title, "@executable_path/../Frameworks", search, copied, fw, &queue); err != nil {
		return "", err
	}
	for len(queue) > 0 {
		f := queue[0]
		queue = queue[1:]
		rel = append(rel, f)
		if _, err := run("install_name_tool", "-id", "@rpath/"+filepath.Base(f), f); err != nil {
			return "", err
		}
		if err := relocate(f, "@loader_path", search, copied, fw, &queue); err != nil {
			return "", err
		}
		// sdl2-compat (SDL2 on top of SDL3) dlopens @loader_path/libSDL3.dylib by name when SDL starts, and
		// shows a modal alert if it is missing: it is not an otool dependency, so add it by hand.
		if strings.HasPrefix(filepath.Base(f), "libSDL2") {
			if _, ok := copied["libSDL3.dylib"]; !ok {
				for _, d := range []string{"/opt/homebrew/opt/sdl3/lib", "/opt/homebrew/lib"} {
					if fileExists(filepath.Join(d, "libSDL3.dylib")) {
						if err := add(filepath.Join(d, "libSDL3.dylib")); err != nil {
							return "", err
						}
						break
					}
				}
			}
		}
	}

	// Resources.
	vk := filepath.Base(in.Kit.Vulkan)
	icd := map[string]any{"file_format_version": "1.0.1", "ICD": map[string]any{"library_path": "../../../Frameworks/" + filepath.Base(in.Kit.KKICD), "api_version": "1.4.0"}}
	jb, _ := json.MarshalIndent(icd, "", "  ")
	if err := os.WriteFile(filepath.Join(res, "vulkan", "icd.d", "kosmickrisp_icd.json"), jb, 0o644); err != nil {
		return "", err
	}
	if err := copyFile(in.Image, filepath.Join(res, "guest_image.bin")); err != nil {
		return "", err
	}
	hasIcon := false
	if t.Icon != "" {
		hasIcon = makeIcns(t.Icon, filepath.Join(res, "AppIcon.icns"), tmp) == nil
	}
	pb, _ := json.MarshalIndent(map[string]any{"title": t, "profile": in.Profile}, "", "  ")
	if err := os.WriteFile(filepath.Join(res, "profile.json"), pb, 0o644); err != nil {
		return "", err
	}
	env := []string{
		"# nps5 launch environment: ${CONTENTS} ${SUPPORT} ${HOME} are expanded by the launcher; variables already set win.",
		"NPS5_TITLE_ID=" + t.ID,
		"NPS5_TITLE=" + sanitize(name),
		"PS4_APP_DIR=" + t.Dump, // the user's dump: read-only /app0, never copied
		"VK_DRIVER_FILES=${CONTENTS}/Resources/vulkan/icd.d/kosmickrisp_icd.json",
		"VK_ICD_FILENAMES=${CONTENTS}/Resources/vulkan/icd.d/kosmickrisp_icd.json",
		"SDL_VULKAN_LIBRARY=${CONTENTS}/Frameworks/" + vk,
		"PS4RECOMP_DATA_DIR=${SUPPORT}/userdata",
		"NPS5_USERDATA_DIR=${SUPPORT}/userdata",
		"NPS5_SAVEDATA_DIR=${SUPPORT}/savedata",
		"PS4RECOMP_CACHE_DIR=${SUPPORT}/cache",
		"ANYPS5_SHADER_CACHE_DIR=${SUPPORT}/gpu-cache",
		"NPS5_GPU_CACHE_DIR=${SUPPORT}/gpu-cache",
		"NPS5_TIER1_CACHE=${SUPPORT}/tier1_cache",
		"NPS5_RUNS_DIR=${SUPPORT}/runs",
		"NPS5_MKDIR=${SUPPORT}/userdata",
		"NPS5_MKDIR=${SUPPORT}/savedata",
	}
	keys := make([]string, 0, len(in.Profile.Env))
	for k := range in.Profile.Env {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		env = append(env, k+"="+in.Profile.Env[k])
	}
	if err := os.WriteFile(filepath.Join(res, "launch.env"), []byte(strings.Join(env, "\n")+"\n"), 0o644); err != nil {
		return "", err
	}
	if err := os.WriteFile(filepath.Join(contents, "Info.plist"), []byte(plist(t, in.Profile, name, hasIcon)), 0o644); err != nil {
		return "", err
	}

	// Ad-hoc signature, inside out. No hardened runtime (so no entitlements): tier-1 fragments are
	// ad-hoc signed dylibs loaded with dlopen and the Vulkan ICD/AnyPS5 libraries are ad-hoc too, which
	// library validation would refuse; without it nothing special is needed (docs/m3/packaging.md).
	for _, f := range rel {
		if _, err := run("codesign", "--force", "--sign", "-", "--timestamp=none", f); err != nil {
			return "", err
		}
	}
	for _, f := range []string{title, filepath.Join(macos, name)} {
		if _, err := run("codesign", "--force", "--sign", "-", "--timestamp=none", f); err != nil {
			return "", err
		}
	}
	if _, err := run("codesign", "--force", "--sign", "-", "--timestamp=none", app); err != nil {
		return "", err
	}
	if _, err := run("codesign", "--verify", "--strict", app); err != nil {
		return "", err
	}
	return app, nil
}

// BuildLauncher compiles the small AppKit game list into <out>/NPS5 Launcher.app.
func BuildLauncher(out string) (string, error) {
	app := filepath.Join(out, "NPS5 Launcher.app")
	_ = os.RemoveAll(app)
	if err := os.MkdirAll(filepath.Join(app, "Contents", "MacOS"), 0o755); err != nil {
		return "", err
	}
	tmp, err := os.MkdirTemp("", "nps5-launcher")
	if err != nil {
		return "", err
	}
	defer os.RemoveAll(tmp)
	src := filepath.Join(tmp, "launcher_gui.m")
	b, _ := assets.ReadFile("assets/launcher_gui.m")
	if err := os.WriteFile(src, b, 0o644); err != nil {
		return "", err
	}
	if err := compile("-O2", "-fobjc-arc", "-target", "arm64-apple-macos26.0", src, "-framework", "Cocoa", "-o", filepath.Join(app, "Contents", "MacOS", "NPS5 Launcher")); err != nil {
		return "", err
	}
	pl := plist(&Title{ID: "launcher", Name: "NPS5 Launcher", Version: "1.0"}, &Profile{}, "NPS5 Launcher", false)
	pl = strings.Replace(pl, "com.nativeps5.launcher", "com.nativeps5.launcher", 1)
	if err := os.WriteFile(filepath.Join(app, "Contents", "Info.plist"), []byte(pl), 0o644); err != nil {
		return "", err
	}
	if _, err := run("codesign", "--force", "--sign", "-", "--timestamp=none", app); err != nil {
		return "", err
	}
	return app, nil
}
