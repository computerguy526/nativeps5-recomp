// SPDX-License-Identifier: GPL-2.0-or-later

package pack

import (
	"crypto/sha256"
	"fmt"
	"io"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"

	"ps4-recomp/pkg/cli"
)

// Options of `nps5 build`.
type Options struct {
	TitleDir string
	Out      string // directory that receives <Name>.app (default: current directory)
	Profile  string // --profile
	Jobs     int
	Force    bool // retranslate even when nothing changed
	Log      io.Writer
}

// FindRepo locates the source tree (pkg/runtime, tools/): $NPS5_REPO, then upwards from the
// executable and from the working directory.
func FindRepo() (string, error) {
	var starts []string
	if r := os.Getenv("NPS5_REPO"); r != "" {
		starts = append(starts, r)
	}
	if exe, err := os.Executable(); err == nil {
		if r, err := filepath.EvalSymlinks(exe); err == nil {
			exe = r
		}
		starts = append(starts, filepath.Dir(exe))
	}
	if wd, err := os.Getwd(); err == nil {
		starts = append(starts, wd)
	}
	for _, s := range starts {
		for d, i := s, 0; i < 6; i, d = i+1, filepath.Dir(d) {
			if fileExists(filepath.Join(d, "pkg", "runtime", "core", "recomp_runtime.h")) && fileExists(filepath.Join(d, "tools", "m1_native_build.sh")) {
				return d, nil
			}
		}
	}
	return "", fmt.Errorf("cannot find the nativeps5 source tree (set NPS5_REPO)")
}

// FindToolkit resolves the AnyPS5 libraries, the KosmicKrisp ICD and the Vulkan loader:
// NPS5_ANYPS5_BUILD, NPS5_KK_PREFIX, NPS5_VULKAN_LIB, else the sibling checkouts of this workspace.
func FindToolkit(repo string) (Toolkit, error) {
	var k Toolkit
	any := os.Getenv("NPS5_ANYPS5_BUILD")
	if any == "" {
		for _, c := range []string{"../anyps5-int/build-arm64", "../anyps5/build-arm64"} {
			if p := filepath.Join(repo, c); fileExists(p) {
				any = p
				break
			}
		}
	}
	if any == "" {
		return k, fmt.Errorf("AnyPS5 build not found: set NPS5_ANYPS5_BUILD to its build-arm64 directory. " +
			"nps5 build packages PS5 titles, which need AnyPS5's arm64 host libraries (also with NPS5_GPU=kyty); " +
			"AnyPS5 is an optional external component and is not part of the public nativeps5 repositories (README)")
	}
	k.AnyPS5Libs = filepath.Join(any, "core", "libs", "libs", "unpatched")
	if !fileExists(filepath.Join(k.AnyPS5Libs, "libSceAgcDriver.prx")) {
		return k, fmt.Errorf("%s has no libSceAgcDriver.prx (NPS5_ANYPS5_BUILD must be an arm64 build)", k.AnyPS5Libs)
	}
	kk := os.Getenv("NPS5_KK_PREFIX")
	if kk == "" {
		kk = filepath.Join(repo, "..", "..", "work", "s3", "install")
	}
	k.KKICD = filepath.Join(kk, "lib", "libvulkan_kosmickrisp.dylib")
	if !fileExists(k.KKICD) {
		return k, fmt.Errorf("KosmicKrisp not found at %s: set NPS5_KK_PREFIX (mesa/build-kosmickrisp.sh install prefix)", k.KKICD)
	}
	k.Vulkan = os.Getenv("NPS5_VULKAN_LIB")
	if k.Vulkan == "" {
		k.Vulkan = "/opt/homebrew/lib/libvulkan.1.dylib"
	}
	if !fileExists(k.Vulkan) {
		return k, fmt.Errorf("Vulkan loader %s not found (brew install vulkan-loader, or NPS5_VULKAN_LIB)", k.Vulkan)
	}
	return k, nil
}

// treeStamp fingerprints the runtime sources: path, size, mtime.
func treeStamp(dir string) string {
	h := sha256.New()
	_ = filepath.WalkDir(dir, func(p string, d fs.DirEntry, err error) error {
		if err == nil && !d.IsDir() {
			if fi, err := d.Info(); err == nil {
				fmt.Fprintf(h, "%s %d %d\n", p, fi.Size(), fi.ModTime().UnixNano())
			}
		}
		return nil
	})
	return fmt.Sprintf("%x", h.Sum(nil))[:16]
}

func fileStamp(p string) string {
	fi, err := os.Stat(p)
	if err != nil {
		return "none"
	}
	return fmt.Sprintf("%d-%d", fi.Size(), fi.ModTime().UnixNano())
}

// Build translates, links and packages one title. It returns the path of the .app.
func Build(o Options) (string, error) {
	logf := func(f string, a ...any) { fmt.Fprintf(o.Log, "[nps5] "+f+"\n", a...) }
	repo, err := FindRepo()
	if err != nil {
		return "", err
	}
	t, err := ReadTitle(o.TitleDir)
	if err != nil {
		return "", err
	}
	prof, profPath, err := LoadProfile(o.Profile, t.ID, repo)
	if err != nil {
		return "", err
	}
	kit, err := FindToolkit(repo)
	if err != nil {
		return "", err
	}
	out := o.Out
	if out == "" {
		out = "."
	}
	if out, err = filepath.Abs(out); err != nil {
		return "", err
	}
	if err := os.MkdirAll(out, 0o755); err != nil {
		return "", err
	}
	if o.Jobs < 1 {
		o.Jobs = runtime.NumCPU()
	}
	logf("%s (%s) v%s, ps5=%v, dump %s", t.Name, t.ID, t.Version, t.PS5, t.Dump)
	if profPath != "" {
		logf("profile %s", profPath)
	}

	work := filepath.Join(TitleDir(t.ID), "build")
	native := filepath.Join(work, "native")
	args, err := prof.Args(cli.SupportsFlag)
	if err != nil {
		return "", err
	}
	if cli.SupportsFlag("obj-store") {
		args = append(args, "-obj-store", filepath.Join(SupportRoot(), "objstore"))
	}
	args = append([]string{"-compile=false", "-j", fmt.Sprint(o.Jobs), "-o", work, "-app-dir", t.Dump}, args...)
	args = append(args, t.Eboot)
	self, _ := os.Executable()
	stamp := fmt.Sprintf("%v|%s|%s|%s|%s|%s", args, fileStamp(t.Eboot), fileStamp(self), treeStamp(filepath.Join(repo, "pkg", "runtime")), fileStamp(kit.AnyPS5Libs+"/libSceAgcDriver.prx"), fileStamp(filepath.Join(repo, "tools", "m1_native_build.sh")))
	stampFile := filepath.Join(work, "nps5.stamp")
	if old, _ := os.ReadFile(stampFile); !o.Force && string(old) == stamp && fileExists(native) {
		logf("translation up to date (%s); use --force to redo it", work)
	} else {
		_ = os.Remove(stampFile)
		// before the translation, so everything after it works on the small copy (docs/reference/disk-space.md)
		MaybeCompressGame(t, repo, DefaultZipEnv(o.Log))
		if err := os.MkdirAll(TitleDir(t.ID), 0o755); err != nil {
			return "", err
		}
		// the translator finds pkg/runtime relative to the working directory
		if wd, _ := os.Getwd(); wd != repo {
			defer os.Chdir(wd)
			if err := os.Chdir(repo); err != nil {
				return "", err
			}
		}
		_ = os.Setenv(cli.AnyPS5BuildEnv, filepath.Dir(filepath.Dir(filepath.Dir(filepath.Dir(kit.AnyPS5Libs)))))
		logf("translating: ps4-recomp %s", strings.Join(args, " "))
		if err := cli.Execute(args); err != nil {
			return "", fmt.Errorf("translate: %w", err)
		}
		logf("compiling and linking the runtime")
		cmd := exec.Command(filepath.Join(repo, "tools", "m1_native_build.sh"), work, native)
		cmd.Env = append(os.Environ(), fmt.Sprintf("M1_JOBS=%d", o.Jobs))
		cmd.Stdout, cmd.Stderr = o.Log, o.Log
		if err := cmd.Run(); err != nil {
			return "", fmt.Errorf("native build: %w", err)
		}
		if err := os.WriteFile(stampFile, []byte(stamp), 0o644); err != nil {
			return "", err
		}
	}

	logf("assembling the bundle")
	app, err := assemble(bundleInput{Title: t, Profile: prof, Out: out, Binary: native, Image: filepath.Join(work, "guest_image.bin"), Kit: kit})
	if err != nil {
		return "", err
	}
	if err := writeRecord(t, app, prof.Name); err != nil {
		return "", err
	}
	logf("built %s", app)
	return app, nil
}
