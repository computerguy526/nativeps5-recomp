package cli

import (
	"bufio"
	"bytes"
	"context"
	"debug/macho"
	"encoding/xml"
	"errors"
	"flag"
	"fmt"
	"io"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"slices"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"ps4-recomp/pkg/analyzer"
	"ps4-recomp/pkg/disasm"
	"ps4-recomp/pkg/elfloader"
	"ps4-recomp/pkg/emitter"
	"ps4-recomp/pkg/events"
	"ps4-recomp/pkg/guestmem"
	"ps4-recomp/pkg/lifter"
	recompruntime "ps4-recomp/pkg/runtime"
	"ps4-recomp/pkg/sfo"
	"ps4-recomp/pkg/tcache"
	"ps4-recomp/pkg/thirdparty"
)

// Config contains runtime configuration parameters parsed from CLI arguments.
type Config struct {
	ElfPath       string
	OutDir        string
	Compile       bool
	Run           bool
	TimeoutSec    int
	Jobs          int
	OptLevel      string
	AllSymbols    bool
	ChunkSize     int
	Verbose       bool
	AppDir        string
	CopyResources bool
	Asan          bool
	// TSO: acquire loads and release stores for non-stack guest memory (x86 memory ordering).
	TSO bool
	// CallIC: per-call-site inline caches for indirect/virtual calls (flat model; docs/m3/callic.md).
	CallIC bool
	// FpCmpFuse, PushPopFuse, JmpIC: the m3-liftmisc lifter speedups (docs/m3/liftmisc.md); each is on by default.
	FpCmpFuse, PushPopFuse, JmpIC bool
	// ROLoads, RedZoneLeaf, BMIPeep: the m3-lift2 lifter options (docs/m3/lift2.md); each is on by default.
	ROLoads, RedZoneLeaf, BMIPeep bool
	// TSOElide: "off" (every RSP/RBP-based access is plain) or "stack" (only accesses the stack
	// privacy pass proves thread-private are plain; docs/m3/tsoelide.md).
	TSOElide string
	// ColdOutline moves the rare paths of the TSO lowering to a cold area at the end of each chunk (docs/m3/coldpath.md).
	ColdOutline bool
	// AllowUnsupported keeps unsupported guest instructions as diagnostic stubs; by default they
	// fail the translation with a list.
	AllowUnsupported bool
	// PackedLayout: the title packs functions with no alignment, ending them with ud2 or an
	// unconditional jmp (-packed-layout, same as translate-time NPS5_SEED_AFTER_UD2=1): relocation
	// targets behind those are entries and fall-into entries are kept (pkg/disasm SetPackedLayout).
	PackedLayout bool
	// SeedsFrom: tier1_misses.jsonl / run bundle / runs dir (comma separated) whose served
	// misses are added to the CFG entry points (docs/m1/tier1.md).
	SeedsFrom string
	CacheDir  string // translation cache (chunk index); default <out>/.nps5cache
	NoCache   bool
	ObjStore  string // content-addressed zstd object store (optional)
	// AddrMode: "base" (x28 + guest address) or "identity" (guest VA == host VA).
	AddrMode string
	// X87: "f80" (80-bit x87 registers on SoftFloat, the default) or "f64" (stack of doubles).
	X87 string
	// SimdLiveness: vector lane liveness in the lifter: "abi" (default), "strict" or "off"
	// (docs/m3/simdregs.md).
	SimdLiveness string
	// GPU: "off" (default) or "anyps5" (see gpu.go).
	GPU string
	// CallModel: "flat" (guest stack authoritative, shadow return stack) or "nested" (host bl/ret).
	CallModel string
	// JSON emits progress as JSON lines on stdout; the human log goes to stderr.
	JSON bool
	// KeepAsm keeps the generated .s files (by default each chunk's assembly is deleted once it
	// is assembled). AsmComments puts the per-instruction guest comments into the assembly; it
	// is implied by KeepAsm unless -asm-comments=false is given.
	KeepAsm     bool
	AsmComments bool
	// AsmOnly translates and assembles the code chunks (streaming, as with -compile) but builds
	// no runtime and does not link.
	AsmOnly        bool
	asmCommentsSet bool
	// ExcludeModules names companion modules left out of the translation (-exclude-module,
	// repeatable). Their imports bind to a logged stub returning 0.
	ExcludeModules []string
	// HLEMalloc replaces the engine's allocator by a native one ("ue5-binned3"); HLEReplace is a
	// JSON table of function replacements (docs/m3/function-hle.md).
	HLEMalloc  string
	HLEReplace string
	// HLEReplaceUnverified admits -hle-replace functions that have neither a sha256 nor a vtable check.
	HLEReplaceUnverified bool
	// Patches is a game patch profile: guest patches lowered as run-time settings (pkg/emitter/patchpoints.go).
	Patches string
}

// asmComments reports whether the emitted assembly carries per-instruction comments.
func (c *Config) asmComments() bool {
	if c.asmCommentsSet {
		return c.AsmComments
	}
	return c.KeepAsm
}

type stringList []string

func (s *stringList) String() string { return strings.Join(*s, ",") }
func (s *stringList) Set(v string) error {
	for _, p := range strings.Split(v, ",") {
		if p = strings.TrimSpace(p); p != "" {
			*s = append(*s, p)
		}
	}
	return nil
}

const banner = `
===================================================================
  PS4Recomp: PlayStation 4 x86-64 to Native ARM64 AOT Recompiler
===================================================================`

const usageText = `
Usage:
  ps4-recomp [flags] <game-dir | eboot.bin | input.elf>
  ps4-recomp build [flags] <game-dir | eboot.bin | input.elf>
  ps4-recomp analyze <game-dir | eboot.bin | input.elf | module.prx>
  ps4-recomp version [--json]

Options:
  -o, -out <dir>          Output directory for generated code and binaries (default: "build")
  -e, -elf <file>         Path to input PS4 ELF or game directory
  -c, -compile            Compile generated code to native ARM64 binary with clang (default: true)
  -j, -jobs <num>         Number of parallel clang workers (default: NumCPU)
  -O, -opt <level>        Clang optimization level (0, 1, 2, 3, s, z) (default: "2")
  -r, -run                Run the compiled binary after successful recompilation (default: false)
  -t, -timeout <sec>      Watchdog timeout in seconds when running binary (0 = no timeout) (default: 0)
      -standalone         Create self-contained macOS .app bundle with all assets and dylibs
      -copy-resources     Alias for -standalone: copy game resources into .app bundle SharedSupport/
      -app-dir <dir>      Host directory mapping to /app0 for game assets (auto-detected if omitted)
  -a, -all-symbols        Recompile all symbols in symbol table even if unreachable (default: false)
  -k, -chunk-size <num>   Number of functions per emitted assembly chunk file (default: 250)
  -v, -verbose            Enable verbose output and detailed timing breakdown (default: false)
      -asan               Compile with AddressSanitizer and LeakSanitizer (default: false)
      -json               Emit progress as JSON lines on stdout (the log goes to stderr)
      -allow-unsupported  Keep unsupported guest instructions as diagnostic stubs (default: fail the build with a list)
      -packed-layout      Discovery for titles packed without function alignment: entries behind ud2/jmp, fall-into entries kept (= NPS5_SEED_AFTER_UD2=1)
      -addr-mode <mode>   Guest memory addressing: base (host = guest + bias, default), identity (guest VA == host VA),
                          or auto (identity when the title has no fixed-address evidence, else base)
      -keep-asm           Keep the generated .s files (default: each chunk is assembled and its .s deleted at once);
                          implies -asm-comments
      -asm-only           Translate and assemble the code chunks only: no runtime build, no link
      -asm-comments       Annotate the assembly with the guest instruction behind each host instruction
                          (default: lean output, the pc map used by crash diagnostics does not need them)
      -x87 <mode>         x87 model: f80 (80-bit extended precision on SoftFloat, default) or f64 (doubles, faster, not
                          bit-exact; for titles whose x87 use is hot and does not need 80 bits)
      -simd-liveness <m>  Vector lane liveness (drops dead YMM-upper zeroing, scalar lane merges and scratch spills):
                          abi (default: System V at calls and returns), strict (no calling convention assumed) or off
      -call-model <model> Call/return model: flat (default: the guest stack is the only call stack, flat host stack,
                          shadow return stack) or nested (guest CALL = host bl, host stack mirrors the guest's; A/B)
      -seeds-from <path>  Fold the tier-1 misses of earlier runs (tier1_misses.jsonl, a run bundle or a runs dir) into the entry points
      -cache-dir <dir>    Translation cache (chunk index, tier-1 fragments); default <out>/.nps5cache
      -no-cache           Do not reuse or record cached chunks
      -obj-store <dir>    Content-addressed zstd store of assembled objects (reused across output directories)
      -hle-malloc <mode>  Replace the engine allocator by a native one: ue5-binned3 (default: off; the binary must match a known
                          profile, docs/m3/function-hle.md)
      -hle-replace <file> JSON table of guest functions to run as host shims instead of being translated (docs/m3/function-hle.md)
      -hle-replace-unverified
                          Accept -hle-replace functions with neither sha256 nor vtable check (default: off, such a table is refused)
      -patches <file>     Game patch profile (profiles/patches/<title id>.json): each patch becomes a run-time setting; the
                          sites test a flag instead of being patched in (the build's instruction bytes are asserted)
      -call-ic            Per-call-site inline caches for indirect/virtual calls (flat model; default: true, -call-ic=false to disable)
      -fp-cmp-fuse        UCOMIS/COMIS followed by Jcc/SETcc/CMOVcc use the FCMP flags directly (default: true)
      -pushpop-fuse       Runs of PUSH/POP r64 become STP/LDP with one rsp update (default: true)
      -jmp-ic             Inline caches at jmp reg / jmp [mem] sites, needs -call-ic (default: true)
      -ro-loads           Constant-address loads from read-only image memory (RELRO, stack guard) are plain (default: true)
      -redzone-leaf       Under -tso-elide stack, the red zone of a call-entered function is private (default: true)
      -bmi-peep           BMI (blsi/blsr/blsmsk/andn/bextr/lzcnt/tzcnt) and narrow-flag peepholes (default: true)
      -cold-outline       Move rare paths (16-byte-crossing and unaligned TSO accesses) out of the hot code (default: true)
      -tso                x86 memory ordering: acquire/release for non-stack guest memory (default: true)
      -tso-elide <mode>   Stack accesses without ordering: stack (default: plain only where the access is proven thread-private),
                          off (escape hatch: every RSP/RBP-based access plain, no analysis; unsound for escaped frames) or stack-region (stack, plus slots
                          below every escaping frame address); docs/m3/tsoelide.md
      -gpu <backend>      Graphics/video/input: off (default, PS4Recomp HLE) or anyps5 (AnyPS5 AGC driver, VideoOut, Pad,
                          UserService; needs NPS5_ANYPS5_BUILD, see docs/m2/anyps5-integration.md)
  -h, -help               Show this help message
`

func normalizeArgs(args []string) ([]string, []string) {
	var flags []string
	var positional []string
	valueFlags := map[string]bool{
		"-o": true, "-out": true, "--out": true,
		"-e": true, "-elf": true, "--elf": true,
		"-t": true, "-timeout": true, "--timeout": true,
		"-j": true, "-jobs": true, "--jobs": true,
		"-O": true, "-opt": true, "--opt": true,
		"-k": true, "-chunk-size": true, "--chunk-size": true,
		"-app-dir": true, "--app-dir": true,
		"-data-dir": true, "--data-dir": true,
		"-dump-dir": true, "--dump-dir": true,
		"-addr-mode": true, "--addr-mode": true,
		"-x87": true, "--x87": true,
		"-simd-liveness": true, "--simd-liveness": true,
		"-exclude-module": true, "--exclude-module": true,
		"-gpu": true, "--gpu": true,
		"-call-model": true, "--call-model": true,
		"-tso-elide": true, "--tso-elide": true,
		"-seeds-from": true, "--seeds-from": true,
		"-cache-dir": true, "--cache-dir": true,
		"-obj-store": true, "--obj-store": true,
		"-hle-malloc": true, "--hle-malloc": true,
		"-hle-replace": true, "--hle-replace": true,
		"-patches": true, "--patches": true,
	}

	for i := 0; i < len(args); i++ {
		arg := args[i]
		if strings.HasPrefix(arg, "-") {
			flags = append(flags, arg)
			base, _, _ := strings.Cut(arg, "=")
			if valueFlags[base] && !strings.Contains(arg, "=") && i+1 < len(args) && !strings.HasPrefix(args[i+1], "-") {
				i++
				flags = append(flags, args[i])
			}
		} else {
			positional = append(positional, arg)
		}
	}
	return flags, positional
}

// ParseArgs parses CLI arguments into a Config structure.
func ParseArgs(args []string) (*Config, error) {
	flagArgs, posArgs := normalizeArgs(args)

	fs := flag.NewFlagSet("ps4-recomp", flag.ContinueOnError)
	fs.SetOutput(io.Discard)

	cfg := &Config{
		OutDir:    "build",
		Compile:   true,
		Jobs:      defaultJobs(),
		OptLevel:  "2",
		ChunkSize: 250,
		TSO:       true,
		CallIC:    true,
		FpCmpFuse: true, PushPopFuse: true, JmpIC: true, ColdOutline: true,
		ROLoads: true, RedZoneLeaf: true, BMIPeep: true,
		TSOElide:  "stack",
		AddrMode:  "base",
		X87:       "f80",
		CallModel: "flat",
	}

	var help bool

	fs.StringVar(&cfg.ElfPath, "elf", "", "Path to input PS4 ELF binary")
	fs.StringVar(&cfg.ElfPath, "e", "", "Path to input PS4 ELF binary (short)")
	fs.StringVar(&cfg.OutDir, "out", cfg.OutDir, "Output directory for recompiled assembly code and binaries")
	fs.StringVar(&cfg.OutDir, "o", cfg.OutDir, "Output directory (short)")
	fs.BoolVar(&cfg.Compile, "compile", cfg.Compile, "Compile generated assembly code to native ARM64 binary with clang")
	fs.BoolVar(&cfg.Compile, "c", cfg.Compile, "Compile generated code (short)")
	fs.BoolVar(&cfg.Run, "run", cfg.Run, "Run the compiled binary after successful compilation")
	fs.BoolVar(&cfg.Run, "r", cfg.Run, "Run binary (short)")
	fs.IntVar(&cfg.TimeoutSec, "timeout", cfg.TimeoutSec, "Execution timeout in seconds")
	fs.IntVar(&cfg.TimeoutSec, "t", cfg.TimeoutSec, "Execution timeout in seconds (short)")
	fs.IntVar(&cfg.Jobs, "jobs", cfg.Jobs, "Parallel compiler jobs")
	fs.IntVar(&cfg.Jobs, "j", cfg.Jobs, "Parallel compiler jobs (short)")
	fs.StringVar(&cfg.OptLevel, "opt", cfg.OptLevel, "Clang optimization level")
	fs.StringVar(&cfg.OptLevel, "O", cfg.OptLevel, "Clang optimization level (short)")
	fs.BoolVar(&cfg.AllSymbols, "all-symbols", cfg.AllSymbols, "Recompile all symbols in symbol table")
	fs.BoolVar(&cfg.AllSymbols, "a", cfg.AllSymbols, "Recompile all symbols (short)")
	fs.IntVar(&cfg.ChunkSize, "chunk-size", cfg.ChunkSize, "Number of functions per C chunk")
	fs.IntVar(&cfg.ChunkSize, "k", cfg.ChunkSize, "Chunk size (short)")
	fs.StringVar(&cfg.AppDir, "app-dir", "", "Host directory mapping to /app0 for game assets")
	// --data-dir / --dump-dir: aliases of --app-dir for a title whose data lives on a separate READ-ONLY volume
	// (the eboot may then be a local file, positional or -e). NPS5_APP0_DIR is the same thing as an env var.
	fs.StringVar(&cfg.AppDir, "data-dir", "", "Alias of --app-dir: read-only data folder mapped to /app0 (never written)")
	fs.StringVar(&cfg.AppDir, "dump-dir", "", "Alias of --app-dir")
	fs.BoolVar(&cfg.CopyResources, "standalone", false, "Create self-contained macOS .app bundle with all assets and dylibs")
	fs.BoolVar(&cfg.CopyResources, "bundle", false, "Alias for --standalone")
	fs.BoolVar(&cfg.CopyResources, "copy-resources", false, "Copy all game assets/resources into macOS .app bundle SharedSupport/")
	fs.BoolVar(&cfg.CopyResources, "bundle-resources", false, "Copy all game assets/resources into macOS .app bundle SharedSupport/ (alias)")
	fs.BoolVar(&cfg.Asan, "asan", false, "Compile with AddressSanitizer and LeakSanitizer")
	fs.BoolVar(&cfg.CallIC, "call-ic", cfg.CallIC, "Inline caches at indirect/virtual call sites (flat call model)")
	fs.BoolVar(&cfg.FpCmpFuse, "fp-cmp-fuse", cfg.FpCmpFuse, "UCOMIS/COMIS + Jcc/SETcc/CMOVcc: consume the FCMP flags directly")
	fs.BoolVar(&cfg.PushPopFuse, "pushpop-fuse", cfg.PushPopFuse, "Runs of PUSH/POP r64 become STP/LDP with one rsp update")
	fs.BoolVar(&cfg.ColdOutline, "cold-outline", cfg.ColdOutline, "Move rare paths out of the hot code into a cold area at the end of each chunk")
	fs.BoolVar(&cfg.JmpIC, "jmp-ic", cfg.JmpIC, "Inline caches at jmp reg / jmp [mem] sites (needs -call-ic)")
	fs.BoolVar(&cfg.ROLoads, "ro-loads", cfg.ROLoads, "Constant-address loads from read-only image memory (RELRO, stack guard) are plain")
	fs.BoolVar(&cfg.RedZoneLeaf, "redzone-leaf", cfg.RedZoneLeaf, "With -tso-elide stack: the red zone of a call-entered function is private")
	fs.BoolVar(&cfg.BMIPeep, "bmi-peep", cfg.BMIPeep, "BMI and narrow-flag peepholes")
	fs.BoolVar(&cfg.TSO, "tso", cfg.TSO, "x86 memory ordering: acquire/release for non-stack guest memory")
	fs.StringVar(&cfg.TSOElide, "tso-elide", cfg.TSOElide, "Stack accesses without ordering: off, stack or stack-region")
	fs.BoolVar(&cfg.AllowUnsupported, "allow-unsupported", false, "Keep unsupported guest instructions as diagnostic stubs")
	fs.BoolVar(&cfg.PackedLayout, "packed-layout", false, "Discovery for titles packed without function alignment (entries behind ud2/jmp; as NPS5_SEED_AFTER_UD2=1)")
	fs.StringVar(&cfg.AddrMode, "addr-mode", cfg.AddrMode, "Guest memory addressing: base, identity or auto")
	fs.StringVar(&cfg.X87, "x87", cfg.X87, "x87 model: f80 (80-bit, default) or f64 (doubles)")
	fs.StringVar(&cfg.SimdLiveness, "simd-liveness", cfg.SimdLiveness, "Vector lane liveness: abi (default), strict or off")
	fs.Var((*stringList)(&cfg.ExcludeModules), "exclude-module", "Companion module to leave out of the translation (repeatable); its imports bind to a stub returning 0")
	fs.StringVar(&cfg.GPU, "gpu", gpuOff, "Graphics/video/input backend: off or anyps5")
	fs.StringVar(&cfg.CallModel, "call-model", cfg.CallModel, "Call/return model: flat or nested")
	fs.StringVar(&cfg.CacheDir, "cache-dir", "", "Translation cache directory (default <out>/.nps5cache)")
	fs.BoolVar(&cfg.NoCache, "no-cache", false, "Do not reuse or record cached chunks")
	fs.StringVar(&cfg.ObjStore, "obj-store", "", "Directory of a content-addressed, zstd-compressed store of assembled objects")
	fs.StringVar(&cfg.SeedsFrom, "seeds-from", "", "Fold tier-1 misses of earlier runs into the entry points")
	fs.StringVar(&cfg.HLEMalloc, "hle-malloc", "", "Replace the engine's allocator by a native one: ue5-binned3 (default: off)")
	fs.StringVar(&cfg.HLEReplace, "hle-replace", "", "JSON table of guest functions to run as host shims (docs/m3/function-hle.md)")
	fs.BoolVar(&cfg.HLEReplaceUnverified, "hle-replace-unverified", false, "Accept -hle-replace functions that no sha256 or vtable check ties to the binary (default: refuse)")
	fs.StringVar(&cfg.Patches, "patches", "", "Game patch profile (JSON): guest patches lowered as run-time settings")
	fs.BoolVar(&cfg.JSON, "json", false, "Emit progress as JSON lines on stdout")
	fs.BoolVar(&cfg.KeepAsm, "keep-asm", false, "Keep the generated .s files")
	fs.BoolVar(&cfg.AsmOnly, "asm-only", false, "Translate and assemble the code chunks only (no runtime build, no link)")
	fs.BoolVar(&cfg.AsmComments, "asm-comments", false, "Annotate the assembly with guest instructions")
	fs.BoolVar(&cfg.Verbose, "verbose", cfg.Verbose, "Verbose logging")
	fs.BoolVar(&cfg.Verbose, "v", cfg.Verbose, "Verbose logging (short)")
	fs.BoolVar(&help, "help", false, "Show help message")
	fs.BoolVar(&help, "h", false, "Show help message (short)")

	if err := fs.Parse(flagArgs); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			fmt.Print(banner + usageText)
			return nil, flag.ErrHelp
		}
		return nil, fmt.Errorf("invalid arguments: %w\nRun 'ps4-recomp --help' for usage", err)
	}

	if help {
		fmt.Print(banner + usageText)
		return nil, flag.ErrHelp
	}
	if cfg.AsmOnly {
		cfg.Compile = false
	}
	fs.Visit(func(f *flag.Flag) {
		if f.Name == "asm-comments" {
			cfg.asmCommentsSet = true
		}
	})

	// "build" is an optional alias of the default command.
	if len(posArgs) > 0 && posArgs[0] == "build" {
		posArgs = posArgs[1:]
	}

	// Positional arguments override if -elf was not explicitly passed
	if len(posArgs) > 0 {
		subcmd := posArgs[0]
		if subcmd != "analyze" {
			if fi, err := os.Stat(subcmd); err == nil && fi.IsDir() {
				for _, candidate := range []string{"eboot.bin", "EBOOT.BIN", filepath.Join("sce_sys", "eboot.bin")} {
					target := filepath.Join(subcmd, candidate)
					if _, err := os.Stat(target); err == nil {
						cfg.ElfPath = target
						if cfg.AppDir == "" {
							cfg.AppDir, _ = filepath.Abs(subcmd)
						}
						break
					}
				}
				if cfg.ElfPath == "" {
					return nil, fmt.Errorf("directory '%s' does not contain an eboot.bin binary", subcmd)
				}
			} else if !strings.HasSuffix(subcmd, ".elf") && !strings.HasSuffix(subcmd, ".bin") && !strings.HasSuffix(subcmd, ".prx") && !strings.HasSuffix(subcmd, ".sprx") {
				return nil, fmt.Errorf("unknown command or invalid input file '%s'. Run 'ps4-recomp --help' for usage", subcmd)
			} else if cfg.ElfPath == "" {
				cfg.ElfPath = subcmd
			}
		}
	}

	if cfg.ElfPath == "" && cfg.AppDir != "" {
		// data-dir only: the eboot sits in the data folder
		for _, candidate := range []string{"eboot.bin", "EBOOT.BIN"} {
			if fi, err := os.Stat(filepath.Join(cfg.AppDir, candidate)); err == nil && fi.Size() > 0 {
				cfg.ElfPath = filepath.Join(cfg.AppDir, candidate)
				break
			}
		}
	}
	if cfg.ElfPath == "" {
		return nil, fmt.Errorf("no input PS4 ELF binary specified\n%s", usageText)
	}

	// NPS5_APP0_DIR (same variable the runtime VFS reads) names the data folder when no flag did.
	if cfg.AppDir == "" {
		if v := os.Getenv("NPS5_APP0_DIR"); v != "" {
			if abs, err := filepath.Abs(v); err == nil {
				cfg.AppDir = abs
			}
		}
	}
	// Auto-detect AppDir (directory containing game files/assets if omitted)
	if cfg.AppDir == "" && cfg.ElfPath != "" {
		elfDir := filepath.Dir(cfg.ElfPath)
		markers := []string{"sce_sys", "assets", "contentid.txt", "sce_module"}
		checkMarkers := func(d string) bool {
			for _, m := range markers {
				if _, err := os.Stat(filepath.Join(d, m)); err == nil {
					return true
				}
			}
			return false
		}

		if checkMarkers(elfDir) {
			if absDir, err := filepath.Abs(elfDir); err == nil {
				cfg.AppDir = absDir
			}
		} else {
			dir := elfDir
			for range 5 {
				if checkMarkers(dir) {
					if absDir, err := filepath.Abs(dir); err == nil {
						cfg.AppDir = absDir
					}
					break
				}
				parent := filepath.Dir(dir)
				if parent == dir {
					break
				}
				dir = parent
			}
		}
	}

	if cfg.Jobs < 1 {
		cfg.Jobs = 1
	}

	return cfg, nil
}

// Version is the ps4-recomp release string, reported as "built_with" by build --json.
const Version = "0.1.0"

// stdout is where machine-readable output (JSON events) goes; tests replace it.
var stdout io.Writer = os.Stdout

// logw receives the human-readable build log. With --json it is stderr, so stdout stays pure
// JSON lines.
var logw io.Writer = os.Stdout

// Execute runs the complete recompilation and optional execution pipeline.
func Execute(args []string) (err error) {
	if len(args) > 0 && (args[0] == "version" || args[0] == "-version" || args[0] == "--version") {
		if slices.Contains(args, "-json") || slices.Contains(args, "--json") {
			ev := events.New(stdout, "version", true)
			ev.Done(map[string]string{"version": Version, "go": runtime.Version()})
			return nil
		}
		fmt.Fprintf(stdout, "ps4-recomp %s\n", Version)
		return nil
	}
	if handled, derr := diagCommand(args); handled {
		return derr
	}
	if len(args) > 0 && args[0] == "fragment-serve" {
		return fragmentServeCommand(args[1:])
	}
	if len(args) > 0 && args[0] == "fragment" {
		return fragmentCommand(args[1:])
	}
	if len(args) > 0 && args[0] == "analyze" {
		if len(args) < 2 {
			return errors.New("usage: ps4-recomp analyze <binary.elf | eboot.bin | module.prx>")
		}
		report, err := analyzer.AnalyzeBinary(args[1])
		if err != nil {
			return fmt.Errorf("analysis failed: %w", err)
		}
		fmt.Println(report.SummaryString())
		return nil
	}

	cfg, err := ParseArgs(args)
	if err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return nil
		}
		if slices.Contains(args, "-json") || slices.Contains(args, "--json") {
			return events.New(stdout, "build", true).Error("bad_input", err.Error())
		}
		return err
	}

	logw = os.Stdout
	var ev *events.Writer
	if cfg.JSON {
		logw = os.Stderr
		ev = events.New(stdout, "build", true)
	} else {
		ev = events.New(io.Discard, "build", false)
	}
	defer func() { logw = os.Stdout }()
	// Every failure of the build path is reported as an error event before it is returned.
	defer func() {
		if err != nil && cfg.JSON {
			code := "build_failed"
			switch {
			case errors.Is(err, errBuildCancelled):
				code = "cancelled"
			case errors.Is(err, fs.ErrNotExist):
				code = "bad_input"
			}
			ev.Error(code, err.Error())
		}
	}()
	// SIGINT or SIGTERM cancels the build: the ninja or clang process groups are stopped,
	// and the build ends at the next stage boundary with errBuildCancelled.
	ctx, stopSignals := buildSignalContext()
	defer stopSignals()

	totalStart := time.Now()
	fmt.Fprintln(logw, banner)
	ds := diagStart(cfg)
	defer func() { ds.close(err) }()

	if noOpRebuild(cfg) {
		// Same inputs and lowering as the translation in the output directory: refresh the runtime
		// sources (the only part that may have changed) and stop.
		stepStart := time.Now()
		if runtimeDir, err := findRuntimeDir(); err == nil {
			if err := copyDir(runtimeDir, filepath.Join(cfg.OutDir, "runtime")); err != nil {
				return fmt.Errorf("failed to copy runtime files: %w", err)
			}
		}
		fmt.Fprintf(logw, "[ps4-recomp] Up to date: translation stamp unchanged (lowering %s); runtime sources refreshed in %v\n",
			emitter.LoweringID(), time.Since(stepStart).Round(time.Millisecond))
		ev.Done(buildResult{BuiltWith: Version})
		return nil
	}

	// -gpu rebinds the import table: before the loader resolves a single import.
	if err := setGPU(cfg.GPU); err != nil {
		return err
	}

	// Step 1: Load ELF
	stepStart := time.Now()
	ev.Progress("load", 1, 4, "stages", "loading the ELF")
	fmt.Fprintf(logw, "[ps4-recomp] [1/4] Loading ELF: %s\n", cfg.ElfPath)
	loaded, err := elfloader.LoadELF(cfg.ElfPath)
	if err != nil {
		return fmt.Errorf("failed to load ELF '%s': %w", cfg.ElfPath, err)
	}
	// Guest memory model (docs/m1/memory.md): pick the addressing mode from the title's own
	// evidence, then place the executable in that mode's guest layout before anything reads
	// addresses out of it. Everything downstream (CFG, lifter, emitter) sees final guest addresses.
	memPlan, err := planGuestMemory(cfg, loaded)
	if err != nil {
		return err
	}
	fmt.Fprintf(logw, "             Entry point: 0x%x | Segments: %d | Relocations: %d | Unwind functions: %d | Time: %v\n",
		loaded.EntryPoint, len(loaded.Segments), len(loaded.Relocations), len(loaded.FuncBounds), time.Since(stepStart).Round(time.Millisecond))
	fmt.Fprintf(logw, "             Memory model: %s (%s) | image 0x%x-0x%x | host bias 0x%x\n",
		memPlan.layout.Mode, memPlan.reason, loaded.MinVAddr, loaded.MaxVAddr, memPlan.layout.HostBase)

	if ctx.Err() != nil {
		return errBuildCancelled
	}

	// Step 2: Disassembly and CFG Analysis
	stepStart = time.Now()
	ev.Progress("cfg", 2, 4, "stages", "analyzing the control flow")
	fmt.Fprintf(logw, "[ps4-recomp] [2/4] Analyzing CFG and discovering reachable code...\n")
	for _, p := range elfloader.AddTLSModule(loaded, loaded) {
		fmt.Fprintf(os.Stderr, "             Warning: %s\n", p)
	}
	moduleInfos, err := loadCompanionModules(cfg, loaded, memPlan.layout)
	if err != nil {
		return err
	}
	if len(cfg.ExcludeModules) > 0 {
		var linked []elfloader.Symbol
		for _, m := range moduleInfos {
			linked = append(linked, m.Exports...)
		}
		loaded.ClearExcluded(linked)
		fmt.Fprintf(logw, "             Excluded modules: %s (%d exported NIDs stubbed)\n", strings.Join(cfg.ExcludeModules, ", "), len(loaded.ExcludedImports))
	}
	elfloader.LayoutStaticTLS(loaded.TLSModules)
	for _, p := range elfloader.ApplyTLSOffsets(loaded) {
		fmt.Fprintf(os.Stderr, "             Warning: %s\n", p)
	}
	if n := len(loaded.TLSModules); n > 0 {
		fmt.Fprintf(logw, "             Static TLS: %d modules, 0x%x bytes\n", n, loaded.TLSModules[n-1].Offset)
	}
	var allCompanionExports []elfloader.Symbol
	for _, m := range moduleInfos {
		allCompanionExports = append(allCompanionExports, m.Exports...)
	}
	if err := elfloader.ResolveModuleRelocations(loaded, allCompanionExports); err != nil {
		return fmt.Errorf("failed resolving module relocations: %w", err)
	}
	hle := emitter.NewHLEReport(loaded, moduleInfos)
	fmt.Fprint(logw, hle.ImportSummary())

	disasm.SetPackedLayout(cfg.PackedLayout)
	cfg.PackedLayout = disasm.PackedLayout() // the environment opt-in counts too (tier-1 manifest, stamp)
	d, err := disasm.NewDisassembler(loaded)
	if err != nil {
		return fmt.Errorf("failed to initialize disassembler: %w", err)
	}
	d.ROLoads = cfg.ROLoads
	d.LeafRedZone = cfg.RedZoneLeaf // before discovery: it records the jump-entered functions
	vecLive, okVec := disasm.ParseVecLiveMode(cfg.SimdLiveness)
	if !okVec {
		return fmt.Errorf("-simd-liveness %q: want abi, strict or off", cfg.SimdLiveness)
	}
	cfg.SimdLiveness = vecLive.String()
	d.VecLive = vecLive

	entries := disasm.SeedEntryPoints(loaded, cfg.AllSymbols)
	if cfg.SeedsFrom != "" {
		seeds, skipped, err := seedsForElf(cfg.SeedsFrom, cfg.ElfPath)
		if err != nil {
			return err
		}
		if skipped != "" {
			fmt.Fprintf(os.Stderr, "             Warning: tier-1 seeds from %s ignored: %s\n", cfg.SeedsFrom, skipped)
		}
		added := 0
		for _, a := range seeds {
			if d.InCode(a) && !slices.Contains(entries, a) {
				entries = append(entries, a)
				added++
			} else if !d.InCode(a) {
				fmt.Fprintf(os.Stderr, "             Warning: tier-1 seed 0x%x is not in executable guest code of this title; ignored\n", a)
			}
		}
		fmt.Fprintf(logw, "             Tier-1 seeds: %d read from %s, %d new entry points\n", len(seeds), cfg.SeedsFrom, added)
	}
	for _, a := range emitter.ReplacementSeeds(loaded, cfg.HLEMalloc, cfg.HLEReplace) {
		if d.InCode(a) && !slices.Contains(entries, a) {
			entries = append(entries, a) // a function only a vtable reaches still needs its stub
		}
	}
	lastReport := time.Now()
	funcs, err := d.DiscoverReachable(entries, func(nFn, nInst int) {
		if time.Since(lastReport) >= 2*time.Second {
			fmt.Fprintf(logw, "             ... %d functions, %d instructions\n", nFn, nInst)
			lastReport = time.Now()
		}
	})
	if err != nil {
		return fmt.Errorf("CFG analysis error: %w", err)
	}

	totalInsts := 0
	for _, f := range funcs {
		totalInsts += f.Insts
	}
	fmt.Fprintf(logw, "             Discovered %d functions, %d instructions | cap hits: %d | privileged stops: %d | Time: %v\n",
		len(funcs), totalInsts, d.CapHits, d.PrivilegedStops, time.Since(stepStart).Round(time.Millisecond))
	if d.CapHits > 0 {
		fmt.Fprintf(os.Stderr, "             Warning: %d function(s) hit decode caps:\n", d.CapHits)
		for i, hit := range d.CapHitDetails {
			if i >= 10 {
				fmt.Fprintf(os.Stderr, "               ... and %d more\n", len(d.CapHitDetails)-10)
				break
			}
			fmt.Fprintf(os.Stderr, "               - %s (0x%x): %s at PC 0x%x (%d insts decoded)\n",
				hit.Name, hit.EntryAddr, hit.Reason, hit.PC, hit.InstCount)
		}
	}

	if ctx.Err() != nil {
		return errBuildCancelled
	}

	// Step 3: C Source Code Generation
	stepStart = time.Now()
	ev.Progress("emit", 3, 4, "stages", "emitting C sources")
	fmt.Fprintf(logw, "[ps4-recomp] [3/4] Emitting source files to '%s/'...\n", cfg.OutDir)
	if err := os.MkdirAll(cfg.OutDir, 0o755); err != nil {
		return fmt.Errorf("failed to create output directory '%s': %w", cfg.OutDir, err)
	}

	runtimeOutDir := filepath.Join(cfg.OutDir, "runtime")
	if runtimeDir, err := findRuntimeDir(); err == nil {
		if err := copyDir(runtimeDir, runtimeOutDir); err != nil {
			return fmt.Errorf("failed to copy runtime files: %w", err)
		}
	} else {
		fmt.Fprintf(logw, "             Using embedded runtime files (standalone mode)...\n")
		if err := recompruntime.Extract(runtimeOutDir); err != nil {
			return fmt.Errorf("failed to extract embedded runtime: %w", err)
		}
	}

	var runtimeCFiles []string
	var runtimeIncludeDirs []string
	seenInc := make(map[string]bool)
	seenInc[cfg.OutDir] = true
	seenInc[runtimeOutDir] = true

	err = filepath.Walk(runtimeOutDir, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if info.IsDir() {
			if !seenInc[path] {
				seenInc[path] = true
				runtimeIncludeDirs = append(runtimeIncludeDirs, path)
			}
			return nil
		}
		if isRuntimeSource(info.Name()) {
			runtimeCFiles = append(runtimeCFiles, path)
		}
		return nil
	})
	if err != nil {
		return fmt.Errorf("failed scanning runtime files: %w", err)
	}

	thirdPartyOutDir := filepath.Join(cfg.OutDir, "3rdparty")
	register3rdPartyDir(thirdPartyOutDir)

	gcnInc, gcnLib := findGcnRecompiler()
	spirvInc, _ := findSpirvCross()
	atracInc, atracLib, atracSrcs := findLibAtrac9()
	minimp3Inc := findMinimp3()

	if (gcnLib == "" || spirvInc == "" || (atracLib == "" && len(atracSrcs) == 0) || minimp3Inc == "") && thirdparty.HasEmbedded() {
		fmt.Fprintf(logw, "             Extracting embedded 3rdparty libraries to '%s'...\n", thirdPartyOutDir)
		if err := thirdparty.Extract(thirdPartyOutDir); err != nil {
			return fmt.Errorf("failed to extract embedded 3rdparty libraries: %w", err)
		}
		gcnInc, gcnLib = findGcnRecompiler()
		spirvInc, _ = findSpirvCross()
		atracInc, atracLib, atracSrcs = findLibAtrac9()
		minimp3Inc = findMinimp3()
	}

	if gcnLib != "" {
		// the runtime is compiled against these headers: the library must be their build
		if err := checkGcnLibrary(gcnLib); err != nil {
			return err
		}
	}
	if gcnInc != "" && !seenInc[gcnInc] {
		seenInc[gcnInc] = true
		runtimeIncludeDirs = append(runtimeIncludeDirs, gcnInc)
	}
	if spirvInc != "" && !seenInc[spirvInc] {
		seenInc[spirvInc] = true
		runtimeIncludeDirs = append(runtimeIncludeDirs, spirvInc)
	}
	if atracInc != "" {
		if !seenInc[atracInc] {
			seenInc[atracInc] = true
			runtimeIncludeDirs = append(runtimeIncludeDirs, atracInc)
		}
		if atracLib == "" {
			runtimeCFiles = append(runtimeCFiles, atracSrcs...)
		}
	}
	if minimp3Inc != "" && !seenInc[minimp3Inc] {
		seenInc[minimp3Inc] = true
		runtimeIncludeDirs = append(runtimeIncludeDirs, minimp3Inc)
	}
	slices.Sort(runtimeCFiles)
	slices.Sort(runtimeIncludeDirs)

	knownFuncs := make(map[uint64]bool, len(funcs))
	for _, f := range funcs {
		knownFuncs[f.Addr] = true
	}

	mode, err := lifter.ParseAddrMode(cfg.AddrMode)
	if err != nil {
		return err
	}
	callModel, err := lifter.ParseCallModel(cfg.CallModel)
	if err != nil {
		return err
	}
	lifter.SetCallModel(callModel)
	lifter.SetCallIC(cfg.CallIC)
	lifter.SetFpCmpFuse(cfg.FpCmpFuse)
	lifter.SetPushPopFuse(cfg.PushPopFuse)
	lifter.SetJmpIC(cfg.JmpIC)
	lifter.SetROLoads(cfg.ROLoads)
	lifter.SetRedZoneLeaf(cfg.RedZoneLeaf)
	lifter.SetBMIPeep(cfg.BMIPeep)
	lifter.SetColdOutline(cfg.ColdOutline)
	l := lifter.NewArm64LifterMode(knownFuncs, mode)
	l.SetTSO(cfg.TSO)
	elide, err := lifter.ParseTSOElide(cfg.TSOElide)
	if err != nil {
		return err
	}
	l.SetTSOElide(elide)
	cfg.TSOElide = elide.String()
	if cfg.X87 == "" {
		cfg.X87 = "f80"
	}
	if err := l.SetX87Mode(cfg.X87); err != nil {
		return err
	}
	fmt.Fprintf(logw, "             x87 model: %s\n", cfg.X87)
	l.SetVecLiveness(vecLive != disasm.VecLiveOff)
	fmt.Fprintf(logw, "             simd liveness: %s\n", cfg.SimdLiveness)
	l.SetAllowUnsupported(cfg.AllowUnsupported)
	em := emitter.NewAsmEmitter(loaded, d, l)
	em.AppDir = cfg.AppDir
	em.Modules = moduleInfos
	em.ChunkSize = cfg.ChunkSize
	em.Funcs = funcs
	em.HLE = hle
	em.FuncInfoOf = ds.funcInfoFn()
	em.Workers = cfg.Jobs
	if err := applyFuncReplMode(em, cfg.HLEMalloc, cfg.HLEReplace, cfg.HLEReplaceUnverified, false, logw); err != nil {
		return err
	}
	if err := applyPatches(em, cfg.Patches, logw); err != nil {
		return err
	}
	mem := memPlan.manifest(loaded, moduleInfos)
	em.Mem = &mem
	em.Comments = cfg.asmComments()
	em.PCMap = ds != nil && os.Getenv("NPS5_NO_PCMAP") == ""
	em.PCMapStrict = os.Getenv("NPS5_PCMAP_STRICT") != ""
	// Streaming build: with compilation on, each chunk is assembled as it is emitted and its
	// .s deleted (unless -keep-asm); with -compile=false the .s files are the product.
	var pipe *asmPipeline
	if cfg.Compile || cfg.AsmOnly {
		pipe = newAsmPipeline(ctx, cfg.Jobs, cfg.KeepAsm)
		em.ChunkSink = pipe
		if !cfg.NoCache {
			cacheDir := cfg.CacheDir
			if cacheDir == "" {
				cacheDir = filepath.Join(cfg.OutDir, ".nps5cache")
			}
			em.Cache = tcache.Open(cacheDir)
			if cfg.ObjStore != "" {
				pipe.store = tcache.OpenObjStore(cfg.ObjStore)
			}
		}
	}
	emitStart := time.Now()
	cFiles, err := em.EmitAll(cfg.OutDir)
	if ds != nil {
		ds.infos = em.FuncInfos
	}
	if err == nil && em.Cache != nil {
		keep := map[string]bool{}
		for i := range em.ChunkStats.Chunks {
			keep[fmt.Sprintf("code_%03d", i)] = true
		}
		em.Cache.Drop("chunk", keep)
		if serr := em.Cache.Save(); serr != nil {
			fmt.Fprintf(os.Stderr, "             Warning: translation cache index not saved: %v\n", serr)
		}
	}
	cs := em.ChunkStats
	fmt.Fprintf(logw, "             Lifting: %d chunks on %d workers (%d lifted, %d served from the cache) | lowering %s | Time: %v\n",
		cs.Chunks, cs.Workers, cs.Lifted, cs.FromCache, emitter.LoweringID(), time.Since(emitStart).Round(time.Millisecond))
	var asmObjs []string
	var asmSt asmStats
	if pipe != nil {
		var perr error
		asmObjs, asmSt, perr = pipe.Finish(cfg.OutDir, err)
		if err == nil {
			err = perr
		}
		fmt.Fprintf(logw, "             Assembly: %d chunks (%d assembled, %d up to date) | generated %s | peak assembly on disk %s, with objects %s | objects %s | Time: %v\n",
			asmSt.Chunks, asmSt.Assembled, asmSt.Reused, humanBytes(asmSt.TotalAsmBytes), humanBytes(asmSt.PeakAsmBytes), humanBytes(asmSt.PeakDiskBytes), humanBytes(asmSt.ObjBytes), time.Since(stepStart).Round(time.Millisecond))
		if len(asmSt.FailedChunkSrc) > 0 {
			fmt.Fprintf(logw, "             Kept for diagnosis: %s\n", strings.Join(asmSt.FailedChunkSrc, ", "))
		}
	}
	if err != nil {
		if errors.Is(err, errBuildCancelled) {
			return errBuildCancelled
		}
		return fmt.Errorf("failed to emit ARM64 assembly: %w", err)
	}
	cFiles = append(cFiles, asmObjs...)
	hlePath := filepath.Join(cfg.OutDir, "hle_report.txt")
	if err := hle.WriteFile(hlePath); err != nil {
		return fmt.Errorf("failed to write HLE report: %w", err)
	}
	diagExtra := ds.afterEmit(cfg, loaded, hle, &em.Facts, asmSt, pipe != nil)
	if tc, terr := writeTier1Artifacts(cfg, funcs); terr != nil {
		fmt.Fprintf(os.Stderr, "             Warning: tier-1 miss fallback disabled for this build: %v\n", terr)
	} else {
		diagExtra = append(diagExtra, tc)
	}
	fmt.Fprint(logw, hle.SyscallSummary())
	fmt.Fprintf(logw, "             Unresolved imports bound to the abort stub: %d calls, %d data (GLOB_DAT/R_64)\n",
		em.UnresolvedFuncImports, em.UnresolvedDataImports)
	if len(em.UnresolvedDataNames) > 0 {
		fmt.Fprintf(logw, "             Unresolved data imports: %s\n", strings.Join(em.UnresolvedDataNames, ", "))
	}
	for _, w := range em.Warnings {
		fmt.Fprintf(logw, "             Warning: %s\n", w)
	}
	fmt.Fprintf(logw, "             Wrote %s\n", hlePath)

	if cfg.AppDir != "" {
		srcAssets := filepath.Join(cfg.AppDir, "assets")
		dstAssets := filepath.Join(cfg.OutDir, "assets")
		if fi, err := os.Stat(srcAssets); err == nil && fi.IsDir() {
			_ = os.RemoveAll(dstAssets)
			_ = copyDir(srcAssets, dstAssets)
		}
	}

	cFiles = append(cFiles, runtimeCFiles...)
	cFiles = append(cFiles, diagExtra...)
	tso := "on (acquire/release for non-stack guest memory)"
	if !cfg.TSO {
		tso = "off"
	} else if cfg.TSOElide != "off" {
		tso = "on (acquire/release except proven thread-private stack slots)"
	}
	nAsm, nObj := 0, 0
	for _, f := range cFiles {
		switch filepath.Ext(f) {
		case ".s":
			nAsm++
		case ".o":
			nObj++
		}
	}
	fmt.Fprintf(logw, "             Emitted %d source files (%d assembly, %d assembled objects, %d C) | TSO: %s | Time: %v\n",
		len(cFiles), nAsm, nObj, len(cFiles)-nAsm-nObj, tso, time.Since(stepStart).Round(time.Millisecond))

	if ctx.Err() != nil {
		return errBuildCancelled
	}

	// Step 4: Compile with Clang
	targetBin := filepath.Join(cfg.OutDir, "ps4_app")
	appBundle := ""
	if cfg.AsmOnly && !cfg.Compile {
		fmt.Fprintln(logw, "[ps4-recomp] [4/4] Skipped (-asm-only): the code chunks are assembled in the output directory.")
		if err := generateNinjaBuild(cFiles, cfg.OutDir, runtimeIncludeDirs, targetBin, cfg.OptLevel, cfg.Asan, cfg.AddrMode == "identity"); err != nil {
			fmt.Fprintf(logw, "             Warning: failed to generate build.ninja: %v\n", err)
		} else {
			writeStamp(cfg)
		}
	} else if cfg.Compile {
		stepStart = time.Now()
		ev.Progress("compile", 0, int64(len(cFiles)), "files", "compiling")
		fmt.Fprintf(logw, "[ps4-recomp] [4/4] Compiling native ARM64 binary with clang (-O%s, %d workers)...\n",
			cfg.OptLevel, cfg.Jobs)
		if err := compileParallel(ctx, cFiles, cfg.OutDir, runtimeIncludeDirs, targetBin, cfg.Jobs, cfg.OptLevel, cfg.Asan, cfg.AddrMode == "identity", func(done, total int64) {
			ev.Progress("compile", done, total, "files", "compiling")
		}); err != nil {
			return fmt.Errorf("compilation failed: %w", err)
		}
		fmt.Fprintf(logw, "             Compiled binary: %s | Time: %v\n",
			targetBin, time.Since(stepStart).Round(time.Millisecond))

		appName := strings.TrimSuffix(filepath.Base(cfg.ElfPath), filepath.Ext(cfg.ElfPath))
		if appName == "" || appName == "." {
			appName = "ps4_app"
		}
		if runtime.GOOS == "darwin" {
			bundleDir, err := packageAppBundle(cfg, appName, targetBin)
			if err != nil {
				return fmt.Errorf("failed to package macOS app bundle: %w", err)
			}
			targetBin = filepath.Join(bundleDir, "Contents", "MacOS", appName)
			fmt.Fprintf(logw, "             Packaged macOS App Bundle: %s\n", bundleDir)
			appBundle = bundleDir
		}
	} else {
		fmt.Fprintln(logw, "[ps4-recomp] [4/4] Clang compilation skipped (--compile=false). Generating build.ninja...")
		if err := generateNinjaBuild(cFiles, cfg.OutDir, runtimeIncludeDirs, targetBin, cfg.OptLevel, cfg.Asan, cfg.AddrMode == "identity"); err != nil {
			fmt.Fprintf(logw, "             Warning: failed to generate build.ninja: %v\n", err)
		} else {
			fmt.Fprintf(logw, "             Generated %s\n", filepath.Join(cfg.OutDir, "build.ninja"))
		}
	}

	fmt.Fprintf(logw, "[ps4-recomp] All tasks completed successfully in %v\n", time.Since(totalStart).Round(time.Millisecond))

	if ctx.Err() != nil {
		return errBuildCancelled
	}
	return finishBuild(ctx, ev, cfg, targetBin, appBundle)
}

// finishBuild runs the optional run step and then reports the build as done, so that with
// --json the last event of the stream is "done" only if the whole command succeeded.
func finishBuild(ctx context.Context, ev *events.Writer, cfg *Config, targetBin, appBundle string) error {
	if cfg.Run && cfg.Compile {
		fmt.Fprintln(logw, "===================================================================")
		if err := runBinary(ctx, targetBin, cfg.TimeoutSec, cfg.AppDir); err != nil {
			return fmt.Errorf("execution failed: %w", cancelledOr(ctx, err))
		}
	}
	if appBundle != "" {
		ev.Done(buildResult{App: appBundle, TitleID: titleIDOf(cfg.AppDir), BuiltWith: Version})
	} else {
		ev.Done(buildResult{BuiltWith: Version})
	}
	return nil
}

func generateNinjaBuild(cFiles []string, outDir string, includeDirs []string, targetBin string, optLevel string, asan bool, identityAddr bool) error {
	ftCflags, ftLibs := findFreeType()
	ffCflags, ffLibs := findFFmpeg()

	var sb strings.Builder
	sb.WriteString("ninja_required_version = 1.5\n\n")

	var incFlags []string
	incFlags = append(incFlags, "-I.")
	for _, inc := range includeDirs {
		relInc, err := filepath.Rel(outDir, inc)
		if err == nil && !strings.HasPrefix(relInc, "..") {
			incFlags = append(incFlags, "-I"+relInc)
		} else {
			incFlags = append(incFlags, "-I"+inc)
		}
	}
	incFlags = append(incFlags, ftCflags...)
	incFlags = append(incFlags, ffCflags...)

	hasAsm := false
	for _, f := range cFiles {
		if ext := filepath.Ext(f); ext == ".s" || ext == ".o" {
			hasAsm = true
			break
		}
	}

	// -ffp-contract=off: keep clang from fusing a*b+c into an arm64 FMA in C runtime code
	// (x86 SSE/AVX without FMA never fuses, and runtime helpers emulate guest float math).
	cflags := []string{"-O" + optLevel, "-ffp-contract=off", "-Wno-parentheses-equality"}
	if asan {
		cflags = append(cflags, "-fsanitize=address,undefined", "-fno-omit-frame-pointer")
	}
	if runtime.GOOS == "darwin" && runtime.GOARCH == "arm64" {
		darwinFlags := []string{"-target", "arm64-apple-darwin", "-mcpu=apple-m1"}
		if hasAsm {
			darwinFlags = append(darwinFlags, "-DRECOMP_ARM64_ASM=1")
		}
		cflags = append(darwinFlags, cflags...)
	}
	// The runtime's guest<->host pointer translation is a compile-time no-op in identity mode
	// (nps5_mem.h); the mode is a property of the translated title, so it is a build flag.
	if identityAddr {
		cflags = append(cflags, "-DNPS5_ADDR_MODE_IDENTITY=1")
	}
	if checkedPtr() {
		cflags = append(cflags, "-DNPS5_CHECKED_PTR=1")
	}
	cflags = append(cflags, gpuCFlags()...)

	fmt.Fprintf(&sb, "cflags = %s\n\n", ninjaShellArgs(append(cflags, incFlags...)))

	sb.WriteString("rule compile_c\n")
	sb.WriteString("  command = clang -MD -MF $out.d $cflags -c $in -o $out\n")
	sb.WriteString("  depfile = $out.d\n")
	sb.WriteString("  deps = gcc\n")
	sb.WriteString("  description = CC $in\n\n")

	sb.WriteString("rule compile_s\n")
	sb.WriteString("  command = clang -MD -MF $out.d $cflags -c $in -o $out\n")
	sb.WriteString("  depfile = $out.d\n")
	sb.WriteString("  deps = gcc\n")
	sb.WriteString("  description = AS $in\n\n")

	sb.WriteString("rule compile_m\n")
	sb.WriteString("  command = clang -MD -MF $out.d $cflags -fobjc-arc -c $in -o $out\n")
	sb.WriteString("  depfile = $out.d\n")
	sb.WriteString("  deps = gcc\n")
	sb.WriteString("  description = OBJC $in\n\n")

	sb.WriteString("rule compile_mm\n")
	sb.WriteString("  command = clang++ -MD -MF $out.d $cflags -fobjc-arc -std=c++17 -c $in -o $out\n")
	sb.WriteString("  depfile = $out.d\n")
	sb.WriteString("  deps = gcc\n")
	sb.WriteString("  description = OBJCXX $in\n\n")

	sb.WriteString("rule compile_cpp\n")
	sb.WriteString("  command = clang++ -MD -MF $out.d $cflags -std=c++17 -c $in -o $out\n")
	sb.WriteString("  depfile = $out.d\n")
	sb.WriteString("  deps = gcc\n")
	sb.WriteString("  description = CXX $in\n\n")

	var ldflags []string
	if asan {
		ldflags = append(ldflags, "-fsanitize=address,undefined")
	}
	if runtime.GOOS == "darwin" {
		ldflags = append(ldflags, "-target", "arm64-apple-darwin", "-Wl,-dead_strip", "-Wl,-export_dynamic", "-Wl,-x",
			"-framework", "Metal", "-framework", "Cocoa", "-framework", "QuartzCore", "-framework", "GameController", "-framework", "AudioToolbox", "-framework", "ImageIO", "-framework", "MetalFX", "-framework", "VideoToolbox", "-framework", "CoreMedia", "-framework", "CoreVideo", "-lz")
	} else {
		ldflags = append(ldflags, "-Wl,--gc-sections", "-Wl,-s", "-lpthread", "-lm")
	}
	var staticLibs []string // the link's implicit inputs: a rebuilt library relinks the game
	if _, gcnLib := findGcnRecompiler(); gcnLib != "" {
		ldflags = append(ldflags, gcnLib)
		staticLibs = append(staticLibs, gcnLib)
	}
	_, spirvLib := findSpirvCross()
	if spirvLib != "" {
		ldflags = append(ldflags, spirvLib, "-lc++")
		staticLibs = append(staticLibs, spirvLib)
	}
	_, atracLib, _ := findLibAtrac9()
	if atracLib != "" {
		ldflags = append(ldflags, atracLib)
		staticLibs = append(staticLibs, atracLib)
	}
	ldflags = append(ldflags, ftLibs...)
	ldflags = append(ldflags, ffLibs...)
	if gl, err := gpuLDFlags(); err != nil {
		return err
	} else {
		ldflags = append(ldflags, gl...)
	}

	fmt.Fprintf(&sb, "ldflags = %s\n\n", ninjaShellArgs(ldflags))
	sb.WriteString("rule link_bin\n")
	sb.WriteString("  command = clang $in $ldflags -o $out\n")
	sb.WriteString("  description = LINK $out\n\n")

	var objFiles []string
	for _, cFile := range cFiles {
		relCFile, err := filepath.Rel(outDir, cFile)
		if err != nil {
			relCFile = cFile
		}
		ext := filepath.Ext(relCFile)
		objFile := strings.TrimSuffix(relCFile, ext) + ".o"
		objFiles = append(objFiles, objFile)
		if ext == ".o" {
			continue // already assembled by the streaming pipeline
		}
		rule := "compile_c"
		if ext == ".s" {
			rule = "compile_s"
		} else if ext == ".m" {
			rule = "compile_m"
		} else if ext == ".mm" {
			rule = "compile_mm"
		} else if ext == ".cpp" {
			rule = "compile_cpp"
		}
		fmt.Fprintf(&sb, "build %s: %s %s\n", ninjaEscape(objFile), rule, ninjaEscape(relCFile))
	}

	relTargetBin, err := filepath.Rel(outDir, targetBin)
	if err != nil {
		relTargetBin = filepath.Base(targetBin)
	}

	sb.WriteString(ninjaLinkBuild(relTargetBin, objFiles, staticLibs))
	fmt.Fprintf(&sb, "default %s\n", ninjaEscape(relTargetBin))

	ninjaPath := filepath.Join(outDir, "build.ninja")
	existing, err := os.ReadFile(ninjaPath)
	content := []byte(sb.String())
	if err == nil && bytes.Equal(existing, content) {
		return nil
	}
	return os.WriteFile(ninjaPath, content, 0o644)
}

// buildResult is the result object of the final "done" event of build --json.
type buildResult struct {
	App       string `json:"app,omitempty"`
	TitleID   string `json:"title_id,omitempty"`
	BuiltWith string `json:"built_with"`
}

// ninjaStatus matches the "[done/total]" prefix of a ninja status line.
var ninjaStatus = regexp.MustCompile(`^\[(\d+)/(\d+)\]`)

// runNinja runs ninja in outDir, forwarding its output to the build log and reporting each
// "[done/total]" status line to progress. When ctx ends, ninja's process group is stopped
// (groupCommand) and errBuildCancelled is returned.
func runNinja(ctx context.Context, ninjaPath, outDir string, numWorkers int, progress func(done, total int64)) error {
	cmd := groupCommand(ctx, ninjaPath, "-C", outDir, fmt.Sprintf("-j%d", numWorkers))
	cmd.Stderr = os.Stderr
	pipe, err := cmd.StdoutPipe()
	if err != nil {
		return err
	}
	if err := cmd.Start(); err != nil {
		return err
	}
	sc := bufio.NewScanner(pipe)
	for sc.Scan() {
		line := sc.Bytes()
		fmt.Fprintf(logw, "%s\n", line)
		if m := ninjaStatus.FindSubmatch(line); m != nil && progress != nil {
			done, _ := strconv.ParseInt(string(m[1]), 10, 64)
			total, _ := strconv.ParseInt(string(m[2]), 10, 64)
			progress(done, total)
		}
	}
	io.Copy(io.Discard, pipe) // drain, so a very long line cannot block ninja
	return cancelledOr(ctx, cmd.Wait())
}

// shellQuote quotes arg for /bin/sh, which runs ninja's commands: an argument with any
// character outside a safe set is put in single quotes, an embedded quote being closed, escaped
// with a backslash and reopened. Paths such as
// ".../PS4Recomp.app/Contents/Resources/3rdparty/..." may contain a space.
func shellQuote(arg string) string {
	if arg != "" && strings.IndexFunc(arg, func(r rune) bool {
		return !(r >= 'a' && r <= 'z' || r >= 'A' && r <= 'Z' || r >= '0' && r <= '9' || strings.ContainsRune("-_./=,+:@%", r))
	}) < 0 {
		return arg
	}
	return "'" + strings.ReplaceAll(arg, "'", `'\''`) + "'"
}

// ninjaShellArgs is the value of a ninja variable expanded into a command line: each argument
// shell-quoted, then ninja's "$" escaped.
func ninjaShellArgs(args []string) string {
	var b strings.Builder
	for i, a := range args {
		if i > 0 {
			b.WriteByte(' ')
		}
		b.WriteString(strings.ReplaceAll(shellQuote(a), "$", "$$"))
	}
	return b.String()
}

// ninjaEscape escapes a path in a ninja build line.
func ninjaEscape(p string) string {
	return strings.NewReplacer("$", "$$", " ", "$ ", ":", "$:").Replace(p)
}

// ninjaLinkBuild is the build line of the game binary: the objects are its inputs, the static
// libraries it links (the shader translator, SPIRV-Cross, LibAtrac9) implicit inputs, so a
// library copied in after a rebuild relinks the game.
func ninjaLinkBuild(target string, objs, libs []string) string {
	var b strings.Builder
	b.WriteString("\nbuild ")
	b.WriteString(ninjaEscape(target))
	b.WriteString(": link_bin")
	for _, o := range objs {
		b.WriteByte(' ')
		b.WriteString(ninjaEscape(o))
	}
	if len(libs) > 0 {
		b.WriteString(" |")
		for _, l := range libs {
			b.WriteByte(' ')
			b.WriteString(ninjaEscape(l))
		}
	}
	b.WriteByte('\n')
	return b.String()
}

// compileParallel compiles cFiles and links targetBin. progress, when non-nil, is called with
// the number of files compiled so far and the total, as files complete. When ctx ends, the
// running ninja or clang process groups are stopped and errBuildCancelled is returned.
func compileParallel(ctx context.Context, cFiles []string, outDir string, includeDirs []string, targetBin string, numWorkers int, optLevel string, asan bool, identityAddr bool, progress func(done, total int64)) error {
	ftCflags, ftLibs := findFreeType()
	if len(ftCflags)+len(ftLibs) == 0 {
		fmt.Fprintln(logw, "             FreeType: not found")
	} else {
		fmt.Fprintf(logw, "             FreeType: %s\n", strings.Join(append(slices.Clone(ftCflags), ftLibs...), " "))
	}
	ffCflags, ffLibs := findFFmpeg()
	if len(ffCflags)+len(ffLibs) == 0 {
		fmt.Fprintln(logw, "             FFmpeg: not found")
	} else {
		fmt.Fprintf(logw, "             FFmpeg: %s\n", strings.Join(append(slices.Clone(ffCflags), ffLibs...), " "))
	}
	// Try building with Ninja first if available
	if ninjaPath := findNinja(); ninjaPath != "" {
		fmt.Fprintf(logw, "             ninja: %s\n", ninjaPath)
		if err := generateNinjaBuild(cFiles, outDir, includeDirs, targetBin, optLevel, asan, identityAddr); err != nil {
			fmt.Fprintf(logw, "             ninja: cannot write build.ninja (%v); compiling with clang directly\n", err)
		} else {
			err := runNinja(ctx, ninjaPath, outDir, numWorkers, progress)
			if err == nil || errors.Is(err, errBuildCancelled) {
				return err
			}
			fmt.Fprintf(logw, "             ninja: build failed (%v); compiling with clang directly\n", err)
		}
	} else {
		fmt.Fprintln(logw, "             ninja: not found; compiling with clang directly")
	}
	total := int64(len(cFiles))
	var finished atomic.Int64
	fileDone := func() {
		if progress != nil {
			progress(finished.Add(1), total)
		}
	}

	// Build a flags fingerprint: any flag change (opt level, asan, includes)
	// must invalidate all cached object files.
	var flagsKey strings.Builder
	flagsKey.WriteString("-O")
	flagsKey.WriteString(optLevel)
	if asan {
		flagsKey.WriteString(" -fsanitize=address,undefined")
	}
	for _, inc := range includeDirs {
		flagsKey.WriteByte(' ')
		flagsKey.WriteString(inc)
	}
	for _, f := range ftCflags {
		flagsKey.WriteByte(' ')
		flagsKey.WriteString(f)
	}
	// The addressing mode changes what the runtime's translation macros expand to.
	if identityAddr {
		flagsKey.WriteString(" -DNPS5_ADDR_MODE_IDENTITY=1")
	}
	if checkedPtr() {
		flagsKey.WriteString(" -DNPS5_CHECKED_PTR=1")
	}
	flagsKey.WriteString(" " + gpuKey())
	flagsFingerprint := flagsKey.String()

	objFiles := make([]string, len(cFiles))
	errChan := make(chan error, len(cFiles))
	jobs := make(chan int, len(cFiles))

	var wg sync.WaitGroup
	for range numWorkers {
		wg.Go(func() {
			for idx := range jobs {
				if ctx.Err() != nil {
					return
				}
				cFile := cFiles[idx]
				ext := filepath.Ext(cFile)
				objFile := strings.TrimSuffix(cFile, ext) + ".o"
				flagsFile := objFile + ".flags"
				objFiles[idx] = objFile
				if ext == ".o" { // already assembled by the streaming pipeline
					fileDone()
					continue
				}

				// Check if object is up-to-date:
				// 1. .o must exist and be newer than .c
				// 2. .flags sidecar must match current compiler flags
				upToDate := false
				cStat, errC := os.Stat(cFile)
				oStat, errO := os.Stat(objFile)
				if errC == nil && errO == nil && oStat.ModTime().After(cStat.ModTime()) {
					if prevFlags, err := os.ReadFile(flagsFile); err == nil {
						upToDate = string(prevFlags) == flagsFingerprint
					}
				}
				if upToDate {
					fileDone()
					continue
				}

				clangArgs := []string{
					"-O" + optLevel,
					"-ffp-contract=off",
					"-Wno-parentheses-equality",
					"-I" + outDir,
				}
				for _, inc := range includeDirs {
					clangArgs = append(clangArgs, "-I"+inc)
				}
				clangArgs = append(clangArgs, ftCflags...)
				clangArgs = append(clangArgs, ffCflags...)
				if identityAddr {
					clangArgs = append(clangArgs, "-DNPS5_ADDR_MODE_IDENTITY=1")
				}
				if checkedPtr() {
					clangArgs = append(clangArgs, "-DNPS5_CHECKED_PTR=1")
				}
				clangArgs = append(clangArgs, gpuCFlags()...)
				if asan {
					clangArgs = append(clangArgs, "-fsanitize=address,undefined", "-fno-omit-frame-pointer")
				}
				compilerBin := "clang"
				if ext == ".m" {
					clangArgs = append(clangArgs, "-fobjc-arc")
				} else if ext == ".mm" {
					clangArgs = append(clangArgs, "-fobjc-arc", "-std=c++17")
					compilerBin = "clang++"
				} else if ext == ".cpp" {
					clangArgs = append(clangArgs, "-std=c++17")
					compilerBin = "clang++"
				}
				clangArgs = append(clangArgs, "-c", cFile, "-o", objFile)
				if runtime.GOOS == "darwin" && runtime.GOARCH == "arm64" {
					clangArgs = append([]string{"-target", "arm64-apple-darwin", "-mcpu=apple-m1", "-DRECOMP_ARM64_ASM=1"}, clangArgs...)
				}

				cmd := groupCommand(ctx, compilerBin, clangArgs...)
				if out, err := cmd.CombinedOutput(); err != nil {
					errChan <- cancelledOr(ctx, fmt.Errorf("error compiling %s: %w\n%s", filepath.Base(cFile), err, string(out)))
					return
				}

				// Write flags sidecar so the next run can detect flag changes.
				_ = os.WriteFile(flagsFile, []byte(flagsFingerprint), 0o644)
				fileDone()
			}
		})
	}

	for i := range cFiles {
		jobs <- i
	}
	close(jobs)

	wg.Wait()
	close(errChan)

	if ctx.Err() != nil {
		return errBuildCancelled
	}
	for err := range errChan {
		if err != nil {
			return err
		}
	}

	linkArgs := []string{"-o", targetBin}
	if asan {
		linkArgs = append(linkArgs, "-fsanitize=address,undefined")
	}
	if runtime.GOOS == "darwin" {
		linkArgs = append(linkArgs, "-target", "arm64-apple-darwin", "-Wl,-dead_strip", "-Wl,-export_dynamic", "-Wl,-x",
			"-framework", "Metal", "-framework", "Cocoa", "-framework", "QuartzCore", "-framework", "GameController", "-framework", "AudioToolbox", "-framework", "ImageIO", "-framework", "MetalFX", "-framework", "VideoToolbox", "-framework", "CoreMedia", "-framework", "CoreVideo", "-lz")
	} else {
		linkArgs = append(linkArgs, "-Wl,--gc-sections", "-Wl,-s", "-lpthread", "-lm")
	}
	if _, gcnLib := findGcnRecompiler(); gcnLib != "" {
		linkArgs = append(linkArgs, gcnLib)
	}
	_, spirvLib := findSpirvCross()
	if spirvLib != "" {
		linkArgs = append(linkArgs, spirvLib, "-lc++")
	}
	_, atracLib, _ := findLibAtrac9()
	if atracLib != "" {
		linkArgs = append(linkArgs, atracLib)
	}
	linkArgs = append(linkArgs, ftLibs...)
	linkArgs = append(linkArgs, ffLibs...)
	linkArgs = append(linkArgs, objFiles...)
	if gl, err := gpuLDFlags(); err != nil {
		return err
	} else {
		linkArgs = append(linkArgs, gl...)
	}

	cmd := groupCommand(ctx, "clang", linkArgs...)
	if out, err := cmd.CombinedOutput(); err != nil {
		return cancelledOr(ctx, fmt.Errorf("link error: %w\n%s", err, string(out)))
	}
	return nil
}

// runBinary runs the built game for --run; ending ctx kills it.
func runBinary(ctx context.Context, targetBin string, timeoutSec int, appDir string) error {
	var cancel context.CancelFunc
	if timeoutSec > 0 {
		ctx, cancel = context.WithTimeout(ctx, time.Duration(timeoutSec)*time.Second)
		defer cancel()
	}

	cmd := exec.CommandContext(ctx, targetBin)
	cmd.Stdout = logw // stderr in JSON mode: the game must not write into the event stream
	cmd.Stderr = os.Stderr
	cmd.Env = os.Environ()
	if appDir != "" {
		cmd.Env = append(cmd.Env, "PS4_APP_DIR="+appDir)
	}

	if timeoutSec > 0 {
		fmt.Fprintf(logw, "[ps4-recomp] Running %s (watchdog timeout: %ds)...\n", targetBin, timeoutSec)
	} else {
		fmt.Fprintf(logw, "[ps4-recomp] Running %s...\n", targetBin)
	}

	err := cmd.Run()
	if ctx.Err() == context.DeadlineExceeded {
		fmt.Fprintf(logw, "[ps4-recomp] Watchdog timeout of %ds reached. Terminated cleanly.\n", timeoutSec)
		return nil
	}
	return err
}

// excludeRefs drops the modules named by -exclude-module. Only their export NIDs are read (the
// image is parsed and discarded, never biased, merged, discovered or lifted) so imports of
// them can be stubbed by name.
func excludeRefs(cfg *Config, main *elfloader.LoadedELF, refs []elfloader.ModuleRef) []elfloader.ModuleRef {
	if len(cfg.ExcludeModules) == 0 {
		return refs
	}
	var keep []elfloader.ModuleRef
	for _, ref := range refs {
		if !elfloader.ExcludedModuleMatch(ref.Path, cfg.ExcludeModules) {
			keep = append(keep, ref)
			continue
		}
		mod, err := elfloader.LoadELF(ref.Path)
		if err != nil {
			fmt.Fprintf(logw, "             Excluded module %s: cannot read exports: %v\n", filepath.Base(ref.Path), err)
			continue
		}
		main.MarkExcludedExports(filepath.Base(ref.Path), mod.ExportedFunctions())
		fmt.Fprintf(logw, "             Excluded module %s (%d exports)\n", filepath.Base(ref.Path), len(mod.ExportedFunctions()))
	}
	return keep
}

func loadCompanionModules(cfg *Config, main *elfloader.LoadedELF, layout guestmem.Layout) ([]emitter.GuestModule, error) {
	refs := excludeRefs(cfg, main, elfloader.DiscoverCompanionModules(cfg.ElfPath, cfg.AppDir, main.MemoryImage))
	referenced := elfloader.ReferencedModuleNames(main.MemoryImage)
	found := make(map[string]struct{})
	for _, ref := range refs {
		found[strings.ToLower(filepath.Base(ref.Path))] = struct{}{}
		for _, alias := range ref.Aliases {
			found[strings.ToLower(filepath.Base(alias))] = struct{}{}
		}
	}
	for _, name := range referenced {
		cleanName := strings.TrimPrefix(strings.ReplaceAll(name, "\\", "/"), "%s/")
		if _, ok := found[strings.ToLower(filepath.Base(name))]; ok {
			continue
		}
		if _, ok := found[strings.ToLower(filepath.Base(cleanName))]; ok {
			continue
		}
		fmt.Fprintf(logw, "             Referenced module %s was not found for AOT linking\n", name)
	}
	if len(refs) == 0 {
		return nil, nil
	}

	nextBase := (main.MaxVAddr + 0xFFFF) &^ 0xFFFF
	if nextBase < 0x10000 {
		nextBase = 0x10000
	}
	var modules []emitter.GuestModule
	for _, ref := range refs {
		mod, err := elfloader.LoadELF(ref.Path)
		if err != nil {
			fmt.Fprintf(logw, "             Skipping module %s: %v\n", filepath.Base(ref.Path), err)
			continue
		}
		// A module linked below the layout's first valid address (relocatable PRXs are linked at
		// 0) must be placed even when it does not overlap the executable: nothing may live in
		// the null guard, and in identity mode nothing may live outside the arena.
		if elfloader.Overlaps(main, mod) || mod.MinVAddr < layout.GuestLo {
			if err := mod.ApplyBias(nextBase - mod.MinVAddr&^0xFFFF); err != nil {
				return nil, fmt.Errorf("failed to relocate module %s: %w", filepath.Base(ref.Path), err)
			}
		}
		for _, p := range elfloader.AddTLSModule(main, mod) {
			fmt.Fprintf(os.Stderr, "             Warning: %s\n", p)
		}
		if err := elfloader.MergeImages(main, mod); err != nil {
			return nil, fmt.Errorf("failed to merge module %s: %w", filepath.Base(ref.Path), err)
		}
		nextBase = (main.MaxVAddr + 0xFFFF) &^ 0xFFFF
		exports := mod.ExportedFunctions()
		modules = append(modules, emitter.GuestModule{
			FileName:       mod.FileName,
			Aliases:        ref.Aliases,
			Exports:        exports,
			Init:           mod.InitRunList(),
			StartAddr:      mod.MinVAddr,
			EndAddr:        mod.MaxVAddr,
			Seg0Addr:       mod.Seg0Addr,
			Seg0Size:       mod.Seg0Size,
			EHFrameHdrAddr: mod.EHFrameHdrAddr,
			EHFrameHdrSize: mod.EHFrameHdrSize,
			EHFrameAddr:    mod.EHFrameAddr,
			EHFrameSize:    mod.EHFrameSize,
			Segments:       mod.Segments,
		})
		fmt.Fprintf(logw, "             Linked module %s (%d exports, load 0x%x-0x%x)\n",
			mod.FileName, len(exports), mod.MinVAddr, mod.MaxVAddr)
	}
	return modules, nil
}

func findRuntimeDir() (string, error) {
	candidates := []string{"pkg/runtime"}

	if exe, err := os.Executable(); err == nil {
		exeDir := filepath.Dir(exe)
		candidates = append(
			candidates,
			filepath.Join(exeDir, "pkg", "runtime"),
			filepath.Join(exeDir, "..", "pkg", "runtime"),
		)
	}

	if wd, err := os.Getwd(); err == nil {
		dir := wd
		for range 5 {
			candidates = append(candidates, filepath.Join(dir, "pkg", "runtime"))
			parent := filepath.Dir(dir)
			if parent == dir {
				break
			}
			dir = parent
		}
	}

	for _, c := range candidates {
		if fi, err := os.Stat(c); err == nil && fi.IsDir() {
			if _, err := os.Stat(filepath.Join(c, "core", "recomp_runtime.h")); err == nil {
				return c, nil
			}
			if _, err := os.Stat(filepath.Join(c, "recomp_runtime.h")); err == nil {
				return c, nil
			}
		}
	}

	return "", fmt.Errorf("unable to locate runtime directory (checked %v)", candidates)
}

func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer func() { _ = in.Close() }()

	srcInfo, err := in.Stat()
	if err != nil {
		return err
	}

	// Skip copy when destination already exists and is up-to-date.
	// This is critical for Ninja incremental builds: if we truncate+rewrite
	// headers even with identical content, Ninja sees a newer mtime on every
	// header file and recompiles all generated C chunks unconditionally.
	if dstInfo, err := os.Stat(dst); err == nil {
		if dstInfo.Size() == srcInfo.Size() && !srcInfo.ModTime().After(dstInfo.ModTime()) {
			return nil
		}
	}

	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return err
	}

	mode := srcInfo.Mode()
	if mode&0o111 != 0 {
		mode = 0o755
	} else {
		mode = 0o644
	}

	out, err := os.OpenFile(dst, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, mode)
	if err != nil {
		return err
	}

	if _, err := io.Copy(out, in); err != nil {
		_ = out.Close()
		return err
	}
	if err := out.Close(); err != nil {
		return err
	}
	_ = os.Chmod(dst, mode)
	return nil
}

// isRuntimeSource reports whether a file of the copied runtime is compiled. AppleDouble
// sidecars ("._<name>", which macOS writes next to each file with extended attributes on a
// volume that has none, such as exFAT) carry a source extension but are not sources.
func isRuntimeSource(name string) bool {
	if strings.HasPrefix(name, "._") {
		return false
	}
	switch filepath.Ext(name) {
	case ".c", ".m", ".mm", ".cpp":
		return true
	}
	return false
}

func copyDir(src, dst string) error {
	return filepath.Walk(src, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(src, path)
		if err != nil {
			return err
		}
		target := filepath.Join(dst, rel)
		if info.IsDir() {
			return os.MkdirAll(target, info.Mode())
		}
		return copyFile(path, target)
	})
}

func copyDirFiltered(src, dst string) error {
	return filepath.Walk(src, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		name := info.Name()
		if strings.HasPrefix(name, ".") {
			if info.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		ext := strings.ToLower(filepath.Ext(name))
		if ext == ".prx" || ext == ".elf" || strings.EqualFold(name, "eboot.bin") {
			if info.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		rel, err := filepath.Rel(src, path)
		if err != nil {
			return err
		}
		target := filepath.Join(dst, rel)
		if info.IsDir() {
			return os.MkdirAll(target, 0o755)
		}
		return copyFile(path, target)
	})
}

func packageAppBundle(cfg *Config, appName, compiledBin string) (string, error) {
	if runtime.GOOS != "darwin" {
		return compiledBin, nil
	}

	bundleDir := filepath.Join(cfg.OutDir, appName+".app")
	contentsDir := filepath.Join(bundleDir, "Contents")
	macosDir := filepath.Join(contentsDir, "MacOS")
	sharedSupportDir := filepath.Join(contentsDir, "SharedSupport")
	resourcesDir := filepath.Join(contentsDir, "Resources")

	if err := os.MkdirAll(macosDir, 0o755); err != nil {
		return "", err
	}
	if err := os.MkdirAll(sharedSupportDir, 0o755); err != nil {
		return "", err
	}
	if err := os.MkdirAll(resourcesDir, 0o755); err != nil {
		return "", err
	}

	// 1. Bundle external dynamic libraries into Contents/Frameworks/ and patch compiledBin BEFORE copying
	if err := bundleDylibDependencies(contentsDir, compiledBin); err != nil {
		fmt.Fprintf(logw, "             Warning: dylib bundling had issues: %v\n", err)
	}

	// 2. Move/copy the cleanly signed binary into Contents/MacOS/<appName>
	bundleBin := filepath.Join(macosDir, appName)
	if err := copyFile(compiledBin, bundleBin); err != nil {
		return "", fmt.Errorf("failed to copy binary to app bundle: %w", err)
	}
	_ = os.Chmod(bundleBin, 0o755)

	// Keep cfg.OutDir/ps4_app updated
	compatBin := filepath.Join(cfg.OutDir, "ps4_app")
	if compatBin != compiledBin {
		_ = os.Remove(compatBin)
		_ = copyFile(compiledBin, compatBin)
		_ = os.Chmod(compatBin, 0o755)
	}

	// Mirror Frameworks to OutDir/Frameworks so ps4_app can run standalone outside the .app bundle
	outFrameworks := filepath.Join(cfg.OutDir, "Frameworks")
	frameworksSrc := filepath.Join(contentsDir, "Frameworks")
	if fi, err := os.Stat(frameworksSrc); err == nil && fi.IsDir() {
		_ = os.MkdirAll(outFrameworks, 0o755)
		_ = copyDir(frameworksSrc, outFrameworks)
	}

	// 3. Copy guest_image.bin into Contents/SharedSupport/
	guestImgSrc := filepath.Join(cfg.OutDir, "guest_image.bin")
	guestImgDst := filepath.Join(sharedSupportDir, "guest_image.bin")
	if fi, err := os.Stat(guestImgSrc); err == nil && !fi.IsDir() {
		_ = copyFile(guestImgSrc, guestImgDst)
	}

	// 4. Copy resources into Contents/SharedSupport/ (unsealed by codesign so signing takes <0.5s)
	if cfg.AppDir != "" {
		if cfg.CopyResources {
			entries, err := os.ReadDir(cfg.AppDir)
			if err == nil {
				absOut, _ := filepath.Abs(cfg.OutDir)
				absElf, _ := filepath.Abs(cfg.ElfPath)
				for _, entry := range entries {
					name := entry.Name()
					if strings.HasPrefix(name, ".") || strings.EqualFold(name, "prx") {
						continue
					}
					srcPath := filepath.Join(cfg.AppDir, name)
					absSrc, _ := filepath.Abs(srcPath)
					if absSrc == absOut || absSrc == absElf {
						continue
					}
					ext := strings.ToLower(filepath.Ext(name))
					if ext == ".prx" || ext == ".elf" || strings.EqualFold(name, "eboot.bin") {
						continue
					}
					dstPath := filepath.Join(sharedSupportDir, name)
					if entry.IsDir() {
						_ = os.RemoveAll(dstPath)
						if err := copyDirFiltered(srcPath, dstPath); err != nil {
							return "", fmt.Errorf("failed to copy %s into app bundle: %w", name, err)
						}
					} else {
						if err := copyFile(srcPath, dstPath); err != nil {
							return "", fmt.Errorf("failed to copy %s into app bundle: %w", name, err)
						}
					}
				}
			}
		} else {
			// Default selective copy of assets/ and sce_sys/ if present
			srcAssets := filepath.Join(cfg.AppDir, "assets")
			dstAssets := filepath.Join(sharedSupportDir, "assets")
			if fi, err := os.Stat(srcAssets); err == nil && fi.IsDir() {
				_ = os.RemoveAll(dstAssets)
				if err := copyDir(srcAssets, dstAssets); err != nil {
					return "", fmt.Errorf("failed to copy assets into app bundle: %w", err)
				}
			}

			srcSceSys := filepath.Join(cfg.AppDir, "sce_sys")
			dstSceSys := filepath.Join(sharedSupportDir, "sce_sys")
			if fi, err := os.Stat(srcSceSys); err == nil && fi.IsDir() {
				_ = os.RemoveAll(dstSceSys)
				if err := copyDir(srcSceSys, dstSceSys); err != nil {
					return "", fmt.Errorf("failed to copy sce_sys into app bundle: %w", err)
				}
			}
		}
	}

	// 5. Generate AppIcon.icns from icon0.png if present in Resources/ (so Finder displays icon)
	hasIcon := false
	if cfg.AppDir != "" {
		iconPath := filepath.Join(cfg.AppDir, "sce_sys", "icon0.png")
		if _, err := os.Stat(iconPath); err == nil {
			icnsDst := filepath.Join(resourcesDir, "AppIcon.icns")
			cmd := exec.Command("sips", "-s", "format", "icns", iconPath, "--out", icnsDst)
			if err := cmd.Run(); err == nil {
				hasIcon = true
			}
		}
	}

	// 6. Generate Contents/Info.plist
	if err := os.WriteFile(filepath.Join(contentsDir, "Info.plist"), []byte(appInfoPlist(appName, hasIcon, bundleMetaFor(cfg.AppDir, appName))), 0o644); err != nil {
		return "", fmt.Errorf("failed to write Info.plist: %w", err)
	}

	// 7. Sign app bundle ad-hoc. Because game data is in SharedSupport, this signs only Info.plist
	// and icon in Resources, completing in <0.2s without freezing on game data.
	_ = exec.Command("codesign", "--force", "--sign", "-", bundleDir).Run()

	return bundleDir, nil
}

// bundleMeta is the identity a game bundle presents to macOS.
type bundleMeta struct{ BundleID, DisplayName, Version string }

// readSFO loads <appDir>/sce_sys/param.sfo, or returns nil when there is none.
func readSFO(appDir string) *sfo.File {
	if appDir == "" {
		return nil
	}
	f, err := sfo.Load(filepath.Join(appDir, "sce_sys", "param.sfo"))
	if err != nil {
		return nil
	}
	return f
}

// titleIDOf returns TITLE_ID from the app dir's param.sfo, or "".
func titleIDOf(appDir string) string {
	if f := readSFO(appDir); f != nil {
		id, _ := f.String("TITLE_ID")
		return id
	}
	return ""
}

// bundleMetaFor derives the bundle identity from <appDir>/sce_sys/param.sfo: bundle id
// org.ps4recomp.<TITLE_ID>, TITLE as the display name and APP_VER as the version (real dumps
// often carry only VERSION, which is the fallback). Missing values keep the generic defaults.
func bundleMetaFor(appDir, appName string) bundleMeta {
	m := bundleMeta{BundleID: "org.ps4recomp." + appName, DisplayName: appName, Version: "1.0"}
	f := readSFO(appDir)
	if f == nil {
		return m
	}
	if id, ok := f.String("TITLE_ID"); ok {
		if id = sanitizeBundleID(id); id != "" {
			m.BundleID = "org.ps4recomp." + id
		}
	}
	if t, ok := f.String("TITLE"); ok && strings.TrimSpace(t) != "" {
		m.DisplayName = t
	}
	for _, key := range []string{"APP_VER", "VERSION"} {
		if v, ok := f.String(key); ok && strings.TrimSpace(v) != "" {
			m.Version = v
			break
		}
	}
	return m
}

// sanitizeBundleID keeps only the characters a bundle identifier allows.
func sanitizeBundleID(s string) string {
	return strings.Map(func(r rune) rune {
		if r >= 'a' && r <= 'z' || r >= 'A' && r <= 'Z' || r >= '0' && r <= '9' || r == '.' || r == '-' {
			return r
		}
		return -1
	}, s)
}

// xmlEscape escapes s for use as plist string content.
func xmlEscape(s string) string {
	var b strings.Builder
	if err := xml.EscapeText(&b, []byte(s)); err != nil {
		return ""
	}
	return b.String()
}

// appInfoPlist is the Contents/Info.plist of a generated game bundle. LSSupportsGameMode and the
// games category let macOS run it in Game Mode (CPU and GPU priority, lower Bluetooth latency for
// controllers) when it is full screen.
func appInfoPlist(appName string, hasIcon bool, meta bundleMeta) string {
	iconXML := ""
	if hasIcon {
		iconXML = "    <key>CFBundleIconFile</key>\n    <string>AppIcon</string>\n"
	}
	return fmt.Sprintf(`<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>en</string>
    <key>CFBundleExecutable</key>
    <string>%s</string>
    <key>CFBundleIdentifier</key>
    <string>%s</string>
    <key>CFBundleName</key>
    <string>%s</string>
    <key>CFBundleDisplayName</key>
    <string>%s</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>%s</string>
    <key>CFBundleVersion</key>
    <string>%s</string>
    <key>LSMinimumSystemVersion</key>
    <string>12.0</string>
    <key>LSApplicationCategoryType</key>
    <string>public.app-category.games</string>
    <key>LSSupportsGameMode</key>
    <true/>
    <key>NSHighResolutionCapable</key>
    <true/>
%s</dict>
</plist>
`, xmlEscape(appName), xmlEscape(meta.BundleID), xmlEscape(meta.DisplayName), xmlEscape(meta.DisplayName),
		xmlEscape(meta.Version), xmlEscape(meta.Version), iconXML)
}

const (
	machoLoadCmdReqDyld       macho.LoadCmd = 0x80000000
	machoLoadCmdLoadWeakDylib macho.LoadCmd = 0x18 | machoLoadCmdReqDyld
	machoLoadCmdReexportDylib macho.LoadCmd = 0x1f | machoLoadCmdReqDyld
	machoLoadCmdLazyLoadDylib macho.LoadCmd = 0x20
	machoLoadCmdLoadUpward    macho.LoadCmd = 0x23 | machoLoadCmdReqDyld
)

func openMachOFile(path string) (*macho.File, func(), error) {
	f, err := macho.Open(path)
	if err == nil {
		return f, func() { _ = f.Close() }, nil
	}
	fat, fatErr := macho.OpenFat(path)
	if fatErr != nil {
		return nil, nil, err
	}
	for _, arch := range fat.Arches {
		if arch.Cpu == macho.CpuArm64 {
			return arch.File, func() { _ = fat.Close() }, nil
		}
	}
	if len(fat.Arches) > 0 {
		return fat.Arches[0].File, func() { _ = fat.Close() }, nil
	}
	_ = fat.Close()
	return nil, nil, fmt.Errorf("empty fat binary: %s", path)
}

func isSystemDylib(dep string) bool {
	return dep == "" || strings.HasPrefix(dep, "/usr/lib/") || strings.HasPrefix(dep, "/System/")
}

func readMachODepsAndRpaths(path string) ([]string, []string, error) {
	f, closeFn, err := openMachOFile(path)
	if err != nil {
		return nil, nil, err
	}
	defer closeFn()

	var deps []string
	var rpaths []string
	for _, load := range f.Loads {
		switch l := load.(type) {
		case *macho.Dylib:
			if !isSystemDylib(l.Name) {
				deps = append(deps, l.Name)
			}
		case *macho.Rpath:
			if l.Path != "" {
				rpaths = append(rpaths, l.Path)
			}
		case macho.LoadBytes:
			if len(l) < 16 {
				continue
			}
			cmd := macho.LoadCmd(f.ByteOrder.Uint32(l[0:4]))
			if cmd != machoLoadCmdLoadWeakDylib && cmd != machoLoadCmdReexportDylib &&
				cmd != machoLoadCmdLazyLoadDylib && cmd != machoLoadCmdLoadUpward {
				continue
			}
			nameOff := f.ByteOrder.Uint32(l[8:12])
			if int(nameOff) >= len(l) {
				continue
			}
			raw := l[nameOff:]
			if end := bytes.IndexByte(raw, 0); end >= 0 {
				raw = raw[:end]
			}
			dep := string(raw)
			if !isSystemDylib(dep) {
				deps = append(deps, dep)
			}
		}
	}
	return deps, rpaths, nil
}

// getNonSystemDylibs returns all non-system dylibs that target directly depends on.
func getNonSystemDylibs(target string) ([]string, error) {
	deps, _, err := readMachODepsAndRpaths(target)
	return deps, err
}

type dylibQueueItem struct {
	bundlePath   string
	origDeclPath string
	origRealPath string
	parentRpaths []string
}

func evalSymlinksOrSelf(path string) string {
	if resolved, err := filepath.EvalSymlinks(path); err == nil {
		return resolved
	}
	return path
}

func appendUniqueDir(dst []string, dir string) []string {
	if dir == "" {
		return dst
	}
	clean := filepath.Clean(dir)
	if !slices.Contains(dst, clean) {
		return append(dst, clean)
	}
	return dst
}

func expandRpaths(rawRpaths []string, loaderRealDir, loaderDeclDir, exeDir string, inherited []string) []string {
	var out []string
	for _, rp := range rawRpaths {
		switch {
		case strings.HasPrefix(rp, "@loader_path"):
			suffix := strings.TrimPrefix(rp, "@loader_path")
			out = appendUniqueDir(out, loaderRealDir+suffix)
			if loaderDeclDir != "" && loaderDeclDir != loaderRealDir {
				out = appendUniqueDir(out, loaderDeclDir+suffix)
			}
		case strings.HasPrefix(rp, "@executable_path"):
			suffix := strings.TrimPrefix(rp, "@executable_path")
			out = appendUniqueDir(out, exeDir+suffix)
		case filepath.IsAbs(rp):
			out = appendUniqueDir(out, rp)
		}
	}
	for _, p := range inherited {
		out = appendUniqueDir(out, p)
	}
	return out
}

func checkCandidateFile(path string) (declPath, realPath string, ok bool) {
	if path == "" {
		return "", "", false
	}
	fi, err := os.Stat(path)
	if err != nil || fi.IsDir() {
		return "", "", false
	}
	return path, evalSymlinksOrSelf(path), true
}

func resolveDylibDep(dep string, item dylibQueueItem, rpaths []string, exeDir string) (declPath, realPath string, ok bool) {
	if filepath.IsAbs(dep) {
		return checkCandidateFile(dep)
	}
	if strings.HasPrefix(dep, "@loader_path/") {
		rel := strings.TrimPrefix(dep, "@loader_path/")
		if d, r, found := checkCandidateFile(filepath.Join(filepath.Dir(item.origRealPath), rel)); found {
			return d, r, true
		}
		if d, r, found := checkCandidateFile(filepath.Join(filepath.Dir(item.origDeclPath), rel)); found {
			return d, r, true
		}
		return "", "", false
	}
	if strings.HasPrefix(dep, "@executable_path/") {
		rel := strings.TrimPrefix(dep, "@executable_path/")
		return checkCandidateFile(filepath.Join(exeDir, rel))
	}
	if strings.HasPrefix(dep, "@rpath/") {
		rel := strings.TrimPrefix(dep, "@rpath/")
		for _, rpDir := range rpaths {
			if d, r, found := checkCandidateFile(filepath.Join(rpDir, rel)); found {
				return d, r, true
			}
		}
		// Fallback: check the referencing dylib's own directory and standard Homebrew prefixes.
		for _, dir := range []string{
			filepath.Dir(item.origRealPath),
			filepath.Dir(item.origDeclPath),
			"/opt/homebrew/lib",
			"/usr/local/lib",
		} {
			if d, r, found := checkCandidateFile(filepath.Join(dir, rel)); found {
				return d, r, true
			}
		}
		return "", "", false
	}
	return checkCandidateFile(dep)
}

// bundleDylibDependencies copies external dynamic libraries into Contents/Frameworks,
// rewrites their IDs and references to use @rpath, and adds @executable_path/../Frameworks
// to the binary rpath so the application runs without external dependencies (Homebrew).
func bundleDylibDependencies(contentsDir, bundleBin string) error {
	frameworksDir := filepath.Join(contentsDir, "Frameworks")
	if err := os.MkdirAll(frameworksDir, 0o755); err != nil {
		return err
	}

	// Add rpath search paths so dylibs resolve in all execution contexts:
	// - inside .app bundle: @executable_path/../Frameworks
	// - standalone CLI in build dir or portable dir: @executable_path/Frameworks
	// - standalone alongside dylibs: @executable_path
	// - standalone anywhere on a Mac with Homebrew: /opt/homebrew/lib, /usr/local/lib
	for _, rpath := range []string{
		"@executable_path/../Frameworks",
		"@executable_path/Frameworks",
		"@executable_path",
		"@loader_path/Frameworks",
		"@loader_path",
		"/opt/homebrew/lib",
		"/usr/local/lib",
		"/opt/homebrew/opt/freetype/lib",
		"/opt/homebrew/opt/ffmpeg/lib",
	} {
		_ = exec.Command("install_name_tool", "-add_rpath", rpath, bundleBin).Run()
	}

	exeDir := filepath.Dir(bundleBin)
	copiedByReal := make(map[string]string) // resolved source path -> dstPath
	copiedByBase := make(map[string]string) // framework basename -> dstPath
	queue := []dylibQueueItem{{
		bundlePath:   bundleBin,
		origDeclPath: bundleBin,
		origRealPath: evalSymlinksOrSelf(bundleBin),
	}}

	for len(queue) > 0 {
		current := queue[0]
		queue = queue[1:]

		deps, rawRpaths, err := readMachODepsAndRpaths(current.origRealPath)
		if err != nil {
			continue
		}
		allRpaths := expandRpaths(
			rawRpaths,
			filepath.Dir(current.origRealPath),
			filepath.Dir(current.origDeclPath),
			exeDir,
			current.parentRpaths,
		)

		for _, dep := range deps {
			depBase := filepath.Base(dep)
			dstPath, alreadyCopied := copiedByBase[depBase]

			if !alreadyCopied {
				declSrc, realSrc, found := resolveDylibDep(dep, current, allRpaths, exeDir)
				if !found {
					continue
				}
				if existing, ok := copiedByReal[realSrc]; ok {
					dstPath = existing
					alreadyCopied = true
				} else {
					dstPath = filepath.Join(frameworksDir, depBase)
					if err := copyFile(realSrc, dstPath); err != nil {
						continue
					}
					_ = os.Chmod(dstPath, 0o755)

					// Set install name ID to @rpath/<depBase>
					_ = exec.Command("install_name_tool", "-id", "@rpath/"+depBase, dstPath).Run()
					_ = exec.Command("install_name_tool", "-add_rpath", "@loader_path/.", dstPath).Run()

					copiedByReal[realSrc] = dstPath
					copiedByBase[depBase] = dstPath
					queue = append(queue, dylibQueueItem{
						bundlePath:   dstPath,
						origDeclPath: declSrc,
						origRealPath: realSrc,
						parentRpaths: allRpaths,
					})
				}
			}

			// Update reference in current to @rpath/<depBase>
			if wantRef := "@rpath/" + depBase; dep != wantRef {
				_ = exec.Command("install_name_tool", "-change", dep, wantRef, current.bundlePath).Run()
			}
		}
	}

	// Re-sign bundleBin and all bundled dylibs so Apple Silicon AMFI kernel never kills them with SIGKILL.
	// Because bundleBin is signed as a flat file outside the bundle, codesign signs ONLY this single binary in ~0.3s.
	for _, dstPath := range copiedByBase {
		_ = exec.Command("codesign", "--force", "--sign", "-", dstPath).Run()
	}
	_ = exec.Command("codesign", "--force", "--sign", "-", bundleBin).Run()

	if len(copiedByBase) > 0 {
		fmt.Fprintf(logw, "             Bundled %d external dynamic libraries into Contents/Frameworks/\n", len(copiedByBase))
	}
	return nil
}

var (
	custom3rdPartyMu   sync.Mutex
	custom3rdPartyDirs []string
)

func register3rdPartyDir(dir string) {
	if dir == "" {
		return
	}
	abs, err := filepath.Abs(dir)
	if err == nil {
		dir = abs
	}
	custom3rdPartyMu.Lock()
	defer custom3rdPartyMu.Unlock()
	for _, existing := range custom3rdPartyDirs {
		if existing == dir {
			return
		}
	}
	custom3rdPartyDirs = append(custom3rdPartyDirs, dir)
}

// findGcnRecompiler locates the GCN shader recompiler library (shadPS4's
// shader_recompiler packaged in 3rdparty/gcn-recompiler): include dir and the
// prebuilt static library.
func findGcnRecompiler(extraDirs ...string) (string, string) {
	var candidates []string
	addCandidate := func(base string) {
		candidates = append(candidates,
			base,
			filepath.Join(base, "gcn-recompiler"),
			filepath.Join(base, "3rdparty", "gcn-recompiler"),
		)
	}
	for _, dir := range extraDirs {
		if dir != "" {
			addCandidate(dir)
		}
	}
	custom3rdPartyMu.Lock()
	for _, dir := range custom3rdPartyDirs {
		if dir != "" {
			addCandidate(dir)
		}
	}
	custom3rdPartyMu.Unlock()
	candidates = append(candidates, "3rdparty/gcn-recompiler")
	if exe, err := os.Executable(); err == nil {
		exeDir := filepath.Dir(exe)
		candidates = append(candidates,
			filepath.Join(exeDir, "3rdparty", "gcn-recompiler"),
			filepath.Join(exeDir, "..", "3rdparty", "gcn-recompiler"),
		)
	}
	if wd, err := os.Getwd(); err == nil {
		dir := wd
		for range 5 {
			candidates = append(candidates, filepath.Join(dir, "3rdparty", "gcn-recompiler"))
			parent := filepath.Dir(dir)
			if parent == dir {
				break
			}
			dir = parent
		}
	}
	for _, dir := range candidates {
		libPath := filepath.Join(dir, "libgcn_recompiler.a")
		if _, err := os.Stat(libPath); err == nil {
			absInc, _ := filepath.Abs(filepath.Join(dir, "include"))
			absLib, _ := filepath.Abs(libPath)
			return absInc, absLib
		}
	}
	return "", ""
}

func findSpirvCross(extraDirs ...string) (string, string) {
	var candidates []string
	addCandidate := func(base string) {
		candidates = append(candidates,
			base,
			filepath.Join(base, "spirv-cross"),
			filepath.Join(base, "3rdparty", "spirv-cross"),
		)
	}
	for _, dir := range extraDirs {
		if dir != "" {
			addCandidate(dir)
		}
	}
	custom3rdPartyMu.Lock()
	for _, dir := range custom3rdPartyDirs {
		if dir != "" {
			addCandidate(dir)
		}
	}
	custom3rdPartyMu.Unlock()
	candidates = append(candidates, "3rdparty/spirv-cross")
	if exe, err := os.Executable(); err == nil {
		exeDir := filepath.Dir(exe)
		candidates = append(candidates,
			filepath.Join(exeDir, "3rdparty", "spirv-cross"),
			filepath.Join(exeDir, "..", "3rdparty", "spirv-cross"),
		)
	}
	if wd, err := os.Getwd(); err == nil {
		dir := wd
		for range 5 {
			candidates = append(candidates, filepath.Join(dir, "3rdparty", "spirv-cross"))
			parent := filepath.Dir(dir)
			if parent == dir {
				break
			}
			dir = parent
		}
	}
	for _, dir := range candidates {
		libPath := filepath.Join(dir, "libspirv-cross.a")
		if _, err := os.Stat(libPath); err == nil {
			absDir, _ := filepath.Abs(dir)
			absLib, _ := filepath.Abs(libPath)
			return absDir, absLib
		}
	}
	return "", ""
}

func findLibAtrac9(extraDirs ...string) (string, string, []string) {
	var candidates []string
	addCandidate := func(base string) {
		candidates = append(candidates,
			base,
			filepath.Join(base, "libatrac9"),
			filepath.Join(base, "3rdparty", "libatrac9"),
		)
	}
	for _, dir := range extraDirs {
		if dir != "" {
			addCandidate(dir)
		}
	}
	custom3rdPartyMu.Lock()
	for _, dir := range custom3rdPartyDirs {
		if dir != "" {
			addCandidate(dir)
		}
	}
	custom3rdPartyMu.Unlock()
	candidates = append(candidates, "3rdparty/libatrac9")
	if exe, err := os.Executable(); err == nil {
		exeDir := filepath.Dir(exe)
		candidates = append(candidates,
			filepath.Join(exeDir, "3rdparty", "libatrac9"),
			filepath.Join(exeDir, "..", "3rdparty", "libatrac9"),
		)
	}
	if wd, err := os.Getwd(); err == nil {
		dir := wd
		for range 5 {
			candidates = append(candidates, filepath.Join(dir, "3rdparty", "libatrac9"))
			parent := filepath.Dir(dir)
			if parent == dir {
				break
			}
			dir = parent
		}
	}
	for _, dir := range candidates {
		hdrDir := filepath.Join(dir, "C", "src")
		hdr := filepath.Join(hdrDir, "libatrac9.h")
		if _, err := os.Stat(hdr); err == nil {
			absInc, _ := filepath.Abs(hdrDir)
			// Check for pre-built static library
			for _, libCandidate := range []string{
				filepath.Join(dir, "libatrac9.a"),
				filepath.Join(dir, "C", "bin", "libatrac9.a"),
				filepath.Join(hdrDir, "libatrac9.a"),
			} {
				if _, err := os.Stat(libCandidate); err == nil {
					absLib, _ := filepath.Abs(libCandidate)
					return absInc, absLib, nil
				}
			}
			matches, _ := filepath.Glob(filepath.Join(absInc, "*.c"))
			return absInc, "", matches
		}
	}
	return "", "", nil
}

func findMinimp3(extraDirs ...string) string {
	var candidates []string
	addCandidate := func(base string) {
		candidates = append(candidates,
			base,
			filepath.Join(base, "minimp3"),
			filepath.Join(base, "3rdparty", "minimp3"),
		)
	}
	for _, dir := range extraDirs {
		if dir != "" {
			addCandidate(dir)
		}
	}
	custom3rdPartyMu.Lock()
	for _, dir := range custom3rdPartyDirs {
		if dir != "" {
			addCandidate(dir)
		}
	}
	custom3rdPartyMu.Unlock()
	candidates = append(candidates, "3rdparty/minimp3")
	if exe, err := os.Executable(); err == nil {
		exeDir := filepath.Dir(exe)
		candidates = append(candidates,
			filepath.Join(exeDir, "3rdparty", "minimp3"),
			filepath.Join(exeDir, "..", "3rdparty", "minimp3"),
		)
	}
	if wd, err := os.Getwd(); err == nil {
		dir := wd
		for range 5 {
			candidates = append(candidates, filepath.Join(dir, "3rdparty", "minimp3"))
			parent := filepath.Dir(dir)
			if parent == dir {
				break
			}
			dir = parent
		}
	}
	for _, dir := range candidates {
		hdr := filepath.Join(dir, "minimp3.h")
		if _, err := os.Stat(hdr); err == nil {
			absDir, _ := filepath.Abs(dir)
			return absDir
		}
	}
	return ""
}

// checkedPtr reports NPS5_CHECKED_PTR=1: build the runtime with range-checked guest-to-host pointer
// translation (nps5_thunk.h). Off by default; the default path stays a bare add.
func checkedPtr() bool { return os.Getenv("NPS5_CHECKED_PTR") == "1" }

// humanBytes formats a byte count for the build log.
func humanBytes(n int64) string {
	const unit = 1024
	if n < unit {
		return fmt.Sprintf("%d B", n)
	}
	div, exp := int64(unit), 0
	for m := n / unit; m >= unit; m /= unit {
		div *= unit
		exp++
	}
	return fmt.Sprintf("%.1f %ciB", float64(n)/float64(div), "KMGT"[exp])
}

// defaultJobs is the default parallelism: NPS5_JOBS when set (shared machines), else the CPU count.
func defaultJobs() int {
	if v, err := strconv.Atoi(os.Getenv("NPS5_JOBS")); err == nil && v > 0 {
		return v
	}
	return runtime.NumCPU()
}
