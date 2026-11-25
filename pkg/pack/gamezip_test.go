// SPDX-License-Identifier: GPL-2.0-or-later

package pack

import (
	"bufio"
	"bytes"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func env(kv ...string) func(string) string {
	m := map[string]string{}
	for i := 0; i+1 < len(kv); i += 2 {
		m[kv[i]] = kv[i+1]
	}
	return func(k string) string { return m[k] }
}

func bigEst() ZipEstimate {
	return MakeEstimate(ZipSurvey{Files: 10, Bytes: 9e9, Candidates: 2, CandBytes: 8e9, Largest: 5e9, ToSegment: 1, FSType: "apfs", Free: 100e9}, true, 2, 8e9, 4e9)
}

func TestDecideZip(t *testing.T) {
	base := func() ZipInput { return ZipInput{Getenv: env(), TTY: true, Est: bigEst()} }
	cases := []struct {
		name string
		mod  func(*ZipInput)
		want ZipAct
		why  string
	}{
		{"tty asks", func(*ZipInput) {}, ZipAsk, ""},
		{"no tty never asks", func(i *ZipInput) { i.TTY = false }, ZipSkip, "not a terminal"},
		{"env 1 packs without tty", func(i *ZipInput) { i.TTY = false; i.Getenv = env("NPS5_GAME_COMPRESS", "1") }, ZipPack, ""},
		{"env 1 packs with tty, no question", func(i *ZipInput) { i.Getenv = env("NPS5_GAME_COMPRESS", "1") }, ZipPack, ""},
		{"env 0 declines", func(i *ZipInput) { i.Getenv = env("NPS5_GAME_COMPRESS", "0") }, ZipDecline, ""},
		{"env 0 declines without tty", func(i *ZipInput) { i.TTY = false; i.Getenv = env("NPS5_GAME_COMPRESS", "0") }, ZipDecline, ""},
		{"env other is ignored", func(i *ZipInput) { i.Getenv = env("NPS5_GAME_COMPRESS", "yes") }, ZipAsk, ""},
		{"game running refuses ask", func(i *ZipInput) { i.Running = []string{"123 /x/native"} }, ZipSkip, "game is running"},
		{"game running refuses env 1", func(i *ZipInput) { i.Running = []string{"123 /x/native"}; i.Getenv = env("NPS5_GAME_COMPRESS", "1") }, ZipSkip, "game is running"},
		{"external volume skips even env 1", func(i *ZipInput) { i.External = true; i.Getenv = env("NPS5_GAME_COMPRESS", "1") }, ZipSkip, "external volume"},
		{"external volume skips env 0", func(i *ZipInput) { i.External = true; i.Getenv = env("NPS5_GAME_COMPRESS", "0") }, ZipSkip, "external volume"},
		{"read-only skips", func(i *ZipInput) { i.ReadOnly = true }, ZipSkip, "not writable"},
		{"not apfs skips", func(i *ZipInput) { i.NotAPFS = "exfat" }, ZipSkip, "exfat"},
		{"no packer skips", func(i *ZipInput) { i.NoPacker = true }, ZipSkip, "gamepack.py"},
		{"already packed skips", func(i *ZipInput) { i.Est = MakeEstimate(ZipSurvey{Segmented: 3}, false, 0, 0, 0) }, ZipSkip, "already packed"},
		{"nothing big skips", func(i *ZipInput) { i.Est = MakeEstimate(ZipSurvey{Files: 5}, false, 0, 0, 0) }, ZipSkip, "512 MiB"},
		{"sampled no gain skips", func(i *ZipInput) {
			i.Est = MakeEstimate(ZipSurvey{Candidates: 1, CandBytes: 3e9, Largest: 3e9}, true, 0, 0, 0)
		}, ZipSkip, "do not compress"},
	}
	for _, c := range cases {
		in := base()
		c.mod(&in)
		d := DecideZip(in)
		if d.Act != c.want || !strings.Contains(d.Reason, c.why) {
			t.Errorf("%s: got %v %q, want %v containing %q", c.name, d.Act, d.Reason, c.want, c.why)
		}
	}
}

func TestAskYes(t *testing.T) {
	cases := []struct {
		in   string
		want bool
	}{{"\n", true}, {"y\n", true}, {"YES\n", true}, {" n \n", false}, {"no\n", false}, {"", false}, {"y", true},
		{"maybe\nn\n", false}, {"maybe\nyes\n", true}, {"x\nx\nx\ny\n", false}}
	for _, c := range cases {
		var out bytes.Buffer
		if got := AskYes(bufio.NewReader(strings.NewReader(c.in)), &out, "Compress the game files?"); got != c.want {
			t.Errorf("%q: got %v want %v", c.in, got, c.want)
		}
		if !strings.Contains(out.String(), "[Y/n]") {
			t.Errorf("%q: prompt has no [Y/n]: %q", c.in, out.String())
		}
	}
}

func TestParseGamepackTotal(t *testing.T) {
	out := "mode   size ratio est. saved  file\nsegment 5.00 GB 0.40 3.00 GB /d/a.ucas\n" +
		"total: 2 file(s) to pack, 8.00 GB -> est. 4.50 GB saved (segment size 1.07 GB, free now 90.00 GB)\n"
	n, size, saved, ok := ParseGamepackTotal(out)
	if !ok || n != 2 || size != 8e9 || saved != 4.5e9 {
		t.Fatalf("got %d %d %d %v", n, size, saved, ok)
	}
	if _, _, _, ok := ParseGamepackTotal("nothing here\n"); ok {
		t.Fatal("parsed garbage")
	}
	n, size, saved, ok = ParseGamepackTotal("total: 0 file(s) to pack, 0 B -> est. 0 B saved (segment size 1.07 GB, free now -)")
	if !ok || n != 0 || size != 0 || saved != 0 {
		t.Fatalf("zero: %d %d %d %v", n, size, saved, ok)
	}
}

func TestEstimateNumbers(t *testing.T) {
	e := bigEst()
	if e.Saved != 4e9 || e.Ratio != 0.5 || !e.Sampled {
		t.Fatalf("%+v", e)
	}
	if e.FreeNeeded != 5e9/2+gamepackMargin {
		t.Fatalf("free needed %d", e.FreeNeeded)
	}
	if e.Seconds != 66 {
		t.Fatalf("seconds %d", e.Seconds)
	}
	p := e.Prompt("Some Game")
	for _, w := range []string{"Compress the game files of Some Game?", "1 file(s) over 1.9 GiB", "lossless and reversible", "same bytes", "sha256", "free disk needed", "unpack"} {
		if !strings.Contains(p, w) {
			t.Errorf("prompt lacks %q:\n%s", w, p)
		}
	}
	u := MakeEstimate(ZipSurvey{Candidates: 1, CandBytes: 4e9, Largest: 4e9}, false, 0, 0, 0)
	if u.Sampled || u.Saved != 2e9 {
		t.Fatalf("fallback estimate %+v", u)
	}
}

// writeSparse makes a file of the given size without writing data.
func writeSparse(t *testing.T, p string, size int64) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		t.Fatal(err)
	}
	f, err := os.Create(p)
	if err != nil {
		t.Fatal(err)
	}
	if err := f.Truncate(size); err != nil {
		t.Fatal(err)
	}
	f.Close()
}

func TestSurveyDumpSynthetic(t *testing.T) {
	d := t.TempDir()
	writeSparse(t, filepath.Join(d, "eboot.bin"), 1<<20)
	writeSparse(t, filepath.Join(d, "a", "mid.pak"), 700<<20)             // candidate, kept whole
	writeSparse(t, filepath.Join(d, "a", "huge.ucas"), 2200<<20)          // candidate, will be segmented
	writeSparse(t, filepath.Join(d, "b", "other.ucas"), 3000<<20)         // candidate, will be segmented
	writeSparse(t, filepath.Join(d, "c", "done.ucas.nps5seg", "0000"), 1) // already packed: not counted
	writeSparse(t, filepath.Join(d, "c", "done.ucas.nps5seg", "manifest.json"), 1)
	s, err := SurveyDump(d)
	if err != nil {
		t.Fatal(err)
	}
	if s.Candidates != 3 || s.ToSegment != 2 || s.Segmented != 1 || s.Files != 4 {
		t.Fatalf("%+v", s)
	}
	if s.Largest != 3000<<20 || s.CandBytes != (700+2200+3000)<<20 {
		t.Fatalf("%+v", s)
	}
	if s.Free <= 0 && s.FSType != "" {
		t.Fatalf("no free space reported: %+v", s)
	}
	e := MakeEstimate(s, false, 0, 0, 0)
	if none, _ := e.Nothing(); none {
		t.Fatal("estimate says nothing to do")
	}
	if e.Saved <= 0 || e.FreeNeeded <= gamepackMargin || e.Seconds <= 0 {
		t.Fatalf("%+v", e)
	}
}

func TestSurveyAlreadyPackedOnly(t *testing.T) {
	d := t.TempDir()
	writeSparse(t, filepath.Join(d, "x.ucas.nps5seg", "0000"), 5)
	writeSparse(t, filepath.Join(d, "small.bin"), 10)
	s, _ := SurveyDump(d)
	if none, why := MakeEstimate(s, false, 0, 0, 0).Nothing(); !none || !strings.Contains(why, "already packed") {
		t.Fatalf("%v %q", none, why)
	}
}

func TestGameProcesses(t *testing.T) {
	ps := "  1 /sbin/launchd\n  200 /srv/x/boot/native_astro\n 201 /Applications/G.app/Contents/MacOS/nps5-title\n 202 /usr/bin/python3\n 203 /ue2boot/native_out3\n  204 /opt/native\n 205 /bin/nativefoo\n 300 /x/native_y\n"
	got := GameProcesses(ps, 300)
	if len(got) != 4 {
		t.Fatalf("%q", got)
	}
}

func TestAnswerFile(t *testing.T) {
	d := t.TempDir()
	g := env("NPS5_DATA_STATE_DIR", filepath.Join(d, "state"))
	if err := RecordAnswer(g, true); err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(AnswerFile(g))
	if string(b) != "yes\n" {
		t.Fatalf("%q", b)
	}
	_ = RecordAnswer(g, false)
	if b, _ = os.ReadFile(AnswerFile(g)); string(b) != "no\n" {
		t.Fatalf("%q", b)
	}
	if h := AnswerFile(env("HOME", "/h")); h != "/h/Library/Application Support/nativeps5/data/game-compress-answer" {
		t.Fatal(h)
	}
}

// fake wires MaybeCompressGame to a synthetic dump and a recording packer.
type fake struct {
	dump, state string
	packed      int
	dry         int
	out         bytes.Buffer
	packErr     error
	running     []string
}

func (f *fake) run(t *testing.T, input string, tty bool, kv ...string) {
	t.Helper()
	repo := t.TempDir()
	if err := os.MkdirAll(filepath.Join(repo, "scripts"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(repo, "scripts", "gamepack.py"), []byte("#"), 0o644); err != nil {
		t.Fatal(err)
	}
	g := env(append([]string{"NPS5_ENTRY_DIR", repo, "NPS5_DATA_STATE_DIR", f.state}, kv...)...)
	MaybeCompressGame(&Title{Name: "Synthetic", Dump: f.dump}, filepath.Join(repo, "x"), ZipEnv{
		Getenv: g, In: bufio.NewReader(strings.NewReader(input)), Out: &f.out, TTY: tty,
		Running: func() []string { return f.running },
		DryRun: func(string, string) (string, error) {
			f.dry++
			return "total: 2 file(s) to pack, 5.00 GB -> est. 2.00 GB saved (segment size 1.07 GB, free now 9.00 GB)\n", nil
		},
		Pack: func(string, string, io.Writer) error { f.packed++; return f.packErr },
	})
}

func newFake(t *testing.T) *fake {
	d := t.TempDir()
	writeSparse(t, filepath.Join(d, "big.ucas"), 2500<<20)
	writeSparse(t, filepath.Join(d, "mid.pak"), 600<<20)
	return &fake{dump: d, state: filepath.Join(t.TempDir(), "st")}
}

func answer(t *testing.T, f *fake) string {
	b, _ := os.ReadFile(AnswerFile(env("NPS5_DATA_STATE_DIR", f.state)))
	return strings.TrimSpace(string(b))
}

func TestFlowYes(t *testing.T) {
	f := newFake(t)
	f.run(t, "\n", true)
	if f.packed != 1 || answer(t, f) != "yes" || f.dry != 1 {
		t.Fatalf("packed=%d answer=%q dry=%d\n%s", f.packed, answer(t, f), f.dry, f.out.String())
	}
	if !strings.Contains(f.out.String(), "Compress the game files?") || !strings.Contains(f.out.String(), "1 file(s) over 1.9 GiB") {
		t.Fatal(f.out.String())
	}
}

func TestFlowNo(t *testing.T) {
	f := newFake(t)
	f.run(t, "n\n", true)
	if f.packed != 0 || answer(t, f) != "no" {
		t.Fatalf("packed=%d answer=%q\n%s", f.packed, answer(t, f), f.out.String())
	}
}

func TestFlowNoTTYNeverAsksOrPacks(t *testing.T) {
	f := newFake(t)
	f.run(t, "y\n", false)
	if f.packed != 0 || f.dry != 0 || answer(t, f) != "" || strings.Contains(f.out.String(), "[Y/n]") {
		t.Fatalf("packed=%d dry=%d answer=%q\n%s", f.packed, f.dry, answer(t, f), f.out.String())
	}
}

func TestFlowEnv(t *testing.T) {
	f := newFake(t)
	f.run(t, "", false, "NPS5_GAME_COMPRESS", "1")
	if f.packed != 1 || answer(t, f) != "yes" || strings.Contains(f.out.String(), "[Y/n]") {
		t.Fatalf("packed=%d answer=%q\n%s", f.packed, answer(t, f), f.out.String())
	}
	g := newFake(t)
	g.run(t, "", true, "NPS5_GAME_COMPRESS", "0")
	if g.packed != 0 || g.dry != 0 || answer(t, g) != "no" {
		t.Fatalf("packed=%d dry=%d answer=%q", g.packed, g.dry, answer(t, g))
	}
}

func TestFlowGameRunning(t *testing.T) {
	f := newFake(t)
	f.running = []string{"9 /x/native"}
	f.run(t, "y\n", true, "NPS5_GAME_COMPRESS", "1")
	if f.packed != 0 || f.dry != 0 || answer(t, f) != "" || !strings.Contains(f.out.String(), "game is running") {
		t.Fatalf("packed=%d\n%s", f.packed, f.out.String())
	}
}

func TestFlowPackFailureKeepsGoing(t *testing.T) {
	f := newFake(t)
	f.packErr = errors.New("exit 4")
	f.run(t, "y\n", true)
	if f.packed != 1 || answer(t, f) != "" || !strings.Contains(f.out.String(), "every original is still in place") {
		t.Fatalf("answer=%q\n%s", answer(t, f), f.out.String())
	}
}

func TestFlowExternalVolumeUntouched(t *testing.T) {
	if !onExternalVolume("/Volumes"+"/Some Disk/game") || onExternalVolume("/srv/x/game") {
		t.Fatal("onExternalVolume prefix rule")
	}
}

func TestFlowNothingToDo(t *testing.T) {
	d := t.TempDir()
	writeSparse(t, filepath.Join(d, "small.bin"), 100)
	f := &fake{dump: d, state: filepath.Join(t.TempDir(), "st")}
	f.run(t, "y\n", true)
	if f.packed != 0 || f.dry != 0 || strings.Contains(f.out.String(), "[Y/n]") {
		t.Fatal(f.out.String())
	}
}
