package diag

import (
	"encoding/binary"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestOperandForm(t *testing.T) {
	cases := []struct{ in, mn, form string }{
		{"VPERMQ Y1, Y0, 0x1b", "VPERMQ", "ymm,ymm,imm"},
		{"VPBROADCASTD X3, [+0x986e9]", "VPBROADCASTD", "xmm,m"},
		{"MOV RAX, [RIP+0x13ff9]", "MOV", "r,m"},
		{"REP MOVSB", "MOVSB", ""},
		{"RET", "RET", ""},
		{"FCOMIP F0, F1 [UNSUPPORTED: x]", "FCOMIP", "r,r"},
	}
	for _, c := range cases {
		mn, form := OperandForm(c.in)
		if mn != c.mn || form != c.form {
			t.Errorf("OperandForm(%q) = %q,%q want %q,%q", c.in, mn, form, c.mn, c.form)
		}
	}
}

func TestBundleAndLatest(t *testing.T) {
	root := t.TempDir()
	in := filepath.Join(t.TempDir(), "in.elf")
	os.WriteFile(in, []byte("hello"), 0o644)
	b, err := NewBundle(root, "my title/x", "translate")
	if err != nil {
		t.Fatal(err)
	}
	b.AddInput(in)
	b.Log("loader", "info", "hi", map[string]any{"n": 1})
	if err := b.WriteJSON("census.json", Census{}); err != nil {
		t.Fatal(err)
	}
	b.Close(nil)
	if !strings.HasSuffix(b.Dir, "-my_title_x-translate") {
		t.Errorf("bundle name %s", b.Dir)
	}
	if got, _ := filepath.EvalSymlinks(filepath.Join(root, "latest")); got != mustEval(t, b.Dir) {
		t.Errorf("latest -> %s, want %s", got, b.Dir)
	}
	var m Manifest
	data, _ := os.ReadFile(filepath.Join(b.Dir, "manifest.json"))
	if err := json.Unmarshal(data, &m); err != nil {
		t.Fatal(err)
	}
	// sha256("hello")
	if len(m.Inputs) != 1 || m.Inputs[0].SHA256 != "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824" {
		t.Errorf("inputs %+v", m.Inputs)
	}
	if m.Host.OSBuild == "" || m.Host.Model == "" {
		t.Errorf("host info missing: %+v", m.Host)
	}
	if m.Result != "ok" {
		t.Errorf("result %q", m.Result)
	}
	logData, _ := os.ReadFile(filepath.Join(b.Dir, "log.jsonl"))
	var rec map[string]any
	if err := json.Unmarshal([]byte(strings.TrimSpace(string(logData))), &rec); err != nil || rec["subsys"] != "loader" {
		t.Errorf("log record %s: %v", logData, err)
	}
	b2, _ := NewBundle(root, "second", "run")
	b2.Close(nil)
	if got, _ := filepath.EvalSymlinks(filepath.Join(root, "latest")); got != mustEval(t, b2.Dir) {
		t.Errorf("latest not updated")
	}
	if got, _ := filepath.EvalSymlinks(filepath.Join(root, "latest-translate")); got != mustEval(t, b.Dir) {
		t.Errorf("latest-translate moved")
	}
}

func mustEval(t *testing.T, p string) string {
	t.Helper()
	r, err := filepath.EvalSymlinks(p)
	if err != nil {
		t.Fatal(err)
	}
	return r
}

func TestBuildScanCensusAndAddrmap(t *testing.T) {
	facts := &AsmFacts{
		Funcs: []AddrMapFunc{
			{Guest: 0x20, Symbol: "_fn_0x20", Insts: 4, Chunk: "code_000.s"},
			{Guest: 0x40, Symbol: "_fn_0x40", Insts: 1, Chunk: "code_000.s"},
			{Guest: 0x60, Symbol: "_fn_0x60", Chunk: "code_000.s"}, // a shim stub
		},
		Blocks: []BlockLabel{{PC: 0x20, Label: "Lf20_20", Func: 0x20}, {PC: 0x40, Label: "Lf40_40", Func: 0x40}},
		Unsupported: []UnsupportedHit{
			{Addr: 0x27, Func: 0x20, Kind: "instruction", Text: "VPERMQ Y1, Y0, 0x1b"},
			{Addr: 0x2d, Func: 0x20, Kind: "indirect_jmp", Text: "JMP RAX"},
		},
		GuestInsts: 5, UD2Stubs: 1, PCEntries: 5,
	}
	funcs := []FuncInfo{
		{Entry: 0x20, Name: "main", JumpTables: map[uint64]int{0x2d: 0}, IndirectJmps: []uint64{0x2d, 0x99}, IndirectCall: 2},
		{Entry: 0x40, JumpTables: map[uint64]int{0x50: 3}, IndirectJmps: []uint64{0x50}},
	}
	res := BuildScan(facts, funcs, 5)
	c := res.Census
	if c.FunctionsEmitted != 2 || c.ShimStubs != 1 || c.FDECount != 5 || c.GuestInstsEmitted != 5 {
		t.Errorf("counts: %+v", c)
	}
	if c.UnsupportedTotal != 2 {
		t.Fatalf("unsupported = %d: %+v", c.UnsupportedTotal, c.Unsupported)
	}
	if u := c.Unsupported[0]; u.Addr != 0x27 || u.Mnemonic != "VPERMQ" || u.Form != "ymm,ymm,imm" || u.Func != 0x20 {
		t.Errorf("hit %+v", u)
	}
	if c.Unsupported[1].Kind != "indirect_jmp" || c.UD2Stubs != 1 {
		t.Errorf("kinds %+v ud2=%d", c.Unsupported[1], c.UD2Stubs)
	}
	if c.IndirectJmpSites != 3 || c.JumpTablesRecover != 1 || len(c.JumpTableSitesMiss) != 2 || c.IndirectCalls != 2 {
		t.Errorf("jump tables: %+v", c)
	}
	am := string(res.AddrMap)
	for _, want := range []string{"F 0x20 _fn_0x20 main 4 code_000.s", "F 0x60 _fn_0x60 - 0 code_000.s", "L 0x40 Lf40_40 0x40", "U 0x27 instruction VPERMQ ymm,ymm,imm"} {
		if !strings.Contains(am, want) {
			t.Errorf("addrmap lacks %q:\n%s", want, am)
		}
	}
}

func TestPCMapLookupAndAddr2Guest(t *testing.T) {
	root := t.TempDir()
	b, _ := NewBundle(root, "x", "run")
	var buf []byte
	buf = append(buf, PCMagic...)
	buf = binary.LittleEndian.AppendUint64(buf, 0x100000000)
	recs := [][3]uint64{{0x1000, 0x4<<56 | 0x20, 0x20}, {0x1010, 0x27, 0x20}, {0x1030, 0x40 | 4<<56, 0x40}}
	buf = binary.LittleEndian.AppendUint64(buf, uint64(len(recs)))
	for _, r := range recs {
		for _, v := range r {
			buf = binary.LittleEndian.AppendUint64(buf, v)
		}
	}
	os.WriteFile(filepath.Join(b.Dir, "pcmap.bin"), buf, 0o644)
	am := filepath.Join(root, "addrmap.txt")
	os.WriteFile(am, []byte("F 0x20 _fn_0x20 main 4 code_000.s\n"), 0o644)
	out, err := Addr2Guest(root, "", am, "0x100001014")
	if err != nil || !strings.Contains(out, "guest RIP 0x27 in main (entry 0x20, +0x7)") {
		t.Errorf("got %q err %v", out, err)
	}
	if out, err = Addr2Guest(root, b.Dir, am, "+0x1030"); err != nil || !strings.Contains(out, "0x40") {
		t.Errorf("got %q err %v", out, err)
	}
	if _, err = Addr2Guest(root, "", am, "0x100000010"); err == nil {
		t.Error("expected error below the first record")
	}
	// a function-end sentinel (flag 8) is not guest code
	recs = append(recs, [3]uint64{0x1040, FlagEnd << 56, 0x40})
	buf = binary.LittleEndian.AppendUint64(buf[:8], 0x100000000)
	buf = binary.LittleEndian.AppendUint64(buf, uint64(len(recs)))
	for _, r := range recs {
		for _, v := range r {
			buf = binary.LittleEndian.AppendUint64(buf, v)
		}
	}
	os.WriteFile(filepath.Join(b.Dir, "pcmap.bin"), buf, 0o644)
	if _, err = Addr2Guest(root, "", am, "0x100001044"); err == nil || !strings.Contains(err.Error(), "not a guest function") {
		t.Errorf("sentinel: %v", err)
	}
}

func TestEnvSubsetAndLast(t *testing.T) {
	env := EnvSubset([]string{"NPS5_LOG=hle=debug", "HOME=/x", "PS4_NO_DIALOGS=1"})
	if len(env) != 2 || env["NPS5_LOG"] != "hle=debug" {
		t.Errorf("env %v", env)
	}
	root := t.TempDir()
	b, _ := NewBundle(root, "t", "run")
	os.WriteFile(filepath.Join(b.Dir, "crash.txt"), []byte("signal: 11\n"), 0o644)
	b.Close(nil)
	var sb strings.Builder
	dir, err := ResolveBundle(root, "")
	if err != nil {
		t.Fatal(err)
	}
	if err := Last(&sb, dir); err != nil || !strings.Contains(sb.String(), "signal: 11") {
		t.Errorf("last: %v\n%s", err, sb.String())
	}
}
