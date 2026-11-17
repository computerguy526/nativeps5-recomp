package cli

import (
	"encoding/binary"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"ps4-recomp/pkg/disasm"
	"ps4-recomp/pkg/emitter"
	"ps4-recomp/pkg/lifter"
)

func writeLog(t *testing.T, dir string, lines ...string) string {
	t.Helper()
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	p := filepath.Join(dir, "tier1_misses.jsonl")
	if err := os.WriteFile(p, []byte(strings.Join(lines, "\n")+"\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	return p
}

func TestReadTier1SeedsServedOnly(t *testing.T) {
	d := t.TempDir()
	writeLog(t, filepath.Join(d, "runs", "a-run"),
		`{"rip":"0x4092a0","outcome":"compiled"}`,
		`{"rip":"0x4092c0","outcome":"cache_hit"}`,
		`{"rip":"0x4092d0","outcome":"waited"}`,
		`{"rip":"0x4092d0","outcome":"waited"}`,
		`{"rip":"0x1000","outcome":"failed","kind":"undecodable"}`,
		`{"rip":"0x2000","outcome":"raced"}`,
		``)
	writeLog(t, filepath.Join(d, "runs", "b-run"), `{"rip":"0x4092a0","outcome":"cache_hit"}`, `{"rip":"0x409300","outcome":"compiled"}`)
	got, err := readTier1Seeds(filepath.Join(d, "runs")) // a runs directory: every bundle below it
	if err != nil {
		t.Fatal(err)
	}
	want := []uint64{0x4092a0, 0x4092c0, 0x4092d0, 0x409300}
	if len(got) != len(want) {
		t.Fatalf("seeds %x, want %x", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("seeds %x, want %x", got, want)
		}
	}
	// a bundle directory and a file path, joined
	one, err := readTier1Seeds(filepath.Join(d, "runs", "a-run") + "," + filepath.Join(d, "runs", "b-run", "tier1_misses.jsonl"))
	if err != nil || len(one) != 4 {
		t.Fatalf("joined sources: %x %v", one, err)
	}
}

func TestReadTier1SeedsErrors(t *testing.T) {
	d := t.TempDir()
	if _, err := readTier1Seeds(filepath.Join(d, "missing")); err == nil {
		t.Error("missing source: want an error")
	}
	p := writeLog(t, d, `{"rip":"0x10","outcome":"compiled"}`, `not json`)
	if _, err := readTier1Seeds(p); err == nil || !strings.Contains(err.Error(), ":2:") {
		t.Errorf("bad json line: want an error naming line 2, got %v", err)
	}
	p = writeLog(t, d, `{"rip":"zz","outcome":"compiled"}`)
	if _, err := readTier1Seeds(p); err == nil {
		t.Error("bad rip: want an error")
	}
}

func TestWriteTier1Artifacts(t *testing.T) {
	d := t.TempDir()
	elf := filepath.Join(d, "x.elf")
	os.WriteFile(elf, []byte("ELF"), 0o644)
	out := filepath.Join(d, "out")
	os.MkdirAll(out, 0o755)
	// the emitter's manifest is merged into, not replaced
	os.WriteFile(filepath.Join(out, "lifter_manifest.json"), []byte(`{"addr_mode":"identity","tso":true}`), 0o644)
	cfg := &Config{ElfPath: elf, OutDir: out, AddrMode: "identity", TSO: true}
	c, err := writeTier1Artifacts(cfg, []disasm.ReachableFunc{{Addr: 0x1000}, {Addr: 0x2000}})
	if err != nil {
		t.Fatal(err)
	}
	if filepath.Base(c) != "nps5_tier1_info.c" {
		t.Errorf("info file %s", c)
	}
	b, _ := os.ReadFile(filepath.Join(out, "lifter_manifest.json"))
	var m map[string]json.RawMessage
	if err := json.Unmarshal(b, &m); err != nil {
		t.Fatal(err)
	}
	if string(m["addr_mode"]) != `"identity"` || m["tier1"] == nil {
		t.Fatalf("manifest %s", b)
	}
	info, err := readTier1Manifest(filepath.Join(out, "lifter_manifest.json"))
	if err != nil {
		t.Fatal(err)
	}
	if info.AddrMode != "identity" || info.CallModel != lifter.GetCallModel().String() || info.ElfSHA256 == "" || info.TranslatorSHA256 == "" || info.ABI != Tier1ABI {
		t.Errorf("info %+v", info)
	}
	aot, _ := os.ReadFile(filepath.Join(out, "aot_funcs.bin"))
	if len(aot) != 16 || binary.LittleEndian.Uint64(aot[8:]) != 0x2000 {
		t.Errorf("aot_funcs.bin %x", aot)
	}
	// idempotent: a second call leaves the files alone
	st1, _ := os.Stat(filepath.Join(out, "aot_funcs.bin"))
	if _, err := writeTier1Artifacts(cfg, []disasm.ReachableFunc{{Addr: 0x1000}, {Addr: 0x2000}}); err != nil {
		t.Fatal(err)
	}
	st2, _ := os.Stat(filepath.Join(out, "aot_funcs.bin"))
	if !st1.ModTime().Equal(st2.ModTime()) {
		t.Error("aot_funcs.bin rewritten although unchanged")
	}
	// a manifest without the section (translated before tier 1) is refused with a clear kind
	os.WriteFile(filepath.Join(d, "old.json"), []byte(`{"addr_mode":"base"}`), 0o644)
	if _, err := readTier1Manifest(filepath.Join(d, "old.json")); err == nil || !strings.Contains(err.Error(), fragBadManifest) {
		t.Errorf("old manifest: %v", err)
	}
}

func TestFragDescAndFlags(t *testing.T) {
	c := fragDescC([]uint64{0x4092a0, 0x4092c0})
	for _, want := range []string{"fn_0x4092a0", "fn_0x4092c0", "nps5_frag_desc", "0x3147524650535035ull", `section$start$__DATA$__nps5_pcmap`} {
		if !strings.Contains(c, want) {
			t.Errorf("descriptor lacks %q", want)
		}
	}
	var m multiHex
	if err := m.Set("0x10,20"); err != nil || len(m) != 2 || m[0] != 0x10 || m[1] != 0x20 {
		t.Errorf("multiHex %v %v", m, err)
	}
	if err := m.Set("xyz"); err == nil {
		t.Error("bad address accepted")
	}
	cfg, err := ParseArgs([]string{"-seeds-from", "runs/x", "foo.elf"})
	if err != nil || cfg.SeedsFrom != "runs/x" || cfg.ElfPath != "foo.elf" {
		t.Errorf("-seeds-from: %+v %v", cfg, err)
	}
}

func TestFragmentCommandUsage(t *testing.T) {
	if err := fragmentCommand(nil); err == nil {
		t.Error("no arguments accepted")
	}
	out := t.TempDir()
	err := fragmentCommand([]string{"--manifest", filepath.Join(out, "nope.json"), "--entry", "0x1000", "--out", out})
	if err == nil || ExitCode(err) != 2 {
		t.Fatalf("missing manifest: %v (exit %d)", err, ExitCode(err))
	}
	b, _ := os.ReadFile(filepath.Join(out, "result.txt"))
	if !strings.Contains(string(b), "ok=0") || !strings.Contains(string(b), "kind=bad_manifest") {
		t.Errorf("result.txt %q", b)
	}
}

// The translation's TSO-relax list and peephole setting travel in the tier-1 manifest: a fragment must lift like the title
// whatever environment the fragment process runs in, and the fragment cache version must change with them.
func TestTier1ManifestCarriesRelaxAndPeephole(t *testing.T) {
	d := t.TempDir()
	elf := filepath.Join(d, "x.elf")
	os.WriteFile(elf, []byte("ELF"), 0o644)
	out := filepath.Join(d, "out")
	os.MkdirAll(out, 0o755)
	cfg := &Config{ElfPath: elf, OutDir: out, AddrMode: "identity", TSO: true}
	t.Setenv("NPS5_TSO_RELAX_FUNCS", "0x61d9e0,0x1000")
	t.Setenv("NPS5_NO_PEEPHOLE", "1")
	if _, err := writeTier1Artifacts(cfg, []disasm.ReachableFunc{{Addr: 0x1000}}); err != nil {
		t.Fatal(err)
	}
	info, err := readTier1Manifest(filepath.Join(out, "lifter_manifest.json"))
	if err != nil {
		t.Fatal(err)
	}
	if len(info.TSORelax) != 2 || info.TSORelax[0] != 0x1000 || info.TSORelax[1] != 0x61d9e0 || !info.NoPeephole {
		t.Fatalf("relax %#x nopeep %v", info.TSORelax, info.NoPeephole)
	}
	plain := *info
	plain.TSORelax, plain.NoPeephole = nil, false
	if tier1Version(info) == tier1Version(&plain) {
		t.Error("fragment cache version ignores the relax list / peephole setting")
	}
}

// A title whose manifest was written against another runtime layout or code ABI is refused (its
// fragments would bake in offsets the title does not have), and the fragment cache version moves
// with the layout, the ABI and every lifter switch.
func TestTier1LayoutIdentity(t *testing.T) {
	cur := func() *tier1Info {
		return &tier1Info{ABI: Tier1ABI, LoweringID: "x", Elf: "e", LayoutHash: emitter.KeyLayoutHash(), CodeABI: emitter.ABIVersion}
	}
	if err := checkTier1Layout(cur()); err != nil {
		t.Fatalf("matching manifest refused: %v", err)
	}
	old := cur()
	old.LayoutHash = ""
	if checkTier1Layout(old) == nil {
		t.Error("a manifest without a layout hash must be refused")
	}
	bad := cur()
	bad.LayoutHash = "ffffffffffffffff"
	if checkTier1Layout(bad) == nil {
		t.Error("a manifest of another layout must be refused")
	}
	abi := cur()
	abi.CodeABI++
	if checkTier1Layout(abi) == nil {
		t.Error("a manifest of another code ABI must be refused")
	}
	title := cur() // translated before the struct changed
	restore := emitter.SetLayoutHashForTest("0123456789abcdef")
	err := checkTier1Layout(title)
	restore()
	if err == nil {
		t.Error("a struct-offset change must refuse the existing title")
	}
	base := tier1Version(cur())
	for name, mod := range map[string]func(*tier1Info){
		"layout": func(i *tier1Info) { i.LayoutHash = "0123456789abcdef" }, "abi": func(i *tier1Info) { i.CodeABI++ },
		"cold": func(i *tier1Info) { i.ColdOutline = true }, "call_ic": func(i *tier1Info) { i.CallIC = true },
		"fpfuse": func(i *tier1Info) { i.FpCmpFuse = true }, "ppfuse": func(i *tier1Info) { i.PushPopFuse = true },
		"jmpic": func(i *tier1Info) { i.JmpIC = true }, "simd": func(i *tier1Info) { i.SimdLiveness = "strict" },
	} {
		i := cur()
		mod(i)
		if tier1Version(i) == base {
			t.Errorf("tier1Version ignores %s", name)
		}
	}
}

// The translation stamp (which deliberately ignores the runtime sources) moves with the layout hash.
func TestStampCarriesLayout(t *testing.T) {
	cfg := &Config{ElfPath: filepath.Join(t.TempDir(), "none.elf")}
	a, err := translationStamp(cfg)
	if err != nil {
		t.Fatal(err)
	}
	restore := emitter.SetLayoutHashForTest("0123456789abcdef")
	b, err := translationStamp(cfg)
	restore()
	if err != nil {
		t.Fatal(err)
	}
	if a == b {
		t.Error("a layout change left the translation stamp unchanged")
	}
	cfg.ColdOutline = true
	c, _ := translationStamp(cfg)
	if c == a {
		t.Error("-cold-outline is not in the stamp")
	}
}
