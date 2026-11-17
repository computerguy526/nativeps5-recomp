package emitter

import (
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"path/filepath"
	"runtime"
	"slices"
	"sync"
	"time"

	"golang.org/x/arch/x86/x86asm"

	"ps4-recomp/pkg/diag"
	"ps4-recomp/pkg/disasm"
	"ps4-recomp/pkg/lifter"
)

// worker returns a private emitter for lifting chunks on one goroutine: own lifter and decoder
// clones (the lifter's label counters and the decoder's entry sets are per-goroutine state),
// everything else shared read-only.
func (e *AsmEmitter) worker() *AsmEmitter {
	w := *e
	w.lifter = e.lifter.Clone()
	w.lifter.SetComments(e.Comments)
	w.disasm = e.disasm.CloneForEmit()
	w.linesBuf = nil
	w.Facts = emptyFacts()
	w.Warnings = nil
	w.FuncInfos = nil
	return &w
}

func (e *AsmEmitter) configKey() string {
	sinkKey := ""
	if k, ok := e.ChunkSink.(interface{ FlagsKey() string }); ok {
		sinkKey = k.FlagsKey()
	}
	vec := "none"
	if e.disasm != nil {
		vec = e.disasm.VecLive.String() // lane liveness mode (-simd-liveness): changes the lowering
	}
	// layout/abi: the runtime struct offsets and contracts the text bakes in; lean: NPS5_LEAN=0 at translate time
	// emits no lean stubs (docs/m3/coldpath.md section 3).
	return fmt.Sprintf("%s vec=%s lean=%t comments=%t pcmap=%t strict=%t gotstub=%t bb=%t layout=%s abi=%d asm=[%s] fmt=1", e.lifter.ConfigKey(), vec, !leanDisabled, e.Comments, e.PCMap, e.PCMapStrict, ShimStubViaGOT, e.bb60fps, layoutHashFn(), ABIVersion, sinkKey)
}

// emitChunks renders the chunks on Workers goroutines and delivers them to the sink in order.
func (e *AsmEmitter) emitChunks(outDir string, chunks [][]uint64, generated *[]string) error {
	n := len(chunks)
	workers := e.Workers
	if workers < 1 {
		workers = 1
	}
	workers = min(workers, max(1, n), runtime.NumCPU()*2)
	t0 := time.Now()
	type result struct {
		out *chunkOut
		err error
	}
	results := make([]chan result, n)
	for i := range results {
		results[i] = make(chan result, 1)
	}
	jobs := make(chan int)
	inflight := make(chan struct{}, workers*3) // rendered chunks waiting for delivery are bounded
	stop := make(chan struct{})
	var stopOnce sync.Once
	halt := func() { stopOnce.Do(func() { close(stop) }) }
	var wg sync.WaitGroup
	for range workers {
		wg.Go(func() {
			w := e.worker()
			for i := range jobs {
				name := fmt.Sprintf("code_%03d.s", i)
				path := filepath.Join(outDir, name)
				out, err := w.renderChunk(path, i, chunks[i])
				if err == nil {
					err = w.writeChunkRegistration(path, i, chunks[i])
				}
				if err != nil {
					err = fmt.Errorf("failed to emit %s: %w", name, err)
				}
				results[i] <- result{out, err}
			}
		})
	}
	go func() {
		defer close(jobs)
		for i := range n {
			select {
			case inflight <- struct{}{}:
			case <-stop:
				return
			}
			select {
			case jobs <- i:
			case <-stop:
				return
			}
		}
	}()
	var firstErr error
	var st ChunkStats
	for i := range n {
		var r result
		select {
		case r = <-results[i]:
		case <-stop:
			// a delivery failed: remaining chunks are not needed
		}
		if firstErr != nil || r.out == nil && r.err == nil {
			break
		}
		<-inflight
		if r.err != nil {
			firstErr = r.err
			halt()
			break
		}
		if err := e.deliverChunk(r.out); err != nil {
			firstErr = fmt.Errorf("failed to emit %s: %w", filepath.Base(r.out.path), err)
			halt()
			break
		}
		if r.out.cached {
			st.FromCache++
		} else {
			st.Lifted++
		}
		if e.ChunkSink == nil {
			*generated = append(*generated, r.out.path)
		}
		*generated = append(*generated, filepath.Join(outDir, fmt.Sprintf("code_%03d_reg.c", i)))
	}
	halt()
	wg.Wait()
	st.Chunks, st.Workers = n, workers
	st.LiftCPU = time.Since(t0).Seconds()
	e.ChunkStats = st
	if firstErr == nil && e.EmitDrops > 0 {
		e.Warnings = append(e.Warnings, fmt.Sprintf("%d weak function entries were dropped while decoding for emission (not at discovery); chunks may differ from a sequential translation", e.EmitDrops))
	}
	return firstErr
}

// chunkDigests hashes what the lowering of a chunk is a function of: the decoded functions
// (guest bytes of every instruction, block structure, flag-liveness bits, jump tables) and the
// analysis facts consulted besides them (known/shimmed targets, landing-pad policy).
func (e *AsmEmitter) chunkDigests(addrs []uint64, fns []*disasm.Function) (guest, facts string) {
	g, f := sha256.New(), sha256.New()
	var b [8]byte
	u := func(h interface{ Write([]byte) (int, error) }, v uint64) {
		binary.LittleEndian.PutUint64(b[:], v)
		h.Write(b[:])
	}
	for i, addr := range addrs {
		u(g, addr)
		if shim, ok := e.shimMap[addr]; ok {
			g.Write([]byte("S" + shim))
			continue
		}
		fn := fns[i]
		if fn == nil {
			g.Write([]byte("N"))
			continue
		}
		g.Write([]byte(fn.Name))
		u(g, fn.EndAddr)
		if fn.UsesYmm {
			g.Write([]byte{1})
		}
		wantPads := e.elf != nil && (len(e.elf.LSDABounds) == 0 || e.elf.FunctionHasLSDA(fn.EntryAddr, fn.EndAddr))
		if wantPads {
			f.Write([]byte{1})
		}
		for _, ba := range fn.BlockOrder {
			bb := fn.Blocks[ba]
			if bb == nil {
				continue
			}
			u(g, ba)
			u(g, bb.EndAddr)
			for _, in := range bb.Insts {
				u(g, in.Address)
				u(g, uint64(in.Inst.Len))
				if e.elf != nil {
					g.Write(e.elf.MemoryImage.Bytes(in.Address, uint64(in.Inst.Len)))
				}
				var fl byte
				for k, v := range []bool{in.SkipFlags, in.CarryDead, in.OverflowDead, in.ParityDead, in.AdjustDead, in.ParityInV, in.ParitySpillPre, in.ParitySpillExit} {
					if v {
						fl |= 1 << k
					}
				}
				g.Write([]byte{fl})
				if e.patch != nil {
					if ps, ok := e.patch.sites[in.Address]; ok {
						f.Write(ps.fact()) // a patch point changes the lowering of this chunk only
						e.patch.markSeen(in.Address)
					}
				}
				if lifter.GetBMIPeep() {
					var zs byte // ZF/SF liveness decides the narrow compare and compare-and-branch lowerings
					if in.ZeroDead {
						zs |= 1
					}
					if in.SignDead {
						zs |= 2
					}
					g.Write([]byte{zs})
				}
				if e.lifter.TSOElide() != lifter.TSOElideOff {
					g.Write([]byte{in.StackPriv}) // the privacy bits decide which accesses are ordered
				}
				if in.ROLoad && lifter.GetROLoads() {
					g.Write([]byte{'R'}) // the constant-load flag decides whether the access is ordered
				}
				if len(in.Inst.Args) > 0 {
					if rel, ok := in.Inst.Args[0].(x86asm.Rel); ok {
						t := uint64(int64(in.Address) + int64(in.Inst.Len) + int64(rel))
						u(f, t)
						k := byte(0)
						if e.lifter.IsKnownFunc(t) {
							k |= 1
						}
						if _, shim := e.shimMap[t]; shim {
							k |= 2
						}
						f.Write([]byte{k})
					}
				}
			}
		}
		sites := make([]uint64, 0, len(fn.JumpTables))
		for s := range fn.JumpTables {
			sites = append(sites, s)
		}
		slices.Sort(sites)
		for _, s := range sites {
			u(g, s)
			for _, t := range fn.JumpTables[s] {
				u(g, t)
				if e.lifter.IsKnownFunc(t) {
					f.Write([]byte{1})
				}
			}
		}
	}
	return fmt.Sprintf("%x", g.Sum(nil)), fmt.Sprintf("%x", f.Sum(nil))
}

func emptyFacts() diag.AsmFacts { return diag.AsmFacts{} }
