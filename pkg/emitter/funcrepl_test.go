package emitter

import (
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"

	"ps4-recomp/pkg/disasm"
	"ps4-recomp/pkg/elfloader"
)

const replBias = 0x400000

// replTestEmitter returns an emitter over a synthetic image whose "functions" are the byte runs
// fn[addr] (link-time address, placed at addr+replBias) plus a vtable at 0x5000 pointing at
// slots; discovered lists the function entries the CFG found.
func replTestEmitter(fns map[uint64][]byte, slots []uint64, discovered []uint64) *AsmEmitter {
	img := make([]byte, 0x800000)
	for a, b := range fns {
		copy(img[a+replBias:], b)
	}
	for i, s := range slots {
		binary.LittleEndian.PutUint64(img[0x5000+replBias+8*uint64(i):], s+replBias)
	}
	l := &elfloader.LoadedELF{FileName: "eboot.bin", MemoryImage: elfloader.NewImage(0, img), Bias: replBias}
	e := &AsmEmitter{elf: l, shimMap: map[uint64]string{}}
	for _, a := range discovered {
		e.Funcs = append(e.Funcs, disasm.ReachableFunc{Addr: a + replBias})
	}
	return e
}

func hexSHA(b []byte) string {
	s := sha256.Sum256(b)
	return hex.EncodeToString(s[:])
}

func replTestSet() (*ReplacementSet, map[uint64][]byte) {
	fns := map[uint64][]byte{
		0x1000: []byte("ctor-body-0123456789"),
		0x1100: []byte("malloc-body-abcdefghijklmnop"),
		0x1200: []byte("free-body-ABCDEFGH"),
	}
	set := &ReplacementSet{
		Name: "test",
		Funcs: []Replacement{
			{Name: "A::A", Addr: 0x1000, Size: uint64(len(fns[0x1000])), SHA256: hexSHA(fns[0x1000]), Shim: "shim_ue5_malloc_ctor"},
			{Name: "A::Malloc", Addr: 0x1100, Size: uint64(len(fns[0x1100])), SHA256: hexSHA(fns[0x1100]), Shim: "shim_ue5_malloc_malloc"},
			{Name: "A::Free", Addr: 0x1200, Size: uint64(len(fns[0x1200])), SHA256: hexSHA(fns[0x1200]), Shim: "shim_ue5_malloc_free"},
		},
		VTableChecks: []VTableCheck{{0x5000, 0, 0x1100}, {0x5000, 1, 0x1200}},
		Params:       []ReplParam{{Key: "ue5m.vtable", Addr: 0x5000}, {Key: "k.literal", Value: 0x2420}},
	}
	return set, fns
}

func TestApplyReplacementsBindsShimsAndParams(t *testing.T) {
	set, fns := replTestSet()
	e := replTestEmitter(fns, []uint64{0x1100, 0x1200}, []uint64{0x1000, 0x1100, 0x1200})
	rep, err := e.ApplyReplacements(set, false)
	if err != nil {
		t.Fatal(err)
	}
	for addr, shim := range map[uint64]string{0x1000: "shim_ue5_malloc_ctor", 0x1100: "shim_ue5_malloc_malloc", 0x1200: "shim_ue5_malloc_free"} {
		if got := e.shimMap[addr+replBias]; got != shim {
			t.Errorf("0x%x bound to %q, want %q", addr+replBias, got, shim)
		}
	}
	if len(rep.Replaced) != 3 || rep.Unverified != 0 || rep.Bias != replBias {
		t.Errorf("report %+v", rep)
	}
	if v := e.replParams["ue5m.vtable"]; v != 0x5000+replBias {
		t.Errorf("vtable param 0x%x, want biased address", v)
	}
	if v := e.replParams["k.literal"]; v != 0x2420 {
		t.Errorf("literal param 0x%x", v)
	}
	dir := t.TempDir()
	p, err := e.emitReplParams(dir)
	if err != nil || filepath.Base(p) != "funcrepl_params.c" {
		t.Fatalf("emitReplParams: %q %v", p, err)
	}
	src, _ := os.ReadFile(p)
	for _, want := range []string{`{"k.literal", 0x2420ULL}`, `{"ue5m.vtable", 0x405000ULL}`, "{0, 0}"} {
		if !strings.Contains(string(src), want) {
			t.Errorf("funcrepl_params.c lacks %q:\n%s", want, src)
		}
	}
	// the stub the emitter writes for a replaced function is a host shim stub for its own address
	var sb strings.Builder
	writeShimStub(&sb, "_fn_0x401100", e.shimMap[0x1100+replBias], 0x1100+replBias)
	for _, want := range []string{"_fn_0x401100:", "_shim_ue5_malloc_malloc@PAGE", "_recomp_call_shim"} {
		if !strings.Contains(sb.String(), want) {
			t.Errorf("stub lacks %q:\n%s", want, sb.String())
		}
	}
}

func TestApplyReplacementsRefusesWithoutChangingAnything(t *testing.T) {
	cases := []struct {
		name    string
		mutate  func(set *ReplacementSet, fns map[uint64][]byte)
		slots   []uint64
		disc    []uint64
		wantErr string
	}{
		{"a different build (digest)", func(s *ReplacementSet, f map[uint64][]byte) { f[0x1100][3] ^= 1 }, []uint64{0x1100, 0x1200}, []uint64{0x1000, 0x1100, 0x1200}, "not the one this table was made for"},
		{"vtable slot points elsewhere", func(s *ReplacementSet, f map[uint64][]byte) {}, []uint64{0x1200, 0x1100}, []uint64{0x1000, 0x1100, 0x1200}, "vtable"},
		{"not a discovered function", func(s *ReplacementSet, f map[uint64][]byte) {}, []uint64{0x1100, 0x1200}, []uint64{0x1000, 0x1100}, "not an entry of a discovered function"},
		{"bytes outside the image", func(s *ReplacementSet, f map[uint64][]byte) { s.Funcs[0].Addr = 0x7ffff00 }, []uint64{0x1100, 0x1200}, []uint64{0x1000, 0x1100, 0x1200}, "not in the image"},
		{"no shim", func(s *ReplacementSet, f map[uint64][]byte) { s.Funcs[2].Shim = "" }, []uint64{0x1100, 0x1200}, []uint64{0x1000, 0x1100, 0x1200}, "no shim"},
		{"duplicate address", func(s *ReplacementSet, f map[uint64][]byte) { s.Funcs[2] = s.Funcs[1]; s.Funcs[2].Name = "dup" }, []uint64{0x1100, 0x1200}, []uint64{0x1000, 0x1100, 0x1200}, "twice"},
		{"no digest and no vtable check", func(s *ReplacementSet, f map[uint64][]byte) { s.Funcs[0].SHA256 = "" }, []uint64{0x1100, 0x1200}, []uint64{0x1000, 0x1100, 0x1200}, "no sha256 and no vtable check"},
		{"zero size", func(s *ReplacementSet, f map[uint64][]byte) { s.Funcs[0].Size = 0; s.Funcs[0].SHA256 = "" }, []uint64{0x1100, 0x1200}, []uint64{0x1000, 0x1100, 0x1200}, "size is zero"},
		{"overlapping ranges", func(s *ReplacementSet, f map[uint64][]byte) { s.Funcs[2].Addr = 0x1110 }, []uint64{0x1100, 0x1200}, []uint64{0x1000, 0x1100, 0x1200}, "overlap"},
		{"range inside another", func(s *ReplacementSet, f map[uint64][]byte) { s.Funcs[2].Addr, s.Funcs[2].Size = 0x1101, 1 }, []uint64{0x1100, 0x1200}, []uint64{0x1000, 0x1100, 0x1200}, "overlap"},
		{"address above 48 bits", func(s *ReplacementSet, f map[uint64][]byte) { s.Funcs[2].Addr = 1 << 48 }, []uint64{0x1100, 0x1200}, []uint64{0x1000, 0x1100, 0x1200}, "48-bit"},
		{"size wraps", func(s *ReplacementSet, f map[uint64][]byte) { s.Funcs[2].Size = ^uint64(0) }, []uint64{0x1100, 0x1200}, []uint64{0x1000, 0x1100, 0x1200}, "48-bit"},
		{"shim with a quote", func(s *ReplacementSet, f map[uint64][]byte) { s.Funcs[2].Shim = "x\"; evil" }, []uint64{0x1100, 0x1200}, []uint64{0x1000, 0x1100, 0x1200}, "not a C identifier"},
		{"shim with a digit first", func(s *ReplacementSet, f map[uint64][]byte) { s.Funcs[2].Shim = "1shim" }, []uint64{0x1100, 0x1200}, []uint64{0x1000, 0x1100, 0x1200}, "not a C identifier"},
		{"shim with a space", func(s *ReplacementSet, f map[uint64][]byte) { s.Funcs[2].Shim = "a b" }, []uint64{0x1100, 0x1200}, []uint64{0x1000, 0x1100, 0x1200}, "not a C identifier"},
		{"negative vtable slot", func(s *ReplacementSet, f map[uint64][]byte) { s.VTableChecks[0].Slot = -1 }, []uint64{0x1100, 0x1200}, []uint64{0x1000, 0x1100, 0x1200}, "negative"},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			set, fns := replTestSet()
			e := replTestEmitter(fns, c.slots, c.disc)
			c.mutate(set, fns) // after the table was made: the image (or the table) differs
			e = replTestEmitter(fns, c.slots, c.disc)
			_, err := e.ApplyReplacements(set, false)
			if err == nil || !strings.Contains(err.Error(), c.wantErr) {
				t.Fatalf("error %v, want one containing %q", err, c.wantErr)
			}
			if len(e.shimMap) != 0 || len(e.replParams) != 0 || len(e.replReports) != 0 {
				t.Errorf("a refused set changed the emitter: shimMap=%v params=%v", e.shimMap, e.replParams)
			}
		})
	}
}

func TestApplyReplacementsConflictAndFragment(t *testing.T) {
	set, fns := replTestSet()
	e := replTestEmitter(fns, []uint64{0x1100, 0x1200}, []uint64{0x1000, 0x1100, 0x1200})
	e.shimMap[0x1100+replBias] = "shim_memcpy"
	if _, err := e.ApplyReplacements(set, false); err == nil || !strings.Contains(err.Error(), "already bound") {
		t.Fatalf("binding over an existing shim: %v", err)
	}
	// a fragment emitter has no function list: the discovery check is skipped, the evidence is not
	e = replTestEmitter(fns, []uint64{0x1100, 0x1200}, nil)
	if _, err := e.ApplyReplacements(set, true); err != nil {
		t.Fatalf("fragment: %v", err)
	}
	if len(e.shimMap) != 3 {
		t.Errorf("fragment bound %d shims", len(e.shimMap))
	}
	if _, err := e.ApplyReplacements(&ReplacementSet{Name: "empty"}, false); err == nil {
		t.Error("an empty set must be an error")
	}
}

func TestReplacementSeedsOnlyForMatchingImage(t *testing.T) {
	set, fns := replTestSet()
	e := replTestEmitter(fns, []uint64{0x1100, 0x1200}, nil)
	if !set.MatchesImage(e.elf) {
		t.Fatal("set does not match its own image")
	}
	fns[0x1200][0] ^= 0xff
	e2 := replTestEmitter(fns, []uint64{0x1100, 0x1200}, nil)
	if set.MatchesImage(e2.elf) {
		t.Error("set matches a modified image")
	}
	if got := ReplacementSeeds(e2.elf, "ue5-binned3", ""); len(got) != 0 {
		t.Errorf("seeds for a binary no profile matches: %v", got)
	}
	if got := set.Entries(replBias); len(got) != 3 || got[1] != 0x1100+replBias {
		t.Errorf("entries %v", got)
	}
}

func TestLoadReplacementFile(t *testing.T) {
	dir := t.TempDir()
	write := func(name, s string) string {
		p := filepath.Join(dir, name)
		if err := os.WriteFile(p, []byte(s), 0o644); err != nil {
			t.Fatal(err)
		}
		return p
	}
	good := write("good.json", `{"name":"x","funcs":[{"name":"M","addr":"0x1100","size":28,"sha256":"`+hexSHA([]byte("malloc-body-abcdefghijklmnop"))+`","shim":"shim_ue5_malloc_malloc"},
	  {"name":"F","addr":"4608","size":1,"shim":"shim_ue5_malloc_free"}],
	  "vtable_checks":[{"vtable":"0x5000","slot":0,"func":"0x1100"}],"params":[{"key":"ue5m.vtable","addr":"0x5000"},{"key":"n","value":"7"}]}`)
	set, err := LoadReplacementFile(good)
	if err != nil {
		t.Fatal(err)
	}
	if set.Name != "x" || len(set.Funcs) != 2 || set.Funcs[0].Addr != 0x1100 || set.Funcs[1].Addr != 4608 || set.Funcs[0].Size != 28 || set.Funcs[0].Shim != "shim_ue5_malloc_malloc" {
		t.Errorf("parsed %+v", set)
	}
	if len(set.VTableChecks) != 1 || set.VTableChecks[0] != (VTableCheck{0x5000, 0, 0x1100}) {
		t.Errorf("vtable checks %+v", set.VTableChecks)
	}
	if len(set.Params) != 2 || set.Params[0].Addr != 0x5000 || set.Params[1].Value != 7 {
		t.Errorf("params %+v", set.Params)
	}
	// an entry with neither digest nor vtable check is refused unless the opt-in is given
	fns := map[uint64][]byte{0x1100: []byte("malloc-body-abcdefghijklmnop")}
	e := replTestEmitter(fns, []uint64{0x1100}, []uint64{0x1100, 4608})
	if _, err := e.ApplyReplacements(set, false); err == nil || !strings.Contains(err.Error(), "no sha256 and no vtable check") {
		t.Fatalf("unverified entry without the opt-in: %v", err)
	}
	if len(e.shimMap) != 0 {
		t.Error("a refused table bound shims")
	}
	set.AllowUnverified = true
	rep, err := e.ApplyReplacements(set, false)
	if err != nil || rep.Unverified != 1 {
		t.Errorf("apply: %v unverified=%d", err, rep.Unverified)
	}
	for name, body := range map[string]string{
		"unknown.json":  `{"funcs":[{"name":"M","addr":"0x1","shim":"s","bogus":1}]}`,
		"badaddr.json":  `{"funcs":[{"name":"M","addr":"zz","shim":"s"}]}`,
		"notjson.json":  `nope`,
		"trailing.json": `{"funcs":[{"name":"M","addr":"0x1100","size":28,"shim":"s"}]} {"x":1}`,
		"negslot.json":  `{"funcs":[{"name":"M","addr":"0x1100","size":28,"sha256":"00","shim":"s"}],"vtable_checks":[{"vtable":"0x5000","slot":-1,"func":"0x1100"}]}`,
		"badshim.json":  `{"funcs":[{"name":"M","addr":"0x1100","size":28,"shim":"s;x"}]}`,
		"zerosize.json": `{"funcs":[{"name":"M","addr":"0x1100","shim":"s"}]}`,
		"overlap.json":  `{"funcs":[{"name":"M","addr":"0x1100","size":32,"shim":"s"},{"name":"N","addr":"0x1110","size":4,"shim":"t"}]}`,
		"octalish.json": `{"funcs":[{"name":"M","addr":"0o10","size":4,"shim":"s"}]}`,
	} {
		if _, err := LoadReplacementFile(write(name, body)); err == nil {
			t.Errorf("%s: accepted", name)
		}
	}
	if _, err := LoadReplacementFile(filepath.Join(dir, "missing.json")); err == nil {
		t.Error("missing file accepted")
	}
}

// The built-in profiles must be internally consistent, and every shim they name must exist in the runtime.
func TestUE5Binned3ProfilesWellFormed(t *testing.T) {
	src, err := os.ReadFile("../runtime/modules/libSceUeMalloc/nps5_uemalloc.c")
	if err != nil {
		t.Fatal(err)
	}
	for _, p := range ue5Binned3Profiles {
		addrs := map[uint64]bool{}
		hasCtor := false
		for _, f := range p.Funcs {
			if addrs[f.Addr] {
				t.Errorf("%s: 0x%x twice", p.Name, f.Addr)
			}
			addrs[f.Addr] = true
			if len(f.SHA256) != 64 || f.Size == 0 {
				t.Errorf("%s: %s needs a full digest and a size", p.Name, f.Name)
			}
			if ok, _ := regexp.Match(`void `+f.Shim+`\(GuestContext \*ctx\)`, src); !ok {
				t.Errorf("%s: shim %s is not defined in nps5_uemalloc.c", p.Name, f.Shim)
			}
			hasCtor = hasCtor || f.Shim == shimUeCtor
		}
		if !hasCtor {
			t.Errorf("%s: no constructor replacement (the vtable pointer would never be stored)", p.Name)
		}
		for _, c := range p.VTableChecks {
			if !addrs[c.Func] {
				t.Errorf("%s: vtable check names 0x%x, which is not replaced", p.Name, c.Func)
			}
		}
		found := false
		for _, pr := range p.Params {
			found = found || pr.Key == "ue5m.vtable"
		}
		if !found {
			t.Errorf("%s: no ue5m.vtable parameter", p.Name)
		}
	}
	e := replTestEmitter(nil, nil, nil)
	if _, err := e.ApplyUE5Binned3("ue5-binned2", false); err == nil || !strings.Contains(err.Error(), "unknown mode") {
		t.Errorf("unknown mode: %v", err)
	}
	if _, err := e.ApplyUE5Binned3("ue5-binned3", false); err == nil || !strings.Contains(err.Error(), "no profile matches") {
		t.Errorf("a binary without a profile must be refused: %v", err)
	}
}

// Addresses are "0x" hex or decimal: a leading zero is not octal.
func TestParseAddrNoOctal(t *testing.T) {
	for in, want := range map[string]uint64{"0x10": 16, "0X10": 16, "010": 10, "10": 10, " 0x1130 ": 0x1130, "0": 0} {
		if got, err := parseAddr(in); err != nil || got != want {
			t.Errorf("parseAddr(%q) = %d, %v; want %d", in, got, err, want)
		}
	}
	for _, in := range []string{"", "0x", "0o10", "0b1", "1_0", "-1", "0xg", "1.5"} {
		if _, err := parseAddr(in); err == nil {
			t.Errorf("parseAddr(%q) accepted", in)
		}
	}
}

// A vtable check ties a function to the binary like a digest does.
func TestApplyReplacementsVTableCheckVerifies(t *testing.T) {
	set, fns := replTestSet()
	set.Funcs[1].SHA256 = "" // Malloc: named by a vtable check
	e := replTestEmitter(fns, []uint64{0x1100, 0x1200}, []uint64{0x1000, 0x1100, 0x1200})
	if rep, err := e.ApplyReplacements(set, false); err != nil || rep.Unverified != 0 {
		t.Fatalf("%v %+v", err, rep)
	}
}
