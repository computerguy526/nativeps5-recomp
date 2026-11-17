# Emitted assembly size and the streaming build

Branch `m2-emitstream`. Problem: the translator wrote ~125-189 bytes of ARM64 assembly text per
guest instruction (about 17 bytes of object code), so a real Unreal title (35-77 M guest
instructions) needed 6.6-14.5 GB of `.s` files on disk before clang saw the first one. Two fixes:

1. **Streaming assemble-and-delete**: each chunk is assembled as soon as it is emitted and its
   `.s` is deleted; peak intermediate disk is a few chunks, not the program.
2. **Lean output by default**: no per-instruction comments, shorter labels and tab indentation,
   and a compact binary pc map instead of a label and a 24-byte record per instruction.

## Usage

| flag | effect |
| --- | --- |
| (default, `-compile` on) | chunks are assembled in a bounded `-j` worker pool, `.s` deleted on success, `.o` kept as `code_NNN.o` |
| `-asm-only` | translate and assemble the chunks only: no runtime build, no link (`-compile` off) |
| `-keep-asm` | keep every `.s`; implies `-asm-comments` |
| `-asm-comments` | annotate the assembly with `// 0xADDR: INST` per guest instruction (`-asm-comments=false` overrides the implication of `-keep-asm`) |
| `-compile=false` | unchanged: the `.s` files are the product (`build.ninja`, `tools/m1_native_build.sh`); lean unless `-asm-comments` |
| `NPS5_PCMAP_STRICT=1` | an assembler-side pc-map size mismatch is an error instead of a warning |

A chunk whose assembly fails keeps its `.s` and the build stops with the clang message. The
`.o` of each chunk has a sidecar `code_NNN.o.hash` (sha256 of the assembly text and the assembler
flags). A rerun skips chunks whose text hash matches (the translator still lifts everything,
which is deterministic; only the assembler step is skipped) and deletes `code_NNN.*` left over
from an earlier, longer translation. Disk numbers go to the translate bundle manifest
(`stats`: `asm_generated_bytes`, `asm_peak_intermediate_bytes`, `asm_peak_disk_bytes_with_objects`,
`asm_object_bytes`, `asm_chunks`, `asm_chunks_assembled`, `asm_chunks_reused`, `asm_workers`,
`guest_insts_emitted`) and to the build log (`Assembly: ...`).

## Design

**Pipeline** (`pkg/cli/asmpipe.go`). `emitter.AsmEmitter.ChunkSink` receives each rendered chunk
(`pkg/emitter/asm_chunk.go`). The sink hashes it, writes the `.s`, and queues it to the pool
(queue depth 2, so the emitter blocks when the assemblers fall behind: disk in flight is
`workers + 2 + 1` chunks). Workers run `clang -target arm64-apple-darwin -mcpu=apple-m1 -c`
into `x.o.tmp`, rename, write the hash, delete the `.s`. `cFiles` then carries the `.o` files; the
direct-compile and ninja paths treat a `.o` entry as an already built link input.

**Lean text** (`leanLine`). The lifter still produces its annotated lines; the emitter drops
comment lines and trailing `//` comments, writes one tab instead of four spaces, drops the space
after commas (not inside `.asciz`), and shortens labels: `L_fn_0x<fn>_loc_0x<pc>` -> `Lf<fn>_<pc>`,
`L_tso_ok_N`/`L_tso_done_N` -> `LoN`/`LdN` (each guest memory access defines and branches to
both). Blank lines go. With `-asm-comments` the lifter's lines are kept as they are (labels still
shortened). Machine code is byte-identical either way
(`TestLeanAssemblyMatchesCommentedAndPCMap`, and `otool -s` of hello_cxx chunks against the old
translator). The lifter was not touched.

**Structured side channel**. Everything the census and addrmap used to learn by regex over
`// 0x..:` / `// UNSUPPORTED:` / `brk #0 // unsupported indirect` comments is now collected by the
emitter while it writes (`diag.AsmFacts`: functions with instruction counts and chunk, block
labels, unsupported hits, guest instruction count, UD2 stubs). `diag.BuildScan` turns that into
`census.json` and `addrmap.txt` (same F/L/U format; F lines are byte-identical to the old ones on
Syobon, L labels are the new short names). `diag.ScanAsm`, which re-read and rewrote the `.s`
files, is gone. Unsupported stubs are detected from the lifter's own `Unsupported()` record, not
from text. The other comment reader, `cmd/lifter-difftest-asm/run.sh` (builds its instruction
listing from the comments), now requires a `-keep-asm` / `-asm-comments` build directory and says
so. `cmd/fexasm` has its own lifter loop and is unaffected.

**Compact pc map** (`pkg/emitter/pcmap.go`, decoder in `pkg/runtime/core/nps5_diag.c`). The old map
was a label per guest instruction plus a 24-byte `{host, guest|flags, name}` record (with a
relocation) per guest instruction: ~100 bytes of text and ~50 bytes of object file each, and an
array of that size malloc'd at startup. The new `__DATA,__nps5_pcmap` section has one record per
function: host symbol (one relocation), guest entry, host code length, name, and a delta stream
with one byte per guest instruction (host distance in 4-byte words and guest delta, nibbles; an
escape for flags or large jumps). No label per instruction: the emitter counts the size of every
line it writes (`asmSizer`: instruction = 4 bytes, `.asciz`/`.zero` exact) and the assembler
checks the total per function (`.if (Le<fn>-_fn_0x<fn>)!=N` -> `.warning`, `.error` with
`NPS5_PCMAP_STRICT=1`). A function with a line shape the sizer does not know (none today) is left
out of the map with a warning. The unsupported-stub's `.balign 4` is replaced by the exact
`.zero n` so the layout stays fixed and the check can be evaluated while parsing.

Runtime: `load_pcmap` indexes the records by function (16 bytes per function; no per-instruction
memory, no sort of millions of entries), and `pc_lookup` binary-searches the function and decodes
its stream; the crash handler stays allocation free. `pcmap.bin` (what `nps5 addr2guest` reads) is
written in the same format as before, streamed from the compact table, plus one end-of-function
sentinel record (flag 8) so a pc in a shim stub or after a function is reported as "not guest
code" instead of the previous function. Crash reports name the function and guest RIP as before
(`tools/diag_acceptance.sh`: guest_ud2, guest_poke_bad_pointer and main resolved; `addr2guest`
on the written `pcmap.bin` agrees).

Function names are in the table; unnamed functions are synthesised as `fn_0x<entry>`.

## Measurements

Guest instructions are the translator's count of reachable code. "before" is the previous
translator (`.s` with comments, a label and a record per instruction in the pc map); "comments"
is `-asm-comments` of the new one (compact pc map); "lean" is the default. Object size is the
sum of `code_*.o` of `clang -c` of the chunks.

| program | guest insts | variant | asm bytes | asm B/inst | .o bytes | .o B/inst |
| --- | ---: | --- | ---: | ---: | ---: | ---: |
| PS5 hello_cxx | 176,640 | before | 32.9 MB | 186.5 | 9.3 MB | 52.9 |
| | | comments | 23.6 MB | 133.8 | 3.9 MB | 22.2 |
| | | lean | 14.8 MB | 83.7 | 3.9 MB | 22.2 |
| Syobon eboot | 657,714 | before | 119.7 MB | 182.0 | 33.4 MB | 50.7 |
| | | comments | 84.4 MB | 128.4 | 13.0 MB | 19.8 |
| | | lean | 49.1 MB | 74.7 | 13.0 MB | 19.8 |
| user game eboot (PPSA30552, 2.5 M insts found today) | 2,505,441 | before | 459.8 MB | 183.5 | 121.2 MB | 48.4 |
| | | comments | 321.1 MB | 128.2 | 43.7 MB | 17.4 |
| | | lean | 187.9 MB | 75.0 | 43.7 MB | 17.4 |

The game eboot loads on this branch only after its section-header table (past EOF) is zeroed in
a scratch copy; nothing of it was kept in the repo and the translated output was deleted after
measuring. Its discovery is incomplete today (2.5 M of about 21.6 M instructions, see the
discovery notes); the per-instruction figures are what scale.

Extrapolation to the whole Unreal title (35-77 M guest instructions, the earlier estimate of
6.6-14.5 GB of asm): before 183 B/inst = 6.4-14.1 GB of `.s` and 1.7-3.7 GB of objects;
now 75 B/inst = 2.6-5.8 GB of `.s`, of which only the in-flight chunks ever exist on disk
(about 50 MB), and 0.6-1.3 GB of objects.

## Peak intermediate disk

Measured with `-asm-only` (streaming, `-j` = 12 cores, other jobs on the machine):

| program | generated asm | peak assembly on disk | peak assembly + objects | objects |
| --- | ---: | ---: | ---: | ---: |
| hello_cxx (17 chunks) | 14.1 MiB | 14.1 MiB (all chunks fit in the pool) | 14.1 MiB | 3.7 MiB |
| Syobon (51 chunks) | 46.8 MiB | 18.6 MiB | 24.4 MiB | 12.4 MiB |
| game eboot (175 chunks) | 179.2 MiB | 21.2 MiB | 53.3 MiB | 41.7 MiB |

Peak assembly is bounded by `-j` + 3 chunks of about 1 MB, independent of the program size.

## Wall time (translate + assemble)

Before: `-compile=false` translation, then `clang -c` of every `.s` with 6 parallel jobs.
After: one streaming run. The machine was shared with other builds, so the figures are
indicative.

| program | before (translate + assemble) | after (`-asm-only`, translate + assemble overlapped) |
| --- | --- | --- |
| Syobon | about 12 s + 11 s | 14 s |
| game eboot (2.5 M insts) | 35.6 s + 87.7 s = 123 s | 53-57 s (28 s on a rerun: unchanged chunks are not reassembled, the lifter is the remaining cost) |

The streaming run is limited by the (single-threaded) lifter once there are enough assembler
workers; the lean text also assembles faster (less to lex).

## Notes

* The lifter is sequential (stateful), so translation is one thread; assembly overlaps with it. For
  a program the lifter produces slower than clang assembles (the usual case with `-j` >= 4)
  the assembly is hidden entirely.
* `.o` of the lean build are 2.4-2.6x smaller than before for the same code: the old map's
  per-instruction relocations were half the object file.
* Remaining text is the lifter's instruction sequences (about 4.3 host instructions per guest
  instruction at ~19 bytes per line); shrinking it further is a lowering change (the TSO access
  sequence alone is 10 lines).
* `_reg.c` registration files (two lines per function) and `addrmap.txt` L lines (one per basic
  block; written twice, into the bundle and the output directory) are unchanged and small next to
  the assembly.
