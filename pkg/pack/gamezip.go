// SPDX-License-Identifier: GPL-2.0-or-later

package pack

// The "compress the game files?" step of `nps5 build`: before translating a title, offer to pack its dump with
// scripts/gamepack.py of the entry repository (APFS transparent compression; files over 1.9 GiB become .nps5seg segment
// folders, which the runtime serves as the original single file). The decision is separated from the work so that it
// can be tested without a terminal, a dump or a Mac volume (gamezip_test.go).

import (
	"bufio"
	"fmt"
	"io"
	"io/fs"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
)

const (
	// gamepackMinFile is gamepack's default --min-size: smaller files are not worth packing.
	gamepackMinFile = 512 << 20
	// gamepackWholeMax is gamepack's --whole-max: bigger files are cut into segments.
	gamepackWholeMax = 19 * (1 << 30) / 10
	// gamepackMargin is the free space gamepack keeps beyond the estimate of the file it works on.
	gamepackMargin = 3 << 30
	// zipRate is a guess (read, compress, write, read back and compare), in bytes per second; it is not a measurement.
	zipRate = 120e6
	// fallbackRatio is the stored-size ratio assumed when gamepack's own sampling is not available.
	fallbackRatio = 0.5
)

// ZipSurvey is what a dump looks like for packing, from a walk of the folder (no file content is read).
type ZipSurvey struct {
	Files      int   // regular files
	Bytes      int64 // sum of the file sizes
	Candidates int   // files of at least 512 MiB that are neither compressed nor already segmented
	CandBytes  int64
	Largest    int64 // largest candidate
	ToSegment  int   // candidates over 1.9 GiB: they become .nps5seg folders
	Segmented  int   // .nps5seg folders already present
	Compressed int   // files with the APFS compressed flag
	FSType     string
	Free       int64 // free bytes on the volume
}

// ZipEstimate is a survey plus the numbers shown to the user.
type ZipEstimate struct {
	ZipSurvey
	Saved      int64   // bytes expected to be freed
	Ratio      float64 // stored size / original size over the candidates
	PackFiles  int     // files gamepack's dry run would pack (Sampled only)
	Sampled    bool    // true when scripts/gamepack.py's own dry run gave Saved
	Seconds    int64   // very rough
	FreeNeeded int64   // free space needed while the largest file is packed (its packed copy plus the margin)
}

// SurveyDump walks dir without reading any file content.
func SurveyDump(dir string) (ZipSurvey, error) {
	var s ZipSurvey
	err := filepath.WalkDir(dir, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return nil // unreadable entries are left out of the count
		}
		if d.IsDir() {
			if strings.HasSuffix(d.Name(), ".nps5seg") {
				s.Segmented++
				return filepath.SkipDir
			}
			return nil
		}
		if !d.Type().IsRegular() {
			return nil
		}
		fi, err := d.Info()
		if err != nil {
			return nil
		}
		s.Files++
		s.Bytes += fi.Size()
		comp := fileCompressed(fi)
		if comp {
			s.Compressed++
		}
		if fi.Size() >= gamepackMinFile && !comp {
			s.Candidates++
			s.CandBytes += fi.Size()
			if fi.Size() > s.Largest {
				s.Largest = fi.Size()
			}
			if fi.Size() > gamepackWholeMax {
				s.ToSegment++
			}
		}
		return nil
	})
	s.FSType, s.Free = volumeInfo(dir)
	return s, err
}

var totalLine = regexp.MustCompile(`total: (\d+) file\(s\) to pack, ([0-9.]+) (GB|MB|KB|B) -> est\. [0-9.]+ (?:GB|MB|KB|B) saved`)
var savedField = regexp.MustCompile(`est\. ([0-9.]+) (GB|MB|KB|B) saved`)

func unitBytes(v, unit string) int64 {
	f, err := strconv.ParseFloat(v, 64)
	if err != nil {
		return 0
	}
	m := map[string]float64{"GB": 1e9, "MB": 1e6, "KB": 1e3, "B": 1}[unit]
	return int64(f * m)
}

// ParseGamepackTotal reads the "total:" line of `gamepack.py pack --dry-run`: files to pack, their size, the estimated saving.
func ParseGamepackTotal(out string) (files int, size, saved int64, ok bool) {
	for _, line := range strings.Split(out, "\n") {
		m := totalLine.FindStringSubmatch(line)
		if m == nil {
			continue
		}
		sv := savedField.FindStringSubmatch(line)
		if sv == nil {
			continue
		}
		files, _ = strconv.Atoi(m[1])
		return files, unitBytes(m[2], m[3]), unitBytes(sv[1], sv[2]), true
	}
	return 0, 0, 0, false
}

// MakeEstimate turns a survey into the numbers of the prompt. total is gamepack's dry-run result when known: its saving
// and file count replace the fallback ratio.
func MakeEstimate(s ZipSurvey, sampled bool, packFiles int, packSize, packSaved int64) ZipEstimate {
	e := ZipEstimate{ZipSurvey: s, Ratio: fallbackRatio}
	size := s.CandBytes
	if sampled {
		e.Sampled = true
		size = packSize
		e.Saved = packSaved
		if packSize > 0 {
			e.Ratio = math.Max(0, math.Min(1, 1-float64(packSaved)/float64(packSize)))
		}
		e.PackFiles = packFiles
	} else {
		e.Saved = int64(float64(size) * (1 - e.Ratio))
	}
	e.Seconds = int64(float64(size) / zipRate)
	e.FreeNeeded = int64(float64(s.Largest)*e.Ratio) + gamepackMargin
	return e
}

// Nothing reports whether packing would change nothing, with the reason.
func (e ZipEstimate) Nothing() (bool, string) {
	switch {
	case e.Candidates == 0 && (e.Segmented > 0 || e.Compressed > 0):
		return true, "the game files are already packed"
	case e.Candidates == 0:
		return true, "no game file is big enough to be worth packing (512 MiB or more)"
	case e.Sampled && (e.PackFiles == 0 || e.Saved <= 0):
		return true, "the big game files do not compress (video or already-compressed archives)"
	}
	return false, ""
}

func humanGB(n int64) string {
	switch {
	case n >= 1e9:
		return fmt.Sprintf("%.1f GB", float64(n)/1e9)
	case n >= 1e6:
		return fmt.Sprintf("%.0f MB", float64(n)/1e6)
	}
	return fmt.Sprintf("%d B", n)
}

func humanTime(sec int64) string {
	if sec < 90 {
		return "about a minute"
	}
	if sec < 5400 {
		return fmt.Sprintf("about %d minutes", (sec+30)/60)
	}
	return fmt.Sprintf("about %.1f hours", float64(sec)/3600)
}

// Prompt is the text shown before the question.
func (e ZipEstimate) Prompt(name string) string {
	var b strings.Builder
	how := "rough estimate"
	if e.Sampled {
		how = "estimate sampled from the files"
	}
	fmt.Fprintf(&b, "\n[nps5] Compress the game files of %s?\n", name)
	fmt.Fprintf(&b, "  now: %s in %d files; %d big file(s) (%s) can be packed\n", humanGB(e.Bytes), e.Files, e.Candidates, humanGB(e.CandBytes))
	fmt.Fprintf(&b, "  saves: about %s (%s)\n", humanGB(e.Saved), how)
	fmt.Fprintf(&b, "  %d file(s) over 1.9 GiB will be cut into .nps5seg segment folders (1 GiB pieces)\n", e.ToSegment)
	fmt.Fprintf(&b, "  time: %s (a guess, mostly disk speed)\n", humanTime(e.Seconds))
	fmt.Fprintf(&b, "  free disk needed while packing: %s (you have %s)\n", humanGB(e.FreeNeeded), humanGB(e.Free))
	b.WriteString("  This is lossless and reversible: the game reads exactly the same bytes (the runtime joins the segments back into the\n")
	b.WriteString("  original file), every file is re-read and compared by sha256 before its original is removed, and\n")
	b.WriteString("  `python3 scripts/gamepack.py unpack <dump>` rebuilds the originals bit for bit.\n")
	return b.String()
}

// ZipAct is what to do about the game files.
type ZipAct int

const (
	ZipSkip    ZipAct = iota // do nothing, say why
	ZipAsk                   // ask the user
	ZipPack                  // pack without asking (NPS5_GAME_COMPRESS=1)
	ZipDecline               // the user (or NPS5_GAME_COMPRESS=0) said no: record it
)

// ZipInput is everything the decision depends on.
type ZipInput struct {
	Getenv   func(string) string
	TTY      bool     // stdin and stderr are terminals
	External bool     // the dump is on an external volume (/Volumes)
	NotAPFS  string   // file system name when it is not APFS, else ""
	ReadOnly bool     // the dump folder is not writable
	Running  []string // game processes found
	NoPacker bool     // scripts/gamepack.py not found
	Est      ZipEstimate
}

// ZipDecision is the result of DecideZip.
type ZipDecision struct {
	Act    ZipAct
	Reason string // shown when the step is skipped
}

// DecideZip is the whole policy. Order: things that make packing impossible or unsafe come first (no packer, external
// volume, not APFS, read-only, a game running, nothing to pack), then NPS5_GAME_COMPRESS, then the terminal: without
// one nothing is asked and the default is off.
func DecideZip(in ZipInput) ZipDecision {
	switch {
	case in.External:
		return ZipDecision{ZipSkip, "the game folder is on an external volume (/Volumes); nps5 never changes a read-only dump there. " +
			"Copy it to the internal disk first if you want it packed (scripts/gamepack.py pack <dump>)"}
	case in.NoPacker:
		return ZipDecision{ZipSkip, "scripts/gamepack.py of the entry repository was not found (set NPS5_GAMEPACK to its path)"}
	case in.NotAPFS != "":
		return ZipDecision{ZipSkip, "the game folder is on " + in.NotAPFS + ", not APFS: no transparent compression there"}
	case in.ReadOnly:
		return ZipDecision{ZipSkip, "the game folder is not writable (packing replaces big files); `chmod -R u+w` it for the run to pack it"}
	}
	if none, why := in.Est.Nothing(); none {
		return ZipDecision{ZipSkip, why}
	}
	v := in.Getenv("NPS5_GAME_COMPRESS")
	if v == "0" {
		return ZipDecision{ZipDecline, "NPS5_GAME_COMPRESS=0"}
	}
	if len(in.Running) > 0 {
		return ZipDecision{ZipSkip, "a game is running (" + in.Running[0] + "); packing never runs while a game does"}
	}
	if v == "1" {
		return ZipDecision{ZipPack, "NPS5_GAME_COMPRESS=1"}
	}
	if !in.TTY {
		return ZipDecision{ZipSkip, "not a terminal, so no question (set NPS5_GAME_COMPRESS=1 to pack, 0 to silence this)"}
	}
	return ZipDecision{ZipAsk, ""}
}

// AskYes asks "[Y/n]" on w and reads one line from r. Enter and y/yes mean yes; n/no mean no; anything else is asked
// again (three times, then no). End of input is no: nothing is packed because a pipe closed.
func AskYes(r *bufio.Reader, w io.Writer, question string) bool {
	for i := 0; i < 3; i++ {
		fmt.Fprintf(w, "%s [Y/n] ", question)
		line, err := r.ReadString('\n')
		if err != nil && line == "" {
			fmt.Fprintln(w)
			return false
		}
		switch strings.ToLower(strings.TrimSpace(line)) {
		case "", "y", "yes":
			return true
		case "n", "no":
			return false
		}
		fmt.Fprintln(w, "Please answer y or n.")
	}
	return false
}

// AnswerFile is where the answer is remembered; scripts/nps5lib/cachezip.py reads it to default the shader-cache question.
func AnswerFile(getenv func(string) string) string {
	d := getenv("NPS5_DATA_STATE_DIR")
	if d == "" {
		home := getenv("HOME")
		if home == "" {
			home, _ = os.UserHomeDir()
		}
		d = filepath.Join(home, "Library", "Application Support", "nativeps5", "data")
	}
	return filepath.Join(d, "game-compress-answer")
}

// RecordAnswer writes "yes" or "no" to the answer file. A failure is returned, never fatal for the caller.
func RecordAnswer(getenv func(string) string, yes bool) error {
	f := AnswerFile(getenv)
	if err := os.MkdirAll(filepath.Dir(f), 0o755); err != nil {
		return err
	}
	v := "no\n"
	if yes {
		v = "yes\n"
	}
	return os.WriteFile(f, []byte(v), 0o644)
}

var gameProc = regexp.MustCompile(`(^|/)(ue2)?boot/native_|/ue2boot/native_out\d+|(^|/)native(_[A-Za-z0-9_.-]+)?$|(^|/)nps5-title$`)

// GameProcesses picks game processes out of `ps -axo pid=,comm=` output (same patterns as gamepack.py, plus a built
// app's nps5-title). self is this process's pid.
func GameProcesses(ps string, self int) []string {
	var hits []string
	for _, line := range strings.Split(ps, "\n") {
		f := strings.Fields(line)
		if len(f) < 2 {
			continue
		}
		if pid, err := strconv.Atoi(f[0]); err != nil || pid == self {
			continue
		}
		if gameProc.MatchString(strings.TrimSpace(line[strings.Index(line, f[1]):])) {
			hits = append(hits, strings.TrimSpace(line))
		}
	}
	return hits
}

func runningGames() []string {
	out, err := exec.Command("ps", "-axo", "pid=,comm=").Output()
	if err != nil {
		return []string{"ps failed: " + err.Error()} // cannot tell: refuse
	}
	return GameProcesses(string(out), os.Getpid())
}

// FindGamepack locates scripts/gamepack.py: $NPS5_GAMEPACK, $NPS5_ENTRY_DIR/scripts, then the entry repository next to
// (or around) the recomp checkout.
func FindGamepack(repo string) string {
	if p := os.Getenv("NPS5_GAMEPACK"); p != "" {
		if fileExists(p) {
			return p
		}
		return ""
	}
	var dirs []string
	if d := os.Getenv("NPS5_ENTRY_DIR"); d != "" {
		dirs = append(dirs, d)
	}
	dirs = append(dirs, filepath.Join(repo, ".."), filepath.Join(repo, "..", "nativeps5"), filepath.Join(repo, "..", "..", "nativeps5"),
		filepath.Join(repo, "..", "nps5"))
	for _, d := range dirs {
		if p := filepath.Join(d, "scripts", "gamepack.py"); fileExists(p) {
			return p
		}
	}
	return ""
}

func writable(dir string) bool {
	f, err := os.CreateTemp(dir, ".nps5-write-test-*")
	if err != nil {
		return false
	}
	name := f.Name()
	f.Close()
	os.Remove(name)
	return true
}

// ZipEnv is what MaybeCompressGame touches besides the dump; tests replace it.
type ZipEnv struct {
	Getenv  func(string) string
	In      *bufio.Reader
	Out     io.Writer
	TTY     bool
	Running func() []string
	DryRun  func(packer, dump string) (string, error)      // gamepack.py pack --dry-run output
	Pack    func(packer, dump string, out io.Writer) error // gamepack.py pack
}

// DefaultZipEnv is the real environment: stdin and stderr, ps, python3.
func DefaultZipEnv(out io.Writer) ZipEnv {
	return ZipEnv{
		Getenv: os.Getenv, In: bufio.NewReader(os.Stdin), Out: out, TTY: isTerminal(os.Stdin) && isTerminal(os.Stderr),
		Running: runningGames,
		DryRun: func(packer, dump string) (string, error) {
			b, err := exec.Command("python3", packer, "pack", "--dry-run", dump).Output()
			return string(b), err
		},
		Pack: func(packer, dump string, out io.Writer) error {
			c := exec.Command("python3", packer, "pack", dump)
			c.Stdout, c.Stderr = out, out
			return c.Run()
		},
	}
}

func isTerminal(f *os.File) bool {
	fi, err := f.Stat()
	return err == nil && fi.Mode()&os.ModeCharDevice != 0
}

// MaybeCompressGame is the step in `nps5 build`. It never fails the build: a refusal, a skip and a failed pack are
// reported on e.Out and the build goes on (a failed pack leaves every original in place).
func MaybeCompressGame(t *Title, repo string, e ZipEnv) {
	say := func(f string, a ...any) { fmt.Fprintf(e.Out, "[nps5] "+f+"\n", a...) }
	dump, err := filepath.EvalSymlinks(t.Dump)
	if err != nil {
		dump = t.Dump
	}
	surv, err := SurveyDump(dump)
	if err != nil {
		say("game files: cannot survey %s: %v (not compressing)", dump, err)
		return
	}
	in := ZipInput{Getenv: e.Getenv, TTY: e.TTY, External: onExternalVolume(dump), ReadOnly: !writable(dump)}
	if surv.FSType != "" && surv.FSType != "apfs" {
		in.NotAPFS = surv.FSType
	}
	packer := FindGamepack(repo)
	in.NoPacker = packer == ""
	// Cheap decisions first: the sampling dry run reads the big files, so it only runs when its answer can matter.
	in.Est = MakeEstimate(surv, false, 0, 0, 0)
	if !in.External && !in.NoPacker && in.NotAPFS == "" && !in.ReadOnly {
		in.Running = e.Running()
	}
	d := DecideZip(in)
	if d.Act == ZipAsk || d.Act == ZipPack {
		say("game files: estimating what packing would save (reads samples of the big files, changes nothing)")
		if out, err := e.DryRun(packer, dump); err == nil {
			if files, size, saved, ok := ParseGamepackTotal(out); ok {
				in.Est = MakeEstimate(surv, true, files, size, saved)
				d = DecideZip(in)
			}
		}
	}
	if d.Act == ZipAsk {
		fmt.Fprint(e.Out, in.Est.Prompt(t.Name))
		if AskYes(e.In, e.Out, "Compress the game files?") {
			d.Act = ZipPack
		} else {
			d.Act = ZipDecline
		}
	}
	switch d.Act {
	case ZipSkip:
		say("game files: not compressed: %s", d.Reason)
	case ZipDecline:
		say("game files: left as they are")
		if err := RecordAnswer(e.Getenv, false); err != nil {
			say("game files: could not remember the answer: %v", err)
		}
	case ZipPack:
		say("game files: packing %s (originals stay until each packed copy is verified; safe to stop with Ctrl-C and run again)", dump)
		if err := e.Pack(packer, dump, e.Out); err != nil {
			say("game files: packing did not finish (%v); every original is still in place, the build goes on. Run `python3 %s pack %s` to continue", err, packer, dump)
			return
		}
		say("game files: packed")
		if err := RecordAnswer(e.Getenv, true); err != nil {
			say("game files: could not remember the answer: %v", err)
		}
	}
}
