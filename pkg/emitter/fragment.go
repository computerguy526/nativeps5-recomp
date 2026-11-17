package emitter

import (
	"fmt"
	"os"
	"path/filepath"
	"slices"
)

// EmitFragment lowers the functions at addrs (already decoded into the emitter's disassembler,
// or decodable from its image) into one assembly file, code_000.s in outDir, for a tier-1 miss
// fragment (docs/m1/tier1.md). Unlike EmitAll it emits no runtime, no dispatch registration
// and no module tables: the fragment is assembled into a dylib whose entries the runtime
// registers itself. The unsupported-instruction policy applies as in a full translation. It
// returns the assembly path and the sorted entry addresses.
func (e *AsmEmitter) EmitFragment(outDir string, addrs []uint64) (string, []uint64, error) {
	if err := os.MkdirAll(outDir, 0o755); err != nil {
		return "", nil, err
	}
	sorted := slices.Clone(addrs)
	slices.Sort(sorted)
	path := filepath.Join(outDir, "code_000.s")
	// The same lean emission as a full translation (asm_chunk.go), with its pc map (the table
	// lives in the dylib, the runtime registers it with diagnostics) and its continuation/landing
	// pad table; the assembly goes to a file, not to a pipeline.
	oldComments, oldPC, oldSink := e.Comments, e.PCMap, e.ChunkSink
	e.Comments, e.PCMap, e.ChunkSink = false, true, nil
	err := e.emitSingleAsmChunk(path, 0, sorted)
	e.Comments, e.PCMap, e.ChunkSink = oldComments, oldPC, oldSink
	if err != nil {
		return "", nil, fmt.Errorf("failed to emit fragment: %w", err)
	}
	// emitSingleAsmChunk also writes code_000_reg.c (full-build dispatch registration); the
	// fragment does not use it.
	_ = os.Remove(filepath.Join(outDir, "code_000_reg.c"))
	if err := e.checkUnsupported(outDir); err != nil {
		return "", nil, err
	}
	return path, sorted, nil
}
