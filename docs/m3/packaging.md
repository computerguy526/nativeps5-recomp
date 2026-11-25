# One-command packaging (`nps5`, SPEC M4 item)

`nps5 build <title dir> [-o <out dir>] [--profile <name>] [-j N] [--force]` turns an extracted PS4/PS5 title into a
self-contained, double-clickable `<Title>.app`. `nps5 list` shows what was built, `nps5 play <id|name> [VAR=value...]`
opens one, `nps5 launcher [-o dir]` builds `NPS5 Launcher.app` (a ~100-line AppKit window: icon, name, Play button per
built title). Every other command line is the translator (`nps5 -gpu anyps5 ... eboot.bin` = `ps4-recomp`); the tier-1 miss
fallback of a built app runs `nps5 fragment`, so the binary that built the app is also its translator.

Code: `cmd/nps5`, `pkg/pack` (title.go, profile.go, build.go, bundle.go, library.go, assets/launcher.c, assets/launcher_gui.m),
`tools/nps5_app_smoke.sh`.

## What a build does

1. **Title identity**: `sce_sys/param.json` (PS5: `titleId`, `localizedParameters.<default lang>.titleName`, `contentVersion`) or
   `param.sfo` (PS4: TITLE_ID, TITLE, APP_VER/VERSION); `sce_sys/icon0.png`; executable `eboot.bin` (else `eboot.elf`).
1a. **Compress the game files?** (`pkg/pack/gamezip.go`) Only when a translation is about to run and the dump has files of 512 MiB or
   more that are not packed yet. It shows a dry-run estimate (size now, expected saving, how many files over 1.9 GiB become
   `.nps5seg` segment folders, a rough time, free disk needed) and asks `Compress the game files? [Y/n]`. Yes runs
   `python3 scripts/gamepack.py pack <dump>` of the entry repository (originals are removed only after the packed copy is
   re-read and compared by sha256; `gamepack.py unpack` undoes it). The answer is written to
   `<NPS5_DATA_STATE_DIR or ~/Library/Application Support/nativeps5/data>/game-compress-answer` (`yes`/`no`), which the entry
   repository's `nps5 cache prepare` reads to default its shader-cache question to yes. Rules: no terminal means no question
   (nothing is packed); `NPS5_GAME_COMPRESS=1` packs without asking, `=0` records no; end of input is no; nothing happens while a
   game runs; a dump under `/Volumes`, a non-APFS or read-only folder is left alone with the reason printed; a failed pack never
   fails the build. `NPS5_GAMEPACK` (path of gamepack.py) or `NPS5_ENTRY_DIR` say where the entry repository is when it is not next to this checkout.
2. **Profile** (below) -> translator flags.
3. **Translate** in-process (`-compile=false -gpu anyps5`) into `~/Library/Application Support/nativeps5/<titleId>/build`, then
   `tools/m1_native_build.sh` compiles the runtime (objects cached across titles) and links `build/native`. GPU `.mm`
   sources that do not compile standalone are skipped and their symbols stubbed by that script, as in every AnyPS5 run so far
   (`STUBBED 9 unresolved symbol(s)` is expected). A stamp (flags, eboot, nps5 binary, runtime tree, AnyPS5 driver) skips
   translation when nothing changed; `--force` redoes it. If the translator knows `-obj-store` it is pointed at
   `<support>/objstore` (m3-iterspeed), so retranslation reuses assembled chunks.
4. **Assemble + sign** the bundle.

Toolkit lookup: `NPS5_REPO` (source tree; else found from the binary or cwd), `NPS5_ANYPS5_BUILD` (AnyPS5 `build-arm64`, default
`../anyps5-int/build-arm64`), `NPS5_KK_PREFIX` (KosmicKrisp install prefix from AnyPS5 `mesa/build-kosmickrisp.sh`, default
`../../work/s3/install`), `NPS5_VULKAN_LIB` (default `/opt/homebrew/lib/libvulkan.1.dylib`), `NPS5_HOME` (support root).

## Bundle layout

```
Title.app/Contents/
  Info.plist            com.nativeps5.<titleId>, LSMinimumSystemVersion 26.0, game category, icon, usage strings
  MacOS/Title           launcher stub (C, CFBundleExecutable): reads launch.env, creates dirs, logs, chdir, exec
  MacOS/nps5-title      the runtime + translated code
  Frameworks/           libSceAgc/AgcDriver/libkernel/libc .prx, libnps5_gpu.dylib, libvulkan_kosmickrisp.dylib, libvulkan.1.dylib,
                        libSDL2 + libSDL3 (sdl2-compat dlopens @loader_path/libSDL3.dylib; not an otool dependency), SPIRV/glslang,
                        FFmpeg, FreeType ... (transitive closure outside /System and /usr/lib, ~72 MB)
  Resources/            AppIcon.icns (iconutil from icon0.png), guest_image.bin, profile.json, launch.env,
                        vulkan/icd.d/kosmickrisp_icd.json (library_path ../../../Frameworks/libvulkan_kosmickrisp.dylib)
```
`install_name_tool` rewrites every dependency to `@rpath/<name>`, ids to `@rpath/<name>`, drops absolute rpaths and adds
`@executable_path/../Frameworks` (title) / `@loader_path` (libs): no build-machine or Homebrew path is left. Moving or renaming
the .app works (tested).

**The game assets are not in the bundle.** `launch.env` carries `PS4_APP_DIR=<absolute path of the dump>` (read-only `/app0`);
the build and the run only read it (checked with a stat/size/mtime/mode hash of the whole dump before and after every smoke run).
If the dump is moved the launcher logs a warning; rebuild with the new path (translation is cached, a rebuild takes seconds).

**TCC:** a bundle is its own TCC client. A dump under `~/Downloads`, `~/Documents`, `~/Desktop` or a removable/network volume makes
macOS ask the user once ("Title would like to access files in your Downloads folder"; the Info.plist usage strings explain it) and
the process blocks in `opendir` until it is answered. Unattended tests therefore used copies of the three small dumps under
`/Users/Shared/nps5-dumps/` (not protected). Granting Full Disk Access to the .app also works.

## Launch environment and data (`launch.env`, expanded by the launcher; variables already set win, e.g. `open --env K=V`)

Everything lives in `~/Library/Application Support/nativeps5/<titleId>/` (`$NPS5_HOME/<titleId>`): `build/` (translated code, tier-1
manifest), `gpu-cache/` (shader cache, VkPipelineCache, KosmicKrisp Metal archive, `docs/nps5-pipeline-cache.md` in AnyPS5), `tier1_cache/`,
`savedata/`, `userdata/`, `cache/`, `runs/` (run bundles), `logs/last.log` (+ `previous.log`; stdout and stderr of the run), `run/` (working
directory: AnyPS5 writes `APS5_DUMP_FRAMES` BMPs there), `title.json` (what `list` and the launcher read).
Variables: `NPS5_TITLE_ID`, `PS4_APP_DIR`, `VK_DRIVER_FILES`/`VK_ICD_FILENAMES`, `SDL_VULKAN_LIBRARY`, `PS4RECOMP_DATA_DIR`, `NPS5_USERDATA_DIR`,
`NPS5_SAVEDATA_DIR`, `PS4RECOMP_CACHE_DIR`, `ANYPS5_SHADER_CACHE_DIR`, `NPS5_GPU_CACHE_DIR`, `NPS5_TIER1_CACHE`, `NPS5_RUNS_DIR`, plus the profile's `env`.
Tier-1 fragments need the translator (`nps5`), `build/lifter_manifest.json` and the ELF at the paths baked at build time; they are kept stable
by building into the support directory, so a moved .app keeps tier 1 as long as `nps5` stays where it was.

## Profiles

`--profile <name|path.json>`; without it `<titleId>.json` is tried; no file = defaults (`-gpu anyps5`). Searched in `$NPS5_PROFILES`,
`<repo>/profiles`, `<support>/profiles`. Unknown fields are an error.
```json
{ "name": "syobon",
  "translate": { "gpu": "anyps5", "x87": "f80", "call_model": "flat", "addr_mode": "auto",
                 "exclude_module": ["libfoo.prx"], "hle_malloc": "ue5-binned3", "args": ["-allow-unsupported"] },
  "env": { "NPS5_ANYPS5_TRACE": "1" }, "microphone": false }
```
`hle_malloc` (m3-mallochle) and other flags are only passed if the translator documents them (`cli.SupportsFlag`); otherwise the build fails with
a clear message rather than silently ignoring the profile. The resolved profile is stored in `Resources/profile.json`.
Retail title (PPSA30552) is not built here; the command is
`nps5 build "<path of PPSA30552 dump>" -o ~/Applications --profile ppsa30552` with a profile carrying its `hle_malloc`/`exclude_module` flags
(see docs/m3/function-hle.md on that branch).

## Signing and entitlements

Ad-hoc (`codesign --force --sign - --timestamp=none`), inside out: every Frameworks library, `nps5-title`, the launcher, then the bundle;
`codesign --verify --strict` must pass. **No entitlements and no hardened runtime were necessary**: library validation (the thing that would refuse
the ad-hoc AnyPS5/KosmicKrisp dylibs and tier-1 fragments, which are ad-hoc signed dylibs loaded with dlopen) and the JIT rules
(`com.apple.security.cs.allow-jit`, `disable-library-validation`) only apply under the hardened runtime, which a plain ad-hoc signature does not
enable. If a notarised build is ever wanted, those two entitlements are what it would need. No notarisation.

## Settings dialog

The runtime no longer shows the inherited PS4Recomp settings dialog (PS4 Pro mode, MetalFX, HUD, language) by default; it is opt-in with
`NPS5_SETTINGS_DIALOG=1` (`ps4_settings_policy.h`, test `TestSettingsDialogOptIn`). Language defaults to the system locale; `MTL_HUD_ENABLED` and `PS4_LANG` still apply.

Game patches of the title's patch profile (`-patches profiles/patches/<title id>.json`) are settings too. An on/off patch is
`patch.<key>=0|1` in the settings file and its own environment variable (ASTRO BOT: `NPS5_ASTRO_FAST_LIGHTING`, default on). A
**choice setting** has options instead (`"options"`, `"default_option"`); its sites are constant loads (`value`: `mov r64/r32, imm`;
`value-f32`: a scalar single load of a rip-relative constant) with one value per option, lowered as the ordinary load followed by a
load from a slot of `nps5_guest_patch_values` (the code pages never change). The runtime picks the option once before the guest
starts: the profile's default < `patch.<key>=<option name>` in the settings file or the dialog's pop-up < the environment
(`NPS5_PATCH_<KEY>` or the profile's own variable; an option name or its 0-based position). An option can imply host environment
(`"setenv"`), applied unless the environment already has the variable. ASTRO BOT 01.007: `upscaling` = `off` (native 1080p, the
game's own upscaler) | `metalfx900` | `metalfx720` (default), variable `NPS5_ASTRO_UPSCALING`; the MetalFX options set
`KYTY_METALFX=spatial`. Its sites are the dynamic-resolution level-0 size, so they take effect with the `dr` patch (level locked).
Tests: `TestPatchChoice*`, `TestShippedProfileUpscaling`, `TestShippedProfileBindsTitle` (with `NPS5_TEST_PPSA21564_EBOOT`),
lifter `TestPatchValue*`, cli `TestSettingsResolveChoicePatch`.

**Live choice sites** (v1.2.2) are for a setting the runtime changes while the game runs. `override`: a 32-bit register move
(`mov r32, r32` / `mov r32, m32`) whose result is replaced by the slot's low 32 bits after the ordinary lowering, unless the slot is
`"keep"` (bit 63 set) or, with `"keep_negative": true`, the move's own result is negative; the code reads the slot each time it runs.
`data32`: a 32-bit word of the image (a constant table), asserted by its bytes and the SHA-256 of an image range around it
(`func`/`func_size`/`func_sha256`), written by the runtime when the setting first needs it. One option must keep the game (`"keep"`).
ASTRO BOT 01.007 `render_scale` = `auto` (default) | `p540` | `p720`: level 1 of the game's own dynamic-resolution pipeline sets is
built at 960x540, the per-frame level selector and the scene level getter are override sites, the level-1 light-tile words data32
sites. `p720` keeps the game everywhere (the 720p of v1.2.1). The controller (`core/nps5_rscale.c`, logic `nps5_rscale_logic.h`) runs on
every flip packet: frame-time EMA (1 s), 540p after 2 s over the budget (`NPS5_RENDER_TARGET_MS`, default 33.3, +5 %), back to 720p
after 8 s under budget at 540p (with an 8 % margin for the dearer level), 20 s dwell, a quick bounce bans the next return (120 s,
doubling); frames over 250 ms (loads) do not count. `NPS5_RENDER=auto|720|540` (wins over the settings file), `NPS5_RENDER_LEVELS=
<level>=<mode>,..,*=<mode>`, `NPS5_RENDER_SWITCH=scene` (switch only at a level change or load), `NPS5_RENDER_TUNE`, `NPS5_RENDER_FILE`
(live override), `NPS5_RENDER_LOG`. One `[rscale]` line per switch. Tests: `TestPatchLive*`, `TestShippedProfileRenderScale`, lifter
`TestPatchOverride*`, `tests/rscale/rscale_unit.c`.

## Results (`tools/nps5_app_smoke.sh`, launched with `open -n`, killed after the wait)

| title | build (warm obj cache) | cold GPU cache | warm, 10 s | moved/renamed .app |
|---|---|---|---|---|
| Calculator3D (FAKE94458) | ~20 s | frames after ~40 s | 4 frames dumped | yes (copy, then another path) |
| Syobon Action (PPSA47402), profile `syobon` | ~1 min | 4 frames within 60 s | 4 frames | `mv` to another directory: runs, frames |
| Greenhollow (FAKE18847) | ~1 min | 4 frames within 60 s | 4 frames | not repeated |

The dump hash was unchanged after every run. The first run of a title compiles shaders/pipelines (KosmicKrisp cold cache) and can
take 30 s or more before the first frame; later runs present within seconds because `gpu-cache/` is kept per title.
The bundles embed the AnyPS5 libraries of the build tree at assembly time (re-run `nps5 build`, which is seconds when translation is cached,
after rebuilding AnyPS5): they were rebuilt against the 20:34 `wt/anyps5-int` libnps5_gpu with the 30 % window (`NPS5_WINDOW_PERCENT`).

## Data folder on a separate read-only volume (--data-dir / NPS5_APP0_DIR)

A title whose data is too big to copy (e.g. a 179 GB dump on an external disk) keeps it where it is:

- Translator: `ps4-recomp --data-dir <DATA> [-e <local-eboot.bin> | positional eboot.bin] -o <local out>` (`--data-dir` and `--dump-dir` are aliases of `--app-dir`; without an eboot argument `<DATA>/eboot.bin` is used when it is non-empty). `NPS5_APP0_DIR=<DATA>` is the env equivalent. Companion .prx files are discovered by walking `<DATA>`; nothing is ever written there.
- Runtime: `NPS5_APP0_DIR=<DATA>` (wins over `PS4_APP_DIR`) is the read-only `/app0`; writes under it already fail with EROFS. Converted outputs, caches, userdata stay local.
- `tools/dump_manifest.py create|verify <DATA> <manifest>` checks that the dump is untouched; `cmd/hlescan` lists the imports of a binary that no host shim satisfies.
