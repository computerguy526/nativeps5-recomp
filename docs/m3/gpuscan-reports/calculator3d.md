# GPU feature scan: Calculator3D

Schema `gpuscan-report` v1, gpuscan 0.1.0. Generated 2026-11-14T20:37:06Z.

**Device**: Apple M4 Pro, driver KosmicKrisp Mesa 27.0.0-devel (git-03ac14ac03), Vulkan 1.4.368 (vulkaninfo:kosmickrisp_mesa_icd.aarch64.json)

**Inputs**: 9 shader modules (9 from cache dirs, resource-plan entries skipped: 5), 1 pipeline journal(s), title `$NPS5_ROOT/work/s3/rosetta/extract`

## Ranked blockers

No blocker-severity feature is both used and unsupported on this device.

## Limits headroom

| check | status | evidence |
|---|---|---|
| `limits.descriptor_sets` | yes | highest set index 0 vs maxBoundDescriptorSets=32 |
| `limits.descriptors` | yes | peak per-stage counts map[storageBuffer:3] vs per-stage limits |
| `limits.push_constants` | yes | peak 128 B vs maxPushConstantsSize=256 |
| `limits.shared_memory` | yes | peak 0 B vs maxComputeSharedMemorySize=32768 |

## Used and supported

| feature | needed by | evidence |
|---|---|---|
| `cap.int64` | 2 shaders | shaderInt64=true |
| `float.signed_zero_inf_nan_preserve.32` | 9 shaders | shaderSignedZeroInfNanPreserveFloat32=true |
| `pipe.format.color.VK_FORMAT_B8G8R8A8_UNORM` | 0 shaders, 2 pipelines | optimal tiling COLOR_ATTACHMENT |
| `shader.clip_distance` | 4 shaders | shaderClipDistance=true |
| `subgroup.ops.ballot` | 4 shaders | VK_SUBGROUP_FEATURE_BALLOT_BIT |

## Pipelines (journal)

2 graphics, 0 compute, 4 modules. Stage combinations: Vertex+Fragment=2. Topologies: TRIANGLE_LIST=2. Sample counts: 1=2.

Color formats: VK_FORMAT_B8G8R8A8_UNORM=2. Depth formats: none.

## Shaders

9 modules; by stage: Fragment=5, Vertex=4. Peaks: shared memory 0 B, push constants 128 B, highest descriptor set 0, unbounded descriptor arrays in 0 shaders.

Shaders needing an unsupported feature (full per-shader detail is in the JSON):

| shader | stage | unsupported features needed |
|---|---|---|
| none | | |

## Title (static)

2 modules scanned.

GPU imports by library: libSceAgc=15, libSceVideoOut=11, libSceAgcDriver=1.

<details><summary>27 GPU-related imports</summary>

`sceAgcCbReleaseMem`, `sceAgcCreateShader`, `sceAgcDcbAcquireMem`, `sceAgcDcbContextStateOp`, `sceAgcDcbDrawIndexAuto`, `sceAgcDcbSetCxRegistersIndirect`, `sceAgcDcbSetFlip`, `sceAgcDcbSetNumInstances`, `sceAgcDcbSetShRegisterDirect`, `sceAgcDcbSetShRegistersIndirect`, `sceAgcDcbSetUcRegistersIndirect`, `sceAgcGetRegisterDefaults`, `sceAgcInit`, `sceAgcLinkShaders`, `sceAgcSuspendPoint`, `sceAgcDriverSubmitDcb`, `sceVideoOutConfigureOutput`, `sceVideoOutGetBufferLabelAddress`, `sceVideoOutGetFlipStatus`, `sceVideoOutGetOutputStatus`, `sceVideoOutInitializeOutputOptions`, `sceVideoOutIsOutputSupported`, `sceVideoOutOpen`, `sceVideoOutRegisterBuffers2`, `sceVideoOutSetBufferAttribute2`, `sceVideoOutSubmitFlip`, `sceVideoOutWaitVblank`

</details>

Embedded AGC shader headers in module images: 4 (Gs=2, Ps=2), 20656 bytes of code.


