package emitter

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io/fs"
	"regexp"
	"sort"
	"strings"
	"sync"

	recompruntime "ps4-recomp/pkg/runtime"
)

// ABIVersion is the contract between generated code and the runtime that the layout hash cannot
// see: the shadow-stack entry layout and its stride, the inline-cache record, the dispatch-table
// shape, the pc-map record format, the shim/lean bridge calling convention. Bump it whenever one of
// them changes. Cached chunks and tier-1 fragments made under another value are refused (their
// keys differ), see docs/m3/coldpath.md section 3.
const ABIVersion = 1

// layoutSources are the runtime headers whose struct-layout assertions the generated code bakes in.
var layoutSources = []string{"core/recomp_context.h", "core/nps5_shadow.h"}

var layoutLineRe = regexp.MustCompile(`^\s*(RECOMP_CTX_OFFSET\(|#define\s+RECOMP_TCB_CTX_SLOT\b|_Static_assert\(|static_assert\()`)

// layoutLines extracts, in file order, the lines of src that fix a layout the assembly depends
// on: the GuestContext offset asserts, the TCB slot, and the sizeof/offsetof static asserts.
// Comments and spacing do not count (a continued assert is read up to its terminating ';').
func layoutLines(src string) []string {
	var out []string
	var cur strings.Builder
	in := false
	for _, ln := range strings.Split(src, "\n") {
		if i := strings.Index(ln, "//"); i >= 0 {
			ln = ln[:i]
		}
		if !in {
			if !layoutLineRe.MatchString(ln) || strings.Contains(ln, "#field") { // not the body of the assert macro
				continue
			}
			in = true
			cur.Reset()
		}
		cur.WriteString(strings.Join(strings.Fields(ln), " "))
		cur.WriteByte(' ')
		t := strings.TrimSpace(ln)
		// A define ends with its line; an assert ends at ';'.
		if strings.HasPrefix(strings.TrimSpace(cur.String()), "#define") || strings.HasSuffix(t, ";") {
			out = append(out, strings.TrimSpace(cur.String()))
			in = false
		}
	}
	return out
}

// layoutHashOf hashes the layout lines of the sources read by read, the Go-side offsets of the
// flat-model fields and the ABI version.
func layoutHashOf(read func(name string) ([]byte, error), abi int) string {
	h := sha256.New()
	fmt.Fprintf(h, "abi=%d\n", abi)
	goConsts := map[string]uint64{"ctxShadowSP": ctxShadowSP, "ctxHostBoundary": ctxHostBoundary, "ctxLeanActive": ctxLeanActive}
	names := make([]string, 0, len(goConsts))
	for k := range goConsts {
		names = append(names, k)
	}
	sort.Strings(names)
	for _, k := range names {
		fmt.Fprintf(h, "go %s=%#x\n", k, goConsts[k])
	}
	for _, name := range layoutSources {
		b, err := read(name)
		if err != nil {
			fmt.Fprintf(h, "missing %s\n", name)
			continue
		}
		fmt.Fprintf(h, "file %s\n", name)
		for _, l := range layoutLines(string(b)) {
			h.Write([]byte(l))
			h.Write([]byte{'\n'})
		}
	}
	return hex.EncodeToString(h.Sum(nil))[:16]
}

var (
	layoutOnce sync.Once
	layoutHash string
)

// LayoutHash identifies the runtime struct layout (GuestContext offsets, TCB slot, boundary
// frame) and ABIVersion that the generated code was built against, as embedded in this binary.
// It is part of the chunk cache key, the translation stamp and the tier-1 manifest.
func LayoutHash() string {
	layoutOnce.Do(func() {
		layoutHash = layoutHashOf(func(n string) ([]byte, error) { return fs.ReadFile(recompruntime.FS, n) }, ABIVersion)
	})
	return layoutHash
}

// layoutHashFn is what the cache keys call (a test replaces it to simulate a changed header).
var layoutHashFn = LayoutHash

// KeyLayoutHash is the layout hash as the cache keys, the translation stamp and the tier-1
// manifest see it.
func KeyLayoutHash() string { return layoutHashFn() }

// SetLayoutHashForTest makes the cache keys see another layout hash (the effect of a changed
// struct offset) and returns the function that restores it. Tests only.
func SetLayoutHashForTest(h string) (restore func()) {
	old := layoutHashFn
	layoutHashFn = func() string { return h }
	return func() { layoutHashFn = old }
}
