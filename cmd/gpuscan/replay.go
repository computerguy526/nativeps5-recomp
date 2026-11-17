package main

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
)

// AnyPS5 arm64 tools are used for anything that needs RDNA/AGC decoding; gpuscan never decodes
// ISA itself. Both helpers only run native arm64 binaries.

func findTool(build, rel string) (string, error) {
	p := filepath.Join(build, "core", "libs", "prx", "libSceAgcDriver", rel)
	if _, err := os.Stat(p); err != nil {
		return "", fmt.Errorf("%s not found (pass -anyps5-build <build-arm64 dir>)", p)
	}
	return p, nil
}

// ReplayCapture runs agc_frame_replay on an APS5_FRAME_CAPTURE directory with a fresh shader
// cache directory, so the driver recompiles every captured shader and persists its SPIR-V and
// pipeline journal; the returned directory is then scanned like any run's cache.
// NOTE: untested against a real capture (none existed when this was written).
func ReplayCapture(build, capture, icd string, timeout time.Duration) (cacheDir string, out string, err error) {
	tool, err := findTool(build, "agc_frame_replay")
	if err != nil {
		return "", "", err
	}
	tmp, err := os.MkdirTemp("", "gpuscan-replay")
	if err != nil {
		return "", "", err
	}
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, tool, capture, "--no-pace", "--cold-pipelines")
	cmd.Env = append(os.Environ(), "ANYPS5_SHADER_CACHE_DIR="+tmp)
	if icd != "" {
		m := icd
		if fi, e := os.Stat(icd); e == nil && fi.IsDir() {
			g, _ := filepath.Glob(filepath.Join(icd, "share", "vulkan", "icd.d", "*.json"))
			if len(g) > 0 {
				m = g[0]
			}
		}
		cmd.Env = append(cmd.Env, "VK_DRIVER_FILES="+m)
	}
	b, err := cmd.CombinedOutput()
	return tmp, strings.TrimSpace(string(b)), err
}

// ReplayRequests runs agc_shader_replay --spv on saved recompile requests (APS5_DUMP_SHADERS=1
// writes shader_<addr>.req) and returns the SPIR-V files it produced.
// NOTE: untested against a real .req (none existed when this was written).
func ReplayRequests(build string, reqs []string) (dir string, err error) {
	tool, err := findTool(build, "agc_shader_replay")
	if err != nil {
		return "", err
	}
	tmp, err := os.MkdirTemp("", "gpuscan-req")
	if err != nil {
		return "", err
	}
	args := append([]string{"--spv"}, reqs...)
	cmd := exec.Command(tool, args...)
	cmd.Dir = tmp
	b, err := cmd.CombinedOutput()
	if err != nil {
		return tmp, fmt.Errorf("agc_shader_replay: %v: %s", err, strings.TrimSpace(string(b)))
	}
	return tmp, nil
}
