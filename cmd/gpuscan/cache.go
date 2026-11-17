package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"io/fs"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"sync"
)

// ---- AnyPS5 shader disk cache (core/shader/recompiler/ShaderDiskCache.cpp) --------------------
// <dir>/<hash>.bin: FileHeader{magic 'APSC', format, sourceVersion, keyBytes, payloadBytes, keyHash,
// payloadHash} (48 bytes), the key, padding to 8, then the payload. A compiled-variant payload starts
// with the artifact: u32 runtimeAbiVersion, u64 count, count SPIR-V words. Resource-plan entries
// (no SPIR-V) share the directory and are skipped.

const cacheMagic = 0x43535041

// SPIRVFromCacheEntry extracts the SPIR-V words of a shader disk-cache file; ok is false for
// resource-plan entries and anything that does not validate.
func SPIRVFromCacheEntry(b []byte) ([]uint32, bool) {
	if len(b) < 48 || binary.LittleEndian.Uint32(b) != cacheMagic {
		return nil, false
	}
	kb := binary.LittleEndian.Uint64(b[16:])
	if kb > uint64(len(b)) {
		return nil, false
	}
	po := (48 + int(kb) + 7) / 8 * 8
	return spirvAfterAbi(b[min(po, len(b)):])
}

// spirvAfterAbi parses u32 abi, u64 count, words.
func spirvAfterAbi(p []byte) ([]uint32, bool) {
	if len(p) < 16 {
		return nil, false
	}
	n := binary.LittleEndian.Uint64(p[4:])
	if n < 5 || n > uint64(len(p)-12)/4 {
		return nil, false
	}
	return WordsFromBytes(p[12 : 12+n*4])
}

// FindCacheDirs returns directories under root (searched to a bounded depth) that hold shader
// disk-cache files or a pipeline journal.
func FindCacheDirs(root string) []string {
	var out []string
	rootDepth := strings.Count(filepath.Clean(root), string(filepath.Separator))
	_ = filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return nil
		}
		if d.IsDir() {
			switch d.Name() {
			case "runs", "lldb", "Library", "userdata", "download0", "temp0", "saves":
				return filepath.SkipDir
			}
			if strings.Count(p, string(filepath.Separator))-rootDepth > 6 {
				return filepath.SkipDir
			}
			return nil
		}
		if d.Name() == "pipelines.journal" {
			out = append(out, filepath.Dir(p))
		} else if strings.HasSuffix(d.Name(), ".bin") && len(d.Name()) == 36 {
			if f, e := os.Open(p); e == nil {
				var m [4]byte
				f.Read(m[:])
				f.Close()
				if binary.LittleEndian.Uint32(m[:]) == cacheMagic {
					out = append(out, filepath.Dir(p))
					return fs.SkipAll
				}
			}
		}
		return nil
	})
	// dedupe, keep order
	seen := map[string]bool{}
	var res []string
	for _, d := range out {
		if !seen[d] {
			seen[d] = true
			res = append(res, d)
		}
	}
	// A directory with both cache files and a journal appears twice at most; the SkipAll above may stop
	// the walk early, so make sure the journal's directory is covered by a second pass.
	if len(res) == 0 || !fileExists(filepath.Join(res[0], "pipelines.journal")) {
		_ = filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
			if err == nil && !d.IsDir() && d.Name() == "pipelines.journal" && !seen[filepath.Dir(p)] {
				seen[filepath.Dir(p)] = true
				res = append(res, filepath.Dir(p))
			}
			return nil
		})
	}
	sort.Strings(res)
	return res
}

func fileExists(p string) bool { _, err := os.Stat(p); return err == nil }

// LoadCacheShaders reads every SPIR-V module of a cache directory.
func LoadCacheShaders(dir string) (mods [][]uint32, plans int, err error) {
	ents, err := os.ReadDir(dir)
	if err != nil {
		return nil, 0, err
	}
	for _, e := range ents {
		if e.IsDir() || !strings.HasSuffix(e.Name(), ".bin") || strings.HasPrefix(e.Name(), "vk-pipelines") {
			continue
		}
		b, err := os.ReadFile(filepath.Join(dir, e.Name()))
		if err != nil {
			continue
		}
		if w, ok := SPIRVFromCacheEntry(b); ok {
			mods = append(mods, w)
		} else if len(b) >= 4 && binary.LittleEndian.Uint32(b) == cacheMagic {
			plans++
		}
	}
	return mods, plans, nil
}

// ---- pipeline journal (Graphics/src/PipelineJournal.cpp, FileFormat 18) -------------------------

// ShaderStage values of ShaderRecompiler::ShaderStage.
var journalStages = []string{"Compute", "Vertex", "TessellationControl", "TessellationEvaluation", "Geometry", "Fragment", "Local", "Mesh"}

type JournalModule struct {
	Stage string
	Words []uint32
	ID    string
}

// GraphicsRecipe is the decoded pipeline state that matters for feature analysis.
type GraphicsRecipe struct {
	Stages       []string
	Path         string
	Mesh, Tess   bool
	TessGeom     bool
	Samples      uint32
	CustomSample bool
	PerSample    bool
	DepthFormat  uint32
	DepthSamples uint32
	DepthTest    bool
	DepthBounds  bool
	DepthBias    bool
	BiasClamp    float32
	DepthClamp   bool
	NegOneToOne  bool
	StencilTest  bool
	ColorFormats []uint32
	BlendEnable  bool
	DualSource   bool
	Topology     uint32
	PrimRestart  bool
	Conservative uint32
	CullMode     uint32
	PolygonMode  uint32
	LineWidth    float32
	ProvokingVtx uint32
	RectList     bool
	Discard      bool
	PointSize    float32
	Features     []string
	Signature    string
}

type Journal struct {
	Path      string
	Valid     bool
	Format    uint32
	Modules   []JournalModule
	Graphics  []GraphicsRecipe
	Computes  int
	Dropped   bool
	NoZstd    bool
	moduleIDs map[[2]uint64]int
}

type jreader struct {
	b   []byte
	pos int
	ok  bool
}

func (r *jreader) take(n int) []byte {
	if !r.ok || n < 0 || r.pos+n > len(r.b) {
		r.ok = false
		return make([]byte, max(n, 0)&0xff)
	}
	s := r.b[r.pos : r.pos+n]
	r.pos += n
	return s
}
func (r *jreader) u32() uint32 {
	b := r.take(4)
	if !r.ok {
		return 0
	}
	return binary.LittleEndian.Uint32(b)
}
func (r *jreader) u64() uint64 {
	b := r.take(8)
	if !r.ok {
		return 0
	}
	return binary.LittleEndian.Uint64(b)
}
func (r *jreader) u8() uint8 {
	b := r.take(1)
	if !r.ok {
		return 0
	}
	return b[0]
}
func (r *jreader) boolean() bool {
	v := r.u8()
	if v > 1 {
		r.ok = false
	}
	return v == 1
}
func (r *jreader) f32() float32 {
	return f32frombits(r.u32())
}
func (r *jreader) skip(n int) { r.take(n) }
func (r *jreader) raws(elem, limit int) [][]byte {
	n := r.u64()
	if !r.ok || n > uint64(limit) {
		r.ok = false
		return nil
	}
	var out [][]byte
	for i := uint64(0); i < n; i++ {
		out = append(out, r.take(elem))
	}
	return out
}

func (r *jreader) mesh() {
	for i := 0; i < 10; i++ {
		r.u32()
	}
}
func (r *jreader) tess() {
	for i := 0; i < 7; i++ {
		r.u32()
	}
	r.boolean()
	r.u32()
	r.boolean()
	r.boolean()
}

// decodeRecipe mirrors decodeRecipe/decodeState in PipelineJournal.cpp (journal format 18).
func decodeRecipe(b []byte) (*GraphicsRecipe, []uint64, bool) {
	r := &jreader{b: b, ok: true}
	g := &GraphicsRecipe{}
	r.u32() // attachment layout
	ns := r.u64()
	if !r.ok || ns > 8 {
		return nil, nil, false
	}
	var ids []uint64
	for i := uint64(0); i < ns; i++ {
		st := r.u32()
		lo, hi := r.u64(), r.u64()
		ids = append(ids, lo, hi)
		r.u32()
		r.boolean()
		if int(st) < len(journalStages) {
			g.Stages = append(g.Stages, journalStages[st])
		} else {
			g.Stages = append(g.Stages, fmt.Sprintf("Stage%d", st))
		}
	}
	r.raws(12, 64)
	r.raws(16, 64)
	r.raws(4, 4096)
	r.raws(4, 4096)
	r.boolean() // splitSets
	// state
	path := r.u32()
	g.Path = []string{"Vertex", "Geometry", "Tessellation", "TessellationGeometry"}[min(int(path), 3)]
	r.u32()
	r.u32()
	r.u32()
	r.u32()
	if r.boolean() {
		g.Mesh = true
		r.mesh()
	}
	if r.boolean() {
		g.Tess = true
		r.tess()
	}
	r.u32()
	if r.boolean() {
		g.TessGeom = true
		r.mesh()
	}
	if r.boolean() {
		g.DepthFormat = r.u32()
		g.DepthSamples = r.u32()
	}
	g.DepthTest = r.boolean()
	r.boolean()
	r.u32()
	g.DepthBounds = r.boolean()
	g.DepthBias = r.boolean()
	g.BiasClamp = r.f32()
	r.boolean()
	g.StencilTest = r.boolean()
	r.skip(28 * 2)
	r.boolean()
	nc := r.u64()
	if !r.ok || nc > 8 {
		return nil, nil, false
	}
	for i := uint64(0); i < nc; i++ {
		g.ColorFormats = append(g.ColorFormats, r.u32())
		r.u32()
		r.u8()
		r.u32()
		r.u32()
	}
	for _, bl := range r.raws(32, 8) {
		if binary.LittleEndian.Uint32(bl) != 0 {
			g.BlendEnable = true
		}
	}
	r.skip(16)
	r.boolean()
	g.RectList = r.boolean()
	g.DualSource = r.boolean()
	r.u32()
	r.u32()
	g.Topology = r.u32()
	g.PrimRestart = r.boolean()
	g.NegOneToOne = r.boolean()
	g.DepthClamp = r.boolean()
	g.Conservative = r.u32()
	g.CullMode = r.u32()
	g.PolygonMode = r.u32()
	g.LineWidth = f32frombits(r.u32())
	r.u32()
	g.ProvokingVtx = r.u32()
	g.Samples = r.u32()
	r.skip(32)
	g.CustomSample = r.boolean()
	r.boolean()
	g.PerSample = r.boolean()
	r.boolean()
	r.boolean()
	r.skip(8)
	r.u32()
	r.boolean()
	g.PointSize = r.f32()
	r.f32()
	r.f32()
	g.Discard = r.boolean()
	if !r.ok || r.pos != len(b) {
		return nil, nil, false
	}
	g.Signature = strings.Join(g.Stages, "+")
	return g, ids, true
}

func zstdDecode(frame []byte) ([]byte, error) {
	cmd := exec.Command("zstd", "-dc", "-q")
	cmd.Stdin = bytes.NewReader(frame)
	var out bytes.Buffer
	cmd.Stdout = &out
	if err := cmd.Run(); err != nil {
		return nil, err
	}
	return out.Bytes(), nil
}

// LoadJournal parses pipelines.journal. Module records are zstd-compressed; the zstd CLI is used
// for them (Go's standard library has no zstd).
func LoadJournal(path string, jobs int) (*Journal, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	j := &Journal{Path: path}
	if len(b) < 32 || binary.LittleEndian.Uint32(b) != 0x4c4a5041 {
		return j, nil
	}
	j.Format = binary.LittleEndian.Uint32(b[4:])
	j.Valid = true
	pad := func(n int) int { return (n + 7) &^ 7 }
	type modJob struct {
		stage uint32
		frame []byte
		id    [2]uint64
	}
	var mods []modJob
	var graphics [][]byte
	cur := pad(32)
	for cur+24 <= len(b) {
		kind := binary.LittleEndian.Uint32(b[cur:])
		plen := binary.LittleEndian.Uint64(b[cur+8:])
		if kind == 0 || kind > 3 || plen > uint64(len(b)-cur-24) {
			j.Dropped = true
			break
		}
		payload := b[cur+24 : cur+24+int(plen)]
		switch kind {
		case 1:
			if len(payload) >= 24 {
				mods = append(mods, modJob{binary.LittleEndian.Uint32(payload[16:]), payload[24:], [2]uint64{binary.LittleEndian.Uint64(payload), binary.LittleEndian.Uint64(payload[8:])}})
			}
		case 2:
			graphics = append(graphics, payload)
		case 3:
			j.Computes++
		}
		cur = pad(cur + 24 + int(plen))
	}
	if _, err := exec.LookPath("zstd"); err != nil && len(mods) > 0 {
		j.NoZstd = true
	} else {
		res := make([]JournalModule, len(mods))
		okm := make([]bool, len(mods))
		var wg sync.WaitGroup
		sem := make(chan struct{}, max(jobs, 1))
		for i := range mods {
			wg.Add(1)
			sem <- struct{}{}
			go func(i int) {
				defer wg.Done()
				defer func() { <-sem }()
				raw, err := zstdDecode(mods[i].frame)
				if err != nil {
					return
				}
				w, ok := spirvAfterAbi(raw)
				if !ok {
					return
				}
				st := "Stage?"
				if int(mods[i].stage) < len(journalStages) {
					st = journalStages[mods[i].stage]
				}
				res[i] = JournalModule{Stage: st, Words: w, ID: spvID(w)}
				okm[i] = true
			}(i)
		}
		wg.Wait()
		j.moduleIDs = map[[2]uint64]int{}
		for i := range res {
			if okm[i] {
				j.moduleIDs[mods[i].id] = len(j.Modules)
				j.Modules = append(j.Modules, res[i])
			}
		}
	}
	for _, p := range graphics {
		if g, _, ok := decodeRecipe(p); ok {
			j.Graphics = append(j.Graphics, *g)
		} else {
			j.Dropped = true
		}
	}
	return j, nil
}

func f32frombits(u uint32) float32 { return math.Float32frombits(u) }
