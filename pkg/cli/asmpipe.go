package cli

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"strings"
	"sync"

	"ps4-recomp/pkg/emitter"
	"ps4-recomp/pkg/tcache"
)

// asmPipeline assembles the emitter's chunks as they are produced: each chunk's .s is written,
// handed to a bounded pool of clang workers, and deleted once its object file exists, so the
// intermediate assembly on disk is bounded by (workers + queue) chunks instead of the whole
// program (about 10x the object code). Chunks whose object file is current (same assembly
// content hash, kept in a sidecar) are not assembled again.
type asmPipeline struct {
	ctx       context.Context
	cancel    context.CancelFunc
	keep      bool
	workers   int
	flagsKey  string
	jobs      chan asmJob
	store     *tcache.ObjStore // optional content-addressed object store (asm hash -> compressed object)
	nRestored int
	wg        sync.WaitGroup

	mu       sync.Mutex
	err      error
	failed   []string
	objs     map[int]string
	nAsm     int
	nReused  int
	liveAsm  int64 // bytes of .s files on disk right now
	liveObj  int64 // bytes of .o files of this build
	peakAsm  int64
	peakDisk int64 // peak of liveAsm + liveObj
	totalAsm int64 // bytes of assembly generated
	nChunks  int
}

type asmJob struct {
	idx   int
	src   string
	obj   string
	hash  string
	bytes int64
}

// asmStats is what the pipeline reports (diagnostics bundle, build log).
type asmStats struct {
	Chunks         int
	Assembled      int
	Reused         int
	TotalAsmBytes  int64
	PeakAsmBytes   int64 // peak intermediate assembly on disk
	PeakDiskBytes  int64 // peak assembly + object files of the chunks
	ObjBytes       int64
	KeptAsm        bool
	Workers        int
	AsmFilesLeft   int
	AsmBytesLeft   int64
	StaleRemoved   int
	Restored       int // objects restored from the object store instead of assembled
	FailedChunkSrc []string
}

// asmTool is the assembler driver for the generated code. The Apple toolchain's integrated
// assembler needs about 19 us per line of the generated assembly; Homebrew's LLVM 23 needs about
// 4 us and produces the same objects (checked byte for byte on sampled chunks, docs/m3/
// iteration-speed.md). NPS5_ASM overrides; without Homebrew LLVM it is the system clang.
func asmTool() string {
	if v := os.Getenv("NPS5_ASM"); v != "" {
		return v
	}
	if p := "/opt/homebrew/opt/llvm/bin/clang"; fileExists(p) {
		return p
	}
	return "clang"
}

func asmFlags() []string {
	if runtime.GOOS == "darwin" && runtime.GOARCH == "arm64" {
		return []string{"-target", "arm64-apple-darwin", "-mcpu=apple-m1"}
	}
	return nil
}

func newAsmPipeline(ctx context.Context, workers int, keep bool) *asmPipeline {
	if workers < 1 {
		workers = 1
	}
	ctx, cancel := context.WithCancel(ctx)
	p := &asmPipeline{
		ctx: ctx, cancel: cancel, keep: keep, workers: workers,
		flagsKey: asmTool() + " " + strings.Join(asmFlags(), " "),
		jobs:     make(chan asmJob, 2), // small queue: the .s files on disk are the jobs in flight plus these
		objs:     map[int]string{},
	}
	for range workers {
		p.wg.Go(p.worker)
	}
	return p
}

func (p *asmPipeline) fail(err error, src string) {
	p.mu.Lock()
	if p.err == nil {
		p.err = err
	}
	if src != "" {
		p.failed = append(p.failed, src)
	}
	p.mu.Unlock()
	p.cancel()
}

func (p *asmPipeline) track(asmDelta, objDelta int64) {
	p.mu.Lock()
	p.liveAsm += asmDelta
	p.liveObj += objDelta
	p.peakAsm = max(p.peakAsm, p.liveAsm)
	p.peakDisk = max(p.peakDisk, p.liveAsm+p.liveObj)
	p.mu.Unlock()
}

// Chunk implements emitter.ChunkSink.
func (p *asmPipeline) Chunk(c emitter.AsmChunk) error {
	if p.ctx.Err() != nil {
		p.mu.Lock()
		defer p.mu.Unlock()
		if p.err != nil {
			return p.err
		}
		return errBuildCancelled
	}
	if c.DataHash == "" && c.Data != nil {
		c.DataHash = tcache.HashBytes(c.Data)
	}
	if c.Bytes == 0 {
		c.Bytes = int64(len(c.Data))
	}
	hash := p.hashOf(c)
	obj := strings.TrimSuffix(c.Path, ".s") + ".o"

	p.mu.Lock()
	p.nChunks++
	p.totalAsm += c.Bytes
	p.mu.Unlock()

	if prev, err := os.ReadFile(obj + ".hash"); err == nil && string(prev) == hash {
		if fi, err := os.Stat(obj); err == nil && fi.Size() > 0 {
			if p.keep {
				if err := os.WriteFile(c.Path, c.Data, 0o644); err != nil {
					p.fail(err, "")
					return err
				}
			}
			p.mu.Lock()
			p.objs[c.Index] = obj
			p.nReused++
			p.mu.Unlock()
			p.track(0, fi.Size())
			return nil
		}
	}
	_ = os.Remove(obj + ".hash") // the object about to be replaced is not current any more
	if p.store != nil && p.store.Has(hash) {
		if err := p.store.Restore(hash, obj); err == nil {
			_ = os.WriteFile(obj+".hash", []byte(hash), 0o644)
			var osz int64
			if fi, err := os.Stat(obj); err == nil {
				osz = fi.Size()
			}
			if p.keep && c.Data != nil {
				if err := os.WriteFile(c.Path, c.Data, 0o644); err != nil {
					p.fail(err, "")
					return err
				}
			}
			p.mu.Lock()
			p.objs[c.Index] = obj
			p.nReused++
			p.nRestored++
			p.mu.Unlock()
			p.track(0, osz)
			return nil
		}
	}
	if c.Data == nil {
		// the emitter promised (CanReuse) an object that is gone: cannot happen unless it was
		// removed in between
		err := fmt.Errorf("chunk %d: cached object for %s disappeared", c.Index, obj)
		p.fail(err, "")
		return err
	}
	if err := os.WriteFile(c.Path, c.Data, 0o644); err != nil {
		p.fail(err, "")
		return err
	}
	p.track(int64(len(c.Data)), 0)
	select {
	case p.jobs <- asmJob{idx: c.Index, src: c.Path, obj: obj, hash: hash, bytes: int64(len(c.Data))}:
		return nil
	case <-p.ctx.Done():
		p.mu.Lock()
		defer p.mu.Unlock()
		if p.err != nil {
			return p.err
		}
		return errBuildCancelled
	}
}

func (p *asmPipeline) worker() {
	for j := range p.jobs {
		if p.ctx.Err() != nil {
			continue // cancelled: the .s stays on disk, nothing is half-built
		}
		tmp := j.obj + ".tmp"
		args := append(asmFlags(), "-c", j.src, "-o", tmp)
		out, err := groupCommand(p.ctx, asmTool(), args...).CombinedOutput()
		if err != nil {
			_ = os.Remove(tmp)
			if p.ctx.Err() != nil {
				// killed by a cancel (the user's, or the failure of another chunk, which has
				// recorded its own error): not a failure of this chunk, its .s stays
				p.fail(errBuildCancelled, "")
				continue
			}
			p.fail(fmt.Errorf("assembling %s failed (the .s is kept for diagnosis): %w\n%s", j.src, err, truncateOut(out)), j.src)
			continue
		}
		if len(out) > 0 {
			fmt.Fprintf(logw, "             assembler output for %s:\n%s\n", filepath.Base(j.src), truncateOut(out))
		}
		if err := os.Rename(tmp, j.obj); err != nil {
			p.fail(err, j.src)
			continue
		}
		_ = os.WriteFile(j.obj+".hash", []byte(j.hash), 0o644)
		if p.store != nil {
			_, _ = p.store.Save(j.hash, j.obj)
		}
		var osz int64
		if fi, err := os.Stat(j.obj); err == nil {
			osz = fi.Size()
		}
		if p.keep {
			p.track(0, osz)
		} else {
			_ = os.Remove(j.src)
			p.track(-j.bytes, osz)
		}
		p.mu.Lock()
		p.objs[j.idx] = j.obj
		p.nAsm++
		p.mu.Unlock()
	}
}

func truncateOut(b []byte) string {
	const max = 4000
	if len(b) > max {
		return string(b[:max]) + "\n... (truncated)"
	}
	return string(b)
}

var chunkFileRE = regexp.MustCompile(`^code_(\d+)\.(s|o|o\.hash|o\.tmp)$`)

// Finish waits for the workers and returns the object files in chunk order. A chunk file is stale if its index is at least the number
// of chunks the emitter produced: chunk files of an earlier, larger translation in the same
// directory are removed once the whole emission succeeded.
func (p *asmPipeline) Finish(outDir string, emitErr error) ([]string, asmStats, error) {
	close(p.jobs)
	p.wg.Wait()
	p.cancel()
	st := asmStats{Workers: p.workers, KeptAsm: p.keep}
	p.mu.Lock()
	defer p.mu.Unlock()
	st.Chunks, st.Assembled, st.Reused = p.nChunks, p.nAsm, p.nReused
	st.TotalAsmBytes, st.PeakAsmBytes, st.PeakDiskBytes, st.ObjBytes = p.totalAsm, p.peakAsm, p.peakDisk, p.liveObj
	st.FailedChunkSrc = p.failed
	st.Restored = p.nRestored
	if emitErr == nil && p.err == nil && p.nChunks > 0 {
		if ents, err := os.ReadDir(outDir); err == nil {
			for _, e := range ents {
				if m := chunkFileRE.FindStringSubmatch(e.Name()); m != nil {
					if n, _ := strconv.Atoi(m[1]); n >= p.nChunks {
						_ = os.Remove(filepath.Join(outDir, e.Name()))
						st.StaleRemoved++
					}
				}
			}
		}
	}
	if ents, err := os.ReadDir(outDir); err == nil {
		for _, e := range ents {
			if m := chunkFileRE.FindStringSubmatch(e.Name()); m != nil && m[2] == "s" {
				st.AsmFilesLeft++
				if fi, err := e.Info(); err == nil {
					st.AsmBytesLeft += fi.Size()
				}
			}
		}
	}
	if p.err != nil {
		return nil, st, p.err
	}
	objs := make([]string, 0, len(p.objs))
	for i := 0; i < p.nChunks; i++ {
		if o, ok := p.objs[i]; ok {
			objs = append(objs, o)
		}
	}
	if len(objs) != p.nChunks {
		return nil, st, fmt.Errorf("assembled %d of %d chunks", len(objs), p.nChunks)
	}
	return objs, st, nil
}

// hashOf is the content hash of a chunk's object: the assembly hash and the assembler flags.
func (p *asmPipeline) hashOf(c emitter.AsmChunk) string {
	return tcache.HashBytes([]byte(p.flagsKey + "\x00" + c.DataHash))
}

// FlagsKey is part of the emitter's chunk cache key.
func (p *asmPipeline) FlagsKey() string { return p.flagsKey }

// objectCurrent reports whether obj is the assembled object of hash.
func objectCurrent(obj, hash string) (int64, bool) {
	prev, err := os.ReadFile(obj + ".hash")
	if err != nil || string(prev) != hash {
		return 0, false
	}
	fi, err := os.Stat(obj)
	if err != nil || fi.Size() == 0 {
		return 0, false
	}
	return fi.Size(), true
}

// CanReuse implements emitter.ObjectReuser: the object of the chunk is already in place (or in
// the object store, in which case it is put in place now).
func (p *asmPipeline) CanReuse(c emitter.AsmChunk) bool {
	if p.keep {
		return false // -keep-asm needs the text
	}
	obj := strings.TrimSuffix(c.Path, ".s") + ".o"
	hash := p.hashOf(c)
	if _, ok := objectCurrent(obj, hash); ok {
		return true
	}
	if p.store != nil && p.store.Has(hash) {
		return true // Chunk restores it
	}
	return false
}

// ObjectInfo implements emitter.ObjectReuser (the cache index records the object's identity).
func (p *asmPipeline) ObjectInfo(c emitter.AsmChunk) (string, int64, int64) {
	return p.hashOf(c), 0, 0
}
