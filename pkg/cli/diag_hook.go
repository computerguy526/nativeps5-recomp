package cli

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"golang.org/x/arch/x86/x86asm"

	"ps4-recomp/pkg/diag"
	"ps4-recomp/pkg/disasm"
	"ps4-recomp/pkg/elfloader"
	"ps4-recomp/pkg/emitter"
)

// diagCommand handles the nps5 helper subcommands (doctor, last, addr2guest). It reports
// whether args named one.
func diagCommand(args []string) (bool, error) {
	if len(args) == 0 {
		return false, nil
	}
	switch args[0] {
	case "doctor":
		out, ok := diag.FormatChecks(diag.Doctor())
		fmt.Fprint(stdout, out)
		if !ok {
			return true, fmt.Errorf("doctor: required checks failed")
		}
		return true, nil
	case "last":
		bundle := ""
		if len(args) > 1 {
			bundle = args[1]
		}
		dir, err := diag.ResolveBundle(diag.RunsRoot(), bundle)
		if err != nil {
			return true, err
		}
		return true, diag.Last(stdout, dir)
	case "addr2guest":
		var bundle, addrmap, pc string
		for i := 1; i < len(args); i++ {
			switch args[i] {
			case "-bundle", "--bundle":
				i++
				if i < len(args) {
					bundle = args[i]
				}
			case "-addrmap", "--addrmap":
				i++
				if i < len(args) {
					addrmap = args[i]
				}
			default:
				pc = args[i]
			}
		}
		if pc == "" {
			return true, fmt.Errorf("usage: addr2guest [-bundle dir] [-addrmap file] <host_pc | +image_offset>")
		}
		s, err := diag.Addr2Guest(diag.RunsRoot(), bundle, addrmap, pc)
		if err != nil {
			return true, err
		}
		fmt.Fprintln(stdout, s)
		return true, nil
	}
	return false, nil
}

// diagSession is the translate-phase bundle of one build.
type diagSession struct {
	b        *diag.Bundle
	inputSHA string
	infos    []diag.FuncInfo
}

// diagStart opens the translate bundle. A failure to create it only disables diagnostics.
func diagStart(cfg *Config) *diagSession {
	if os.Getenv("NPS5_NO_BUNDLE") != "" {
		return nil
	}
	title := strings.TrimSuffix(filepath.Base(cfg.ElfPath), filepath.Ext(cfg.ElfPath))
	b, err := diag.NewBundle("", title, "translate")
	if err != nil {
		fmt.Fprintf(os.Stderr, "             Warning: diagnostics bundle disabled: %v\n", err)
		return nil
	}
	b.AddInput(cfg.ElfPath)
	for _, k := range []struct {
		n string
		v any
	}{{"out_dir", cfg.OutDir}, {"compile", cfg.Compile}, {"opt", cfg.OptLevel}, {"all_symbols", cfg.AllSymbols},
		{"exclude_modules", cfg.ExcludeModules}, {"chunk_size", cfg.ChunkSize}, {"tso", cfg.TSO}, {"asan", cfg.Asan},
		{"keep_asm", cfg.KeepAsm}, {"asm_comments", cfg.asmComments()}} {
		b.SetSetting(k.n, k.v)
	}
	s := &diagSession{b: b}
	if len(b.ManifestSnapshot().Inputs) > 0 {
		s.inputSHA = b.ManifestSnapshot().Inputs[0].SHA256
	}
	b.WriteManifest()
	b.Log("lifter", "info", "translate started", map[string]any{"input": cfg.ElfPath})
	fmt.Fprintf(logw, "             Diagnostics bundle: %s\n", b.Dir)
	return s
}

func (s *diagSession) log(subsys, level, msg string, f map[string]any) {
	if s != nil {
		s.b.Log(subsys, level, msg, f)
	}
}

func (s *diagSession) close(err error) {
	if s != nil {
		s.b.Close(err)
	}
}

// funcInfo extracts the census facts from the discovered functions.
func funcInfo(f *disasm.Function) diag.FuncInfo {
	{
		fi := diag.FuncInfo{Entry: f.EntryAddr, End: f.EndAddr, Name: f.Name, JumpTables: map[uint64]int{}}
		for site, t := range f.JumpTables {
			fi.JumpTables[site] = len(t)
		}
		for _, addr := range f.BlockOrder {
			bb := f.Blocks[addr]
			if bb == nil {
				continue
			}
			for _, in := range bb.Insts {
				if len(in.Inst.Args) == 0 {
					continue
				}
				switch in.Inst.Op {
				case x86asm.JMP:
					switch a := in.Inst.Args[0].(type) {
					case x86asm.Reg:
						fi.IndirectJmps = append(fi.IndirectJmps, in.Address)
					case x86asm.Mem:
						if a.Base == x86asm.RIP {
							fi.GotTailJmps++
						} else {
							fi.IndirectJmps = append(fi.IndirectJmps, in.Address)
						}
					}
				case x86asm.CALL:
					switch in.Inst.Args[0].(type) {
					case x86asm.Reg, x86asm.Mem:
						fi.IndirectCall++
					}
				}
			}
		}
		return fi
	}
}

func importsReport(hle *emitter.HLEReport) diag.Imports {
	im := diag.Imports{NeededLibs: hle.NeededLibs, Imports: []diag.ImportEntry{}}
	for _, s := range hle.Imports {
		e := diag.ImportEntry{Library: s.Library, NID: elfloader.NIDPrefix(s.Name), Relocs: s.Relocs}
		if canon, ok := elfloader.ResolveNID(s.Name); ok {
			e.Name = canon
		} else if s.Shim != "" {
			e.Name = s.Shim
		} else {
			e.Name = e.NID
		}
		switch s.Class {
		case emitter.ImportExcluded:
			e.Status, e.Via = "excluded-module", s.Library
			im.Excluded++
		case emitter.ImportHostShim:
			e.Status, e.Via = "bound", "host-shim:"+s.Shim
			im.Bound++
		case emitter.ImportGuestExport, emitter.ImportDefined:
			e.Status, e.Via = "bound", s.Class.String()
			im.Bound++
		default:
			e.Status = "missing"
			im.Missing++
		}
		im.Imports = append(im.Imports, e)
	}
	return im
}

// observer returns the emitter hook collecting per-function census facts into s.infos.
func (s *diagSession) funcInfoFn() func(fn *disasm.Function) diag.FuncInfo {
	if s == nil {
		return nil
	}
	return funcInfo
}

// afterEmit builds the census and addrmap from the emitter's facts, writes census.json,
// imports.json and addrmap.txt, records the assembly pipeline's disk statistics, and returns
// the extra C file to compile (nps5_buildinfo.c).
func (s *diagSession) afterEmit(cfg *Config, loaded *elfloader.LoadedELF, hle *emitter.HLEReport, facts *diag.AsmFacts, asm asmStats, streamed bool) []string {
	if s == nil {
		return nil
	}
	res := diag.BuildScan(facts, s.infos, len(loaded.FuncBounds))
	if streamed {
		s.b.SetStat("asm_chunks", asm.Chunks)
		s.b.SetStat("asm_chunks_assembled", asm.Assembled)
		s.b.SetStat("asm_chunks_reused", asm.Reused)
		s.b.SetStat("asm_generated_bytes", asm.TotalAsmBytes)
		s.b.SetStat("asm_peak_intermediate_bytes", asm.PeakAsmBytes)
		s.b.SetStat("asm_peak_disk_bytes_with_objects", asm.PeakDiskBytes)
		s.b.SetStat("asm_object_bytes", asm.ObjBytes)
		s.b.SetStat("asm_kept", asm.KeptAsm)
		s.b.SetStat("asm_workers", asm.Workers)
	}
	s.b.SetStat("guest_insts_emitted", res.Census.GuestInstsEmitted)
	_ = s.b.WriteJSON("census.json", res.Census)
	_ = s.b.WriteJSON("imports.json", importsReport(hle))
	_ = s.b.WriteFile("addrmap.txt", res.AddrMap)
	_ = os.WriteFile(filepath.Join(cfg.OutDir, "addrmap.txt"), res.AddrMap, 0o644)
	if data, err := os.ReadFile(filepath.Join(cfg.OutDir, "hle_report.txt")); err == nil {
		_ = s.b.WriteFile("hle_report.txt", data)
	}
	s.b.SetStat("pcmap_entries", res.PCEntries)
	s.b.SetStat("unsupported", res.Census.UnsupportedTotal)
	s.b.SetStat("functions_emitted", res.Census.FunctionsEmitted)
	s.log("lifter", "info", "census: "+res.Census.SummaryLine(), nil)
	if res.Census.UnsupportedTotal > 0 {
		s.log("lifter", "warn", "unsupported instruction stubs emitted", map[string]any{"count": res.Census.UnsupportedTotal})
	}
	fmt.Fprintf(logw, "             Census: %s\n", res.Census.SummaryLine())
	bi, err := diag.WriteBuildInfo(cfg.OutDir, s.b, s.inputSHA, cfg.ElfPath, filepath.Join(cfg.OutDir, "addrmap.txt"), int(loaded.MemoryImage.End()))
	if err != nil {
		s.log("lifter", "error", "buildinfo write failed", map[string]any{"err": err.Error()})
		return nil
	}
	return []string{bi}
}
