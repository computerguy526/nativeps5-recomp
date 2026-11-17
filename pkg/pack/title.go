// SPDX-License-Identifier: GPL-2.0-or-later

// Package pack builds a self-contained, double-clickable macOS .app from an extracted PS4/PS5 title
// directory, and lists/launches the built titles (docs/m3/packaging.md).
package pack

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"ps4-recomp/pkg/sfo"
)

// Title is what a title directory says about itself (sce_sys/param.json on PS5, param.sfo on PS4).
type Title struct {
	ID      string `json:"id"`
	Name    string `json:"name"`
	Version string `json:"version"`
	Dump    string `json:"dump"`  // absolute path of the title directory (read-only /app0)
	Eboot   string `json:"eboot"` // absolute path of the executable
	Icon    string `json:"-"`     // sce_sys/icon0.png, "" when absent
	PS5     bool   `json:"ps5"`
}

// ReadTitle inspects an extracted title directory.
func ReadTitle(dir string) (*Title, error) {
	abs, err := filepath.Abs(dir)
	if err != nil {
		return nil, err
	}
	if abs, err = filepath.EvalSymlinks(abs); err != nil {
		return nil, err
	}
	if st, err := os.Stat(abs); err != nil || !st.IsDir() {
		return nil, fmt.Errorf("%s: not a title directory", dir)
	}
	t := &Title{Dump: abs}
	for _, c := range []string{"eboot.bin", "EBOOT.BIN", "eboot.elf"} {
		if _, err := os.Stat(filepath.Join(abs, c)); err == nil {
			t.Eboot = filepath.Join(abs, c)
			break
		}
	}
	if t.Eboot == "" {
		return nil, fmt.Errorf("%s: no eboot.bin or eboot.elf", abs)
	}
	sys := filepath.Join(abs, "sce_sys")
	if _, err := os.Stat(filepath.Join(sys, "icon0.png")); err == nil {
		t.Icon = filepath.Join(sys, "icon0.png")
	}
	if b, err := os.ReadFile(filepath.Join(sys, "param.json")); err == nil {
		var p struct {
			TitleID        string                     `json:"titleId"`
			ContentVersion string                     `json:"contentVersion"`
			MasterVersion  string                     `json:"masterVersion"`
			Localized      map[string]json.RawMessage `json:"localizedParameters"`
		}
		if err := json.Unmarshal(b, &p); err != nil {
			return nil, fmt.Errorf("%s/param.json: %w", sys, err)
		}
		t.PS5, t.ID, t.Version = true, p.TitleID, p.ContentVersion
		if t.Version == "" {
			t.Version = p.MasterVersion
		}
		def := ""
		_ = json.Unmarshal(p.Localized["defaultLanguage"], &def)
		for _, lang := range []string{def, "en-US"} {
			var l struct {
				TitleName string `json:"titleName"`
			}
			if raw, ok := p.Localized[lang]; ok && json.Unmarshal(raw, &l) == nil && l.TitleName != "" {
				t.Name = l.TitleName
				break
			}
		}
	} else if f, err := sfo.Load(filepath.Join(sys, "param.sfo")); err == nil {
		t.ID, _ = f.String("TITLE_ID")
		t.Name, _ = f.String("TITLE")
		if t.Version, _ = f.String("APP_VER"); t.Version == "" {
			t.Version, _ = f.String("VERSION")
		}
	}
	if t.ID = sanitize(t.ID); t.ID == "" {
		return nil, fmt.Errorf("%s: no title id in sce_sys/param.json or param.sfo", abs)
	}
	if strings.TrimSpace(t.Name) == "" {
		t.Name = t.ID
	}
	if t.Version == "" {
		t.Version = "1.0"
	}
	return t, nil
}

// sanitize keeps what a bundle identifier and a directory name can hold.
func sanitize(s string) string {
	return strings.Map(func(r rune) rune {
		if r >= 'a' && r <= 'z' || r >= 'A' && r <= 'Z' || r >= '0' && r <= '9' || r == '-' || r == '_' {
			return r
		}
		return -1
	}, s)
}

// fileName is the title name as a file name: no path separators or characters Finder dislikes.
func fileName(s string) string {
	s = strings.Map(func(r rune) rune {
		if r < 32 || strings.ContainsRune(`/:\*?"<>|`, r) {
			return '-'
		}
		return r
	}, s)
	if s = strings.Trim(strings.TrimSpace(s), "."); s == "" {
		return "Title"
	}
	return s
}
