// SPDX-License-Identifier: GPL-2.0-or-later

package cli

import (
	"encoding/json"
	"testing"
)

func TestPackedLayoutFlag(t *testing.T) {
	cfg, err := ParseArgs([]string{"-packed-layout", "-allow-unsupported", "eboot.bin"})
	if err != nil {
		t.Fatal(err)
	}
	if !cfg.PackedLayout || !cfg.AllowUnsupported {
		t.Fatalf("PackedLayout=%v AllowUnsupported=%v, want both true", cfg.PackedLayout, cfg.AllowUnsupported)
	}
	cfg, err = ParseArgs([]string{"eboot.bin"})
	if err != nil {
		t.Fatal(err)
	}
	if cfg.PackedLayout {
		t.Fatal("-packed-layout must default to off")
	}
}

// The tier-1 manifest carries the switch so fragments discover like the translation, and a title
// translated without it keeps its fragment-cache version and manifest bytes.
func TestPackedLayoutTier1Manifest(t *testing.T) {
	base := tier1Info{ABI: Tier1ABI, Elf: "/x/eboot.bin", AddrMode: "base", CallModel: "flat", GPU: "anyps5"}
	on := base
	on.PackedLayout = true
	if tier1Version(&base) == tier1Version(&on) {
		t.Fatal("tier-1 version must change with the packed layout")
	}
	b, _ := json.Marshal(base)
	var m map[string]any
	_ = json.Unmarshal(b, &m)
	if _, ok := m["packed_layout"]; ok {
		t.Fatal("packed_layout written for a title translated without it")
	}
	b, _ = json.Marshal(on)
	var back tier1Info
	if err := json.Unmarshal(b, &back); err != nil || !back.PackedLayout {
		t.Fatalf("packed_layout did not round-trip: %v %+v", err, back)
	}
}

// Turning the packed layout on must re-translate (stamp changes); off leaves the stamp alone.
func TestPackedLayoutStamp(t *testing.T) {
	t.Setenv("NPS5_SEED_AFTER_UD2", "")
	cfg := &Config{ElfPath: t.TempDir() + "/eboot.bin", AddrMode: "base", CallModel: "flat", GPU: "anyps5"}
	off, err := translationStamp(cfg)
	if err != nil {
		t.Fatal(err)
	}
	cfg.PackedLayout = true
	on, err := translationStamp(cfg)
	if err != nil {
		t.Fatal(err)
	}
	if off == on {
		t.Fatal("stamp ignores -packed-layout")
	}
	cfg.PackedLayout = false
	t.Setenv("NPS5_SEED_AFTER_UD2", "1")
	env, _ := translationStamp(cfg)
	if env != on {
		t.Fatal("the environment opt-in and the flag must give the same stamp")
	}
}

// nps5 build checks a profile's flags against the usage text.
func TestPackedLayoutInUsage(t *testing.T) {
	if !SupportsFlag("packed-layout") {
		t.Fatal("-packed-layout missing from the usage text")
	}
}
