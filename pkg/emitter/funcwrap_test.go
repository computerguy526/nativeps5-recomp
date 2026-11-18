package emitter

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"ps4-recomp/pkg/lifter"
)

// A wrapped replacement ("wrap": true) keeps the guest body: the chunk has the shim stub under
// _fn_0x<addr> (and its jump entry) and the translated body under _fn_0x<addr>_orig (and
// _fnj_0x<addr>_orig in the flat model), and it assembles; without the wrap only the stub is there.
func TestWrappedReplacementKeepsBody(t *testing.T) {
	if runtime.GOOS != "darwin" {
		t.Skip("needs the Apple arm64 assembler")
	}
	if _, err := exec.LookPath("clang"); err != nil {
		t.Skip("no clang")
	}
	defer lifter.SetCallModel(lifter.GetCallModel())
	dir := t.TempDir()
	for _, model := range []lifter.CallModel{lifter.CallNested, lifter.CallFlat} {
		lifter.SetCallModel(model)
		for _, wrap := range []bool{false, true} {
			em := pcmapTestEmitter(t)
			em.shimMap[0x40] = "shim_x"
			if wrap {
				em.wrapMap = map[uint64]string{0x40: "shim_x"}
			}
			name := map[bool]string{false: "plain", true: "wrap"}[wrap] + map[lifter.CallModel]string{lifter.CallNested: "_n", lifter.CallFlat: "_f"}[model]
			src := filepath.Join(dir, name+".s")
			if err := em.emitSingleAsmChunk(src, 0, []uint64{0x40}); err != nil {
				t.Fatal(err)
			}
			b, err := os.ReadFile(src)
			if err != nil {
				t.Fatal(err)
			}
			text := string(b)
			if strings.Count(text, "\n_fn_0x40:") != 1 && !strings.HasPrefix(text, "_fn_0x40:") {
				t.Errorf("%s: shim stub _fn_0x40 not defined once:\n%s", name, text)
			}
			if got := strings.Contains(text, "_fn_0x40_orig:"); got != wrap {
				t.Errorf("%s: kept body present = %v:\n%s", name, got, text)
			}
			if wrap && model == lifter.CallFlat && strings.Count(text, "_fnj_0x40_orig:") != 1 {
				t.Errorf("%s: no jump entry of the kept body:\n%s", name, text)
			}
			if wrap && !strings.Contains(text, "shim_x") {
				t.Errorf("%s: the stub does not call the shim", name)
			}
			assembleTest(t, src, strings.TrimSuffix(src, ".s")+".o")
		}
	}
}

// ApplyReplacements records the wrapped entries, and the generated funcrepl_params.c lists their kept
// bodies (and still has the params table).
func TestWrappedReplacementTable(t *testing.T) {
	set, fns := replTestSet()
	set.Funcs[1].Wrap = true
	e := replTestEmitter(fns, []uint64{0x1100, 0x1200}, []uint64{0x1000, 0x1100, 0x1200})
	rep, err := e.ApplyReplacements(set, false)
	if err != nil {
		t.Fatal(err)
	}
	g := uint64(0x1100) + replBias
	if e.wrapMap[g] != set.Funcs[1].Shim || len(e.wrapMap) != 1 || e.shimMap[g] != set.Funcs[1].Shim {
		t.Fatalf("wrapMap %v shimMap[%#x] %q", e.wrapMap, g, e.shimMap[g])
	}
	nw := 0
	for _, r := range rep.Replaced {
		if r.Wrap {
			nw++
		}
	}
	if nw != 1 {
		t.Errorf("report marks %d wrapped entries", nw)
	}
	dir := t.TempDir()
	p, err := e.emitReplParams(dir)
	if err != nil || p == "" {
		t.Fatalf("emitReplParams: %q %v", p, err)
	}
	b, _ := os.ReadFile(p)
	text := string(b)
	sym := strings.TrimPrefix(WrapSymbol(g), "_")
	for _, want := range []string{"nps5_repl_params[]", "nps5_repl_wraps[]", "extern void " + sym + "(void *ctx);", `{"` + set.Funcs[1].Shim + `", 0x` + strings.TrimPrefix(WrapSymbol(g), "_fn_0x")} {
		if !strings.Contains(text, strings.TrimSuffix(want, "_orig")) {
			t.Errorf("funcrepl_params.c lacks %q:\n%s", want, text)
		}
	}
	// wraps alone (no params) still produce the file, without a params table
	e2 := &AsmEmitter{wrapMap: map[uint64]string{0x40: "s"}}
	p2, err := e2.emitReplParams(dir)
	b2, _ := os.ReadFile(p2)
	if err != nil || !strings.Contains(string(b2), "nps5_repl_wraps[]") || strings.Contains(string(b2), "nps5_repl_params[]") {
		t.Errorf("wraps-only file: %v\n%s", err, b2)
	}
	// the file form
	f := filepath.Join(dir, "w.json")
	os.WriteFile(f, []byte(`{"name":"w","funcs":[{"name":"S","addr":"0x1100","size":4,"sha256":"00","shim":"s","wrap":true}]}`), 0o644)
	ls, err := LoadReplacementFile(f)
	if err != nil || len(ls.Funcs) != 1 || !ls.Funcs[0].Wrap {
		t.Errorf("wrap not read from the file: %+v %v", ls, err)
	}
}
