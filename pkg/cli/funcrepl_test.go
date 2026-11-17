package cli

import (
	"strings"
	"testing"
)

func TestParseArgsHLEMalloc(t *testing.T) {
	cfg, err := ParseArgs([]string{"-hle-malloc", "ue5-binned3", "--hle-replace=t.json", "game.elf"})
	if err != nil {
		t.Fatal(err)
	}
	if cfg.HLEMalloc != "ue5-binned3" || cfg.HLEReplace != "t.json" || cfg.ElfPath != "game.elf" {
		t.Fatalf("%+v", cfg)
	}
	// default: off
	cfg, err = ParseArgs([]string{"game.elf"})
	if err != nil || cfg.HLEMalloc != "" || cfg.HLEReplace != "" {
		t.Fatalf("default must be off: %+v %v", cfg, err)
	}
}

func TestTier1InfoRoundTripsHLEFlags(t *testing.T) {
	if absOrEmpty("") != "" || !strings.HasPrefix(absOrEmpty("x.json"), "/") {
		t.Fatalf("absOrEmpty: %q %q", absOrEmpty(""), absOrEmpty("x.json"))
	}
}

// -patches takes a value in both spellings and does not swallow the input file.
func TestParsePatchesFlag(t *testing.T) {
	for _, args := range [][]string{{"-patches", "p.json", "game.elf"}, {"--patches=p.json", "game.elf"}, {"game.elf", "-patches", "p.json"}} {
		cfg, err := ParseArgs(args)
		if err != nil {
			t.Fatalf("%v: %v", args, err)
		}
		if cfg.Patches != "p.json" || cfg.ElfPath != "game.elf" {
			t.Errorf("%v: patches %q elf %q", args, cfg.Patches, cfg.ElfPath)
		}
	}
}

// -hle-replace-unverified is a plain switch, off by default, and does not swallow the input file.
func TestParseHLEReplaceUnverified(t *testing.T) {
	cfg, err := ParseArgs([]string{"-hle-replace", "t.json", "-hle-replace-unverified", "game.elf"})
	if err != nil || !cfg.HLEReplaceUnverified || cfg.HLEReplace != "t.json" || cfg.ElfPath != "game.elf" {
		t.Fatalf("%+v %v", cfg, err)
	}
	cfg, err = ParseArgs([]string{"-hle-replace", "t.json", "game.elf"})
	if err != nil || cfg.HLEReplaceUnverified {
		t.Fatalf("the opt-in must default to off: %+v %v", cfg, err)
	}
}
