package main

import (
	"encoding/binary"
	"encoding/json"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"ps4-recomp/pkg/elfloader"
)

// asm assembles SPIR-V text with spirv-as (Homebrew spirv-tools); tests skip when it is missing.
func asm(t *testing.T, text string) []uint32 {
	t.Helper()
	bin, err := exec.LookPath("spirv-as")
	if err != nil {
		t.Skip("spirv-as not installed")
	}
	dir := t.TempDir()
	in, out := filepath.Join(dir, "a.spvasm"), filepath.Join(dir, "a.spv")
	if err := os.WriteFile(in, []byte(text), 0o644); err != nil {
		t.Fatal(err)
	}
	if b, err := exec.Command(bin, "--target-env", "spv1.5", "-o", out, in).CombinedOutput(); err != nil {
		t.Fatalf("spirv-as: %v\n%s", err, b)
	}
	b, _ := os.ReadFile(out)
	w, ok := WordsFromBytes(b)
	if !ok {
		t.Fatal("bad module")
	}
	return w
}

func analyze(t *testing.T, text string) *ShaderInfo {
	t.Helper()
	s, err := AnalyzeSPIRV(asm(t, text))
	if err != nil {
		t.Fatal(err)
	}
	return s
}

func has(s *ShaderInfo, f string) bool {
	for _, x := range s.Features {
		if x == f {
			return true
		}
	}
	return false
}

func wantFeatures(t *testing.T, s *ShaderInfo, want ...string) {
	t.Helper()
	for _, f := range want {
		if !has(s, f) {
			t.Errorf("missing feature %s; got %v", f, s.Features)
		}
	}
}

const compHead = `OpCapability Shader
%CAPS%
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 8 4 2
%DECOR%
%void = OpTypeVoid
%fn = OpTypeFunction %void
%u32 = OpTypeInt 32 0
`

func comp(caps, decor, body string) string {
	s := strings.Replace(compHead, "%CAPS%", caps, 1)
	s = strings.Replace(s, "%DECOR%", decor, 1)
	return s + body
}

func TestMeshShader(t *testing.T) {
	s := analyze(t, `OpCapability Shader
OpCapability MeshShadingEXT
OpExtension "SPV_EXT_mesh_shader"
OpMemoryModel Logical GLSL450
OpEntryPoint MeshEXT %main "main"
OpExecutionMode %main LocalSize 32 1 1
OpExecutionMode %main OutputVertices 64
OpExecutionMode %main OutputPrimitivesEXT 126
OpExecutionMode %main OutputTrianglesEXT
%void = OpTypeVoid
%fn = OpTypeFunction %void
%main = OpFunction %void None %fn
%l = OpLabel
OpReturn
OpFunctionEnd
`)
	wantFeatures(t, s, "exec.mesh")
	if s.Stage != "MeshEXT" {
		t.Errorf("stage %s", s.Stage)
	}
	joined := strings.Join(s.Modes, ",")
	if !strings.Contains(joined, "OutputVertices.64") || !strings.Contains(joined, "OutputPrimitivesEXT.126") {
		t.Errorf("mesh modes: %v", s.Modes)
	}
}

func TestTaskAndGeometryStages(t *testing.T) {
	s := analyze(t, `OpCapability Shader
OpCapability MeshShadingEXT
OpMemoryModel Logical GLSL450
OpEntryPoint TaskEXT %main "main"
OpExecutionMode %main LocalSize 1 1 1
%void = OpTypeVoid
%fn = OpTypeFunction %void
%main = OpFunction %void None %fn
%l = OpLabel
OpReturn
OpFunctionEnd
`)
	wantFeatures(t, s, "exec.task", "exec.mesh")
	g := analyze(t, `OpCapability Shader
OpCapability Geometry
OpMemoryModel Logical GLSL450
OpEntryPoint Geometry %main "main"
OpExecutionMode %main InputPoints
OpExecutionMode %main Invocations 1
OpExecutionMode %main OutputPoints
OpExecutionMode %main OutputVertices 1
%void = OpTypeVoid
%fn = OpTypeFunction %void
%main = OpFunction %void None %fn
%l = OpLabel
OpReturn
OpFunctionEnd
`)
	wantFeatures(t, g, "exec.geometry")
}

const bufHead = `OpDecorate %block Block
OpMemberDecorate %block 0 Offset 0
OpDecorate %var DescriptorSet 0
OpDecorate %var Binding 0
`

func TestInt64BufferAtomic(t *testing.T) {
	s := analyze(t, comp("OpCapability Int64\nOpCapability Int64Atomics", bufHead, `%u64 = OpTypeInt 64 0
%block = OpTypeStruct %u64
%pblk = OpTypePointer StorageBuffer %block
%var = OpVariable %pblk StorageBuffer
%ptr = OpTypePointer StorageBuffer %u64
%c0 = OpConstant %u32 0
%c1 = OpConstant %u64 1
%scope = OpConstant %u32 1
%sem = OpConstant %u32 0
%main = OpFunction %void None %fn
%l = OpLabel
%p = OpAccessChain %ptr %var %c0
%r = OpAtomicIAdd %u64 %p %scope %sem %c1
OpReturn
OpFunctionEnd
`))
	wantFeatures(t, s, "atomics.int64.buffer", "cap.int64", "atomics.int64.any")
	if has(s, "atomics.int64.shared") {
		t.Error("buffer atomic classified as shared")
	}
	if s.Descriptors["storageBuffer"] != 1 {
		t.Errorf("descriptors %v", s.Descriptors)
	}
	if len(s.LocalSize) != 3 || s.LocalSize[0] != 8 {
		t.Errorf("local size %v", s.LocalSize)
	}
}

func TestInt64SharedAtomicAndSize(t *testing.T) {
	s := analyze(t, comp("OpCapability Int64\nOpCapability Int64Atomics", "", `%u64 = OpTypeInt 64 0
%c512 = OpConstant %u32 512
%arr = OpTypeArray %u64 %c512
%pw = OpTypePointer Workgroup %arr
%var = OpVariable %pw Workgroup
%pe = OpTypePointer Workgroup %u64
%c0 = OpConstant %u32 0
%c1 = OpConstant %u64 1
%scope = OpConstant %u32 1
%sem = OpConstant %u32 0
%main = OpFunction %void None %fn
%l = OpLabel
%p = OpAccessChain %pe %var %c0
%r = OpAtomicIAdd %u64 %p %scope %sem %c1
OpReturn
OpFunctionEnd
`))
	wantFeatures(t, s, "atomics.int64.shared")
	if s.SharedBytes != 4096 {
		t.Errorf("shared bytes = %d, want 4096", s.SharedBytes)
	}
}

func TestSharedMemoryOverLimit(t *testing.T) {
	s := analyze(t, comp("", "", `%c16k = OpConstant %u32 16384
%arr = OpTypeArray %u32 %c16k
%pw = OpTypePointer Workgroup %arr
%var = OpVariable %pw Workgroup
%main = OpFunction %void None %fn
%l = OpLabel
OpReturn
OpFunctionEnd
`))
	if s.SharedBytes != 65536 {
		t.Fatalf("shared bytes = %d, want 65536 (RDNA2 64 KiB LDS)", s.SharedBytes)
	}
	dev := testDevice(t)
	r := Build(BuildInput{Name: "x", Device: dev, Shaders: []*ShaderInfo{s}})
	f := r.feature("limits.shared_memory")
	if f.Supported != No || f.Needed.Shaders != 1 {
		t.Errorf("limits.shared_memory = %+v", f)
	}
	if len(r.Blockers) != 1 || r.Blockers[0] != "limits.shared_memory" {
		t.Errorf("blockers = %v", r.Blockers)
	}
}

func TestFloat64AndFloatAtomic(t *testing.T) {
	s := analyze(t, comp("OpCapability Float64", "", `%f64 = OpTypeFloat 64
%c = OpConstant %f64 1.5
%main = OpFunction %void None %fn
%l = OpLabel
%x = OpFAdd %f64 %c %c
OpReturn
OpFunctionEnd
`))
	wantFeatures(t, s, "cap.float64")
	a := analyze(t, comp("OpCapability AtomicFloat32AddEXT", "OpExtension \"SPV_EXT_shader_atomic_float_add\"\n"+bufHead, `%f32 = OpTypeFloat 32
%block = OpTypeStruct %f32
%pblk = OpTypePointer StorageBuffer %block
%var = OpVariable %pblk StorageBuffer
%ptr = OpTypePointer StorageBuffer %f32
%c0 = OpConstant %u32 0
%c1 = OpConstant %f32 1
%scope = OpConstant %u32 1
%sem = OpConstant %u32 0
%main = OpFunction %void None %fn
%l = OpLabel
%p = OpAccessChain %ptr %var %c0
%r = OpAtomicFAddEXT %f32 %p %scope %sem %c1
OpReturn
OpFunctionEnd
`))
	wantFeatures(t, a, "atomics.float32.buffer.add")
}

func TestSubgroupOpsAndStage(t *testing.T) {
	mk := func(model string) *ShaderInfo {
		return analyze(t, `OpCapability Shader
OpCapability GroupNonUniform
OpCapability GroupNonUniformBallot
OpCapability GroupNonUniformArithmetic
OpCapability GroupNonUniformClustered
OpMemoryModel Logical GLSL450
OpEntryPoint `+model+` %main "main"
`+map[string]string{"Vertex": "", "GLCompute": "OpExecutionMode %main LocalSize 1 1 1\n"}[model]+`%void = OpTypeVoid
%fn = OpTypeFunction %void
%u32 = OpTypeInt 32 0
%bool = OpTypeBool
%c3 = OpConstant %u32 3
%c0 = OpConstant %u32 0
%c4 = OpConstant %u32 4
%t = OpConstantTrue %bool
%v4 = OpTypeVector %u32 4
%main = OpFunction %void None %fn
%l = OpLabel
%b = OpGroupNonUniformBallot %v4 %c3 %t
%s = OpGroupNonUniformIAdd %u32 %c3 Reduce %c0
%c = OpGroupNonUniformIAdd %u32 %c3 ClusteredReduce %c0 %c4
OpReturn
OpFunctionEnd
`)
	}
	v := mk("Vertex")
	wantFeatures(t, v, "subgroup.ops.ballot", "subgroup.ops.arithmetic", "subgroup.ops.clustered", "subgroup.stage.vertex_pipeline")
	c := mk("GLCompute")
	if has(c, "subgroup.stage.vertex_pipeline") {
		t.Error("compute shader flagged as vertex-pipeline subgroup user")
	}
	wantFeatures(t, c, "subgroup.ops.ballot")
	// device judgement: KK-like device lacks subgroup ops in the vertex stage
	r := Build(BuildInput{Name: "x", Device: testDevice(t), Shaders: []*ShaderInfo{v}})
	if f := r.feature("subgroup.stage.vertex_pipeline"); f.Supported != No {
		t.Errorf("vertex-stage subgroup = %+v", f)
	}
}

func TestDemoteAndKill(t *testing.T) {
	s := analyze(t, `OpCapability Shader
OpCapability DemoteToHelperInvocation
OpExtension "SPV_EXT_demote_to_helper_invocation"
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main"
OpExecutionMode %main OriginUpperLeft
%void = OpTypeVoid
%fn = OpTypeFunction %void
%main = OpFunction %void None %fn
%l = OpLabel
OpDemoteToHelperInvocation
OpKill
OpFunctionEnd
`)
	wantFeatures(t, s, "shader.demote", "shader.kill")
}

func TestFragmentInterlockBarycentricAndFloatModes(t *testing.T) {
	s := analyze(t, `OpCapability Shader
OpCapability FragmentShaderPixelInterlockEXT
OpCapability FragmentBarycentricKHR
OpCapability DenormPreserve
OpCapability SignedZeroInfNanPreserve
OpExtension "SPV_EXT_fragment_shader_interlock"
OpExtension "SPV_KHR_fragment_shader_barycentric"
OpExtension "SPV_KHR_float_controls"
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %bary
OpExecutionMode %main OriginUpperLeft
OpExecutionMode %main PixelInterlockOrderedEXT
OpExecutionMode %main DenormPreserve 32
OpExecutionMode %main SignedZeroInfNanPreserve 32
OpDecorate %bary BuiltIn BaryCoordKHR
%void = OpTypeVoid
%fn = OpTypeFunction %void
%f32 = OpTypeFloat 32
%v3 = OpTypeVector %f32 3
%pin = OpTypePointer Input %v3
%bary = OpVariable %pin Input
%main = OpFunction %void None %fn
%l = OpLabel
OpReturn
OpFunctionEnd
`)
	wantFeatures(t, s, "frag.interlock", "frag.barycentric", "float.denorm_preserve.32", "float.signed_zero_inf_nan_preserve.32")
	r := Build(BuildInput{Name: "x", Device: testDevice(t), Shaders: []*ShaderInfo{s}})
	for id, want := range map[string]string{"frag.interlock": No, "frag.barycentric": No, "float.denorm_preserve.32": No, "float.signed_zero_inf_nan_preserve.32": Yes} {
		if got := r.feature(id).Supported; got != want {
			t.Errorf("%s supported=%s want %s", id, got, want)
		}
	}
	if len(r.Blockers) == 0 || r.Blockers[0] != "frag.interlock" {
		t.Errorf("blocker order %v", r.Blockers)
	}
}

func TestStorageImageFormatAndDescriptors(t *testing.T) {
	s := analyze(t, comp("OpCapability StorageImageExtendedFormats", `OpDecorate %img DescriptorSet 2
OpDecorate %img Binding 0
OpDecorate %samp DescriptorSet 0
OpDecorate %samp Binding 1
`, `%f32 = OpTypeFloat 32
%image = OpTypeImage %f32 2D 0 0 0 2 R11fG11fB10f
%pimg = OpTypePointer UniformConstant %image
%img = OpVariable %pimg UniformConstant
%sampler = OpTypeSampler
%c4 = OpConstant %u32 4
%sarr = OpTypeArray %sampler %c4
%psarr = OpTypePointer UniformConstant %sarr
%samp = OpVariable %psarr UniformConstant
%main = OpFunction %void None %fn
%l = OpLabel
OpReturn
OpFunctionEnd
`))
	wantFeatures(t, s, "image.format.R11fG11fB10f", "tex.storage_extended_formats")
	if s.Descriptors["storageImage"] != 1 || s.Descriptors["sampler"] != 4 || s.MaxSet != 2 {
		t.Errorf("descriptors=%v maxset=%d", s.Descriptors, s.MaxSet)
	}
}

func TestRuntimeDescriptorArrayAndSparse(t *testing.T) {
	s := analyze(t, `OpCapability Shader
OpCapability RuntimeDescriptorArray
OpCapability SparseResidency
OpCapability ImageGatherExtended
OpExtension "SPV_EXT_descriptor_indexing"
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main"
OpExecutionMode %main OriginUpperLeft
OpDecorate %arr DescriptorSet 0
OpDecorate %arr Binding 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%f32 = OpTypeFloat 32
%image = OpTypeImage %f32 2D 0 0 0 1 Unknown
%rt = OpTypeRuntimeArray %image
%p = OpTypePointer UniformConstant %rt
%arr = OpVariable %p UniformConstant
%main = OpFunction %void None %fn
%l = OpLabel
OpReturn
OpFunctionEnd
`)
	wantFeatures(t, s, "desc.runtime_array", "sparse.residency", "tex.gather_extended")
	if !s.Unbounded {
		t.Error("unbounded flag not set")
	}
}

func TestUnmappedCapabilityIsNotDropped(t *testing.T) {
	s := analyze(t, comp("OpCapability Float16", "", `%main = OpFunction %void None %fn
%l = OpLabel
OpReturn
OpFunctionEnd
`))
	wantFeatures(t, s, "cap.float16")
	if d := LookupFeature("cap.TotallyNew"); d.Check == nil {
		t.Error("synthesised capability def has no check")
	} else if sup, _ := d.Check(testDevice(t)); sup != Unknown {
		t.Errorf("unmapped capability support = %s, want unknown", sup)
	}
}

func TestAnalyzeRejectsGarbage(t *testing.T) {
	if _, err := AnalyzeSPIRV([]uint32{1, 2, 3, 4, 5}); err == nil {
		t.Error("expected error")
	}
	if _, err := AnalyzeSPIRV([]uint32{0x07230203, 0x10500, 0, 10, 0, 0}); err == nil {
		t.Error("expected malformed-instruction error")
	}
}

// ---- device -------------------------------------------------------------------------------------

const miniVulkanInfo = `{"capabilities":{"device":{
"extensions":{"VK_EXT_depth_clip_control":1,"VK_EXT_conditional_rendering":1},
"features":{"VkPhysicalDeviceFeatures":{"geometryShader":true,"shaderFloat64":false,"shaderInt64":true,"depthBounds":false,"sampleRateShading":true},
 "VkPhysicalDeviceVulkan12Features":{"shaderBufferInt64Atomics":false,"bufferDeviceAddress":true}},
"properties":{"VkPhysicalDeviceProperties":{"apiVersion":4206592,"deviceName":"Test GPU","limits":{"maxComputeSharedMemorySize":32768,"maxPushConstantsSize":256,"maxBoundDescriptorSets":32,"framebufferColorSampleCounts":["VK_SAMPLE_COUNT_1_BIT","VK_SAMPLE_COUNT_4_BIT"]}},
 "VkPhysicalDeviceVulkan11Properties":{"subgroupSize":32,"subgroupSupportedStages":["VK_SHADER_STAGE_FRAGMENT_BIT","VK_SHADER_STAGE_COMPUTE_BIT"],"subgroupSupportedOperations":["VK_SUBGROUP_FEATURE_BASIC_BIT","VK_SUBGROUP_FEATURE_BALLOT_BIT","VK_SUBGROUP_FEATURE_ARITHMETIC_BIT"]},
 "VkPhysicalDeviceVulkan12Properties":{"driverName":"Test","driverInfo":"1.0","shaderDenormPreserveFloat32":false,"shaderSignedZeroInfNanPreserveFloat32":true},
 "VkPhysicalDeviceVulkan13Properties":{"minSubgroupSize":32,"maxSubgroupSize":32}},
"formats":{"VK_FORMAT_R8G8B8A8_UNORM":{"VkFormatProperties":{"linearTilingFeatures":[],"optimalTilingFeatures":["VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT","VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT"],"bufferFeatures":[]}},
 "VK_FORMAT_D32_SFLOAT":{"VkFormatProperties":{"linearTilingFeatures":[],"optimalTilingFeatures":["VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT"],"bufferFeatures":[]}}},
"queueFamiliesProperties":[{"VkQueueFamilyProperties":{"queueCount":1,"queueFlags":["VK_QUEUE_GRAPHICS_BIT","VK_QUEUE_COMPUTE_BIT"]}}]}}}`

func testDevice(t *testing.T) *Device {
	t.Helper()
	d, err := ParseVulkanInfo([]byte(miniVulkanInfo))
	if err != nil {
		t.Fatal(err)
	}
	d.Source = "test"
	return d
}

func TestParseVulkanInfo(t *testing.T) {
	d := testDevice(t)
	if d.Name != "Test GPU" || d.Driver != "Test 1.0" || d.APIVersion != "1.3.0" {
		t.Errorf("identity %q %q %q", d.Name, d.Driver, d.APIVersion)
	}
	if !d.Feat("shaderInt64") || d.Feat("shaderFloat64") || d.Feat("depthBounds") {
		t.Error("feature flattening wrong")
	}
	if !d.HasExt("VK_EXT_depth_clip_control") || d.HasExt("VK_EXT_mesh_shader") {
		t.Error("extensions wrong")
	}
	if v, _ := d.Limit("maxComputeSharedMemorySize"); v != 32768 {
		t.Errorf("limit %v", v)
	}
	if d.Queues != 1 {
		t.Errorf("queues %d", d.Queues)
	}
	if !d.SubgroupOp["VK_SUBGROUP_FEATURE_BALLOT_BIT"] || d.SubgroupSt["VK_SHADER_STAGE_VERTEX_BIT"] {
		t.Error("subgroup props wrong")
	}
	if has, known := d.FormatHas("VK_FORMAT_D32_SFLOAT", "DEPTH_STENCIL_ATTACHMENT"); !has || !known {
		t.Error("format caps")
	}
	if has, known := d.FormatHas("VK_FORMAT_D24_UNORM_S8_UINT", "DEPTH_STENCIL_ATTACHMENT"); has || !known {
		t.Error("absent format must read as unsupported")
	}
}

func TestFeatureChecks(t *testing.T) {
	d := testDevice(t)
	cases := map[string]string{
		"exec.mesh": No, "exec.geometry": Yes, "cap.float64": No, "cap.int64": Yes, "atomics.int64.buffer": No,
		"pipe.depth_bounds": No, "pipe.msaa.4": Yes, "pipe.msaa.8": No, "pipe.depth_clip_control": Yes,
		"image.format.Rgba8": Yes, "image.format.Rg16f": No, "subgroup.ops.ballot": Yes, "subgroup.ops.shuffle": No,
		"subgroup.size.32": Yes, "subgroup.size.64": No, "queue.async_compute": Partial, "mem.gds": No,
		"pipe.format.depth.VK_FORMAT_D32_SFLOAT": Yes, "pipe.format.depth.VK_FORMAT_D24_UNORM_S8_UINT": No,
		"float.denorm_preserve.32": No, "float.signed_zero_inf_nan_preserve.32": Yes,
	}
	for id, want := range cases {
		if got, ev := LookupFeature(id).Check(d); got != want {
			t.Errorf("%s: got %s (%s), want %s", id, got, ev, want)
		}
	}
	if got, _ := LookupFeature("exec.mesh").Check(nil); got != Unknown {
		t.Errorf("nil device must give unknown, got %s", got)
	}
}

// ---- cache + journal -------------------------------------------------------------------------

func TestSPIRVFromCacheEntry(t *testing.T) {
	words := []uint32{0x07230203, 0x10500, 0, 5, 0, 0xdeadbeef}
	key := []byte("some-key")
	payload := make([]byte, 12)
	binary.LittleEndian.PutUint32(payload, 7) // runtimeAbiVersion
	binary.LittleEndian.PutUint64(payload[4:], uint64(len(words)))
	for _, w := range words {
		payload = binary.LittleEndian.AppendUint32(payload, w)
	}
	hdr := make([]byte, 48)
	binary.LittleEndian.PutUint32(hdr, cacheMagic)
	binary.LittleEndian.PutUint64(hdr[16:], uint64(len(key)))
	file := append(hdr, key...)
	for len(file)%8 != 0 {
		file = append(file, 0)
	}
	file = append(file, payload...)
	got, ok := SPIRVFromCacheEntry(file)
	if !ok || len(got) != len(words) || got[5] != 0xdeadbeef {
		t.Fatalf("got %v ok=%v", got, ok)
	}
	// resource-plan entries carry no SPIR-V
	plan := append(append(append([]byte{}, hdr...), key...), 0)
	for len(plan)%8 != 0 {
		plan = append(plan, 0)
	}
	plan = append(plan, make([]byte, 64)...)
	if _, ok := SPIRVFromCacheEntry(plan); ok {
		t.Error("plan entry accepted as shader")
	}
	if _, ok := SPIRVFromCacheEntry([]byte("short")); ok {
		t.Error("short file accepted")
	}
}

type enc struct{ b []byte }

func (e *enc) u32(v uint32) { e.b = binary.LittleEndian.AppendUint32(e.b, v) }
func (e *enc) u64(v uint64) { e.b = binary.LittleEndian.AppendUint64(e.b, v) }
func (e *enc) bl(v bool) {
	if v {
		e.b = append(e.b, 1)
	} else {
		e.b = append(e.b, 0)
	}
}
func (e *enc) z(n int)       { e.b = append(e.b, make([]byte, n)...) }
func (e *enc) f32(v float32) { e.u32(math.Float32bits(v)) }

// encodeTestRecipe mirrors encodeRecipe/encodeState in PipelineJournal.cpp for one pipeline.
func encodeTestRecipe(mesh, tess bool, samples uint32, depthFmt, colorFmt uint32, depthBounds, restart bool) []byte {
	e := &enc{}
	e.u32(0)
	e.u64(2) // stages
	for _, st := range []uint32{1, 5} {
		e.u32(st)
		e.u64(11)
		e.u64(22)
		e.u32(0)
		e.bl(false)
	}
	e.u64(0) // vertex bindings
	e.u64(0) // attributes
	e.u64(0) // main layout key
	e.u64(0) // fragment layout key
	e.bl(false)
	// state
	e.u32(0)
	e.u32(64)
	e.u32(64)
	e.u32(0)
	e.u32(0)
	e.bl(mesh)
	if mesh {
		e.z(40)
	}
	e.bl(tess)
	if tess {
		e.z(7*4 + 1 + 4 + 1 + 1)
	}
	e.u32(0)
	e.bl(false) // tessellationGeometry
	e.bl(depthFmt != 0)
	if depthFmt != 0 {
		e.u32(depthFmt)
		e.u32(samples)
	}
	e.bl(true) // depthTest
	e.bl(true) // depthWrite
	e.u32(1)   // compare
	e.bl(depthBounds)
	e.bl(false) // depthBias
	e.f32(0)
	e.bl(false)
	e.bl(false) // stencilTest
	e.z(56)
	e.bl(false)
	e.u64(1) // colors
	e.u32(colorFmt)
	e.u32(0)
	e.z(1)
	e.u32(0)
	e.u32(0)
	e.u64(1) // blends
	e.z(32)
	e.z(16)
	e.bl(true)
	e.bl(false)
	e.bl(false)
	e.u32(0)
	e.u32(0)
	e.u32(3) // topology TRIANGLE_LIST
	e.bl(restart)
	e.bl(false)
	e.bl(false)
	e.u32(0)
	e.u32(0)
	e.u32(0)
	e.f32(1)
	e.u32(0)
	e.u32(0)
	e.u32(samples)
	e.z(32)
	e.bl(false)
	e.bl(false)
	e.bl(false)
	e.bl(false)
	e.bl(false)
	e.z(8)
	e.u32(0)
	e.bl(false)
	e.f32(1)
	e.f32(1)
	e.f32(1)
	e.bl(false)
	return e.b
}

func TestDecodeRecipe(t *testing.T) {
	g, ids, ok := decodeRecipe(encodeTestRecipe(true, true, 4, 130, 50, true, true))
	if !ok {
		t.Fatal("decode failed")
	}
	if len(ids) != 4 || ids[0] != 11 || ids[1] != 22 {
		t.Errorf("ids %v", ids)
	}
	if g.Signature != "Vertex+Fragment" || !g.Mesh || !g.Tess || g.Samples != 4 || g.DepthFormat != 130 || !g.DepthBounds || !g.PrimRestart || g.Topology != 3 {
		t.Errorf("recipe %+v", g)
	}
	if len(g.ColorFormats) != 1 || g.ColorFormats[0] != 50 {
		t.Errorf("colors %v", g.ColorFormats)
	}
	feats := strings.Join(recipeFeatures(g), ",")
	for _, w := range []string{"exec.mesh", "exec.tessellation", "pipe.msaa.4", "pipe.depth_bounds", "pipe.primitive_restart", "pipe.format.depth.VK_FORMAT_D32_SFLOAT_S8_UINT"} {
		if !strings.Contains(feats, w) {
			t.Errorf("missing %s in %s", w, feats)
		}
	}
	// truncated or padded data must not decode
	b := encodeTestRecipe(false, false, 1, 0, 50, false, false)
	if _, _, ok := decodeRecipe(b[:len(b)-3]); ok {
		t.Error("truncated recipe decoded")
	}
	if _, _, ok := decodeRecipe(append(b, 0)); ok {
		t.Error("recipe with trailing byte decoded")
	}
}

// ---- report, ranking, compare ----------------------------------------------------------------

func TestBuildRankingAndKnown(t *testing.T) {
	mesh := &ShaderInfo{ID: "m1", Stage: "MeshEXT", Features: []string{"exec.mesh", "cap.int64"}}
	atom := &ShaderInfo{ID: "a1", Stage: "GLCompute", Features: []string{"atomics.int64.buffer", "cap.int64"}}
	ok := &ShaderInfo{ID: "o1", Stage: "Fragment", Features: []string{"cap.int64", "shader.demote"}}
	hint := &TitleInfo{Hints: []Hint{{Pattern: "r.RayTracing", Count: 3, Implies: []string{"exec.raytracing"}}, {Pattern: "import:Acb", Count: 2, Implies: []string{"queue.async_compute"}}}}
	r := Build(BuildInput{Name: "t", Device: testDevice(t), Shaders: []*ShaderInfo{mesh, atom, ok}, Title: hint, Known: []string{"exec.mesh"}})
	if len(r.Blockers) != 2 {
		t.Fatalf("blockers %v", r.Blockers)
	}
	// equal severity and usage: ordered by id
	if r.Blockers[0] != "atomics.int64.buffer" || r.Blockers[1] != "exec.mesh" {
		t.Errorf("order %v", r.Blockers)
	}
	if len(r.NextUp) != 1 || r.NextUp[0] != "atomics.int64.buffer" {
		t.Errorf("after known %v", r.NextUp)
	}
	if !r.feature("exec.mesh").Known {
		t.Error("known flag missing")
	}
	if f := r.feature("exec.raytracing"); !f.Predicted || f.Rank == 0 {
		t.Errorf("predicted raytracing %+v", f)
	}
	for _, b := range r.Blockers {
		if b == "exec.raytracing" {
			t.Error("a predicted feature must not be listed as a blocker")
		}
	}
	if len(r.Predicted) != 2 {
		t.Errorf("predicted list %v", r.Predicted)
	}
	if r.feature("cap.int64").Supported != Yes || r.feature("cap.int64").Rank != 0 {
		t.Error("supported feature ranked")
	}
	if !strings.Contains(r.Markdown(), "Predicted next blockers") {
		t.Error("markdown lacks predicted section")
	}
	// JSON round trip keeps the schema header
	b, _ := json.Marshal(r)
	var back Report
	if err := json.Unmarshal(b, &back); err != nil || back.Schema != SchemaName || back.Version != SchemaVersion {
		t.Errorf("round trip %v %+v", err, back.Schema)
	}
}

func TestNoDeviceGivesUnknown(t *testing.T) {
	s := &ShaderInfo{ID: "m1", Stage: "MeshEXT", Features: []string{"exec.mesh"}}
	r := Build(BuildInput{Name: "t", Shaders: []*ShaderInfo{s}})
	if r.feature("exec.mesh").Supported != Unknown || len(r.Blockers) != 0 {
		t.Errorf("no-device report: %+v blockers=%v", r.feature("exec.mesh"), r.Blockers)
	}
}

func TestCompare(t *testing.T) {
	dev := testDevice(t)
	a := Build(BuildInput{Name: "before", Device: dev, Shaders: []*ShaderInfo{{ID: "s1", Features: []string{"exec.mesh", "atomics.int64.buffer"}}}})
	dev2 := testDevice(t)
	dev2.ext["VK_EXT_mesh_shader"] = true
	dev2.Features["shaderBufferInt64Atomics"] = true
	dev2.Features["shaderFloat64"] = false
	b := Build(BuildInput{Name: "after", Device: dev2, Shaders: []*ShaderInfo{{ID: "s1", Features: []string{"exec.mesh", "atomics.int64.buffer", "cap.float64"}}}})
	c := Compare(a, b)
	if len(c.Resolved) != 2 || c.Resolved[0] != "atomics.int64.buffer" || c.Resolved[1] != "exec.mesh" {
		t.Errorf("resolved %v", c.Resolved)
	}
	if len(c.Regressed) != 1 || c.Regressed[0] != "cap.float64" {
		t.Errorf("regressed %v", c.Regressed)
	}
	if c.ShadersCommon != 1 {
		t.Errorf("common %d", c.ShadersCommon)
	}
	if !strings.Contains(c.Markdown(), "Resolved in B") {
		t.Error("markdown")
	}
	dir := t.TempDir()
	p := filepath.Join(dir, "r.json")
	j, _ := json.Marshal(a)
	os.WriteFile(p, j, 0o644)
	if _, err := LoadReport(p); err != nil {
		t.Error(err)
	}
	os.WriteFile(p, []byte(`{"schema":"other"}`), 0o644)
	if _, err := LoadReport(p); err == nil {
		t.Error("foreign schema accepted")
	}
	os.WriteFile(p, []byte(`{"schema":"gpuscan-report","schemaVersion":99}`), 0o644)
	if _, err := LoadReport(p); err == nil {
		t.Error("newer schema version accepted")
	}
}

func TestShaderHeaderScan(t *testing.T) {
	img := make([]byte, 512)
	p := 128
	copy(img[p:], []byte{0x31, 0x32, 0x33, 0x34, 0x18, 0, 0, 0})
	binary.LittleEndian.PutUint32(img[p+64:], 0x100)
	binary.LittleEndian.PutUint32(img[p+68:], 0x400)
	binary.LittleEndian.PutUint32(img[p+76:], 0xe)
	img[p+90] = 1 // Ps
	// a decoy with an impossible type
	q := 300
	copy(img[q:], []byte{0x31, 0x32, 0x33, 0x34, 0x18, 0, 0, 0})
	binary.LittleEndian.PutUint32(img[q+64:], 0x100)
	binary.LittleEndian.PutUint32(img[q+68:], 0x400)
	img[q+90] = 77
	var e EmbeddedShaders
	scanShaderHeaders(img, &e)
	if e.Headers != 1 || e.ByType["Ps"] != 1 || e.ByTarget["0xe"] != 1 || e.CodeBytes != 0x400 {
		t.Errorf("%+v", e)
	}
}

func TestNIDNames(t *testing.T) {
	// the table is built from AnyPS5 export names; a known entry must round-trip through its NID
	n, ok := nidName(elfloaderNID("sceAgcCreateShader") + "#A#B")
	if !ok || n != "sceAgcCreateShader" {
		t.Errorf("got %q %v", n, ok)
	}
	if !isGPULib("libSceAgcDriver") || !isGPULib("libSceVideoOut") || !isGPULib("libSceAmpr") || isGPULib("libkernel") {
		t.Error("gpu library classification")
	}
}

func elfloaderNID(n string) string { return elfloader.CalculateNID(n) }
