// Package diag implements the run-bundle, census/imports/addrmap reports and the
// `doctor`, `last` and `addr2guest` helpers of SPEC 8.2.
package diag

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strings"
	"sync"
	"time"
)

// GitCommit can be stamped with -ldflags "-X ps4-recomp/pkg/diag.GitCommit=...".
var GitCommit string

// Levels in increasing severity.
var levelNames = []string{"trace", "debug", "info", "warn", "error"}

// Bundle is a runs/<timestamp>-<title>-<phase>/ directory with a manifest and a JSONL log.
type Bundle struct {
	Dir   string
	Phase string
	Title string

	mu    sync.Mutex
	logf  *os.File
	start time.Time
	man   Manifest
}

// Manifest is manifest.json.
type Manifest struct {
	Phase      string            `json:"phase"`
	Title      string            `json:"title"`
	Started    string            `json:"started"`
	Finished   string            `json:"finished,omitempty"`
	WallMs     int64             `json:"wall_ms,omitempty"`
	Result     string            `json:"result,omitempty"`
	Error      string            `json:"error,omitempty"`
	Tool       ToolInfo          `json:"tool"`
	Inputs     []FileHash        `json:"inputs,omitempty"`
	Settings   map[string]any    `json:"settings,omitempty"`
	Env        map[string]string `json:"env,omitempty"`
	Host       HostInfo          `json:"host"`
	Artifacts  []string          `json:"artifacts,omitempty"`
	ExtraStats map[string]any    `json:"stats,omitempty"`
}

type ToolInfo struct {
	Name      string `json:"name"`
	GitCommit string `json:"git_commit"`
	GitDirty  bool   `json:"git_dirty"`
	GoVersion string `json:"go_version"`
}

type FileHash struct {
	Path   string `json:"path"`
	SHA256 string `json:"sha256"`
	Size   int64  `json:"size"`
}

type HostInfo struct {
	Model   string `json:"model"`
	OSBuild string `json:"os_build"`
	OS      string `json:"os"`
	Arch    string `json:"arch"`
	NumCPU  int    `json:"ncpu"`
}

var titleSanitize = regexp.MustCompile(`[^A-Za-z0-9._-]+`)

// RunsRoot is where bundles go: $NPS5_RUNS_DIR or ./runs.
func RunsRoot() string {
	if d := os.Getenv("NPS5_RUNS_DIR"); d != "" {
		return d
	}
	return "runs"
}

// SanitizeTitle makes a title safe as a path component.
func SanitizeTitle(t string) string {
	t = titleSanitize.ReplaceAllString(t, "_")
	t = strings.Trim(t, "._-")
	if t == "" {
		t = "untitled"
	}
	if len(t) > 48 {
		t = t[:48]
	}
	return t
}

// NewBundle creates root/<timestamp>-<title>-<phase>/ with an initial manifest and updates the
// latest symlinks. The timestamp has millisecond resolution so bundles never collide.
func NewBundle(root, title, phase string) (*Bundle, error) {
	if root == "" {
		root = RunsRoot()
	}
	title = SanitizeTitle(title)
	now := time.Now()
	var dir string
	for i := 0; ; i++ {
		name := fmt.Sprintf("%s-%s-%s", now.Format("20060102T150405.000"), title, phase)
		if i > 0 {
			name = fmt.Sprintf("%s-%d", name, i)
		}
		dir = filepath.Join(root, name)
		if err := os.MkdirAll(root, 0o755); err != nil {
			return nil, err
		}
		if err := os.Mkdir(dir, 0o755); err == nil {
			break
		} else if !os.IsExist(err) || i > 50 {
			return nil, err
		}
	}
	lf, err := os.OpenFile(filepath.Join(dir, "log.jsonl"), os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644)
	if err != nil {
		return nil, err
	}
	b := &Bundle{Dir: dir, Phase: phase, Title: title, logf: lf, start: now}
	b.man = Manifest{
		Phase: phase, Title: title, Started: now.UTC().Format(time.RFC3339Nano),
		Tool: toolInfo(), Host: hostInfo(), Env: EnvSubset(os.Environ()),
	}
	UpdateLatest(root, filepath.Base(dir), phase)
	return b, nil
}

// UpdateLatest points root/latest and root/latest-<phase> at the bundle.
func UpdateLatest(root, name, phase string) {
	for _, l := range []string{"latest", "latest-" + phase} {
		p := filepath.Join(root, l)
		tmp := p + ".tmp"
		_ = os.Remove(tmp)
		if os.Symlink(name, tmp) == nil {
			_ = os.Rename(tmp, p)
		}
	}
}

// EnvSubset keeps the environment variables that influence translation or a run.
func EnvSubset(env []string) map[string]string {
	out := map[string]string{}
	for _, kv := range env {
		k, v, _ := strings.Cut(kv, "=")
		if strings.HasPrefix(k, "NPS5_") || strings.HasPrefix(k, "PS4_") || strings.HasPrefix(k, "PS4RECOMP_") ||
			k == "GOFLAGS" || k == "MTL_DEBUG_LAYER" || strings.HasPrefix(k, "VK_") {
			out[k] = v
		}
	}
	return out
}

func sysctl(name string) string {
	out, err := exec.Command("/usr/sbin/sysctl", "-n", name).Output()
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(out))
}

func hostInfo() HostInfo {
	return HostInfo{Model: sysctl("hw.model"), OSBuild: sysctl("kern.osversion"), OS: runtime.GOOS,
		Arch: runtime.GOARCH, NumCPU: runtime.NumCPU()}
}

func toolInfo() ToolInfo {
	ti := ToolInfo{Name: "nps5/ps4-recomp", GoVersion: runtime.Version(), GitCommit: GitCommit}
	var dirs []string
	if exe, err := os.Executable(); err == nil {
		dirs = append(dirs, filepath.Dir(exe))
	}
	if wd, err := os.Getwd(); err == nil {
		dirs = append(dirs, wd)
	}
	for _, d := range dirs {
		out, err := exec.Command("git", "-C", d, "rev-parse", "HEAD").Output()
		if err != nil {
			continue
		}
		ti.GitCommit = strings.TrimSpace(string(out))
		if st, err := exec.Command("git", "-C", d, "status", "--porcelain", "--untracked-files=no").Output(); err == nil {
			ti.GitDirty = len(strings.TrimSpace(string(st))) > 0
		}
		break
	}
	return ti
}

// HashFile returns the sha256 of a file.
func HashFile(path string) (FileHash, error) {
	f, err := os.Open(path)
	if err != nil {
		return FileHash{}, err
	}
	defer f.Close()
	h := sha256.New()
	n, err := io.Copy(h, f)
	if err != nil {
		return FileHash{}, err
	}
	return FileHash{Path: path, SHA256: hex.EncodeToString(h.Sum(nil)), Size: n}, nil
}

// AddInput records an input file hash in the manifest.
func (b *Bundle) AddInput(path string) {
	if fh, err := HashFile(path); err == nil {
		if abs, err := filepath.Abs(path); err == nil {
			fh.Path = abs
		}
		b.man.Inputs = append(b.man.Inputs, fh)
	} else {
		b.Log("loader", "warn", "cannot hash input", map[string]any{"path": path, "err": err.Error()})
	}
}

// SetSetting records a setting in the manifest.
func (b *Bundle) SetSetting(k string, v any) {
	if b.man.Settings == nil {
		b.man.Settings = map[string]any{}
	}
	b.man.Settings[k] = v
}

// SetStat records a result statistic in the manifest.
func (b *Bundle) SetStat(k string, v any) {
	if b.man.ExtraStats == nil {
		b.man.ExtraStats = map[string]any{}
	}
	b.man.ExtraStats[k] = v
}

// WriteJSON writes a JSON artefact into the bundle and lists it in the manifest.
func (b *Bundle) WriteJSON(name string, v any) error {
	data, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		return err
	}
	return b.WriteFile(name, append(data, '\n'))
}

// WriteFile writes a file into the bundle and lists it in the manifest.
func (b *Bundle) WriteFile(name string, data []byte) error {
	if err := os.WriteFile(filepath.Join(b.Dir, name), data, 0o644); err != nil {
		return err
	}
	b.addArtifact(name)
	return nil
}

func (b *Bundle) addArtifact(name string) {
	b.mu.Lock()
	defer b.mu.Unlock()
	for _, a := range b.man.Artifacts {
		if a == name {
			return
		}
	}
	b.man.Artifacts = append(b.man.Artifacts, name)
}

// Log appends a JSONL record. Levels: trace, debug, info, warn, error.
func (b *Bundle) Log(subsys, level, msg string, fields map[string]any) {
	rec := map[string]any{"t": float64(time.Since(b.start).Microseconds()) / 1e6, "level": level, "subsys": subsys, "msg": msg}
	for k, v := range fields {
		rec[k] = v
	}
	data, err := json.Marshal(rec)
	if err != nil {
		return
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	b.logf.Write(append(data, '\n'))
}

// Close finalises the manifest. err is the translate/run error, if any.
func (b *Bundle) Close(err error) {
	now := time.Now()
	b.man.Finished = now.UTC().Format(time.RFC3339Nano)
	b.man.WallMs = now.Sub(b.start).Milliseconds()
	b.man.Result = "ok"
	if err != nil {
		b.man.Result = "error"
		b.man.Error = err.Error()
	}
	b.mu.Lock()
	sort.Strings(b.man.Artifacts)
	data, _ := json.MarshalIndent(b.man, "", "  ")
	b.mu.Unlock()
	_ = os.WriteFile(filepath.Join(b.Dir, "manifest.json"), append(data, '\n'), 0o644)
	b.logf.Close()
}

// WriteManifest writes the manifest now (so a killed process still leaves one).
func (b *Bundle) WriteManifest() {
	b.mu.Lock()
	data, _ := json.MarshalIndent(b.man, "", "  ")
	b.mu.Unlock()
	_ = os.WriteFile(filepath.Join(b.Dir, "manifest.json"), append(data, '\n'), 0o644)
}

// ManifestSnapshot returns the manifest being built (used by tooling that needs input hashes).
func (b *Bundle) ManifestSnapshot() Manifest { return b.man }
