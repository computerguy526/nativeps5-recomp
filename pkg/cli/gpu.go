package cli

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"ps4-recomp/pkg/emitter"
)

// -gpu selects the graphics/video/input backend of the translated title (docs/m2/anyps5-integration.md).
//
//	off      (default) the PS4Recomp HLE modules, as before.
//	anyps5   imports of libSceAgc, libSceAgcDriver, libSceVideoOut, libScePad and libSceUserService are
//	         routed by NID to AnyPS5's arm64 libraries (generated thunks, pkg/thunkgen/anyps5.go), the
//	         runtime is built with -DNPS5_HAVE_ANYPS5_AGC=1 and linked against them.
const (
	gpuOff     = "off"
	gpuAnyPS5  = "anyps5"
	anyps5Env  = "NPS5_ANYPS5_BUILD" // AnyPS5 arm64 build directory (the one containing core/libs)
	anyps5Libs = "core/libs/libs/unpatched"
)

// gpuMode is the backend of the current run, like the address mode a process-wide property of the
// translation (set once in Run).
var gpuMode = gpuOff

// setGPU validates and applies -gpu. It rebinds the import table, so it must run before emission.
func setGPU(mode string) error {
	switch mode {
	case "", gpuOff:
		gpuMode = gpuOff
		return nil
	case gpuAnyPS5:
		if err := emitter.EnableAnyPS5Shims(); err != nil {
			return err
		}
		gpuMode = gpuAnyPS5
		return nil
	}
	return fmt.Errorf("unknown -gpu %q (want off or anyps5)", mode)
}

// gpuCFlags are the extra compile flags of the runtime for the selected backend.
func gpuCFlags() []string {
	if gpuMode == gpuAnyPS5 {
		return []string{"-DNPS5_HAVE_ANYPS5_AGC=1"}
	}
	return nil
}

// anyps5BuildDir finds the AnyPS5 arm64 build directory: $NPS5_ANYPS5_BUILD, else ./build-arm64 of a
// sibling anyps5 checkout is not guessed; the variable is required.
func anyps5BuildDir() (string, error) {
	d := os.Getenv(anyps5Env)
	if d == "" {
		return "", fmt.Errorf("-gpu anyps5 needs %s (the AnyPS5 build-arm64 directory built with -DAPS5_HOST_ARM64=ON). "+
			"AnyPS5 is an optional external component and is not part of the public nativeps5 repositories: "+
			"without it use -gpu off (README, \"Graphics and system-library back ends\")", anyps5Env)
	}
	if _, err := os.Stat(filepath.Join(d, anyps5Libs, "libSceAgcDriver.prx")); err != nil {
		return "", fmt.Errorf("%s=%s: %w", anyps5Env, d, err)
	}
	return d, nil
}

// gpuLDFlags are the extra link flags: AnyPS5's libraries and what they need from the host.
func gpuLDFlags() ([]string, error) {
	if gpuMode != gpuAnyPS5 {
		return nil, nil
	}
	b, err := anyps5BuildDir()
	if err != nil {
		return nil, err
	}
	lib := filepath.Join(b, anyps5Libs)
	var f []string
	// Dylibs: loaded at start-up through @rpath. The prx are what AnyPS5 builds; their dependencies
	// (libkernel.prx, libc.prx, glslang, SDL2, ...) are found through the same rpath/-L.
	for _, p := range []string{"libSceAgc.prx", "libSceAgcDriver.prx", "libkernel.prx", "libc.prx"} {
		f = append(f, filepath.Join(lib, p))
	}
	f = append(f, "-Wl,-rpath,"+lib, "-Wl,-rpath,/opt/homebrew/lib")
	// VideoOut + Pad + UserService (+ Mouse/Keyboard/Ime, the PNG decoder, SDL2) are one dylib: their exported names
	// equal C functions the runtime defines itself, so they are reached through dlsym (nps5_anyps5_startup).
	f = append(f, filepath.Join(lib, "libnps5_gpu.dylib"))
	f = append(f, "-lc++", "-framework", "CoreFoundation")
	return f, nil
}

// gpuKey is a fingerprint of the GPU build options for the runtime object cache.
func gpuKey() string { return strings.Join(gpuCFlags(), " ") }
