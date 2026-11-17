# gpuscan: GPU feature scanner for nativeps5

`cmd/gpuscan` answers one question for any PS5 title: **which GPU features does it need, which of them does the
target Vulkan device (Mesa KosmicKrisp on Metal) lack, and in what order should they be fixed?** The output is a
versioned JSON report (for agents and for `--compare`) plus a short Markdown summary (for people).

It does not decode RDNA itself. Shaders are analysed as the **SPIR-V that AnyPS5's RDNA recompiler already produced**
(from its shader disk cache and pipeline journal), so a feature that shows up in a report is a feature the
recompiled shader really declares. Raw AGC/RDNA decoding, when needed, goes through AnyPS5's arm64 tools
(`agc_frame_replay`, `agc_shader_replay`).

Build and run (arm64 only, never executes x86):

    GOFLAGS=-mod=vendor go build -o gpuscan ./cmd/gpuscan
    LIFTER_NO_X86=1 GOFLAGS=-mod=vendor go test ./cmd/gpuscan

External tools used: `vulkaninfo` (device capabilities), `zstd` (journal shader modules are zstd frames; Go has no
zstd in its standard library), `spirv-as` (tests only). All from Homebrew.

## Usage

    gpuscan -title DIR -cache RUNDIR -icd INSTALL_OR_ICD_JSON -json out.json -md out.md [-known exec.mesh,exec.task]
    gpuscan -compare A.json B.json [-json delta.json] [-md delta.md]

| flag | meaning |
|---|---|
| `-title DIR` | PS5 title dir (`eboot.bin`, `sce_module/*.prx`, `sce_sys/param.json`). Enumerates GPU imports with names, scans module images for embedded AGC shader headers, greps the executable for engine hints. |
| `-cache DIR` (repeatable) | any directory of a run (a `w` workdir, `home/cache/anyps5`, the hash dir itself). Searched (depth 6) for the shader disk cache `<hash>.bin` files and `pipelines.journal`. |
| `-icd PATH` | KosmicKrisp install dir (`.../install`), an ICD manifest, or a saved `vulkaninfo --json` file. Default: the system loader. The `vulkaninfo` result is cached in `$XDG_CACHE_HOME`/`~/Library/Caches/gpuscan/` keyed by manifest + library size/mtime; `-no-device-cache` forces a re-run. |
| `-spv FILE_OR_DIR` | extra `.spv` files to analyse. |
| `-capture DIR`, `-req FILE`, `-anyps5-build DIR` | run AnyPS5's `agc_frame_replay` on an `APS5_FRAME_CAPTURE` dir into a scratch shader cache (then scanned), or `agc_shader_replay --spv` on saved recompile requests. **Untested**: no captures or `.req` files existed when this was written. |
| `-known ids` | feature ids already being worked on. The report lists `blockersAfterKnown`. |
| `-skip-module S`, `-max-module-mb N` | skip big modules (e.g. `kitt_webkit`) when scanning a title. |
| `-no-timestamp` | reproducible output. |
| `-j N` | parallel zstd decodes (default 3). |
| `-compare A B` | diff two reports (two games, or before/after a driver patch). |

Without `-json`/`-md` the JSON goes to stdout.

### What is analysed

* **Per SPIR-V module** (binary walk, no spirv-dis): capabilities, extensions, execution models and modes (with
  operands: `OutputVertices.64`, `DenormPreserve.32`, `SubgroupSize.64`), 8/16/32/64-bit types, atomics classified
  by element type x storage class (int64 buffer/shared/image, float16/32/64 add/min-max/other, int32 image),
  subgroup operation classes (basic, vote, ballot, shuffle, shuffle-relative, arithmetic, clustered, quad, rotate)
  and the stage they occur in, demote/terminate/kill, workgroup memory bytes, push-constant bytes, descriptor counts
  per type (arrays expanded, runtime arrays flagged), highest descriptor set, storage image formats, sparse
  residency ops, fragment interlock, barycentrics, VRS, stencil export, float controls, transform feedback, multiview,
  layer/viewport index outputs, and every other capability as `cap.<Name>` (support `unknown`, never dropped).
* **Pipeline journal** (`pipelines.journal`, AnyPS5 journal format 18; decoded by `decodeRecipe` mirroring
  `PipelineJournal.cpp`): stage combinations, mesh/tessellation/geometry paths, MSAA sample counts, custom sample
  locations, per-sample shading, depth bounds/clamp/bias clamp, depth clip control, dual-source blend, polygon
  mode, wide lines, conservative raster, provoking vertex, primitive restart, topologies, colour/depth attachment
  formats vs the device format table. Journal modules are also analysed as shaders. If AnyPS5 bumps the journal
  format the decoder reports "unreadable records" and the pipeline section degrades; update `decodeRecipe`.
* **Title** (static): GPU imports (libSceAgc*, libSceGnm*, libSceVideoOut*, libSceAmpr*, libSceGpu*, libSceRazorCpu)
  with names (a NID table is computed from `gpunames.txt`, the export names of AnyPS5's GPU libraries), embedded
  AGC `Shader` headers (magic `0x34333231`, version `0x18`: count by binary type and target, code bytes), engine
  string and UE console variable names (Nanite, Lumen, ray tracing, VRS, virtual shadow maps), and import-derived
  predictions (async compute `sceAgcAcb*`, GDS, predication). Hints and import rules produce **predicted**
  features: they appear in `predictedBlockers`, never in `blockers`.
* **Device** (`vulkaninfo --json`): features (all feature structs flattened; an absent struct reads as unsupported),
  extensions, limits, per-format optimal-tiling features, subgroup stages/operations, queue count.

Not observable from SPIR-V or the journal today: sampler features (custom border colour, min/max filter reduction),
multiview *draws*, depth-bounds use that the driver emulates, anything inside shaders that were never compiled
(the cache only contains shaders the title reached). Static RDNA decoding of the embedded shader headers is not
done; only their stage/size/target are counted.

## JSON schema (version 1)

`schema` is `"gpuscan-report"`, `schemaVersion` is `1`. Adding fields keeps version 1; renaming, removing or
re-meaning a field bumps it. `gpuscan -compare` refuses newer versions. `docs/m3/gpuscan.schema.json` is the
machine-readable (JSON Schema draft 2020-12) form of the top level.

    schema, schemaVersion, tool, generated?, name
    inputs      { title?, cacheDirs[], captures[], cacheShaderModules, cacheResourcePlanEntries, journals, device }
    device      { source, name, driver, apiVersion, features{name:bool}, limits{name:number}, extensions[], computeCapableQueues }
    summary     { shaders, shadersByStage{}, featuresUsed, unsupportedFeatures, blockers, unknownSupport,
                  peaks{sharedMemoryBytes, pushConstantBytes, maxDescriptorSet, descriptorsPerStage{}, ...}, spirvExtensions{} }
    blockers[]            ranked feature ids: used + unsupported/partial + severity "blocker"
    blockersAfterKnown[]  blockers minus -known (only present when -known was given)
    predictedBlockers[]   unsupported blocker/major features only predicted by title hints/imports
    known[]
    features[]  { id, category, title, severity, supported, evidence, workaround, rank?, known?, predicted?,
                  neededBy{shaders, shaderIds[<=25], pipelines, hints[]}, alsoNeededByThoseShaders{id:count}? }
    pipelines   { source, journalFormat, graphics, compute, modules, stageCombinations{}, topologies{}, sampleCounts{},
                  colorFormats{}, depthFormats{}, notes[] }
    shaders[]   { id, stage, executionModels[], words, spirvVersion, capabilities[], extensions[], executionModes[],
                  features[], sharedMemoryBytes?, pushConstantBytes?, descriptors{type:count}?, maxDescriptorSet?,
                  unboundedDescriptorArray?, storageImageFormats[]?, localSize[]?, origin }
    title       { path, titleId?, name?, engine?, modules[], gpuImports[{library,name,nid,nameKnown,modules[]}],
                  gpuLibraries{}, importedLibraries{}, embeddedShaders{headers,byType,byTarget,codeBytes}, hints[], notes[] }
    notes[]

* `supported`: `yes | no | partial | unknown` (`unknown` = no device data or no device mapping for that capability).
* `severity` applies when the feature is unsupported: `blocker` (shader/pipeline cannot be created or the draw is
  lost), `major` (renders wrongly or needs emulation to be correct), `minor`, `info`.
* `rank` (1 = worst) is set for every used, unsupported or partial feature: severity, then number of shaders +
  pipelines using it, then id. Predicted features rank after all proven ones.
* Feature ids are stable and dotted: `exec.mesh`, `atomics.int64.buffer`, `subgroup.ops.ballot`,
  `subgroup.stage.vertex_pipeline`, `float.denorm_preserve.32`, `image.format.Rgba8`, `pipe.msaa.8`,
  `pipe.format.depth.VK_FORMAT_D24_UNORM_S8_UINT`, `limits.shared_memory`, `cap.<SpirvCapability>`.
  The catalogue is `staticDefs` in `cmd/gpuscan/features.go`; parameterised families are synthesised by
  `LookupFeature`.
* Shader ids are the first 16 hex digits of the SHA-1 of the SPIR-V words (content addressed, so two reports share
  ids for identical shaders). Reports contain hashes and statistics only, never shader bytes.
* `shaders[].origin`: `cache`, `journal`, `cache+journal`, `spv`.

## Feeding a report to an agent

1. Read `blockersAfterKnown` (or `blockers`), then the matching `features[]` entries: each has `evidence` (the device
   field that decided it), `workaround`, and `neededBy.shaderIds` (take one id and look at `shaders[]` for its modes
   and descriptors).
2. For a shader id, the module itself lives in the AnyPS5 shader cache (`<hash>.bin`; SPIR-V starts 12 bytes after
   the payload offset, see `SPIRVFromCacheEntry`) or can be re-dumped with `agc_shader_replay --spv`.
3. After a driver change, regenerate and run `gpuscan -compare before.json after.json`: `resolvedInB` lists features
   that stopped being blockers, `regressedInB` new ones.
4. Treat `predictedBlockers` as a checklist of things to verify, not as facts.

Prompt skeleton:

> Here is a gpuscan report for <title> against KosmicKrisp. Take the first entry of `blockersAfterKnown`, read its
> `workaround` and `evidence`, find where AnyPS5 (core/libs/prx/libSceAgcDriver) creates the affected pipeline, and
> propose the smallest change. List which other features share the same shaders (`alsoNeededByThoseShaders`).

## Reports in this repo

`docs/m3/gpuscan-reports/` holds the Calculator3D and Greenhollow homebrew reports (JSON + Markdown). Reports of
commercial titles are kept outside the repo.

## Prior art

Design ideas (stable finding ids that an agent can act on, a severity vocabulary, scanning the binary for engine
console-variable names) come from nxnative's `dm/tools` census/triage scripts (MIT, read-only clone at
`repos/nxnative`). No code was copied, so no NOTICE entry is needed.
