package disasm

import (
	"fmt"
	"maps"
	"os"
	"runtime"
	"slices"
	"sync"

	"golang.org/x/arch/x86/x86asm"
)

// collectEntries fills d.entries with the walk's seeds and with the direct call targets found by
// a linear sweep of the executable ranges (call rel32 names a function start). FDE-covered code
// needs no sweep: its extents already bound every function. The sweep is chunked across cores;
// a chunk boundary that lands mid-instruction desynchronises for a few instructions only, and a
// target is kept only if it passes gapEntryOK, so a stray byte pattern cannot split a function.
func (d *Disassembler) collectEntries(seeds []uint64) {
	if d.entries == nil {
		d.entries = make(map[uint64]struct{}, len(seeds)*2)
	}
	for _, s := range seeds {
		d.entries[s] = struct{}{}
	}
	if d.sweepDone {
		d.sortEntries()
		return
	}
	d.sweepDone = true
	const chunk = 1 << 20
	type span struct{ lo, hi uint64 }
	var spans []span
	for _, r := range d.elf.ExecRanges {
		for lo := r.Start; lo < r.End; lo += chunk {
			hi := lo + chunk
			if hi > r.End {
				hi = r.End
			}
			spans = append(spans, span{lo, hi})
		}
	}
	img := d.elf.MemoryImage
	var mu sync.Mutex
	var branches [][2]uint64
	var wg sync.WaitGroup
	sem := make(chan struct{}, runtime.NumCPU())
	for _, sp := range spans {
		wg.Add(1)
		sem <- struct{}{}
		go func(sp span) {
			defer wg.Done()
			defer func() { <-sem }()
			var found []uint64
			var br [][2]uint64
			pc := sp.lo
			for pc < sp.hi {
				if d.elf.IsFuncEntry(pc) || d.elf.InFuncExtent(pc) {
					// skip FDE-covered code in 16-byte steps only when no extent boundary is
					// near: simply advance to the extent end.
					if ext, ok := d.elf.FuncExtent(pc); ok && ext.End > pc {
						pc = ext.End
						continue
					}
				}
				code := img.From(pc)
				if len(code) == 0 {
					break
				}
				inst, err := Decode(code)
				if err != nil || inst.Len == 0 {
					pc++
					continue
				}
				if inst.Op == x86asm.CALL {
					if rel, ok := inst.Args[0].(x86asm.Rel); ok {
						t := uint64(int64(pc) + int64(inst.Len) + int64(rel))
						if d.inCode(t) && !d.elf.InFuncExtent(t) && gapEntryOK(img, t) {
							found = append(found, t)
						}
					}
				}
				if inst.Op == x86asm.JMP || isJcc(inst.Op) {
					if rel, ok := inst.Args[0].(x86asm.Rel); ok {
						t := uint64(int64(pc) + int64(inst.Len) + int64(rel))
						if t+64 < pc || t > pc+64 {
							br = append(br, [2]uint64{pc, t})
						}
					}
				}
				pc += uint64(inst.Len)
			}
			if len(found) > 0 || len(br) > 0 {
				mu.Lock()
				for _, t := range found {
					d.entries[t] = struct{}{}
				}
				branches = append(branches, br...)
				mu.Unlock()
			}
		}(sp)
	}
	wg.Wait()
	d.splitSharedTargets(branches)
	d.dropMidInstructionEntries()
	d.sortEntries()
}

// dropMidInstructionEntries removes weak function entries that land strictly inside an
// instruction of the function decoded linearly from the entry before them. A pointer or a
// stray sweep hit outside unwind coverage can name an address that is 16-aligned and decodes
// cleanly yet sits in the middle of an instruction (a displacement byte): left in, it cuts the
// earlier function's extent mid-instruction, the tail is lifted from the wrong offset and the
// fall-through lands on an address that is not an instruction boundary. FDE starts and function
// symbols are never dropped. Dropped addresses are remembered in d.rejected so the walk does
// not queue them. The scan is chunked across cores: each chunk trusts its first entry.
func (d *Disassembler) dropMidInstructionEntries() {
	ents := make([]uint64, 0, len(d.entries))
	for e := range d.entries {
		ents = append(ents, e)
	}
	slices.Sort(ents)
	strong := func(a uint64) bool {
		if d.elf.IsFuncEntry(a) || d.elf.InFuncExtent(a) {
			return true
		}
		if sym, ok := d.elf.SymbolByAddr[a]; ok && sym.Type == 2 /* STT_FUNC */ {
			return true
		}
		return false
	}
	img := d.elf.MemoryImage
	const per = 4096
	var mu sync.Mutex
	var wg sync.WaitGroup
	sem := make(chan struct{}, runtime.NumCPU())
	var dropped []uint64
	for lo := 0; lo < len(ents); lo += per {
		hi := min(lo+per, len(ents))
		wg.Add(1)
		sem <- struct{}{}
		go func(lo, hi int) {
			defer wg.Done()
			defer func() { <-sem }()
			var drop []uint64
			for k := lo; k < hi-1; k++ {
				e := ents[k]
				if strong(e) || !d.inCode(e) || d.rejectedLocal(drop, e) {
					continue
				}
				pc := e
				j := k + 1
				for j < hi {
					limit := ents[j]
					ok, stop := true, false
					for pc < limit {
						code := img.From(pc)
						inst, err := Decode(code)
						if err != nil || inst.Len == 0 {
							ok = false
							break
						}
						pc += uint64(inst.Len)
						switch inst.Op {
						case x86asm.RET, x86asm.JMP, x86asm.UD2, x86asm.INT, x86asm.HLT:
							// Straight-line decoding ends here: what follows is another function
							// or padding, so a later entry cannot be shown to be mid-instruction.
							stop = true
						}
						if stop {
							break
						}
					}
					if !ok || (pc <= limit && (stop || pc == limit)) {
						break
					}
					// pc > limit: entry j starts inside an instruction.
					if !strong(limit) {
						drop = append(drop, limit)
					}
					j++
				}
			}
			if len(drop) > 0 {
				mu.Lock()
				dropped = append(dropped, drop...)
				mu.Unlock()
			}
		}(lo, hi)
	}
	wg.Wait()
	if len(dropped) > 0 && d.rejected == nil {
		d.rejected = make(map[uint64]struct{}, len(dropped))
	}
	for _, a := range dropped {
		if explainAddr(a) {
			fmt.Fprintf(os.Stderr, "[explain] 0x%x: dropped as mid-instruction (linear decode from the entry before it)\n", a)
		}
		delete(d.entries, a)
		d.rejected[a] = struct{}{}
	}
	d.droppedEntries = len(dropped)
}

func (d *Disassembler) rejectedLocal(drop []uint64, a uint64) bool {
	for i := len(drop) - 1; i >= 0 && i >= len(drop)-8; i-- {
		if drop[i] == a {
			return true
		}
	}
	return false
}

// DroppedEntries is the number of weak entries removed as mid-instruction (diagnostics).
func (d *Disassembler) DroppedEntries() int { return d.droppedEntries }

func (d *Disassembler) sortEntries() {
	d.sortedEntries = d.sortedEntries[:0]
	for e := range d.entries {
		d.sortedEntries = append(d.sortedEntries, e)
	}
	slices.Sort(d.sortedEntries)
}

// nextEntry returns the first known function entry above addr (0 if none).
func (d *Disassembler) nextEntry(addr uint64) uint64 {
	i, found := slices.BinarySearch(d.sortedEntries, addr)
	if found {
		i++
	}
	if i < len(d.sortedEntries) {
		return d.sortedEntries[i]
	}
	return 0
}

// splitSharedTargets makes a branch target a function entry when the branch leaves the interval
// between two known entries (the target sits in, or is shared with, another function): tail
// merged cold paths and common error blocks, which every branching function would otherwise
// decode a private copy of. Two rounds, since a new entry splits intervals for later branches.
// Branches within 64 bytes are not recorded: those are local control flow.
func (d *Disassembler) splitSharedTargets(branches [][2]uint64) {
	for round := 0; round < 2; round++ {
		starts := make([]uint64, 0, len(d.entries))
		for e := range d.entries {
			starts = append(starts, e)
		}
		slices.Sort(starts)
		floor := func(a uint64) int {
			i, _ := slices.BinarySearch(starts, a)
			if i < len(starts) && starts[i] == a {
				return i
			}
			return i - 1
		}
		added := 0
		for _, b := range branches {
			t := b[1]
			if _, ok := d.entries[t]; ok || !d.inCode(t) || d.elf.InFuncExtent(t) {
				continue
			}
			if floor(b[0]) != floor(t) {
				d.entries[t] = struct{}{}
				added++
			}
		}
		if added == 0 {
			return
		}
	}
}

// IsRejected reports whether addr was dropped as a mid-instruction entry (diagnostics).
func (d *Disassembler) IsRejected(addr uint64) bool { _, ok := d.rejected[addr]; return ok }

// dropStraddledEntry removes the weak entry at addr (the bound of the function at entry) because
// a decoded instruction of that function runs across it. It reports whether it was removed.
func (d *Disassembler) dropStraddledEntry(entry, addr uint64) bool {
	if _, ok := d.entries[addr]; !ok || addr <= entry || d.elf == nil {
		return false
	}
	if d.elf.IsFuncEntry(addr) || d.elf.InFuncExtent(addr) {
		return false
	}
	if sym, ok := d.elf.SymbolByAddr[addr]; ok && sym.Type == 2 {
		return false
	}
	if _, fall := d.fallInto[addr]; fall && keepFallInto {
		return false // a proven boundary: the decode that straddles it is the wrong one
	}
	if explainAddr(addr) {
		fmt.Fprintf(os.Stderr, "[explain] 0x%x: dropped as straddled by function 0x%x (clone %v)\n", addr, entry, d.isClone)
	}
	delete(d.entries, addr)
	if d.rejected == nil {
		d.rejected = map[uint64]struct{}{}
	}
	d.rejected[addr] = struct{}{}
	if i, found := slices.BinarySearch(d.sortedEntries, addr); found {
		d.sortedEntries = slices.Delete(d.sortedEntries, i, i+1)
	}
	d.droppedEntries++
	if d.isClone {
		d.undo = append(d.undo, addr)
	}
	return true
}

// CloneForEmit returns a copy for decoding functions on another goroutine after discovery: the
// image and the symbol tables are shared (read only), the entry sets are private. Each function
// is decoded as if it were the first one after discovery (call RestoreDrops after it).
func (d *Disassembler) CloneForEmit() *Disassembler {
	c := *d
	c.pendingData = nil
	c.entries = maps.Clone(d.entries)
	c.rejected = maps.Clone(d.rejected)
	c.sortedEntries = slices.Clone(d.sortedEntries)
	c.CapHits, c.CapHitDetails, c.PrivilegedStops = 0, nil, 0
	c.isClone, c.undo = true, nil
	return &c
}

// RestoreDrops undoes the weak-entry drops made by decoding since the last call and returns how
// many there were (zero after a normal discovery: the drops were already made then).
func (d *Disassembler) RestoreDrops() int {
	n := len(d.undo)
	for _, a := range d.undo {
		d.entries[a] = struct{}{}
		delete(d.rejected, a)
		i, found := slices.BinarySearch(d.sortedEntries, a)
		if !found {
			d.sortedEntries = slices.Insert(d.sortedEntries, i, a)
		}
		d.droppedEntries--
	}
	d.undo = d.undo[:0]
	return n
}
