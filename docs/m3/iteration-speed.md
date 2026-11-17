# Iteration speed: translate, build, run (branch m3-iterspeed)

Goal: shorten the translate -> build -> run loop for a big title (the user's Unreal title: 26.5 M guest
instructions, 448 k functions, 2,199 chunks, 619 MiB of objects). All numbers below were measured on that
title on a machine shared with other builds (load average 16-85 during the runs, noted per row); read them as
indicative and compare ratios, not absolutes. Parallelism was `-j 14` for the first measurements and `-j 3`
(NPS5_JOBS=3) after the coordinator asked to cap it.

## Where the time went (profile of the full translate, before)

`NPS5_PPROF=<dir>` (new) writes `cpu.pprof`, `heap.pprof`, `allocs.pprof` of a run.

| phase | time | notes |
| --- | ---: | --- |
| load + discovery | 1.2 s + 25.5 s | one thread; decodes every function |
| emission (lift) | 148 s with no assembler running (`-compile=false`) | **one thread**: decode again (30%), lift (34%), text (11%), GC/alloc (43% of samples) |
| emission + assembly | 295 s | the lifter thread competes with 14 `clang` processes |
| assembling 2.3 GiB of text | about 19 us per line with the Apple toolchain's `clang -c` | 1.2 s for a 1.2 MB chunk; about 2,600 CPU-s for the title |
| link (tools/m1_native_build.sh) | 27-30 s | recompiled all 2,400 generated `.c` files every time, then ld64 |
| tier-1 miss | 2.4-3.1 s | 2.9 s of it loading the title in a new process; 0.2 s lifting and building |

Findings that drove the work:

1. The lifter was single threaded and the assembler was the larger CPU cost. Both fixed.
2. **Homebrew LLVM's `clang` assembles the same text 4.5x faster** than Apple's (0.27 s against 1.2 s for the
   same 65 k-line chunk). The objects are byte identical (below). `asmTool()` prefers
   `/opt/homebrew/opt/llvm/bin/clang`, `NPS5_ASM` overrides, the system `clang` is the fallback; the tool is part
   of the cache key.
3. The whole generate-and-compile loop repeated work that had not changed: lifting 2,199 chunks for a runtime
   change, rediscovering the title, recompiling 2,400 unchanged generated C files at link time, and loading
   the title again for each tier-1 miss.

## What changed

### 1. Parallel emission (pkg/emitter/chunkpar.go, asm_chunk.go)

`EmitChunkedAsm` renders chunks on `-j` workers and delivers them to the sink in chunk order. Each worker owns
a lifter clone (`Arm64Lifter.Clone`) and a decoder clone (`Disassembler.CloneForEmit`: private entry sets, drops
made while decoding one function are undone, `RestoreDrops`; none happen after discovery on the title,
`EmitDrops` reports it). Everything else is shared read only. The facts that used to be appended to the
emitter as a side effect (census facts, warnings, unsupported sites, function infos) are collected per chunk
and merged in order, so `census.json` and `addrmap.txt` do not depend on `-j` (checked: identical for `-j 1`
and `-j 8`, and equal to the old translator's on hello_world).

Label counters (`L_tso_ok_N`, `.Lcpuid_*`, `L_uh_skip_N`, ...) restart per chunk (`ResetChunk`), so the text of
a chunk depends on its own functions only; this changes label names, never machine code. The lifter no longer
formats the `// 0xADDR: INST` line when comments are off (`SetComments`), 8% of the old translation.

Output equality, before against after, same inputs (no seeds): **2,193 of 2,194 chunk objects of the title are
byte identical to the old translator's; the 2,194th differs only in the names of local `.Lcpuid_*` symbols** (code,
pads and pc-map sections identical, checked with `otool`/`size`). All 300 chunks of hello_world are byte
identical. `-j 1` and `-j 8` produce identical `.s`. A Go test (`TestParallelChunksAndCache`) pins the equality
of 1 and 4 workers and the cache behaviour; `-race` on hello_world is clean.

### 2. Incremental rebuilds (pkg/tcache, emitter, cli)

**Index.** `<out>/.nps5cache/index.jsonl` (override: `-cache-dir`; `-no-cache` turns it off), JSON Lines, one row
per chunk and per tier-1 fragment, rewritten atomically. No SQLite or zstd library is vendored (only
`golang.org/x/arch`), and the index of the title is 1.2 MB, so a text file loaded in a few ms was the simplest
thing that fits. A chunk row:

| field | meaning |
| --- | --- |
| `id`, `key` | `code_NNN`; sha256(lowering id, lifter/emitter configuration, assembler tool and flags, chunk name, `guest_hash`, `facts_hash`) |
| `first`, `last`, `nfuncs`, `insts` | the chunk's function range |
| `guest_hash` | the chunk's functions as decoded: guest bytes of every instruction, block structure, flag-liveness bits, jump tables, names |
| `facts_hash` | the analysis facts the lowering consults besides the bytes: for each direct call/jump target "known function / shim", the landing-pad policy |
| `lowering` | the lowering id (below) |
| `asm_hash`, `asm_bytes` | sha256 and size of the emitted text |
| `obj_hash`, `obj_size`, `obj_zsize` | identity and sizes of the object (the sink's hash: assembler flags + asm hash) |
| `ud2`, `pc_entries`, `unsupp`, `unsupp_sites`, `warnings` | what a hit cannot recompute from the decoded functions |
| `used` | last used time |

Facts that a hit can recompute (per-block labels, function sizes, function infos for the census) are rebuilt from
the decoded functions, which keeps the index small (the 4 M block labels are not stored).

**Lowering id** (`emitter.LoweringID`): the hash of the non-test Go sources of `pkg/lifter`, `pkg/emitter` and
`pkg/disasm`, embedded in the binary by the root package (`lowering_src.go`, go:embed) so it names what was
built, not what the tree holds later. `NPS5_LOWERING_ID` overrides. The user asked for per-mnemonic-family
version ids so a fix to one lowering invalidates only the chunks that use it. **Not done, with a concrete
reason**: the lifter is a large set of mutually calling functions (flag liveness, TSO wrapping, upper-half
fixups run after every lowering), so "which lowerings an instruction used" cannot be read from the source
without instrumenting every path, and a wrong dependency table would silently serve stale code. Instead any
lowering-source change invalidates the lift (a parallel 51 s at `-j 3` on the title) and the **assembler is
skipped for every chunk whose emitted text is unchanged** (content hash): for a change that touches one
instruction form, nine chunks of 2,199 were reassembled.

**Skipping the lift.** A chunk whose key is unchanged and whose object is still on disk (or in the object store)
is not lifted at all (the decoded functions are still needed to compute the key; that is the cheap part).

**Whole-translation stamp** (`pkg/cli/stamp.go`). After a successful `-asm-only` run the hash of the inputs
(lowering id, options, stat of the ELF and every module in its directories, the seed list, relevant `NPS5_*`
variables) is stored. A run with an equal stamp and an intact output (every chunk's object present with the
right hash, the generated files there) does not load the ELF or run discovery: it refreshes the runtime sources
(a runtime-only change never retranslates) and stops. `-asm-only` now also writes `build.ninja`, so
`tools/m1_native_build.sh` works after it.

**Object store** (`-obj-store DIR`, `tcache.ObjStore`): content addressed by (assembler, flags, asm hash),
objects compressed with the `zstd` command line tool (brew; stored raw if absent). Before assembling, a chunk
looks there, so objects survive deleting or changing the output directory.

| | size | time |
| --- | ---: | --- |
| 2,199 objects, raw | 619 MiB | |
| zstd -3 store | 131 MB (4.7x) | saving is done by the assembler workers |
| restoring all 2,181 objects from the store instead of assembling (`-j 3`, load 34) | | translate 61 s in all (discovery 27 s, lift/restore 30 s) against about 6 minutes to assemble |
| the index | 1.2 MB | loaded in a few ms |

### 3. Linking (tools/m1_native_build.sh)

The script recompiled every non-runtime source (2,199 `code_NNN_reg.c`, `dispatch.c`, ...) on each run. It now
records the flags+headers hash (`.nps5_gkey`) and keeps an object that is newer than its source (the translator
rewrites a generated file only when its text changes). `NPS5_JOBS` caps the parallelism. Runtime objects were
already cached by content in `tests/ps5/build/objcache` (so a runtime `.c` change recompiles one file; a header
change recompiles the runtime, as before).

| link of the 619 MiB title (`-j 3`, load 30) | time |
| --- | ---: |
| before (all generated sources recompiled, two `ld` runs because of the stubbed gnm symbols) | 27 s |
| now, relink with nothing changed | 7.8 s |
| now, first link in a fresh directory (runtime compiled into an empty cache) | 61 s |

A prebuilt runtime archive was not made: with the content-keyed object cache the runtime is not recompiled, and
what remains is `ld` itself (about 4 s per link).

### 4. Tier 1 (pkg/cli/fragserve.go, fragment.go, runtime/core/nps5_tier1.c)

* **Persistent translator** `ps4-recomp fragment-serve --manifest M --socket S --parent PID`: loads the title once
  (2.1 s), listens on a unix socket, one request per connection
  (`entry=0x.. out=DIR timeout-ms=N` -> `done`), builds fragments concurrently from clones of the decoder
  and writes `result.txt` exactly like `fragment`. The runtime starts it lazily on the first miss
  (`NPS5_TIER1_SERVER=0` disables), exits with it (`--parent` poll, `atexit` SIGTERM, idle timeout), and falls
  back to process-per-miss if the server cannot start or answers badly.
* Per miss inside the fragment build: the descriptor and the assembly compile in parallel, the assembler is the
  fast one, and no `codesign` run (ld64 signs arm64 images ad hoc itself; checked with `codesign -dvv`).
* **Mismatch check** uses the lowering id instead of the translator binary's hash, so a runtime-only change does
  not make misses fail with `translator_mismatch`.
* **Cache** root is now `-cache-dir/tier1` when given (default still `<out>/tier1_cache`), keyed by (title ELF
  sha, version, guest address) where the version is the hash of ABI, lowering id and translation options rather
  than the translator binary. A retranslation with the same version finds every earlier fragment in place; the
  translator lists them as `frag` rows of the index (`Tier-1: N cached fragment(s) valid for this translation`).

| tier-1 miss, user title (30 of the misses recorded by the boot runs; load 38) | latency |
| --- | ---: |
| process per miss (before) | 2.4 s reported, 3.1 s measured here |
| server, first request (title load) | 2.1 s once |
| server, per miss | **median 0.124 s**, p90 0.137 s, max 0.144 s |

`-seeds-from <runs dir>` **works**: 218 served misses of the boot runs were read, all 218 became entry points
(448,366 functions against 446,891) and all 218 are in the new build's `aot_funcs.bin`. `tools/tier1_test.sh` (both
address modes) passes, including the seeded rebuild with no miss in the third run.

## Results

Load average in brackets. Before = the translator at the start of this branch (main 41f2376 + m3-boot) with
its stock default pipeline; after = this branch.

| | before | after |
| --- | ---: | ---: |
| full translate + assemble, cold, `-j 14` | 322 s [25-58] (discovery 25 s + emission 295 s) | **159 s** [30-60] (lift 127 s; discovery 27 s) |
| full translate + assemble, cold, `-j 3` | not measured (about 3x worse) | 405 s [30-85] (assembler bound, 2.2 GiB of text) |
| no-op rebuild | about 175 s (rediscover 27 s, relift all chunks 148 s; only the assembler was skipped) | **0.3-0.5 s** |
| runtime-only change (edit a runtime `.c`) | about 175 s | **0.3 s** + relink 7.8 s |
| one-line lifter change (a `nop` in the CPUID lowering) | about 175 s relift + 9 chunks assembled | **85 s** at `-j 3` [27-56] (discovery 27 s, lift 51 s, 9 assembled, 2,190 kept) |
| lowering-id change that changes no text | same | 85 s, 0 assembled (restored from the object store) |
| link | 27 s | 7.8 s relink (61 s first time with an empty runtime cache) |
| tier-1 per miss | 2.4-3.1 s | 0.124 s median |
| cache | none | 1.2 MB index; objects 619 MiB raw, 131 MB zstd |

What is left: discovery (27 s, single threaded) runs on every translate that is not a no-op; persisting its
result keyed by the decoder sources would take the lifter-change loop to about 55 s. The decoder allocates
about 80 GB per translate (an `Instruction` is copied three times: `instAtAddr`, `curBlock`, the block's
slice), which is most of the Go runtime's page-fault time; it was left alone because `pkg/disasm` is being
changed on the boot branch. Content-defined chunk boundaries would keep chunk ids stable when `-seeds-from`
adds functions (today the chunks after the first new function shift and are relifted, still cheap to
assemble because the object store has most of the text).

## Usage

```
ps4-recomp -asm-only -j 3 -gpu anyps5 ... -seeds-from <runs> -obj-store <dir> -o out <title>
NPS5_JOBS=3 tools/m1_native_build.sh out native
```
`translate.sh` of the boot branch runs the default (`-compile`), which tries the full runtime build in-process
and fails on the missing gcn header before `m1_native_build.sh` does the real build; `-asm-only` skips that.
`NPS5_PPROF=<dir>`, `NPS5_JOBS`, `NPS5_ASM`, `NPS5_LOWERING_ID`, `NPS5_TIER1_SERVER=0` are the knobs.
