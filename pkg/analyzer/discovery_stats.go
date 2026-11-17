package analyzer

import (
	"debug/elf"
	"encoding/binary"
	"fmt"
	"sort"
	"strings"

	"ps4-recomp/pkg/disasm"
	"ps4-recomp/pkg/elfloader"

	"golang.org/x/arch/x86/x86asm"
)

// DiscoveryStats measures static code discovery on one binary: how many functions the
// seeding and CFG walk find versus the .eh_frame FDE count, how many indirect jumps were
// resolved to a table, and sanity checks on the seeds (decode failures, mid-instruction
// entries).
type DiscoveryStats struct {
	SeedSources *disasm.SeedReport
	// Unrecovered lists unresolved indirect jump sites (address -> preceding instruction text).
	Unrecovered   map[uint64][]string
	Seeds         int // distinct code seeds from SeedEntryPoints
	FDEs          int
	Functions     int // functions decoded by the walk (seeds + discovered)
	FDEsReached   int // FDE starts that are decoded functions
	SeedDecodeErr int // seeds whose decode failed
	IndirectJmps  int // jmp reg / jmp [mem] (excluding RIP-relative GOT/PLT slots)
	Recovered     int // of those, resolved to a jump table
	TableTargets  int
	// Unresolved sites by shape: tail jumps through a loaded pointer are dispatches (GOT or
	// vtable), not switch tables, and are resolved at run time by the miss fallback.
	GOTTail, MemTail, Other int
	OtherKinds              map[string]int
	// TableShaped: unresolved sites whose register is computed by add + indexed load with no
	// plain memory load in the function (a switch table the matcher missed).
	TableShaped int
	// Oracle: .llvm_jump_table_sizes (clang-built SDK images) lists every jump table's address
	// and length. OracleTables is the count, OracleFound those whose address was recovered,
	// OracleExact those recovered with the exact entry count, OracleMissing the addresses not found.
	OracleTables, OracleFound, OracleExact int
	OracleMissing                          []uint64
	OracleInexact                          []string
	OracleEntries, OracleEntriesMissed     int
	MidInst                                int // function entries that fall inside another function's instruction
	Insts                                  int
}

func (s DiscoveryStats) String() string {
	pct := 0.0
	if s.IndirectJmps > 0 {
		pct = 100 * float64(s.Recovered) / float64(s.IndirectJmps)
	}
	sw := s.IndirectJmps - s.GOTTail - s.MemTail
	pct2 := 0.0
	if sw > 0 {
		pct2 = 100 * float64(s.Recovered) / float64(sw)
	}
	orc := ""
	if s.OracleTables > 0 {
		orc = fmt.Sprintf("oracle{tables=%d found=%d exact=%d entries=%d missed=%d} ", s.OracleTables, s.OracleFound, s.OracleExact, s.OracleEntries, s.OracleEntriesMissed)
	}
	return orc + fmt.Sprintf("switchLike=%d recovered/switchLike=%.1f%% unresolved{gotTail=%d memTail=%d other=%d tableShaped=%d} ", sw, pct2, s.GOTTail, s.MemTail, s.Other, s.TableShaped) + fmt.Sprintf("seeds=%d fdes=%d funcs=%d fdesReached=%d seedDecodeErr=%d indJmp=%d recovered=%d (%.1f%%) tableTargets=%d midInstEntries=%d insts=%d",
		s.Seeds, s.FDEs, s.Functions, s.FDEsReached, s.SeedDecodeErr, s.IndirectJmps, s.Recovered, pct, s.TableTargets, s.MidInst, s.Insts)
}

// MeasureDiscovery runs the recompiler's seeding and reachability walk over path.
func MeasureDiscovery(path string) (*DiscoveryStats, error) {
	return MeasureDiscoveryOpts(path, disasm.DefaultSeedOptions())
}

// MeasureDiscoveryOpts is MeasureDiscovery with explicit seed-source switches.
func MeasureDiscoveryOpts(path string, opts disasm.SeedOptions) (*DiscoveryStats, error) {
	loaded, err := elfloader.LoadELF(path)
	if err != nil {
		return nil, err
	}
	d, err := disasm.NewDisassembler(loaded)
	if err != nil {
		return nil, err
	}
	d.SkipFlagLiveness = true
	seeds, rep := disasm.SeedEntryPointsOpts(loaded, opts)
	st := &DiscoveryStats{SeedSources: rep, OtherKinds: map[string]int{}, Unrecovered: map[uint64][]string{}, Seeds: len(seeds), FDEs: len(loaded.FuncBounds)}
	for _, s := range seeds {
		if _, _, err := d.DisasmFunction(s); err != nil {
			st.SeedDecodeErr++
		}
	}
	type span struct{ start, end uint64 }
	tables := map[uint64]int{} // recovered table address -> max proven count
	tableTargets := map[uint64][]uint64{}
	tableFmt := map[uint64]disasm.ResolvedJumpTable{}
	var insts []span
	entries := map[uint64]bool{}
	err = d.WalkReachable(seeds, func(fn *disasm.Function) error {
		st.Functions++
		entries[fn.EntryAddr] = true
		for _, b := range fn.Blocks {
			for _, in := range b.Insts {
				st.Insts++
				insts = append(insts, span{in.Address, in.Address + uint64(in.Inst.Len)})
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
				st.IndirectJmps++
				if t, ok := fn.JumpTables[in.Address]; ok {
					st.Recovered++
					st.TableTargets += len(t)
					if tb, ok := fn.Tables[in.Address]; ok {
						tableTargets[tb.TableAddr] = append(tableTargets[tb.TableAddr], t...)
						tableFmt[tb.TableAddr] = tb
						if tb.Count > tables[tb.TableAddr] {
							tables[tb.TableAddr] = tb.Count
						} else if _, seen := tables[tb.TableAddr]; !seen {
							tables[tb.TableAddr] = tb.Count
						}
					}
				} else {
					var ctx []string
					for _, bi := range b.Insts {
						if bi.Address <= in.Address {
							ctx = append(ctx, fmt.Sprintf("%x: %s", bi.Address, x86asm.IntelSyntax(bi.Inst, bi.Address, nil)))
							if len(ctx) > 10 {
								ctx = ctx[1:]
							}
						}
					}
					kind := classifyTail(b.Insts, in)
					if kind == "" {
						kind = deepKind(fn, in)
					}
					st.Unrecovered[in.Address] = append([]string{"kind=" + kind}, ctx...)
					switch kind {
					case "got":
						st.GOTTail++
					case "mem", "epilogue":
						st.MemTail++
					default:
						st.Other++
					}
					st.OtherKinds[kind]++
					if strings.Contains(kind, "add") && strings.Contains(kind, "idx-load") && !strings.Contains(kind, "mem-load") {
						st.TableShaped++
					}
				}
			}
		}
		return nil
	})
	if err != nil {
		return nil, err
	}
	for _, o := range jumpTableOracle(path) {
		st.OracleTables++
		if c, ok := tables[o.addr]; ok {
			st.OracleFound++
			if c == o.count {
				st.OracleExact++
			} else {
				st.OracleInexact = append(st.OracleInexact, fmt.Sprintf("%#x ours=%d oracle=%d fmt=%d targets=%d", o.addr, c, o.count, tableFmt[o.addr].Format, len(tableTargets[o.addr])))
			}
			// every entry of the oracle-sized table, read in our format, must be a recovered target
			have := map[uint64]bool{}
			for _, x := range tableTargets[o.addr] {
				have[x] = true
			}
			tb := tableFmt[o.addr]
			sz := map[disasm.JumpTableFormat]int{disasm.TableFormatRel32: 4, disasm.TableFormatAbs64: 8, disasm.TableFormatRel8: 1, disasm.TableFormatRel16: 2}[tb.Format]
			for i := 0; i < o.count && sz > 0; i++ {
				off := o.addr + uint64(i*sz)
				b := loaded.MemoryImage.Bytes(off, uint64(sz))
				if b == nil {
					break
				}
				var tgt uint64
				switch tb.Format {
				case disasm.TableFormatRel32:
					tgt = uint64(int64(tb.BaseAddr) + int64(int32(binary.LittleEndian.Uint32(b))))
				case disasm.TableFormatAbs64:
					tgt = binary.LittleEndian.Uint64(b)
				case disasm.TableFormatRel8:
					tgt = uint64(int64(tb.BaseAddr) + int64(int8(b[0])))
				case disasm.TableFormatRel16:
					tgt = uint64(int64(tb.BaseAddr) + int64(int16(binary.LittleEndian.Uint16(b))))
				}
				st.OracleEntries++
				if !have[tgt] {
					st.OracleEntriesMissed++
				}
			}
		} else {
			st.OracleMissing = append(st.OracleMissing, o.addr)
		}
	}
	for _, b := range loaded.FuncBounds {
		if entries[b.Start] {
			st.FDEsReached++
		}
	}
	sort.Slice(insts, func(i, j int) bool { return insts[i].start < insts[j].start })
	for e := range entries {
		i := sort.Search(len(insts), func(i int) bool { return insts[i].end > e })
		if i < len(insts) && insts[i].start < e && e < insts[i].end {
			st.MidInst++
		}
	}
	return st, nil
}

// classifyTail names the shape of an unresolved indirect jump: "got" when the target register
// was last loaded from a RIP-relative slot (import/GOT tail call), "mem" when loaded from a
// register-based memory operand (vtable/callback tail call), "" otherwise.
func classifyTail(insts []disasm.Instruction, jmp disasm.Instruction) string {
	var reg x86asm.Reg
	switch a := jmp.Inst.Args[0].(type) {
	case x86asm.Reg:
		reg = a
		// jmp reg right after the frame is torn down (pop, leave, add rsp) is a tail call
		// through a pointer; a switch dispatch never follows an epilogue.
		for i := len(insts) - 1; i >= 0; i-- {
			if insts[i].Address >= jmp.Address {
				continue
			}
			switch insts[i].Inst.Op {
			case x86asm.POP, x86asm.LEAVE:
				return "epilogue"
			case x86asm.ADD:
				if r, ok := insts[i].Inst.Args[0].(x86asm.Reg); ok && r == x86asm.RSP {
					return "epilogue"
				}
			}
			break
		}
	case x86asm.Mem:
		if a.Base != x86asm.RIP && a.Base != x86asm.RSP {
			return "mem"
		}
		return ""
	}
	for i := len(insts) - 1; i >= 0; i-- {
		in := insts[i]
		if in.Address >= jmp.Address {
			continue
		}
		if in.Inst.Op != x86asm.MOV {
			continue
		}
		dst, ok := in.Inst.Args[0].(x86asm.Reg)
		if !ok || dst != reg {
			continue
		}
		if m, ok := in.Inst.Args[1].(x86asm.Mem); ok {
			if m.Base == x86asm.RIP {
				return "got"
			}
			if m.Index != 0 && m.Disp == -1 {
				return "mem" // Itanium pointer-to-member-function: virtual slot at [vptr + ptr - 1]
			}
			return "mem"
		}
		return ""
	}
	return ""
}

type oracleTable struct {
	addr  uint64
	count int
}

// jumpTableOracle reads SHT_LLVM_JT_SIZES (0x6fff4c0d): (table address, entry count) pairs
// that clang emits for every jump table. Returns nil when the section is absent.
func jumpTableOracle(path string) []oracleTable {
	f, err := elf.Open(path)
	if err != nil {
		return nil
	}
	defer f.Close()
	for _, sec := range f.Sections {
		if sec.Type != elf.SectionType(0x6fff4c0d) {
			continue
		}
		data, err := sec.Data()
		if err != nil {
			return nil
		}
		var out []oracleTable
		for i := 0; i+16 <= len(data); i += 16 {
			out = append(out, oracleTable{binary.LittleEndian.Uint64(data[i:]), int(binary.LittleEndian.Uint64(data[i+8:]))})
		}
		return out
	}
	return nil
}

// deepKind classifies an unresolved `jmp reg` whose source is not in its own block by looking
// at every write to the register anywhere in the function: "table-load" if some write loads
// from an indexed memory operand (a jump table whose load is in another block), "param" if the
// register is never written (an incoming argument: a thunk or tail call), else "other:<op>".
func deepKind(fn *disasm.Function, jmp disasm.Instruction) string {
	reg, ok := jmp.Inst.Args[0].(x86asm.Reg)
	if !ok {
		return "nonreg"
	}
	var kinds []string
	for _, b := range fn.Blocks {
		for _, in := range b.Insts {
			if in.Address == jmp.Address {
				continue
			}
			dst, ok := in.Inst.Args[0].(x86asm.Reg)
			if !ok || dst != reg {
				continue
			}
			switch in.Inst.Op {
			case x86asm.CMP, x86asm.TEST, x86asm.PUSH:
				continue
			case x86asm.MOV, x86asm.MOVSXD, x86asm.MOVZX, x86asm.MOVSX:
				if m, ok := in.Inst.Args[1].(x86asm.Mem); ok && m.Index != 0 && m.Disp == -1 && m.Base != x86asm.RIP {
					return "mem" // Itanium pointer-to-member-function virtual slot: [vptr + ptr - 1]
				}
				if m, ok := in.Inst.Args[1].(x86asm.Mem); ok {
					if m.Index != 0 {
						kinds = append(kinds, "idx-load")
					} else if m.Base == x86asm.RIP {
						kinds = append(kinds, "rip-load")
					} else {
						kinds = append(kinds, "mem-load")
					}
				} else {
					kinds = append(kinds, "mov-reg")
				}
			case x86asm.LEA:
				kinds = append(kinds, "lea")
			case x86asm.ADD:
				kinds = append(kinds, "add")
			case x86asm.POP:
				kinds = append(kinds, "pop")
			default:
				kinds = append(kinds, "op:"+in.Inst.Op.String())
			}
		}
	}
	if len(kinds) == 0 {
		return "param"
	}
	sort.Strings(kinds)
	out := kinds[0]
	for _, k := range kinds[1:] {
		if k != out && !strings.Contains(out, k) {
			out += "+" + k
		}
	}
	return out
}
