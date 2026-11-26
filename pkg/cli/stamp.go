package cli

// The translation stamp: a no-op rebuild (same title, same translator lowering, same options)
// must not load the ELF, rediscover 26 M instructions or look at 2,200 chunks. After a successful
// -asm-only translation the stamp of its inputs is stored in <cache>/translate.stamp; the next
// run with an equal stamp and an intact output only refreshes the runtime sources (a runtime-only
// change never retranslates) and stops. See docs/m3/iteration-speed.md.

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"ps4-recomp/pkg/emitter"
	"ps4-recomp/pkg/tcache"
)

func cacheDirOf(cfg *Config) string {
	if cfg.CacheDir != "" {
		return cfg.CacheDir
	}
	return filepath.Join(cfg.OutDir, ".nps5cache")
}

func statLine(h interface{ Write([]byte) (int, error) }, p string) {
	if fi, err := os.Stat(p); err == nil {
		fmt.Fprintf(h, "%s|%d|%d\n", p, fi.Size(), fi.ModTime().UnixNano())
	} else {
		fmt.Fprintf(h, "%s|missing\n", p)
	}
}

// translationStamp hashes everything the translation depends on except the runtime sources.
func translationStamp(cfg *Config) (string, error) {
	h := sha256.New()
	fmt.Fprintf(h, "lowering=%s\n", emitter.LoweringID())
	fmt.Fprintf(h, "cfg=%s|%s|%s|%t|%t|%t|%s|%s|%d|%t|%t|%s|%s\n", cfg.AddrMode, cfg.CallModel, cfg.X87, cfg.TSO, cfg.AllowUnsupported,
		cfg.AllSymbols, cfg.GPU, strings.Join(cfg.ExcludeModules, ","), cfg.ChunkSize, cfg.asmComments(), cfg.KeepAsm, cfg.AppDir, os.Getenv("NPS5_ANYPS5_BUILD"))
	fmt.Fprintf(h, "liftmisc=%t|%t|%t|%t\n", cfg.CallIC, cfg.FpCmpFuse, cfg.PushPopFuse, cfg.JmpIC)
	if cfg.PackedLayout || os.Getenv("NPS5_SEED_AFTER_UD2") == "1" {
		fmt.Fprintf(h, "packed=1\n") // only when on: other titles keep their stamp
	}
	if cfg.ROLoads || cfg.RedZoneLeaf || cfg.BMIPeep {
		fmt.Fprintf(h, "lift2=%t|%t|%t\n", cfg.ROLoads, cfg.RedZoneLeaf, cfg.BMIPeep)
	}
	fmt.Fprintf(h, "cold=%t|layout=%s|abi=%d|lean=%s\n", cfg.ColdOutline, emitter.KeyLayoutHash(), emitter.ABIVersion, os.Getenv("NPS5_LEAN"))
	fmt.Fprintf(h, "env=%s|%s|%s\n", os.Getenv("NPS5_NO_PCMAP"), os.Getenv("NPS5_PCMAP_STRICT"), os.Getenv("NPS5_ASM"))
	statLine(h, cfg.ElfPath)
	dirs := []string{filepath.Dir(cfg.ElfPath)}
	if cfg.AppDir != "" {
		dirs = append(dirs, cfg.AppDir, filepath.Join(cfg.AppDir, "sce_module"))
	}
	for _, d := range dirs {
		ents, err := os.ReadDir(d)
		if err != nil {
			continue
		}
		names := make([]string, 0, len(ents))
		for _, e := range ents {
			if !e.IsDir() {
				names = append(names, e.Name())
			}
		}
		sort.Strings(names)
		for _, n := range names {
			statLine(h, filepath.Join(d, n))
		}
	}
	if cfg.Patches != "" {
		pb, err := os.ReadFile(cfg.Patches)
		if err != nil {
			return "", err
		}
		fmt.Fprintf(h, "patches=%x\n", sha256.Sum256(pb))
	}
	if cfg.HLEReplace != "" {
		// the replacement table decides which functions become shims or wraps: a changed table is a different translation
		rb, err := os.ReadFile(cfg.HLEReplace)
		if err != nil {
			return "", err
		}
		fmt.Fprintf(h, "hlereplace=%x|%t\n", sha256.Sum256(rb), cfg.HLEReplaceUnverified)
	}
	if cfg.SeedsFrom != "" {
		seeds, err := readTier1Seeds(cfg.SeedsFrom)
		if err != nil {
			return "", err
		}
		fmt.Fprintf(h, "seeds=%v\n", seeds)
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

func stampPath(cfg *Config) string { return filepath.Join(cacheDirOf(cfg), "translate.stamp") }

// noOpRebuild reports whether the output directory already holds the translation of these inputs.
func noOpRebuild(cfg *Config) bool {
	if !cfg.AsmOnly || cfg.Compile || cfg.NoCache || cfg.KeepAsm {
		return false
	}
	want, err := translationStamp(cfg)
	if err != nil {
		return false
	}
	got, err := os.ReadFile(stampPath(cfg))
	if err != nil || strings.TrimSpace(string(got)) != want {
		return false
	}
	// an intact output: the index lists the chunks, every one has its object
	x := tcache.Open(cacheDirOf(cfg))
	rows := x.Rows("chunk")
	if len(rows) == 0 {
		return false
	}
	for _, r := range rows {
		if _, ok := objectCurrent(filepath.Join(cfg.OutDir, r.ID+".o"), tcache.HashBytes([]byte(asmTool()+" "+strings.Join(asmFlags(), " ")+"\x00"+r.AsmHash))); !ok {
			return false
		}
	}
	for _, f := range []string{"main.c", "dispatch.c", "guest_image.bin", "lifter_manifest.json", "build.ninja"} {
		if _, err := os.Stat(filepath.Join(cfg.OutDir, f)); err != nil {
			return false
		}
	}
	return true
}

func writeStamp(cfg *Config) {
	if !cfg.AsmOnly || cfg.Compile || cfg.NoCache || cfg.KeepAsm {
		return
	}
	if s, err := translationStamp(cfg); err == nil {
		_ = os.MkdirAll(cacheDirOf(cfg), 0o755)
		_ = os.WriteFile(stampPath(cfg), []byte(s+"\n"), 0o644)
	}
}
