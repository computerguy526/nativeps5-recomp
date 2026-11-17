package emitter

import (
	"io/fs"
	"path/filepath"
	"strings"
	"testing"

	recompruntime "ps4-recomp/pkg/runtime"
	"ps4-recomp/pkg/tcache"
)

func embeddedHeader(t *testing.T, name string) string {
	t.Helper()
	b, err := fs.ReadFile(recompruntime.FS, name)
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

func hashWith(t *testing.T, abi int, edit func(name, src string) string) string {
	t.Helper()
	return layoutHashOf(func(name string) ([]byte, error) {
		src := embeddedHeader(t, name)
		if edit != nil {
			src = edit(name, src)
		}
		return []byte(src), nil
	}, abi)
}

// The layout hash follows every offset the generated code bakes in, and nothing else.
func TestLayoutHashFollowsOffsets(t *testing.T) {
	base := hashWith(t, ABIVersion, nil)
	if base != LayoutHash() {
		t.Fatalf("LayoutHash %s is not the hash of the embedded headers %s", LayoutHash(), base)
	}
	n := 0
	for _, name := range layoutSources {
		n += len(layoutLines(embeddedHeader(t, name)))
	}
	if n < 40 {
		t.Fatalf("only %d layout lines extracted from %v: the parser lost the asserts", n, layoutSources)
	}
	hdr := embeddedHeader(t, "core/recomp_context.h")
	for _, c := range []struct{ old, new string }{
		{"RECOMP_CTX_OFFSET(shadow_sp, 0xc58)", "RECOMP_CTX_OFFSET(shadow_sp, 0xc60)"},
		{"RECOMP_CTX_OFFSET(rax, 0x00)", "RECOMP_CTX_OFFSET(rax, 0x08)"},
		{"RECOMP_CTX_OFFSET(mem_base, 0x330)", "RECOMP_CTX_OFFSET(mem_base, 0x338)"},
		{"#define RECOMP_TCB_CTX_SLOT 0x70ULL", "#define RECOMP_TCB_CTX_SLOT 0x78ULL"},
	} {
		if !strings.Contains(hdr, c.old) {
			t.Fatalf("header no longer contains %q", c.old)
		}
		got := hashWith(t, ABIVersion, func(name, src string) string {
			if name == "core/recomp_context.h" {
				return strings.Replace(src, c.old, c.new, 1)
			}
			return src
		})
		if got == base {
			t.Errorf("changing %q left the layout hash unchanged", c.old)
		}
	}
	// the boundary frame of the flat trampoline
	got := hashWith(t, ABIVersion, func(name, src string) string {
		if name == "core/nps5_shadow.h" {
			return strings.Replace(src, "sizeof(RecompBoundary) == 208", "sizeof(RecompBoundary) == 216", 1)
		}
		return src
	})
	if got == base {
		t.Error("changing the RecompBoundary size assert left the layout hash unchanged")
	}
	if hashWith(t, ABIVersion+1, nil) == base {
		t.Error("the ABI version is not in the hash")
	}
	// comments and spacing do not count
	same := hashWith(t, ABIVersion, func(name, src string) string {
		return strings.Replace(src, "RECOMP_CTX_OFFSET(rcx, 0x08);", "RECOMP_CTX_OFFSET(rcx,   0x08);   // a comment", 1)
	})
	if same != base {
		t.Error("whitespace and comments changed the layout hash")
	}
}

// A cached chunk made under another layout (or with NPS5_LEAN=0 at translate time) is not served.
func TestChunkCacheRefusedOnLayoutMismatch(t *testing.T) {
	chunks := [][]uint64{{0x40}, {0x40}}
	dir := t.TempDir()
	cache := tcache.Open(filepath.Join(dir, "cache"))
	sink := &fakeSink{have: map[string]bool{}, data: map[int][]byte{}}
	run := func() *AsmEmitter {
		em := pcmapTestEmitter(t)
		em.Workers, em.Cache, em.ChunkSink = 2, cache, sink
		var gen []string
		if err := em.emitChunks(dir, chunks, &gen); err != nil {
			t.Fatal(err)
		}
		return em
	}
	if r := run(); r.ChunkStats.Lifted != 2 {
		t.Fatalf("first run: %+v", r.ChunkStats)
	}
	if r := run(); r.ChunkStats.FromCache != 2 {
		t.Fatalf("same configuration must be served from the cache: %+v", r.ChunkStats)
	}
	restore := SetLayoutHashForTest("0123456789abcdef") // a GuestContext offset moved
	r := run()
	restore()
	if r.ChunkStats.FromCache != 0 || r.ChunkStats.Lifted != 2 {
		t.Fatalf("a layout change must invalidate the cached chunks: %+v", r.ChunkStats)
	}
	// the cache now holds the other layout's rows: the real layout is refused in turn
	if r := run(); r.ChunkStats.FromCache != 0 {
		t.Fatalf("rows of another layout were served: %+v", r.ChunkStats)
	}
	if r := run(); r.ChunkStats.FromCache != 2 {
		t.Fatalf("rebuilt rows must be served again: %+v", r.ChunkStats)
	}
	leanDisabled = true // NPS5_LEAN=0 in the translator's environment emits other stubs
	r = run()
	leanDisabled = false
	if r.ChunkStats.FromCache != 0 {
		t.Fatalf("NPS5_LEAN=0 must not be served lean chunks: %+v", r.ChunkStats)
	}
}

func TestConfigKeyCarriesLayoutABIAndLean(t *testing.T) {
	em := pcmapTestEmitter(t)
	k := em.configKey()
	for _, want := range []string{"layout=" + LayoutHash(), "abi=", "lean=true"} {
		if !strings.Contains(k, want) {
			t.Errorf("configKey %q lacks %q", k, want)
		}
	}
}
