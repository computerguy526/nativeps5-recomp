package emitter

import (
	"bytes"
	"fmt"
	"path/filepath"
	"slices"
	"strconv"
	"strings"

	"golang.org/x/arch/x86/x86asm"

	"ps4-recomp/pkg/diag"
	"ps4-recomp/pkg/disasm"
	"ps4-recomp/pkg/lifter"
	"ps4-recomp/pkg/tcache"
)

// blockLabel is the local label of the basic block at guest address pc of the function at fn.
// The lifter names these labels L_fn_0x<fn>_loc_0x<pc> in the branches it emits; leanLine
// rewrites every occurrence to this shorter form (a label appears once per definition and once
// per branch to it, so the long form is a measurable share of the text).
func blockLabel(fn, pc uint64) string { return fmt.Sprintf("Lf%x_%x", fn, pc) }

// The TSO fast-path labels (two definitions and two branches per guest memory access) get the same
// treatment. The new prefixes cannot collide with the lifter's other local labels, which all
// start "L_".
var labelShortener = strings.NewReplacer("L_fn_0x", "Lf", "_loc_0x", "_", "L_tso_ok_", "Lo", "L_tso_done_", "Ld", "L_tso_cold_", "Lc")

// leanLine turns one line produced by the lifter into the line written to the assembly file;
// keep is false for a line that is dropped. Lean output (the default) has no comments, one tab
// of indentation and no space after commas; with comments the lifter's text is kept as it is
// (apart from the block label names). The assembled code is the same either way.
func leanLine(line string, comments bool) (out string, keep bool) {
	if strings.Contains(line, "L_") {
		line = labelShortener.Replace(line)
	}
	if comments {
		return line, true
	}
	t := strings.TrimSpace(line)
	if t == "" || strings.HasPrefix(t, "//") {
		return "", false
	}
	if !strings.HasPrefix(t, ".asc") { // .asciz/.ascii hold arbitrary text
		if i := strings.Index(t, "//"); i >= 0 {
			t = strings.TrimSpace(t[:i])
		}
		if strings.Contains(t, ", ") {
			t = strings.ReplaceAll(t, ", ", ",")
		}
	}
	if line[0] == ' ' || line[0] == '\t' {
		return "\t" + t, true
	}
	return t, true
}

// asmSizer follows the byte size of the code of one function as its lines are written, so the
// host offset of every guest instruction is known without a label per instruction. The
// assembler checks the final size (see chunkRender.endFunc); a line shape the sizer does not
// know marks the function irregular, and its pc-map record is then left out.
type asmSizer struct {
	off       int
	irregular bool
	inData    bool // between .pushsection and .popsection (call-site inline cache records): no code
}

func (s *asmSizer) line(l string) {
	t := strings.TrimSpace(l)
	if t == "" || strings.HasPrefix(t, "//") {
		return
	}
	if strings.HasSuffix(t, ":") && !strings.ContainsAny(t, " \t") {
		return // label
	}
	if s.inData { // a data record in another section; only the end of it is looked at
		if strings.HasPrefix(t, ".popsection") {
			s.inData = false
		}
		return
	}
	if strings.HasPrefix(t, ".pushsection") {
		s.inData = true
		return
	}
	if t[0] != '.' {
		s.off += 4 // one AArch64 instruction (the lowerings use no pseudo-instruction that expands)
		return
	}
	f := strings.Fields(t)
	switch f[0] {
	case ".asciz", ".string":
		n, ok := asmStringLen(strings.TrimSpace(strings.TrimPrefix(t, f[0])))
		if !ok {
			s.irregular = true
			return
		}
		s.off += n + 1
	case ".zero":
		n, err := strconv.Atoi(strings.TrimSpace(strings.TrimPrefix(t, ".zero")))
		if err != nil || n < 0 {
			s.irregular = true
			return
		}
		s.off += n
	case ".balign", ".p2align":
		if len(f) != 2 {
			s.irregular = true
			return
		}
		var n int
		if _, err := fmt.Sscanf(f[1], "%d", &n); err != nil {
			s.irregular = true
			return
		}
		if f[0] == ".p2align" {
			if n < 0 || n > 2 {
				s.irregular = true
				return
			}
			n = 1 << n
		}
		if n <= 0 || n > 4 || n&(n-1) != 0 { // only alignments the function start already has
			s.irregular = true
			return
		}
		s.off = (s.off + n - 1) &^ (n - 1)
	default:
		s.irregular = true
	}
}

// asmStringLen is the byte length of the quoted string operand of .asciz (without the NUL),
// for the escapes the lifter produces (\").
func asmStringLen(q string) (int, bool) {
	if len(q) < 2 || q[0] != '"' || q[len(q)-1] != '"' {
		return 0, false
	}
	n := 0
	body := q[1 : len(q)-1]
	for i := 0; i < len(body); i++ {
		if body[i] == '\\' {
			if i+1 >= len(body) || body[i+1] != '"' {
				return 0, false
			}
			i++
		}
		n++
	}
	return n, true
}

type pcEntry struct {
	off   int // byte offset of the instruction's first host instruction from the function start
	guest uint64
	flags uint8
}

// chunkRender renders one chunk of functions.
type chunkRender struct {
	e        *AsmEmitter
	buf      bytes.Buffer
	pcm      bytes.Buffer // the chunk's pc-map records
	cold     bytes.Buffer // outlined cold bodies (lifter.TakeCold), written after the last function
	sz       asmSizer
	ents     []pcEntry
	insts    int
	funcs    int
	baseName string
	entrySym string // set while the kept body of a wrapped function is emitted (Replacement.Wrap)
}

// put writes one lifter line, dropping its comments in lean mode, and accounts for its size.
func (c *chunkRender) put(line string) {
	if strings.IndexByte(line, '\n') >= 0 {
		for _, l := range strings.Split(strings.TrimRight(line, "\n"), "\n") {
			c.put(l)
		}
		return
	}
	out, keep := leanLine(line, c.e.Comments)
	if !keep {
		return
	}
	// The unsupported-instruction stub aligns after its inline string. The function is laid out
	// from a 4-byte aligned start, so the padding is known here; a fixed-size .zero instead of
	// an alignment keeps the layout fixed, which lets the assembler check the function size
	// while parsing (an alignment fragment does not).
	if strings.TrimSpace(out) == ".balign 4" && !c.sz.irregular {
		pad := (4 - c.sz.off%4) % 4
		if pad == 0 {
			return
		}
		out = fmt.Sprintf("%s.zero %d", out[:strings.Index(out, ".")], pad)
	}
	c.buf.WriteString(out)
	c.buf.WriteByte('\n')
	c.sz.line(out)
}

// putColdLines moves the lifter's outlined bodies into the chunk's cold area. They are not part
// of any function's host code, so the sizer does not see them.
func (c *chunkRender) putColdLines() {
	for _, l := range c.e.lifter.TakeCold() {
		if out, keep := leanLine(l, c.e.Comments); keep {
			c.cold.WriteString(out)
			c.cold.WriteByte('\n')
		}
	}
}

func (c *chunkRender) beginInst(addr uint64) {
	c.ents = append(c.ents, pcEntry{off: c.sz.off, guest: addr})
	c.insts++
}

func (c *chunkRender) flagLast(f uint8) {
	if n := len(c.ents); n > 0 {
		c.ents[n-1].flags |= f
	}
}

// chunkOut is the product of rendering one chunk: the assembly and everything the emitter and
// the diagnostics learn from it (merged into the AsmEmitter in chunk order).
type chunkOut struct {
	idx      int
	path     string
	asm      []byte // nil for a chunk served from the cache
	dataHash string // sha256 of the assembly
	asmBytes int64
	cached   bool
	facts    diag.AsmFacts
	infos    []diag.FuncInfo
	unsupp   []lifter.UnsupportedSite
	warnings []string
	insts    int
	funcs    int
	drops    int
	row      *tcache.Row // set when the chunk is to be (re)recorded in the cache index
}

// renderChunk renders chunk chunkIdx. It runs on a worker emitter (e is a private copy, see
// worker): nothing shared is written. With a cache it first decodes the functions, computes the
// chunk key and, when the index has an up to date entry whose object the sink still has, returns
// without lifting.
func (e *AsmEmitter) renderChunk(path string, chunkIdx int, chunkAddrs []uint64) (*chunkOut, error) {
	e.Facts = diag.AsmFacts{}
	e.Warnings = nil
	e.lifter.ResetChunk()
	out := &chunkOut{idx: chunkIdx, path: path, funcs: len(chunkAddrs)}
	c := &chunkRender{e: e, baseName: filepath.Base(path)}

	// Decode every function of the chunk (shimmed addresses have none).
	fns := make([]*disasm.Function, len(chunkAddrs))
	for i, addr := range chunkAddrs {
		if _, shimmed := e.shimMap[addr]; shimmed {
			if _, wrapped := e.wrapMap[addr]; !wrapped {
				continue
			}
		}
		fn, err := e.loadFunction(addr)
		if err != nil {
			return nil, fmt.Errorf("0x%x: %w", addr, err)
		}
		out.drops += e.disasm.RestoreDrops()
		fns[i] = fn
		if fn != nil && e.FuncInfoOf != nil {
			out.infos = append(out.infos, e.FuncInfoOf(fn))
		}
	}

	var key, guestHash, factsHash string
	if e.Cache != nil && e.ChunkSink != nil {
		guestHash, factsHash = e.chunkDigests(chunkAddrs, fns)
		key = tcache.HashBytes([]byte(strings.Join([]string{LoweringID(), e.configKey(), c.baseName, guestHash, factsHash}, "\x00")))
		id := strings.TrimSuffix(c.baseName, ".s")
		if row, ok := e.Cache.Lookup("chunk", id, key); ok {
			if ru, can := e.ChunkSink.(ObjectReuser); can && ru.CanReuse(AsmChunk{Index: chunkIdx, Path: path, DataHash: row.AsmHash}) {
				out.cached, out.dataHash, out.asmBytes = true, row.AsmHash, row.AsmBytes
				e.noteCachedFacts(c, out, chunkAddrs, fns, &row)
				out.facts, out.warnings, out.unsupp = e.Facts, row.Warnings, row.UnsuppSite
				out.insts = row.Insts
				return out, nil
			}
		}
	}

	if e.Comments {
		fmt.Fprintf(&c.buf, "// Chunk %d (%d functions)\n", chunkIdx, len(chunkAddrs))
	}
	fmt.Fprintf(&c.buf, "%s\n.p2align 2\n", guestTextSection)
	if e.Comments {
		c.buf.WriteByte('\n')
	}

	type padEntry struct {
		guest uint64
		label string
	}
	var pads []padEntry
	for i, addr := range chunkAddrs {
		if shim, isShimmed := e.shimMap[addr]; isShimmed {
			// Forward directly to host shim via bridge
			var stub bytes.Buffer
			writeShimStub(&stub, fmt.Sprintf("_fn_0x%x", addr), shim, addr)
			for _, l := range strings.Split(strings.TrimRight(stub.String(), "\n"), "\n") {
				if o, keep := leanLine(l, e.Comments); keep {
					c.buf.WriteString(o)
					c.buf.WriteByte('\n')
				}
			}
			e.Facts.Funcs = append(e.Facts.Funcs, diag.AddrMapFunc{Guest: addr, Symbol: fmt.Sprintf("_fn_0x%x", addr), Chunk: c.baseName})
			if _, wrapped := e.wrapMap[addr]; !wrapped || fns[i] == nil {
				continue
			}
			// Replacement.Wrap: the guest body stays, under its own symbol, for the shim to call.
			c.entrySym = WrapSymbol(addr)
		}
		fn := fns[i]
		if fn == nil {
			continue
		}
		err := c.emitFunction(addr, fn, func(guest uint64, label string) {
			pads = append(pads, padEntry{guest, label})
		})
		c.entrySym = ""
		if err != nil {
			return nil, err
		}
	}
	if c.cold.Len() > 0 {
		// Rare paths of the functions above (docs/m3/coldpath.md), in the same text section so the
		// runtime's "pc is in guest code" test holds for them; a branch out and back, no fall-through.
		if e.Comments {
			c.buf.WriteString("// cold area\n")
		}
		c.buf.WriteString(".p2align 2\n")
		c.buf.Write(c.cold.Bytes())
	}
	if len(pads) > 0 {
		c.buf.WriteString(".section __DATA,__nps5_pads,regular,no_dead_strip\n.p2align 3\n")
		for _, p := range pads {
			if e.Comments {
				fmt.Fprintf(&c.buf, "    .quad 0x%x, %s\n", p.guest, p.label)
			} else {
				fmt.Fprintf(&c.buf, "\t.quad 0x%x,%s\n", p.guest, p.label)
			}
		}
	}
	if c.pcm.Len() > 0 {
		c.buf.WriteString(".section __DATA,__nps5_pcmap,regular,no_dead_strip\n.p2align 3\n")
		c.buf.Write(c.pcm.Bytes())
	}
	out.asm = c.buf.Bytes()
	out.asmBytes = int64(len(out.asm))
	out.dataHash = tcache.HashBytes(out.asm)
	out.facts, out.warnings, out.insts = e.Facts, e.Warnings, c.insts
	out.unsupp = slices.Clone(e.lifter.Unsupported())
	if key != "" {
		id := strings.TrimSuffix(c.baseName, ".s")
		out.row = &tcache.Row{Kind: "chunk", ID: id, Key: key, GuestHash: guestHash, FactsHash: factsHash, Lowering: LoweringID(),
			AsmHash: out.dataHash, AsmBytes: out.asmBytes, NFuncs: len(chunkAddrs), Insts: c.insts,
			UD2: e.Facts.UD2Stubs, PCEntries: e.Facts.PCEntries, Unsupp: e.Facts.Unsupported, UnsuppSite: out.unsupp, Warnings: out.warnings}
		if len(chunkAddrs) > 0 {
			out.row.First, out.row.Last = chunkAddrs[0], chunkAddrs[len(chunkAddrs)-1]
		}
	}
	return out, nil
}

// noteCachedFacts rebuilds the census facts of a cached chunk from the decoded functions (what
// emitFunction would have recorded) and the cache row (what only the lowering knows).
func (e *AsmEmitter) noteCachedFacts(c *chunkRender, out *chunkOut, addrs []uint64, fns []*disasm.Function, row *tcache.Row) {
	for i, addr := range addrs {
		_, wrapped := e.wrapMap[addr]
		if _, shimmed := e.shimMap[addr]; shimmed {
			e.Facts.Funcs = append(e.Facts.Funcs, diag.AddrMapFunc{Guest: addr, Symbol: fmt.Sprintf("_fn_0x%x", addr), Chunk: c.baseName})
			if !wrapped {
				continue
			}
		}
		fn := fns[i]
		if fn == nil {
			continue
		}
		n := 0
		for _, ba := range fn.BlockOrder {
			if fn.Blocks[ba] == nil {
				continue
			}
			lbl := blockLabel(addr, ba)
			e.Facts.Blocks = append(e.Facts.Blocks, diag.BlockLabel{PC: ba, Label: lbl, Func: addr})
			n += len(fn.Blocks[ba].Insts)
		}
		e.Facts.GuestInsts += n
		if !wrapped {
			e.Facts.Funcs = append(e.Facts.Funcs, diag.AddrMapFunc{Guest: addr, Symbol: fmt.Sprintf("_fn_0x%x", addr), Insts: n, Chunk: c.baseName})
		}
	}
	e.Facts.UD2Stubs = row.UD2
	e.Facts.PCEntries = row.PCEntries
	e.Facts.Unsupported = row.Unsupp
}

// emitSingleAsmChunk renders and delivers one chunk on the emitter itself (fragments, tests).
func (e *AsmEmitter) emitSingleAsmChunk(path string, chunkIdx int, chunkAddrs []uint64) error {
	cache, prevFacts, prevWarn := e.Cache, e.Facts, e.Warnings
	e.Cache = nil
	out, err := e.renderChunk(path, chunkIdx, chunkAddrs)
	e.Cache, e.Facts, e.Warnings = cache, prevFacts, prevWarn
	if err != nil {
		return err
	}
	e.lifter.ResetChunk() // the sites are in out; deliverChunk adds them back once
	if err := e.writeChunkRegistration(path, chunkIdx, chunkAddrs); err != nil {
		return err
	}
	return e.deliverChunk(out)
}

func (e *AsmEmitter) writeChunkRegistration(path string, chunkIdx int, chunkAddrs []uint64) error {
	return e.emitChunkRegistration(strings.TrimSuffix(path, ".s")+"_reg.c", chunkIdx, chunkAddrs)
}

// deliverChunk merges a rendered chunk into the emitter (in chunk order) and hands it on.
func (e *AsmEmitter) deliverChunk(out *chunkOut) error {
	e.Facts.Funcs = append(e.Facts.Funcs, out.facts.Funcs...)
	e.Facts.Blocks = append(e.Facts.Blocks, out.facts.Blocks...)
	e.Facts.Unsupported = append(e.Facts.Unsupported, out.facts.Unsupported...)
	e.Facts.GuestInsts += out.facts.GuestInsts
	e.Facts.UD2Stubs += out.facts.UD2Stubs
	e.Facts.PCEntries += out.facts.PCEntries
	e.Warnings = append(e.Warnings, out.warnings...)
	e.lifter.AddUnsupported(out.unsupp)
	e.FuncInfos = append(e.FuncInfos, out.infos...)
	e.EmitDrops += out.drops
	if e.ChunkSink != nil {
		ac := AsmChunk{Index: out.idx, Path: out.path, Data: out.asm, DataHash: out.dataHash, Funcs: out.funcs, Insts: out.insts, Cached: out.cached, Bytes: out.asmBytes}
		if err := e.ChunkSink.Chunk(ac); err != nil {
			return err
		}
		if out.row != nil && e.Cache != nil {
			if oh, ok := e.ChunkSink.(ObjectReuser); ok {
				out.row.ObjHash, out.row.ObjSize, out.row.ObjZSize = oh.ObjectInfo(ac)
			}
			e.Cache.Put(*out.row)
		} else if out.cached && e.Cache != nil {
			e.Cache.Touch("chunk", strings.TrimSuffix(filepath.Base(out.path), ".s"))
		}
		return nil
	}
	return writeFileIfChanged(out.path, out.asm)
}

func (c *chunkRender) emitFunction(addr uint64, fn *disasm.Function, addPad func(guest uint64, label string)) error {
	e := c.e
	sym := fmt.Sprintf("_fn_0x%x", addr)
	wrapped := c.entrySym != ""
	if wrapped {
		sym = c.entrySym // the kept body of a wrapped function (Replacement.Wrap); _fn_0x<addr> is its shim stub
	}
	fmt.Fprintf(&c.buf, ".globl %s\n%s:\n", sym, sym)
	c.sz = asmSizer{}
	c.ents = c.ents[:0]
	nInsts := 0
	flat := lifter.GetCallModel() == lifter.CallFlat
	if flat {
		// Call entry: push the shadow entry; the jump entry (the body) follows at
		// lifter.FlatJumpEntryOffset. No host frame (docs/m1/flatstack.md).
		for _, line := range lifter.FlatCallEntry() {
			c.put(line)
		}
		j := lifter.FlatJumpEntrySymbol(sym)
		fmt.Fprintf(&c.buf, ".globl %s\n%s:\n", j, j)
	} else {
		// Host frame = [x30, entry guest rsp]: the second word records the guest stack
		// position of this activation's return address (x26 on entry), which lets the runtime
		// unwind the host stack to a guest frame (longjmp, C++ landing pads; docs/m1/setjmp-eh.md).
		// Same size and cost as a plain "str x30"; the epilogue "ldr x30, [sp], #16" is unchanged.
		c.put("    stp x30, x26, [sp, #-16]!")
	}
	// Return sites already labelled in this function (overlapping instruction streams can
	// decode two calls that end at the same address).
	var raSeen map[uint64]bool

	// Landing-pad table (docs/m1/setjmp-eh.md): the unwinder's context install resumes a guest
	// function at a block that no branch reaches, so the runtime needs guest pc -> host address
	// for the blocks of functions that carry an LSDA (all non-entry blocks when the image has
	// no LSDA information at all).
	// In the flat model the table is the continuation table of the slow paths (a guest address
	// can be continued at any block, since no function has a host frame): every non-entry block
	// and every return site is listed.
	wantPads := flat || e.elf != nil && (len(e.elf.LSDABounds) == 0 || e.elf.FunctionHasLSDA(fn.EntryAddr, fn.EndAddr))
	for bi, blockAddr := range fn.BlockOrder {
		block := fn.Blocks[blockAddr]
		if block == nil {
			continue
		}

		lbl := blockLabel(addr, blockAddr)
		if e.Comments {
			c.buf.WriteByte('\n')
		}
		c.buf.WriteString(lbl)
		c.buf.WriteString(":\n")
		e.Facts.Blocks = append(e.Facts.Blocks, diag.BlockLabel{PC: blockAddr, Label: lbl, Func: addr})
		if wantPads && blockAddr != addr {
			addPad(blockAddr, lbl)
		}
		e.lifter.ResetFlags()
		e.lifter.SetBlock(block)

		var lastInst disasm.Instruction
		for _, inst := range block.Insts {
			nInsts++
			if e.bb60fps {
				inst = bloodbornePatchInst(inst)
				if custom, ok := bloodborneDynamicDeltaLines(inst.Address); ok {
					c.beginInst(inst.Address)
					for _, line := range custom {
						c.put(e.lifter.RewriteGuestMem(line))
					}
					lastInst = inst
					continue
				}
			}
			lastInst = inst
			c.beginInst(inst.Address)
			nextPC := inst.Address + uint64(inst.Inst.Len)
			nUnsup := len(e.lifter.Unsupported())
			lines, err := e.lifter.LiftInstructionToBuf(inst, nextPC, fn, e.linesBuf[:0])
			if err != nil {
				e.lifter.RecordUnsupported(inst, addr, err.Error())
				e.noteUnsupported(inst, addr, "instruction")
				c.flagLast(1)
				if e.Comments {
					c.put(fmt.Sprintf("    // 0x%x: %s [UNSUPPORTED: %v]", inst.Address, inst.Inst.String(), err))
				}
				c.put("    brk #0")
				continue
			}
			e.linesBuf = lines
			if e.patch != nil {
				if site, ok := e.patch.sites[inst.Address]; ok {
					// The lowering must have taken the patch point: a fused or special-cased branch that
					// ignored it would leave a setting that does nothing.
					if !linesMention(lines, site.symbol()) {
						return fmt.Errorf("patch point at 0x%x: the lowering of %s does not support one here", inst.Address, inst.Inst.String())
					}
					e.patch.markSeen(inst.Address)
				}
			}
			c.putColdLines()
			if len(e.lifter.Unsupported()) > nUnsup {
				e.noteUnsupported(inst, addr, "instruction")
				c.flagLast(1)
			}
			for _, line := range lines {
				// The lowerings of an unsupported indirect jump/call end in a "brk #0" tagged by
				// a trailing comment; that comment is the only marker, so it is read here.
				if i := strings.Index(line, "brk #0 // unsupported indirect"); i >= 0 {
					kind := "indirect_jmp"
					if strings.Contains(line, "indirect call") {
						kind = "indirect_call"
					}
					e.noteUnsupported(inst, addr, kind)
					c.flagLast(2)
				} else if inst.Inst.Op == x86asm.UD2 && strings.TrimSpace(line) == "brk #0" {
					e.Facts.UD2Stubs++
				}
				c.put(line)
			}
			// The host continuation of the call's guest return address: the instruction after
			// the lowering (which ends with the bl, or with the local label of a call to a block
			// of this function).
			if flat && inst.Inst.Op == x86asm.CALL && isCallContinuation(lines) && !raSeen[nextPC] {
				if raSeen == nil {
					raSeen = map[uint64]bool{}
				}
				raSeen[nextPC] = true
				rl := labelShortener.Replace(lifter.ReturnSiteLabel(addr, nextPC))
				c.put(rl + ":")
				if fn.Blocks[nextPC] == nil {
					addPad(nextPC, rl)
				}
			}
		}
		// A block that falls through hands the next one CF in the block-boundary sense.
		if n := len(block.Insts); n > 0 {
			for _, line := range e.lifter.EndBlock(lastInst) {
				c.put(line)
			}
			// The next emitted block is not always the fall-through successor
			// (overlapping instruction streams): branch to it then.
			var next uint64
			hasNext := false
			for _, a := range fn.BlockOrder[bi+1:] {
				if fn.Blocks[a] != nil {
					next, hasNext = a, true
					break
				}
			}
			for _, line := range e.lifter.FallThrough(fn, lastInst, next, hasNext) {
				c.put(line)
			}
		}
	}
	if flat {
		// Control never falls off the end of a function; the label is the target of the
		// function's RET mismatch branches.
		c.put("    brk #0x3e7")
		c.put(lifter.RetSlowLabel(addr) + ":")
		c.put("    b _recomp_ret_slow")
	} else {
		c.put("    ldr x30, [sp], #16")
		c.put("    ret")
	}
	if e.Comments {
		c.buf.WriteByte('\n')
	}

	e.Facts.GuestInsts += nInsts
	if !wrapped { // the guest entry of a wrapped function is its shim stub (recorded by renderChunk)
		e.Facts.Funcs = append(e.Facts.Funcs, diag.AddrMapFunc{Guest: addr, Symbol: fmt.Sprintf("_fn_0x%x", addr), Insts: nInsts, Chunk: c.baseName})
	}
	if e.PCMap {
		c.endFunc(addr, fn.Name)
	}
	return nil
}

// noteUnsupported records an unsupported stub in the census facts.
func (e *AsmEmitter) noteUnsupported(inst disasm.Instruction, fn uint64, kind string) {
	e.Facts.Unsupported = append(e.Facts.Unsupported, diag.UnsupportedHit{
		Addr: diag.Addr(inst.Address), Func: diag.Addr(fn), Kind: kind, Text: disasm.InstString(inst.Inst),
	})
}
