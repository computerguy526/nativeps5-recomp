# nativeps5-recomp

The translator and runtime of [nativeps5](https://github.com/computerguy526/nativeps5): static recompilation of
PlayStation 4 and PlayStation 5 programs (x86-64 ELF) to native arm64 macOS programs. No JIT, no interpreter.

**Start at the entry repository**: https://github.com/computerguy526/nativeps5 has the build instructions
(`BUILDING.md`), the contribution rules (`CONTRIBUTING.md`, `AGENTS.md`), the pre-commit check, the game profiles
and the KosmicKrisp patch series. This repository is one of its components and is normally cloned by its
`scripts/fetch-components.sh` as `ps5recomp/`.

This is a fork of [wie-project/PS4Recomp](https://github.com/wie-project/PS4Recomp) (GPL-2.0). The history is
upstream's up to the fork point, then the nativeps5 work as a short series of squashed commits. Upstream's own
README is kept as [docs/PS4Recomp-README.md](docs/PS4Recomp-README.md).

**No game data is here and none is accepted**: no game executables, assets, shaders, keys, firmware, frame
captures or prebuilt test programs. You need your own, legally dumped copy of a game you own.
Not affiliated with or endorsed by Sony Interactive Entertainment or any game publisher.

## What is in it

| Path | What |
|---|---|
| `cmd/ps4-recomp` | the translator: ELF/SELF loader, disassembly and analysis, x86-64 to arm64 lifter, emitter, link driver |
| `cmd/nps5` | packager and launcher front end (`nps5 build`, `nps5 play`, `nps5 list`) |
| `pkg/runtime` | the C runtime linked into every translated program: guest memory, threads, kernel and library HLE |
| `pkg/runtime/modules/libSceAnyPS5` | the bridge to an external PS5 system-library and graphics provider (see below) |
| `tests/`, `tools/` | unit tests, the instruction-level differential suite, regression gates, run wrappers |
| `docs/` | design notes by milestone (`m0`..`m3`), `SPEC.md` |

## Build and test

```sh
go build -o bin/ps4-recomp ./cmd/ps4-recomp      # Go modules are vendored: no network needed
go build -o bin/nps5 ./cmd/nps5
go test ./pkg/...                                # CPU only; a few minutes
```

Test payloads are never committed. Tests that need one build it from the C source next to them when a
[ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) install is given in `PS5_SDK_DIR`, and skip otherwise;
tests that load upstream's `hello_world.elf` skip unless `NPS5_TEST_HELLO_ELF` points at a copy.

## Graphics and system-library back ends

| `-gpu` | Needs | State |
|---|---|---|
| `off` (default) | nothing outside this repository | PS4Recomp's own HLE and Metal renderer (PS4 programs, simple PS5 payloads) |
| `anyps5` | the arm64 host libraries of an AnyPS5 build in `NPS5_ANYPS5_BUILD` | PS5 titles: system libraries, pad, video out and, by default, the AGC graphics driver |
| `anyps5` + `NPS5_GPU=kyty` at run time | the same, plus `kyty_gpu.dylib` from [nativeps5-kyty](https://github.com/computerguy526/nativeps5-kyty) in `NPS5_KYTY_GPU_LIB` | PS5 titles drawn by KytyPS5's renderer ([docs/m3/kyty-seam.md](docs/m3/kyty-seam.md)) |

The AnyPS5 fork that provides those host libraries is **not public**. Everything in this repository builds and
its unit tests pass without it; `-gpu anyps5` and `nps5 build` stop with a message that names the missing piece.
The Kyty renderer replaces the 84 graphics exports only: the PS5 system libraries (libkernel, libc, pad and about
1,300 other exports) still come from the AnyPS5 host libraries, so a PS5 title cannot be linked from the public
repositories alone today. Closing that gap (system libraries without AnyPS5) is open work.

## Licence

GPL-2.0-only ([LICENSE](LICENSE)), as upstream. Third-party work and credits: [NOTICE](NOTICE).
