// SPDX-License-Identifier: GPL-2.0-or-later

package pack

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// Profile is the per-title recipe (docs/m3/packaging.md): how to translate the title and which
// environment its bundle runs with. Every field is optional.
type Profile struct {
	Name      string            `json:"name,omitempty"`
	Translate TranslateProfile  `json:"translate"`
	Env       map[string]string `json:"env,omitempty"` // added to the launch environment of the bundle
	// Microphone adds NSMicrophoneUsageDescription to Info.plist.
	Microphone bool `json:"microphone,omitempty"`
}

// TranslateProfile maps one-to-one onto ps4-recomp flags.
type TranslateProfile struct {
	GPU           string   `json:"gpu,omitempty"`            // default anyps5
	X87           string   `json:"x87,omitempty"`            // f80 | f64
	CallModel     string   `json:"call_model,omitempty"`     // flat | nested
	AddrMode      string   `json:"addr_mode,omitempty"`      // base | identity | auto
	ExcludeModule []string `json:"exclude_module,omitempty"` // -exclude-module, repeated
	HLEMalloc     string   `json:"hle_malloc,omitempty"`     // -hle-malloc <mode>
	Args          []string `json:"args,omitempty"`           // anything else, passed through verbatim
}

// LoadProfile resolves --profile. name may be a path, or a name looked up as <name>.json in
// $NPS5_PROFILES, <repo>/profiles and <support root>/profiles. With no name the title id is tried
// the same way; no file at all gives the defaults.
func LoadProfile(name, titleID, repo string) (*Profile, string, error) {
	dirs := []string{}
	if d := os.Getenv("NPS5_PROFILES"); d != "" {
		dirs = append(dirs, d)
	}
	if repo != "" {
		dirs = append(dirs, filepath.Join(repo, "profiles"))
	}
	dirs = append(dirs, filepath.Join(SupportRoot(), "profiles"))
	try := func(n string) string {
		if strings.ContainsRune(n, '/') || strings.HasSuffix(n, ".json") {
			if _, err := os.Stat(n); err == nil {
				return n
			}
			return ""
		}
		for _, d := range dirs {
			if p := filepath.Join(d, n+".json"); fileExists(p) {
				return p
			}
		}
		return ""
	}
	path := ""
	if name != "" {
		if path = try(name); path == "" {
			return nil, "", fmt.Errorf("profile %q not found (looked in %s)", name, strings.Join(dirs, ", "))
		}
	} else {
		path = try(titleID)
	}
	p := &Profile{}
	if path != "" {
		b, err := os.ReadFile(path)
		if err != nil {
			return nil, "", err
		}
		dec := json.NewDecoder(strings.NewReader(string(b)))
		dec.DisallowUnknownFields()
		if err := dec.Decode(p); err != nil {
			return nil, "", fmt.Errorf("profile %s: %w", path, err)
		}
	}
	if p.Translate.GPU == "" {
		p.Translate.GPU = "anyps5"
	}
	if p.Name == "" {
		p.Name = name
	}
	return p, path, nil
}

// Args are the ps4-recomp flags of the profile; supports reports whether the translator knows a flag
// (some are only on later branches: it is an error to ask for one that is missing).
func (p *Profile) Args(supports func(flag string) bool) ([]string, error) {
	t := p.Translate
	a := []string{"-gpu", t.GPU}
	add := func(flag, v string) error {
		if v == "" {
			return nil
		}
		if !supports(flag) {
			return fmt.Errorf("profile asks for -%s but this translator does not have it", flag)
		}
		a = append(a, "-"+flag, v)
		return nil
	}
	for _, kv := range [][2]string{{"x87", t.X87}, {"call-model", t.CallModel}, {"addr-mode", t.AddrMode}, {"hle-malloc", t.HLEMalloc}} {
		if err := add(kv[0], kv[1]); err != nil {
			return nil, err
		}
	}
	for _, m := range t.ExcludeModule {
		a = append(a, "-exclude-module", m)
	}
	return append(a, t.Args...), nil
}

func fileExists(p string) bool { _, err := os.Stat(p); return err == nil }
