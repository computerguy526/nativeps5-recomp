// Package tcache is the per-title translation cache (docs/m3/iteration-speed.md): an index with
// one row per translated chunk (and per tier-1 fragment), and an optional compressed object store.
//
// The index is an append-friendly JSON Lines file rewritten atomically on Save (no SQLite or
// zstd library is vendored; the index of a 2,200-chunk title is about 1 MB). A chunk row records
// what its emitted assembly was derived from, so a rebuild can tell which chunks are still valid
// without lifting them:
//
//	Key       sha256(lowering id, lifter/emitter configuration, GuestHash, FactsHash, chunk name)
//	GuestHash hash of the chunk's functions as decoded: guest bytes of every instruction, block
//	          structure, flag-liveness bits, jump tables
//	FactsHash the analysis facts the lowering consulted besides the bytes: whether each direct
//	          call/jump target is a known function, shimmed entries, landing-pad policy
//	AsmHash   sha256 of the emitted assembly (the assembler is skipped when it is unchanged)
//	ObjHash   sha256 of the assembled object
package tcache

import (
	"bufio"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"sync"
	"time"

	"ps4-recomp/pkg/diag"
	"ps4-recomp/pkg/lifter"
)

// Row is one index entry.
type Row struct {
	Kind      string `json:"kind"` // "chunk" or "frag"
	ID        string `json:"id"`   // chunk: code_NNN; frag: guest rip in hex
	Key       string `json:"key"`
	First     uint64 `json:"first,omitempty"`
	Last      uint64 `json:"last,omitempty"`
	NFuncs    int    `json:"nfuncs,omitempty"`
	Insts     int    `json:"insts,omitempty"`
	GuestHash string `json:"guest_hash,omitempty"`
	FactsHash string `json:"facts_hash,omitempty"`
	Lowering  string `json:"lowering,omitempty"`
	AsmHash   string `json:"asm_hash,omitempty"`
	AsmBytes  int64  `json:"asm_bytes,omitempty"`
	ObjHash   string `json:"obj_hash,omitempty"`
	ObjSize   int64  `json:"obj_size,omitempty"`
	ObjZSize  int64  `json:"obj_zsize,omitempty"`
	Used      int64  `json:"used"`

	// what a cache hit cannot recompute from the decoded functions
	UD2        int                      `json:"ud2,omitempty"`
	PCEntries  int                      `json:"pc_entries,omitempty"`
	Unsupp     []diag.UnsupportedHit    `json:"unsupp,omitempty"`
	UnsuppSite []lifter.UnsupportedSite `json:"unsupp_sites,omitempty"`
	Warnings   []string                 `json:"warnings,omitempty"`
}

// Index is the in-memory index of one cache directory.
type Index struct {
	dir   string
	mu    sync.Mutex
	rows  map[string]*Row // kind + "/" + id
	dirty bool
}

func rowKey(kind, id string) string { return kind + "/" + id }

// Open reads <dir>/index.jsonl (a missing or damaged file is an empty index).
func Open(dir string) *Index {
	x := &Index{dir: dir, rows: map[string]*Row{}}
	f, err := os.Open(filepath.Join(dir, "index.jsonl"))
	if err != nil {
		return x
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 1<<20), 64<<20)
	for sc.Scan() {
		var r Row
		if json.Unmarshal(sc.Bytes(), &r) == nil && r.ID != "" {
			x.rows[rowKey(r.Kind, r.ID)] = &r
		}
	}
	return x
}

func (x *Index) Dir() string { return x.dir }

// Lookup returns the row of (kind, id) when its key equals key.
func (x *Index) Lookup(kind, id, key string) (Row, bool) {
	x.mu.Lock()
	defer x.mu.Unlock()
	r := x.rows[rowKey(kind, id)]
	if r == nil || r.Key != key {
		return Row{}, false
	}
	return *r, true
}

// Get returns the row of (kind, id) whatever its key.
func (x *Index) Get(kind, id string) (Row, bool) {
	x.mu.Lock()
	defer x.mu.Unlock()
	if r := x.rows[rowKey(kind, id)]; r != nil {
		return *r, true
	}
	return Row{}, false
}

// Put stores or replaces a row.
func (x *Index) Put(r Row) {
	r.Used = time.Now().Unix()
	x.mu.Lock()
	x.rows[rowKey(r.Kind, r.ID)] = &r
	x.dirty = true
	x.mu.Unlock()
}

// Touch marks a row as used now (in memory; written by the next Save).
func (x *Index) Touch(kind, id string) {
	x.mu.Lock()
	if r := x.rows[rowKey(kind, id)]; r != nil {
		r.Used = time.Now().Unix()
		x.dirty = true
	}
	x.mu.Unlock()
}

// Drop removes the rows of kind whose id is not in keep.
func (x *Index) Drop(kind string, keep map[string]bool) int {
	x.mu.Lock()
	defer x.mu.Unlock()
	n := 0
	for k, r := range x.rows {
		if r.Kind == kind && !keep[r.ID] {
			delete(x.rows, k)
			n++
		}
	}
	if n > 0 {
		x.dirty = true
	}
	return n
}

// Rows returns all rows of kind sorted by id.
func (x *Index) Rows(kind string) []Row {
	x.mu.Lock()
	defer x.mu.Unlock()
	var out []Row
	for _, r := range x.rows {
		if r.Kind == kind {
			out = append(out, *r)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out
}

// Save writes the index atomically (no-op when nothing changed).
func (x *Index) Save() error {
	x.mu.Lock()
	defer x.mu.Unlock()
	if !x.dirty {
		return nil
	}
	if err := os.MkdirAll(x.dir, 0o755); err != nil {
		return err
	}
	keys := make([]string, 0, len(x.rows))
	for k := range x.rows {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	var b bytes.Buffer
	enc := json.NewEncoder(&b)
	for _, k := range keys {
		if err := enc.Encode(x.rows[k]); err != nil {
			return err
		}
	}
	tmp := filepath.Join(x.dir, "index.jsonl.tmp")
	if err := os.WriteFile(tmp, b.Bytes(), 0o644); err != nil {
		return err
	}
	x.dirty = false
	return os.Rename(tmp, filepath.Join(x.dir, "index.jsonl"))
}

// HashBytes is the hex sha256 of b.
func HashBytes(b []byte) string { s := sha256.Sum256(b); return hex.EncodeToString(s[:]) }

// HashFile is the hex sha256 of a file.
func HashFile(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

// ObjStore is a content-addressed store of assembled objects, compressed with the zstd command
// line tool when it is installed (stored raw otherwise). An object is keyed by the hash of the
// assembly it came from (plus the assembler flags), so any build of the same title or another
// output directory reuses it instead of running clang.
type ObjStore struct {
	dir  string
	zstd string
	mu   sync.Mutex
	raw  int64
	comp int64
}

// OpenObjStore opens (creating) the store below dir.
func OpenObjStore(dir string) *ObjStore {
	_ = os.MkdirAll(dir, 0o755)
	z, _ := exec.LookPath("zstd")
	if z == "" {
		if _, err := os.Stat("/opt/homebrew/bin/zstd"); err == nil {
			z = "/opt/homebrew/bin/zstd"
		}
	}
	return &ObjStore{dir: dir, zstd: z}
}

func (s *ObjStore) path(hash string) string {
	ext := ".o"
	if s.zstd != "" {
		ext = ".o.zst"
	}
	return filepath.Join(s.dir, hash[:2], hash+ext)
}

// Has reports whether an object for hash is stored.
func (s *ObjStore) Has(hash string) bool {
	for _, ext := range []string{".o.zst", ".o"} {
		if _, err := os.Stat(filepath.Join(s.dir, hash[:2], hash+ext)); err == nil {
			return true
		}
	}
	return false
}

// Save stores the object file obj under hash and returns its stored size.
func (s *ObjStore) Save(hash, obj string) (int64, error) {
	dst := s.path(hash)
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return 0, err
	}
	tmp := dst + ".tmp"
	var err error
	if s.zstd != "" {
		err = exec.Command(s.zstd, "-q", "-3", "-f", "-o", tmp, obj).Run()
	} else {
		err = copyFile(obj, tmp)
	}
	if err != nil {
		_ = os.Remove(tmp)
		return 0, err
	}
	if err := os.Rename(tmp, dst); err != nil {
		return 0, err
	}
	fi, err := os.Stat(dst)
	if err != nil {
		return 0, err
	}
	return fi.Size(), nil
}

// Restore writes the stored object for hash to obj.
func (s *ObjStore) Restore(hash, obj string) error {
	tmp := obj + ".tmp"
	var err error
	if p := filepath.Join(s.dir, hash[:2], hash+".o.zst"); fileExists(p) {
		if s.zstd == "" {
			return fmt.Errorf("zstd not installed")
		}
		err = exec.Command(s.zstd, "-q", "-d", "-f", "-o", tmp, p).Run()
	} else {
		err = copyFile(filepath.Join(s.dir, hash[:2], hash+".o"), tmp)
	}
	if err != nil {
		_ = os.Remove(tmp)
		return err
	}
	return os.Rename(tmp, obj)
}

func fileExists(p string) bool { _, err := os.Stat(p); return err == nil }

func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.Create(dst)
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, in); err != nil {
		out.Close()
		return err
	}
	return out.Close()
}
