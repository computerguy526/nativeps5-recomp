package cli

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"ps4-recomp/pkg/emitter"
)

const okAsm = ".section __TEXT,__recomp_guest,regular,pure_instructions\n.p2align 2\n.globl _fn_0x40\n_fn_0x40:\n\tret\n"

func skipWithoutAssembler(t *testing.T) {
	if runtime.GOOS != "darwin" || runtime.GOARCH != "arm64" {
		t.Skip("needs the Apple arm64 assembler")
	}
	if _, err := exec.LookPath("clang"); err != nil {
		t.Skip("no clang")
	}
}

func runChunks(t *testing.T, dir string, keep bool, chunks []string) ([]string, asmStats, error) {
	p := newAsmPipeline(context.Background(), 2, keep)
	var sinkErr error
	for i, c := range chunks {
		if err := p.Chunk(emitter.AsmChunk{Index: i, Path: filepath.Join(dir, "code_00"+string(rune('0'+i))+".s"), Data: []byte(c)}); err != nil {
			sinkErr = err
			break
		}
	}
	return p.Finish(dir, sinkErr)
}

// Each chunk is assembled and its .s deleted; a second run with the same content reuses the
// objects; -keep-asm keeps the .s; a stale chunk of an earlier, longer run is removed.
func TestAsmPipelineStreamsAndResumes(t *testing.T) {
	skipWithoutAssembler(t)
	dir := t.TempDir()
	stale := filepath.Join(dir, "code_009.o")
	os.WriteFile(stale, []byte("x"), 0o644)
	chunks := []string{okAsm, strings.ReplaceAll(okAsm, "_fn_0x40", "_fn_0x44")}
	objs, st, err := runChunks(t, dir, false, chunks)
	if err != nil {
		t.Fatal(err)
	}
	if len(objs) != 2 || st.Assembled != 2 || st.Reused != 0 || st.AsmFilesLeft != 0 {
		t.Fatalf("objs=%v stats=%+v", objs, st)
	}
	if st.PeakAsmBytes == 0 || st.PeakAsmBytes > st.TotalAsmBytes || st.ObjBytes == 0 {
		t.Errorf("disk stats: %+v", st)
	}
	if _, err := os.Stat(filepath.Join(dir, "code_000.s")); !os.IsNotExist(err) {
		t.Error("the .s of an assembled chunk must be deleted")
	}
	if _, err := os.Stat(stale); !os.IsNotExist(err) {
		t.Error("a stale chunk object was not removed")
	}
	// second run: nothing to assemble
	objs, st, err = runChunks(t, dir, false, chunks)
	if err != nil || len(objs) != 2 || st.Assembled != 0 || st.Reused != 2 {
		t.Fatalf("resume: objs=%v stats=%+v err=%v", objs, st, err)
	}
	// a changed chunk is assembled again, the other one is not
	chunks[1] = strings.ReplaceAll(okAsm, "_fn_0x40", "_fn_0x48")
	_, st, err = runChunks(t, dir, false, chunks)
	if err != nil || st.Assembled != 1 || st.Reused != 1 {
		t.Fatalf("partial resume: stats=%+v err=%v", st, err)
	}
	// -keep-asm
	_, st, err = runChunks(t, dir, true, chunks)
	if err != nil || st.AsmFilesLeft != 2 {
		t.Fatalf("keep: stats=%+v err=%v", st, err)
	}
}

func TestAsmPipelineKeepsFailedChunk(t *testing.T) {
	skipWithoutAssembler(t)
	dir := t.TempDir()
	_, st, err := runChunks(t, dir, false, []string{okAsm, "\tthis is not an instruction\n"})
	if err == nil || !strings.Contains(err.Error(), "code_001.s") {
		t.Fatalf("want an error naming the failed chunk, got %v", err)
	}
	if _, serr := os.Stat(filepath.Join(dir, "code_001.s")); serr != nil {
		t.Errorf("the failed chunk's .s must be kept: %v", serr)
	}
	if len(st.FailedChunkSrc) != 1 {
		t.Errorf("stats: %+v", st)
	}
	if _, serr := os.Stat(filepath.Join(dir, "code_001.o")); serr == nil {
		t.Error("a failed chunk must leave no object")
	}
}

func TestAsmCommentsFlags(t *testing.T) {
	cases := []struct {
		args []string
		want bool
	}{
		{nil, false},
		{[]string{"-asm-comments"}, true},
		{[]string{"-keep-asm"}, true},
		{[]string{"-keep-asm", "-asm-comments=false"}, false},
	}
	for _, c := range cases {
		cfg, err := ParseArgs(append(c.args, "x.elf"))
		if err != nil {
			t.Fatal(err)
		}
		if got := cfg.asmComments(); got != c.want {
			t.Errorf("%v: asmComments = %v, want %v", c.args, got, c.want)
		}
	}
}
