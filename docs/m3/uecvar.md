# Unreal feature switcher (uecvar)

Per-game Unreal Engine console-variable profile, applied at startup, that turns off or down the
features KosmicKrisp/Apple cannot do or afford. Generic tool, first profile for PPSA30552
("A Game About Digging A Hole", UE 5.6).

## Pieces

| Piece | Where |
|---|---|
| Scanner / checker / overlay writer | `cmd/uecvar`, `pkg/uecvar` (tests: `go test ./pkg/uecvar`) |
| Profile (names and values only) | `profiles/ppsa30552.uecvars` |
| Apply mechanism | VFS overlay in `pkg/runtime/kernel/vfs/ps4_vfs.c` (`NPS5_OVERLAY_DIR`) |

```
uecvar scan  <eboot.bin>                      # cvar-looking names (UTF-16/32/ASCII strings), one per line
uecvar check <profile.uecvars> <eboot.bin>    # reports profile names the binary does not register (not fatal)
uecvar emit  <profile.uecvars> <overlay-dir> [-app-dir <dump>]   # writes Engine.ini (+ UECommandLine.txt for @cmdline)
```

Profile format: `name=value` per line, `#` comments, `[Section]` switches the ini section
(default `SystemSettings`), `@cmdline <args>` appends to UECommandLine.txt.

## Options investigated

Facts from the title (UE log + eboot strings, nothing written to the dump):

* The eboot is a **Shipping-style** build: no `ExecCmds`, no `-cvar`, no `ConsoleVariables.ini`
  string in the binary. So (b) `-ExecCmds` is not available. It does read `/app0/uecommandline.txt`
  ("Using command-line arguments from /app0/uecommandline.txt!"; content is only `-project=...`),
  but a command line cannot set cvars here.
* UE reads Engine.ini layers from the pak (Base/Default*) then from disk; the highest layer is
  `<Saved>/Config/PS5/Engine.ini`, and `[SystemSettings]` there applies cvars at
  `SetBySystemSettingsIni`, above the project's settings. The title probes `/app0/engine/saved/config/ps5/`
  (editorsettings.ini, Manifest.ini) and `/download0/digginggame/saved/...`.
* The cvars are registered by name as UTF-16 strings (4,966 candidate names, 3,123 `r.*`), all the
  ones we care about present (list kept under `work/ppsa30552/uecvar/`, not committed).

| Option | Verdict |
|---|---|
| (a) config injection via a VFS overlay | **Chosen.** Zero guest changes, works for ReadOnly cvars (ini is the only way to set those, and they must be set before RHI init), generic across UE titles, nothing written to /app0. |
| (b) `-ExecCmds` / command line | Not compiled into Shipping. `@cmdline` is still supported for titles that do have it. |
| (c) static patch of TAutoConsoleVariable defaults at translate time | Works on any build (nxnative does it; its host-side writer needs ARM and a known registration sequence, here x86-64 and UTF-16 names), but needs a registration-sequence xref pass per UE version, and fixes defaults only. Fallback if ini injection proves insufficient. |
| (d) runtime hook on registration (`m3-mallochle` function replacement) | Most flexible (can force any value after ini), but needs per-title addresses and a guest-callable shim. Fallback only. |

## Mechanism

`ps4_vfs_init` mounts every regular file under `NPS5_OVERLAY_DIR` (default
`$NPS5_USERDATA_DIR/overlay` when that exists) read-only at the same relative path under `/app0`.
`/app0` stays EROFS and the dump is never written (run.sh still verifies the manifest). Guest path
matching for mounts is now case-insensitive (the console file system is).

```
uecvar emit profiles/ppsa30552.uecvars $NPS5_USERDATA_DIR/overlay
```

writes `engine/saved/config/ps5/engine.ini` and `digginggame/saved/config/ps5/engine.ini`.

## Results

See the section "Test evidence" below (filled from the runs).

### Test evidence (PPSA30552, build out9uc = out9 + this runtime)

Reproduce (paths under `work/ppsa30552/`, never committed):

```
go build -o /tmp/uecvar ./cmd/uecvar
/tmp/uecvar check profiles/ppsa30552.uecvars <dump>/eboot.bin          # 59 entries, 0 unknown
/tmp/uecvar emit  profiles/ppsa30552.uecvars boot/userdata/overlay      # -> digginggame/config/ps5/ps5engine.ini
NPS5_VFS_TRACE_INI=1 boot/run.sh ...                                    # trace of every *.ini lookup/read
```

* Scanner: 4,966 cvar-looking names in the eboot (3,123 `r.*`), all 59 profile names found.
  A profile with a bogus name (`r.NotARealVariable...`) is reported as `UNKNOWN line N` by `check`; at
  run time the engine ignores it (the game ran identically, no fatal).
* Where UE looks (from `NPS5_VFS_TRACE_INI=1`): `/app0/digginggame/config/ps5/ps5engine.ini`,
  `/app0/engine/config/ps5/ps5engine.ini`, `.../sony/sonyengine.ini`, `.../generated*.ini`, and
  `/download0/digginggame/saved/ps5/engine.ini` (the generated layer, under the writable data root).
  They are probed by `sceKernelStat` and read through the **APR** async-file API
  (`sceKernelAprResolveFilepathsToIds...` then Ampr preads), which also goes through `ps4_vfs_resolve`,
  so the overlay applies there too. Pak-resident configs never touch the VFS.
* Proof the overlay is consumed: `[apr] ini read: <overlay>/digginggame/config/ps5/ps5engine.ini off=0 got=1569`
  (three times per boot: the three config loads), dump manifest unchanged after every run.
* (Proven later, see "Read-back" below.) **Was not proven at this point: that the values took effect.** The shipping build prints no UE log lines, and no
  rendering difference is visible before the mesh-shader abort (identical shader registrations, same
  abort in frame 1 with and without the overlay; `t.MaxFPS=1` also did not change timing because the
  abort comes in the first tick). Reading the cvar back needs the registration xref (option c/d below).

### Mesh shaders

With the full profile (r.Nanite=0, r.Nanite.MeshShaderRasterization=0, r.Nanite.PrimShaderRasterization=0, ...)
loaded, frame 1 still aborts with `AGC graphics: device does not support mesh shaders`. The failing draw
is a vertex shader of stage 7 with a pixel shader of stage 5 and `PA_CL_VS_OUT_CNTL` layer/viewport
exports, i.e. an ordinary NGG (primitive shader) vertex stage that AnyPS5 lowers to MeshEXT, not
necessarily a Nanite raster pass. `r.Agc.GsFlags` and `r.Agc.CxFiltering` exist in the binary and are
the likely switches for NGG culling/GS behaviour on this RHI; their value semantics are unknown, so they
are left out of the profile. Conclusion: the Nanite/mesh cvars alone do **not** demonstrably sidestep the
mesh path; the mesh-shader work (wt/anyps5-mesh) stays necessary.

## Read-back (m3-uecvar2)

### Finding the registrations (`uecvar xref`)

```
uecvar xref [-o out.json] [-stats] <eboot>            # name -> kind, type, default, storage VA, flags (JSON; game data, keep under work/)
uecvar dumplist [-base 0x400000] [-profile p] <xref.json>   # NPS5_UECVAR_LIST for the runtime dump
uecvar dumpcmp [-profile p] [-xref x.json] <baseline-dump> <overlay-dump>
uecvar refs <eboot> <va>...                           # instructions that address a global rip-relatively
uecvar dis  <eboot> <va> <bytes>                      # Intel listing
```

Method (x86-64 counterpart of nxnative `ue_cvar.py`): UTF-16 literals that look like cvar names in the
non-executable segments; every `lea reg,[rip+d]` in the code segment that loads one; then a small
register tracker (lea, mov imm, xor, movss/vmovss) over the instructions around it up to the call.
UE registers a variable with an `IConsoleManager` virtual call, name in `rsi`, `this` in `rdi`:

| vtable offset | registration | default |
|---|---|---|
| 0x00 / 0x08 / 0x10 / 0x18 | `TAutoConsoleVariable` bool / int / float / FString | `edx` (bool, int, `mov edx,imm`), `xmm0` (float, `vmovss xmm0,[rip+k]`), flags `r8d` (`ecx` for float) |
| 0x20 FString&, 0x28 bool&, 0x30 int&, 0x38 float&, 0x40 TArray<FString>& | `FAutoConsoleVariableRef` | the global (`lea rdx,[rip+g]`) is the storage; its initial image value is the default |

Slot types were fixed from the allocation size of each `FConsoleManager` method and from the initial
values of the Ref globals (0x28: packed bytes 0x01010101, 0x38: float bit patterns, 0x30: small ints and -1).
The returned `IConsoleVariable*` is wrapped by `FAutoConsoleObject::FAutoConsoleObject(this, ivar)`
(a direct call with `rdi` = the `lea` of the global object, `rsi` = the returned pointer): that object
address is the "storage" of a `TAutoConsoleVariable`.

PPSA30552: 4,915 names, 4,751 registrations (1,539 int, 466 float, 128 bool, 5 FString `TAutoConsoleVariable`; the
Refs: bool&/int&/float&/FString&/TArray). The ~160 names without one are console commands and strings that
only look like names. All 58 profile names resolve. Addresses are linked VAs; the eboot loads at guest 0x400000 + VA.

Value layout (from `FConsoleManager::RegisterConsoleVariable`'s constructor, confirmed by the dump):
`FAutoConsoleObject` is `[+0] vtable, [+8] IConsoleVariable*`; the `FConsoleVariable<T>` it points at holds
`TConsoleVariableData<T>` values at `+0x50` (game thread) and `+0x54` (render thread; bool is a byte at +0x50),
flags at `+0x18`.

### Runtime dump (`NPS5_UECVAR_DUMP`)

`pkg/runtime/core/nps5_uecvar.c` (a constructor that starts one reader thread; no other runtime file is touched):

```
NPS5_UECVAR_DUMP=<file>   NPS5_UECVAR_LIST=<list>   NPS5_UECVAR_AT=25,50,75   # seconds after start
```

Read-only: every read goes through `mach_vm_read_overwrite`, a stale address prints `unmapped`. Lines:
`name kind type value raw=<hex of the first 0x58 bytes>`. Tests: `go test ./pkg/runtime -run TestUecvarDump`
(builds `tests/uecvar_dump/dump.c` with the arena), `go test ./pkg/uecvar` (synthetic x86 for the xref finder).

### Result: the overlay values take effect

Build b2/out1 of the boot2 agent with this runtime (b2/uc1, runtime-only relink), run 100 s each with a fresh
`NPS5_USERDATA_DIR`, dump at 25/50/75 s (identical values at all three), dump manifest unchanged.

* Baseline (no overlay) shows the project's own values: e.g. `r.AntiAliasingMethod` 4 (static default) -> 2,
  `r.FXAA.Quality` 4 -> 1, the rest equal to the static defaults.
* Overlay run: 45 of the 58 profile cvars changed and all 58 read back as the profile value (the other 13 were
  already at it). Examples (static / baseline / overlay): `r.Nanite` 1/1/0, `r.Nanite.MeshShaderRasterization` 1/1/0,
  `r.Lumen.DiffuseIndirect.Allow` 1/1/0, `r.ScreenPercentage` 0/0/50, `r.ShadowQuality` 5/5/2,
  `r.Shadow.MaxResolution` 2048/2048/1024, `r.AntiAliasingMethod` 4/2/1, `r.VolumetricFog` 1/1/0.
  Nothing re-sets them later (the dump at 75 s still shows the overlay values), so no DeviceProfile override and
  no overlay fix were needed; translate-time default patching (option c) is not needed for these.
* Both runs end in the known unrelated guest crash after `sceSaveDataMount MainSave -> NOT_FOUND`.

### r.Agc.GsFlags / r.Agc.CxFiltering

See the findings section at the end of this file.

## r.Agc.GsFlags and r.Agc.CxFiltering: findings

Both are `FAutoConsoleVariableRef` int globals registered next to each other (storage VA 0x6c613ec and 0x6c613f0,
flags 0x20 = RenderThreadSafe, so the ini can set them), default 1 and 1, read at run time as 1 and 1. Found with
`uecvar refs`:

* `r.Agc.CxFiltering` has one reader, 0x3ea1123, at the top of a function that takes (this, a state block, a cached
  value): `if (CxFiltering && cached == new && new != -1) skip the context-register write`. It is a redundant
  context-register (CX) write filter of the AGC command-buffer builder (state caching), a performance feature.
  Not related to NGG; turning it off only emits more register writes.
* `r.Agc.GsFlags` has three readers (0x3e9fe32, 0x3e9fed4, 0x3e9ff0e) in one pipeline-state creation function that walks the
  stage shaders of a pipeline (slots [state+0x18..0x38], each `[shader+0xa0]` the compiled program). Only `!= 0` is
  tested. When set, it asks the shader for entry 6 of its 16-bit slot table (0xffff = absent) and, if present, sets the
  byte at state+0xac. The code does not show whether that is the NGG / primitive-shader selection or a GS-stage
  user-data flag, so the meaning is not established.
* Experiment: overlay `r.Agc.GsFlags=0` + `r.Agc.CxFiltering=0` is applied (read back 0 and 0 at 40 s), and the run
  behaves like the baseline up to the same guest crash. No difference in the AGC/mesh path was visible in the logs.

Decision: neither is in the profile (CxFiltering is harmless but only costs; GsFlags is unexplained).
The way to settle GsFlags is to log, in AnyPS5, the VGT_SHADER_STAGES_EN of the first mesh draw with GsFlags 1 versus 0
(needs the mesh-shader build to reach those draws).

### Next steps

1. With the mesh-shader build past frame 1, re-run with the profile and compare frame time and the shader set against the
   unmodified run (also check that `r.Nanite=0` does not make meshes vanish: cooked Nanite meshes may have stripped fallbacks).
2. `NPS5_UECVAR_AT` is seconds only; a frame-count trigger needs a hook in the flip path.

## Setting values and the real resolution switch (NPS5_UECVAR_SET, NPS5_TITLE_RESOLUTION)

Finding (PPSA30552): the title always registers a 3840x2160 presentation buffer (`sceVideoOutRegisterBuffers2`); it calls
`sceVideoOutGetOutputStatus` once and ignores the answer (`APS5_OUTPUT_RESOLUTION` / `ANYPS5_VIDEO_OUT_MAX_*` change nothing).
The scene is rendered at `r.DynamicRes.{Min,Max}ScreenPercentage` of 4K (50 / 74.07 = 2848x1600 at the top) because
`r.DynamicRes.OperationMode` is 2. Those three cvars are set at ConsoleVariablesIni priority by the game itself, and an overlay
`[SystemSettings]` or `[ConsoleVariables]` section does not change them (read back after 25 s/45 s).

`core/nps5_uecvar.c` can now write: `NPS5_UECVAR_SET=name=value,...` (needs `NPS5_UECVAR_LIST`; stores both thread copies of a
TAutoConsoleVariable at +0x50/+0x54, or the bound global, every 100 ms for `NPS5_UECVAR_SET_FOR` s, default 120) and
`NPS5_TITLE_RESOLUTION=WxH` (<= 3840x2160; needs `NPS5_UECVAR_LIST` like `NPS5_UECVAR_SET`, else it says so and sets nothing), which sets `r.ScreenPercentage`, `r.DynamicRes.MinScreenPercentage` and
`MaxScreenPercentage` to the percentage of 4K. Result with 1280x720: scene targets 1280x720 (was 2848x1600); presentation
and UI stay 3840x2160. Re-applying stops after `NPS5_UECVAR_SET_FOR` seconds (default 120) by design: afterwards the title's own value applies (a scene resolution reverts to its dynamic resolution) and a line is logged; use a large value to keep it for the whole run. Stores go through `mach_vm_write`, so a cvar in a read-only page is skipped instead of crashing the helper thread.
