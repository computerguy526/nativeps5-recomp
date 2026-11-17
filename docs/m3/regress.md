# Pre-merge regression gate (`tools/regress.sh`)

One command that answers "is this branch safe to merge?": it builds the translator once, runs the repo's own test suites, then
translates, builds and *runs* the three homebrew titles (Calculator3D, Syobon, Greenhollow) sandboxed for a fixed time with scripted
input and audio capture, and prints a single PASS/FAIL table. It exists because a regression (the titles stop flipping after 3 frames,
introduced by the `m3-gpufu` merge `cda2f15`) reached `main` unnoticed: every unit test and `m1_run.sh` was green, only running the titles shows it.

```
tools/regress.sh [--quick|--full] [--rev <rev>] [--ref <rev>] [--nested] [--only a,b] [-j N] [--rebuild] [--keep-wt]
                 [--update-refs] [--seconds title=S]
```

| option | meaning |
|---|---|
| `--quick` (default) | `gotest`, `m1_flat` (-fplt variant only), `calc3d` (10 s). About 5-7 minutes on the loaded shared machine (go test 2-4 min, m1 about 2 min, Calculator3D about 1 min). |
| `--full` | `gotest`, `memtests --strict`, `m1_flat`, `tier1`, `mem_prove`, `diag`, `calc3d` (30 s), `syobon` (60 s), `greenhollow` (80 s). |
| `--rev <rev>` | judge `<rev>` (temporary worktree under `work/regress/wt/`) instead of the checkout the script lives in. |
| `--ref <rev>` | afterwards run the same checks on `<rev>` and print both tables plus the checks whose result differs. For bisects. |
| `--nested` | additionally `m1_run.sh --call-model nested`. |
| `--only a,b` | only these checks: `gotest memtests m1_flat m1_nested tier1 mem_prove diag calc3d syobon greenhollow`. |
| `-j N` | CPU budget, default **3** (shared machine): `GOMAXPROCS`, translator `-j`, clang workers. |
| `--rebuild` | rebuild title binaries even when nothing changed (they are cached by a hash of translator, source diff, input and AnyPS5 build). |
| `--update-refs` | store this run's checkpoint frames as the new references (see below). |

Exit status is nonzero if any check fails. Output: the table on stdout, `work/regress/runs/regress-<timestamp>.json` with every metric
(per check, per title: rc, crash info, frames presented, longest flip gap, fps, audio statistics, every checkpoint comparison, thresholds used), and
logs of every check under `work/regress/runs/<timestamp>/{subject,ref}/logs/`. Every run is terminated: `gtimeout` with `-k`, a final
`pkill -9` of the title binary, an `EXIT` trap that kills helper loops. No x86 binary is ever executed, `scripts/build.sh` is not used,
recompiled outputs run only through `tools/run_sandboxed.sh`.

## What each check proves

| check | how it is run (unchanged tool, one wrapper) | what a failure means |
|---|---|---|
| `build` | `go build ./cmd/ps4-recomp`, once; every other check uses this binary (`PS4RECOMP_BIN`) | the translator does not compile |
| `gotest` | `LIFTER_NO_X86=1 GOFLAGS=-mod=vendor go test -p 3 ./pkg/...` | a Go package test (lifter, emitter, loader, thunkgen, runtime C harnesses) fails |
| `memtests` | `tools/memtests.sh --strict --no-doc` (race detector, host C harnesses, 240 guest cases base+identity; KNOWN-FAIL counts as failure) | the guest memory model regressed |
| `m1_flat` | `tools/m1_run.sh --call-model flat`: 19 PS5 test programs x plt/noplt, stdout compared with `tests/ps5/expected`, broken_* programs must crash with the right report | the lifter/emitter/runtime miscompiles a test program (the M1 suite) |
| `m1_nested` | same with `--call-model nested` (`--nested`) | the nested call model regressed |
| `tier1` | `tools/tier1_test.sh` (flat) | the miss fallback (tier 1): compile on miss, disk cache, corruption, caps, seeds |
| `mem_prove` | `tools/mem_prove.sh`: hello_ps4, hello_k (base/identity/auto), fixed-address maps vs `tests/ps5/memory/expected` | the memory-model proof runs differ |
| `diag` | `tools/diag_acceptance.sh`, which now ends with per-case verdicts and exits nonzero on a deviation (it only printed output before) | a diagnostics bundle (SPEC 8.2) is missing or wrong: bad access (SIGSEGV, crash.txt), unresolved import (named in crash.txt), guest ud2 (SIGTRAP), hello programs; AVX2 VPERMQ is accepted as a clean run (the lifter implements it since m1) or as an "unsupported" report |
| `calc3d`, `syobon`, `greenhollow` | translate `-gpu anyps5`, `m1_native_build.sh`, run under `run_sandboxed.sh` with the sub-checks below | the title does not run, stalls, runs slowly, is silent, or renders differently |

Title sub-checks (all thresholds live in `work/regress/refs/<title>/thresholds.json`, not in git):

| sub-check | measured from | threshold (calc3d / syobon / greenhollow) |
|---|---|---|
| `run` | exit status of the sandboxed run and `crash.txt` of its diagnostics bundle | the run must still be alive at the harness timeout (rc 124, so SIGTERM was what ended it) and `crash.txt` must not report a signal other than 15. A crash/abort (rc 134, `__cxa_allocate_exception` unresolved import) fails here |
| `frames` | number of presented frames: `APS5_DUMP_FRAMES_EVERY=N` makes AnyPS5 log one `[gpu] frame_NNN.bmp ... at X s` line per Nth presented frame, so presented = dumps x N (error < N) | >= 100 / >= 300 / >= 600 |
| `max_gap` | largest time between two consecutive dump lines (0.1 s resolution), and the time between the last dump and the end of the run (a title that stops flipping and then hangs or aborts) | <= 1.0 s (tail: 1.0 s + 1.5 s slack for the kill and polling) |
| `fps` | steady-state frames per second between first and last dump | >= 25 / >= 12 / >= 30 (observed 60 / 27 / 59 on a heavily loaded machine) |
| `audio` | `NPS5_AUDIO_CAPTURE` WAV of port 1 (`NPS5_AUDIO_NULL=1`: the clock-paced mixer, no device): share of 100 ms windows whose peak exceeds -80 dBFS | calc3d has no audio; syobon >= 30 % (observed 63 %, the first ~5 s are silent); greenhollow >= 80 % (observed 100 %). `SKIP` on revs whose runtime has no capture hook |
| `frame0` (calc3d) | frame_000 of a dedicated 12 s run with `APS5_DUMP_FRAMES=1 APS5_DUMP_SCALE=1` vs `refs/calc3d/frame0_rosetta.bmp`, the Rosetta x86 reference (1920x1080) | **exact**: 100 % of pixels, max channel difference 0 |
| `frames_ref` | fixed checkpoint dumps (index k = present number (k+1) x N, i.e. a fixed time into the run) vs `refs/<title>/frame_KKK.bmp`, best of +-2 neighbouring dumps; metric = share of pixels with every channel within +-16 (the exact share is recorded too) | per checkpoint, see the JSON: calc3d #10, #30 >= 90 %; syobon #3, #10 >= 99.5 %, #20 >= 98 %, #30 >= 80 %; greenhollow #3, #20, #35 >= 99.5 %, #50, #58 >= 97.5 % |

Why these checkpoints: they were chosen from two independent runs of the same good revision (see the numbers in `refs/<title>/thresholds.json`):
Greenhollow's intro and character creation reproduce pixel-exactly (idx 10-60 identical, 100-260 98.9 %), its first fade (idx 3) and the village
after ~60 s (idx >= 300, the hero walks) do not; Syobon is identical up to idx 20 (about 4.5 s after the first flip) and diverges once the scripted
pad script and the game's randomness part ways (idx 25+: 90 %, 40: 37 %). The frame-0 check of Calculator3D is deliberately strict; the rest tolerate animation phase.
Rendering defects that the gate must not miss but that are small (a missing sprite is a few percent of a frame) are *not* caught by
`frames_ref`; the Rosetta comparison of `docs/m3` verification (`tools/frame_compare.py`) is the tool for that.

## Cold caches, timing and what the gate does about them

* AnyPS5's pipeline/shader cache lives in `work/regress/state/cache` (linked into each run's HOME) and survives between gate runs. The first
  frame of a title with an empty cache can exceed the title's own flip timeout (finding 7 of `docs/m2/anyps5-integration.md`) and the title then aborts
  with the same signature as the regression this gate looks for. A title/build whose cache was never warmed therefore gets one **unmeasured warm-up run
  (30 s)**; a stamp in `work/regress/state/warm-<title>` records it (AnyPS5 build + title binary).
* Save data and runs/ bundles are fresh for each run (`NPS5_WORKDIR` under the run directory).
* The scripted pad (`refs/inputs/{syobon,greenhollow}.pad`, `APS5_PAD_INPUT_FILE`) is copied into place when the first frame dump appears, so the
  input timeline starts at the same point of the title in every run. Calculator3D gets no input (frame 0 must not depend on it).
* Frame counts and gaps come from dump lines because under `-gpu anyps5` the old runtime flip counter is not used and `NPS5_ANYPS5_TRACE` is too
  intrusive; resolution is one dump interval (calc3d 10 frames, syobon 6, greenhollow 12) and 0.1 s.
* Nothing else heavy should run next to a gate run: the fps and gap thresholds have a factor of 2 or more of headroom against the load average (25+) seen while they were set, but not against a stalled machine.

## `--ref`: diffing two revisions, bisecting

`--rev` and `--ref` check out a temporary worktree of the repo (`work/regress/wt/<sha>`; `--keep-wt` keeps it and makes the next run on the same
rev faster), fill the empty `3rdparty` submodule directories from the harness checkout, and **overlay this checkout's `tools/`** so every revision is judged by the same
harness. Revisions that predate the opt-in audio capture (`ae1075d`, m3-verify) get that patch applied (instrumentation only, off unless `NPS5_AUDIO_CAPTURE` is set);
if it does not apply, `audio` is `SKIP`. The final table lists the subject, then the reference, then the checks that differ:

```
tools/regress.sh --full --rev HEAD --ref 03ee3ac        # what changed since the last known good?
# bisect (inside a scratch worktree of the repo, tools come from the harness checkout):
git bisect start <bad> <good>
git bisect run sh -c '$NPS5_ROOT/wt/regress/tools/regress.sh --only calc3d,greenhollow --rev "$(git rev-parse HEAD)"'
```
(a build failure of a rev is a FAIL as well; wrap it to return 125 if you want bisect to skip those).

## Updating the references

References are data about *known good* behaviour, so update them on purpose, from a revision you trust:

1. `tools/regress.sh --only syobon,greenhollow --rev <good rev> --update-refs` runs the titles on that revision and copies the checkpoint
   frames (the indices listed in `thresholds.json`; for a title without any, six evenly spread ones) into `refs/<title>/` and writes
   `refs_meta` (rev, duration, dump parameters) into `thresholds.json`.
2. Look at the frames (`tools/frame_compare.py`, or convert with any BMP viewer) and at `frames_ref` of a *second* run on the same revision
   (run it again without `--update-refs`): set each `min_near_pct` a few points under the worst value of the two runs. Checkpoints in scenes that
   depend on the pad script's timing must be dropped or given a low tolerance.
3. `refs/calc3d/frame0_rosetta.bmp` comes from the Rosetta x86 run (`work/s3/rosetta/frames_x86/frame_000.bmp`) and is *not* an arm64 output; it
   must only change when the x86 reference run is redone.
4. A deliberate rendering change: re-record the affected titles as in 1, commit the reason in the merge message.

Reference sources: **Greenhollow and Calculator3D frame 0 are compared with the Rosetta x86 reference frames** (`work/verify/x86gh/frames_x86`
subsampled to the dump grid; `work/s3/rosetta/frames_x86/frame_000.bmp`), never with an older nativeps5 build: the `03ee3ac` build renders Greenhollow
without sprites (hero preview, trees, NPCs) and an earlier version of this gate that used it as reference flagged the *fixed* renderer. Main 521c553 matches the x86 frames
in 60 of 60 frames at >= 98.0 % exact (mean 99.7 %). Syobon has no x86 reference with comparable timing (Rosetta runs it at 7 fps), so its checkpoints come from an arm64 build,
`1857da1` (`03ee3ac` plus the m3-homebrew fixes; `03ee3ac` itself cannot run Syobon: the hollow `libc.prx` stub of the title hijacks every libc import there, fixed by `4f64e06`).
The title inputs are `work/s3/rosetta/extract/eboot.elf`, `testbed/bin/syobon/extracted/PPSA47402/eboot.bin`, `work/verify/gh_extract/eboot.bin`
(override with `CALC_ELF`, `SYOBON_ELF`, `GH_ELF`); AnyPS5 comes from `wt/anyps5-int/build-arm64` (`NPS5_ANYPS5_BUILD`), the ICD from `work/s3/kk-mesh/install-arm64` (`KK_ICD`; KosmicKrisp with mesh patch 0003, default since the 2026-11-14 merge, Homebrew titles verified).

## Changes to existing tools

* `tools/diag_acceptance.sh`: verdict per case and exit status (it printed only).
* `tools/mem_prove.sh`, `tools/memtests_guest.sh`: use `$PS4RECOMP_BIN` instead of building their own `out/ps4-recomp` when it is set.
* The translator wrapper (`work/regress/runs/<ts>/subject/bin/ps4-recomp`) passes `-j 3` to translate-only invocations. Invocations that also compile
  (`mem_prove`, `diag_acceptance`, `memtests_guest` go through the CLI's ninja step) keep the default worker count and run at `nice 15`: that step
  stops at the first error of the known Ajm/Gnm modules (`build_wt.sh` documents it) and the rest of the link relies on the other objects being
  finished by then, so `-j 3` there leaves objects missing and produces a binary that silently prints nothing.
* The merge of `m3-verify` (opt-in `NPS5_AUDIO_CAPTURE`, `frame_compare.py`, `audio_analyze.py`) is part of this branch; it changes no behaviour.

## Validation (shared machine, load average 15-30)

| revision | result |
|---|---|
| `7b5453b` (main when the flip regression was found) | **FAIL**: calc3d and greenhollow abort after ~3 frames (rc 134, `__cxa_allocate_exception`; frames 0, max_gap none), syobon max gap 1.1 s |
| `521c553` (main now, includes m3-gfx flip fix and the MOVSS/PMULUDQ/CVTPS2DQ/DIV sprite fixes) | full: **PASS** on all suites; calc3d, syobon, greenhollow PASS (greenhollow 99.9 % vs x86 frames) |
| `m3-leanshim` (c15877c) greenhollow | PASS (98.8 % worst checkpoint) |
| `03ee3ac` | gotest, memtests, m1, tier1, mem_prove, diag, calc3d PASS; **greenhollow FAIL on frames_ref only** (flips fine, sprites missing); syobon FAIL (hollow libc.prx, fixed after it by `4f64e06`) |
| `1857da1` | calc3d, syobon, greenhollow flip/audio checks PASS |

Full run on main: about 33 minutes (gotest 2-4 min, memtests 2, m1_run 3-4, tier1 1.5, mem_prove 1.2, diag 1.3, titles 2+2+2 min plus builds and one-time 30 s warm-ups).
Note: the presentation rate of the same build varies between 60 and 120 fps with the machine's display state, hence the time-independent checkpoint search; and
a concurrent rebuild of AnyPS5 (`libnps5_gpu.dylib` briefly missing) makes a title fail with a dyld error: re-run.

