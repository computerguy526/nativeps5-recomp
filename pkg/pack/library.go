// SPDX-License-Identifier: GPL-2.0-or-later

package pack

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"text/tabwriter"
	"time"
)

// SupportRoot is ~/Library/Application Support/nativeps5 (NPS5_HOME overrides it, for tests).
func SupportRoot() string {
	if h := os.Getenv("NPS5_HOME"); h != "" {
		return h
	}
	home, _ := os.UserHomeDir()
	return filepath.Join(home, "Library", "Application Support", "nativeps5")
}

// TitleDir is the per-title data directory: build/ (translated code, tier-1 manifest), gpu-cache/,
// tier1_cache/, savedata/, runs/ (run bundles), logs/, run/ (working directory).
func TitleDir(id string) string { return filepath.Join(SupportRoot(), id) }

// Record is <titleDir>/title.json, written by build and read by list and the launcher.
type Record struct {
	Title
	App     string `json:"app"`
	Profile string `json:"profile"`
	Built   string `json:"built"`
}

// List returns the built titles that still have their .app.
func List() ([]Record, error) {
	ents, err := os.ReadDir(SupportRoot())
	if err != nil && !os.IsNotExist(err) {
		return nil, err
	}
	var out []Record
	for _, e := range ents {
		b, err := os.ReadFile(filepath.Join(SupportRoot(), e.Name(), "title.json"))
		if err != nil {
			continue
		}
		var r Record
		if json.Unmarshal(b, &r) == nil && r.App != "" {
			out = append(out, r)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	return out, nil
}

// PrintList is `nps5 list`.
func PrintList(w io.Writer) error {
	recs, err := List()
	if err != nil {
		return err
	}
	if len(recs) == 0 {
		fmt.Fprintln(w, "no titles built yet (nps5 build <title dir>)")
		return nil
	}
	tw := tabwriter.NewWriter(w, 2, 4, 2, ' ', 0)
	fmt.Fprintln(tw, "ID\tNAME\tVERSION\tBUNDLE\tDUMP")
	for _, r := range recs {
		state := r.App
		if !fileExists(r.App) {
			state += " (missing)"
		}
		dump := r.Dump
		if !fileExists(r.Dump) {
			dump += " (missing)"
		}
		fmt.Fprintf(tw, "%s\t%s\t%s\t%s\t%s\n", r.ID, r.Name, r.Version, state, dump)
	}
	return tw.Flush()
}

// Play opens the bundle of a title (by id or name) as a new instance, like a double click.
func Play(key string, env []string) error {
	recs, err := List()
	if err != nil {
		return err
	}
	for _, r := range recs {
		if r.ID == key || r.Name == key {
			args := []string{"-n"}
			for _, e := range env {
				args = append(args, "--env", e)
			}
			return exec.Command("open", append(args, r.App)...).Run()
		}
	}
	return fmt.Errorf("no built title %q (nps5 list)", key)
}

func writeRecord(t *Title, app, profile string) error {
	d := TitleDir(t.ID)
	if err := os.MkdirAll(d, 0o755); err != nil {
		return err
	}
	b, _ := json.MarshalIndent(Record{Title: *t, App: app, Profile: profile, Built: time.Now().Format(time.RFC3339)}, "", "  ")
	return os.WriteFile(filepath.Join(d, "title.json"), b, 0o644)
}
