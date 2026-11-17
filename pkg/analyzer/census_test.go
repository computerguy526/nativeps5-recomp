package analyzer

import (
	"bufio"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"

	"golang.org/x/arch/x86/x86asm"

	"ps4-recomp/pkg/disasm"
	"ps4-recomp/pkg/elfloader"
)

// DISCO_CENSUS="a.prx:b.bin" [DISCO_STARTS=<hex text offsets, one per line, for the first file>]
// go test ./pkg/analyzer -run TestDiscoveryCensus -v
// prints counts only (functions, decoded instructions, jump-table recovery, time, RSS); nothing
// derived from the binaries is written anywhere.
func TestDiscoveryCensus(t *testing.T) {
	list := os.Getenv("DISCO_CENSUS")
	if list == "" {
		t.Skip("DISCO_CENSUS not set")
	}
	for i, p := range strings.Split(list, ":") {
		starts := ""
		if i == 0 {
			starts = os.Getenv("DISCO_STARTS")
		}
		t0 := time.Now()
		loaded, err := elfloader.LoadELF(p)
		if err != nil {
			t.Errorf("%s: %v", p, err)
			continue
		}
		d, err := disasm.NewDisassembler(loaded)
		if err != nil {
			t.Errorf("%s: %v", p, err)
			continue
		}
		d.SkipFlagLiveness = true
		seeds, rep := disasm.SeedEntryPointsOpts(loaded, disasm.DefaultSeedOptions())
		textLo := loaded.ExecRanges[0].Start
		var textBytes uint64
		for _, r := range loaded.ExecRanges {
			textBytes += r.End - r.Start
			if r.Start < textLo {
				textLo = r.Start
			}
		}
		seen := make([]uint64, (loaded.MaxVAddr+63)/64+1)
		funcs, insts, uniq, jmps, rec, tableTargets := 0, 0, 0, 0, 0, 0
		entries := map[uint64]bool{}
		unrec := map[string]int{}
		var sample []uint64
		var sampleKind []string
		_ = d.WalkReachable(seeds, func(fn *disasm.Function) error {
			funcs++
			entries[fn.EntryAddr] = true
			for _, b := range fn.Blocks {
				for _, in := range b.Insts {
					insts++
					w, bit := in.Address/64, in.Address%64
					if seen[w]&(1<<bit) == 0 {
						seen[w] |= 1 << bit
						uniq++
					}
					if in.Inst.Op != x86asm.JMP {
						continue
					}
					switch a := in.Inst.Args[0].(type) {
					case x86asm.Reg:
					case x86asm.Mem:
						if a.Base == x86asm.RIP {
							continue
						}
					default:
						continue
					}
					jmps++
					if tg, ok := fn.JumpTables[in.Address]; ok {
						rec++
						tableTargets += len(tg)
						continue
					}
					kind := classifyTail(b.Insts, in)
					if kind == "" {
						kind = deepKind(fn, in)
					}
					unrec[kind]++
					if kind != "mem" && kind != "got" && len(sample) < 4000 {
						sample = append(sample, in.Address)
						sampleKind = append(sampleKind, kind)
					}
				}
			}
			return nil
		})
		var ru syscall.Rusage
		_ = syscall.Getrusage(syscall.RUSAGE_SELF, &ru)
		pct := 0.0
		if jmps > 0 {
			pct = 100 * float64(rec) / float64(jmps)
		}
		t.Logf("%s: text=%d B (%.1f M) seeds=%d fdes=%d funcs=%d insts=%d uniqueInsts=%d dup=%.2fx jmpIndirect=%d recovered=%d (%.1f%%) tableTargets=%d time=%v maxRSS=%d MB",
			filepath.Base(p), textBytes, float64(textBytes)/1e6, len(seeds), len(loaded.FuncBounds), funcs, insts, uniq, float64(insts)/float64(max(uniq, 1)), jmps, rec, pct, tableTargets,
			time.Since(t0).Round(time.Millisecond), ru.Maxrss/(1<<20))
		t.Logf("   switch-like (indirect minus got/mem tails): %d, recovered %.1f%%", jmps-unrec["got"]-unrec["mem"]-unrec["epilogue"], 100*float64(rec)/float64(max(jmps-unrec["got"]-unrec["mem"]-unrec["epilogue"], 1)))
		t.Logf("   seed sources new=%v rejected=%v", rep.New, rep.Rejected)
		var kinds []string
		for k, n := range unrec {
			kinds = append(kinds, fmt.Sprintf("%s=%d", k, n))
		}
		sort.Slice(kinds, func(a, b int) bool {
			na, _ := strconv.Atoi(kinds[a][strings.LastIndex(kinds[a], "=")+1:])
			nb, _ := strconv.Atoi(kinds[b][strings.LastIndex(kinds[b], "=")+1:])
			return na > nb
		})
		if len(kinds) > 25 {
			kinds = kinds[:25]
		}
		t.Logf("   unrecovered kinds: %s", strings.Join(kinds, " "))
		if dir := os.Getenv("DISCO_SAMPLES"); dir != "" { // addresses only
			var sb strings.Builder
			for i := range sample {
				fmt.Fprintf(&sb, "%x %s\n", sample[i], sampleKind[i])
			}
			_ = os.WriteFile(filepath.Join(dir, "unrec_"+filepath.Base(p)+".txt"), []byte(sb.String()), 0o644)
		}
		if starts != "" {
			f, err := os.Open(starts)
			if err != nil {
				t.Error(err)
				continue
			}
			sc := bufio.NewScanner(f)
			total, hit := 0, 0
			for sc.Scan() {
				off, err := strconv.ParseUint(strings.TrimSpace(sc.Text()), 16, 64)
				if err != nil {
					continue
				}
				total++
				if entries[textLo+off] {
					hit++
				}
			}
			_ = f.Close()
			t.Logf("   objdump function starts: %d, ours matching: %d (%.1f%%); our functions %d (%.2fx of oracle)",
				total, hit, 100*float64(hit)/float64(max(total, 1)), funcs, float64(funcs)/float64(max(total, 1)))
		}
	}
}
