package tcache

import (
	"os"
	"path/filepath"
	"testing"
)

func TestIndexRoundTripAndObjStore(t *testing.T) {
	dir := t.TempDir()
	x := Open(dir)
	x.Put(Row{Kind: "chunk", ID: "code_000", Key: "k1", AsmHash: "a"})
	x.Put(Row{Kind: "chunk", ID: "code_001", Key: "k2"})
	if err := x.Save(); err != nil {
		t.Fatal(err)
	}
	y := Open(dir)
	if _, ok := y.Lookup("chunk", "code_000", "k1"); !ok {
		t.Fatal("row lost")
	}
	if _, ok := y.Lookup("chunk", "code_000", "other"); ok {
		t.Fatal("stale key accepted")
	}
	if n := y.Drop("chunk", map[string]bool{"code_000": true}); n != 1 {
		t.Fatalf("dropped %d", n)
	}
	s := OpenObjStore(filepath.Join(dir, "objs"))
	obj := filepath.Join(dir, "x.o")
	body := make([]byte, 100000)
	for i := range body {
		body[i] = byte(i % 7)
	}
	os.WriteFile(obj, body, 0o644)
	h := HashBytes(body)
	if _, err := s.Save(h, obj); err != nil {
		t.Fatal(err)
	}
	if !s.Has(h) {
		t.Fatal("not stored")
	}
	out := filepath.Join(dir, "y.o")
	if err := s.Restore(h, out); err != nil {
		t.Fatal(err)
	}
	got, _ := os.ReadFile(out)
	if string(got) != string(body) {
		t.Fatal("restored object differs")
	}
}
