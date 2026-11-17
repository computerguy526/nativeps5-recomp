package emitter

import (
	"bytes"
	"path/filepath"
	"strings"
	"sync"
	"testing"

	"ps4-recomp/pkg/tcache"
)

type fakeSink struct {
	mu   sync.Mutex
	have map[string]bool
	data map[int][]byte
	n    int
}

func (s *fakeSink) Chunk(c AsmChunk) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.n++
	s.have[c.DataHash] = true
	if c.Data != nil {
		s.data[c.Index] = append([]byte(nil), c.Data...)
	}
	return nil
}
func (s *fakeSink) CanReuse(c AsmChunk) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.have[c.DataHash]
}
func (s *fakeSink) ObjectInfo(c AsmChunk) (string, int64, int64) { return c.DataHash, 0, 0 }
func (s *fakeSink) FlagsKey() string                             { return "test" }

// Chunks lifted on several workers equal those lifted on one; a second run with the cache serves
// every chunk without lifting and reproduces the census facts.
func TestParallelChunksAndCache(t *testing.T) {
	chunks := [][]uint64{{0x40}, {0x40}, {0x40}, {0x40}, {0x40}}
	dir := t.TempDir()
	run := func(workers int, cache *tcache.Index, sink *fakeSink) *AsmEmitter {
		em := pcmapTestEmitter(t)
		em.Workers, em.Cache, em.ChunkSink = workers, cache, sink
		var gen []string
		if err := em.emitChunks(dir, chunks, &gen); err != nil {
			t.Fatal(err)
		}
		return em
	}
	s1 := &fakeSink{have: map[string]bool{}, data: map[int][]byte{}}
	a := run(1, nil, s1)
	s4 := &fakeSink{have: map[string]bool{}, data: map[int][]byte{}}
	cache := tcache.Open(filepath.Join(dir, "cache"))
	b := run(4, cache, s4)
	for i := range chunks {
		if !bytes.Equal(s1.data[i], s4.data[i]) || len(s4.data[i]) == 0 {
			t.Fatalf("chunk %d differs between 1 and 4 workers", i)
		}
	}
	if b.ChunkStats.Lifted != len(chunks) || b.ChunkStats.FromCache != 0 {
		t.Fatalf("stats %+v", b.ChunkStats)
	}
	c := run(4, cache, s4) // sink still has the objects
	if c.ChunkStats.FromCache != len(chunks) || c.ChunkStats.Lifted != 0 {
		t.Fatalf("second run stats %+v", c.ChunkStats)
	}
	if len(s4.data) != len(chunks) || s4.n != 2*len(chunks) {
		t.Fatalf("sink saw %d chunks", s4.n)
	}
	for _, x := range []*AsmEmitter{b, c} {
		if x.Facts.GuestInsts != a.Facts.GuestInsts || len(x.Facts.Blocks) != len(a.Facts.Blocks) ||
			len(x.Facts.Funcs) != len(a.Facts.Funcs) || x.Facts.UD2Stubs != a.Facts.UD2Stubs ||
			len(x.Facts.Unsupported) != len(a.Facts.Unsupported) || x.Facts.PCEntries != a.Facts.PCEntries ||
			len(x.lifter.Unsupported()) != len(a.lifter.Unsupported()) {
			t.Fatalf("facts differ: %+v vs %+v", x.Facts, a.Facts)
		}
	}
	// the sink lost its objects: the cache must not serve the chunks
	d := run(2, cache, &fakeSink{have: map[string]bool{}, data: map[int][]byte{}})
	if d.ChunkStats.Lifted < 1 { // (identical chunks share one object once the first is delivered)
		t.Fatalf("chunks served without their objects: %+v", d.ChunkStats)
	}
}

// NPS5_LEAN=0 at translate time changes the shim stub text (no lean stubs), so it must change the chunk cache key: a chunk
// cached with lean stubs must not be reused when lean is off, and vice versa.
func TestConfigKeyIncludesLean(t *testing.T) {
	em := pcmapTestEmitter(t)
	old := leanDisabled
	defer func() { leanDisabled = old }()
	leanDisabled = false
	on := em.configKey()
	leanDisabled = true
	off := em.configKey()
	if on == off {
		t.Fatalf("configKey does not depend on NPS5_LEAN: %q", on)
	}
	if !strings.Contains(on, "lean=true") || !strings.Contains(off, "lean=false") {
		t.Fatalf("keys: %q / %q", on, off)
	}
}
