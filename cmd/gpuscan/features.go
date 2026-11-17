package main

import (
	"fmt"
	"sort"
	"strings"
)

// Support values of a feature on the target device.
const (
	Yes     = "yes"
	No      = "no"
	Partial = "partial"
	Unknown = "unknown"
)

// Severity values, in decreasing weight. A severity applies when the feature is not supported.
const (
	SevBlocker = "blocker" // the shader/pipeline cannot be created or the draw is lost
	SevMajor   = "major"   // renders wrongly or needs emulation to be correct
	SevMinor   = "minor"   // small visual / precision difference
	SevInfo    = "info"
)

func sevWeight(s string) int {
	switch s {
	case SevBlocker:
		return 4
	case SevMajor:
		return 3
	case SevMinor:
		return 2
	}
	return 1
}

// FeatureDef describes one GPU feature the scanner can detect and how to judge it on a device.
type FeatureDef struct {
	ID         string
	Category   string
	Title      string
	Severity   string
	Workaround string
	Check      func(d *Device) (support, evidence string)
}

func featYes(name string) func(*Device) (string, string) {
	return func(d *Device) (string, string) {
		if d == nil {
			return Unknown, "no device data"
		}
		if d.Feat(name) {
			return Yes, name + "=true"
		}
		if _, ok := d.Features[name]; ok {
			return No, name + "=false"
		}
		return No, name + " not reported (feature struct absent)"
	}
}

func anyFeat(names ...string) func(*Device) (string, string) {
	return func(d *Device) (string, string) {
		if d == nil {
			return Unknown, "no device data"
		}
		var have []string
		for _, n := range names {
			if d.Feat(n) {
				have = append(have, n)
			}
		}
		if len(have) == len(names) {
			return Yes, strings.Join(names, "&&") + "=true"
		}
		if len(have) > 0 {
			return Partial, "only " + strings.Join(have, ",")
		}
		return No, strings.Join(names, "/") + "=false or absent"
	}
}

func extYes(names ...string) func(*Device) (string, string) {
	return func(d *Device) (string, string) {
		if d == nil {
			return Unknown, "no device data"
		}
		for _, n := range names {
			if d.HasExt(n) {
				return Yes, n + " present"
			}
		}
		return No, strings.Join(names, " / ") + " not exposed"
	}
}

func propYes(name string) func(*Device) (string, string) {
	return func(d *Device) (string, string) {
		if d == nil {
			return Unknown, "no device data"
		}
		if v, ok := d.Limit(name); ok && v != 0 {
			return Yes, name + "=true"
		}
		return No, name + "=false or absent"
	}
}

func always(ev string) func(*Device) (string, string) {
	return func(*Device) (string, string) { return Yes, ev }
}

var staticDefs = []FeatureDef{
	// ---- stages
	{"exec.mesh", "stage", "Mesh shader stage (MeshEXT/MeshNV)", SevBlocker, "No driver-side workaround yet. Options: native Metal mesh pipelines exposed through KosmicKrisp, or compute-shader meshlet expansion + indirect draw in the AGC driver (owned by the mesh-shader workstream).", extYes("VK_EXT_mesh_shader", "VK_NV_mesh_shader")},
	{"exec.task", "stage", "Task/amplification shader stage (TaskEXT/TaskNV)", SevBlocker, "Same as mesh shaders; task shaders additionally need a compute pre-pass that writes the meshlet dispatch.", extYes("VK_EXT_mesh_shader", "VK_NV_mesh_shader")},
	{"exec.geometry", "stage", "Geometry shader stage", SevMajor, "ES/GS stages can be emulated through compute + vertex pulling (AnyPS5 tessellationGeometry/mesh configuration path).", featYes("geometryShader")},
	{"exec.tessellation", "stage", "Tessellation shaders (HS/DS)", SevMajor, "Emulate with compute tessellation into a vertex buffer (AnyPS5 TessellationConfiguration path).", featYes("tessellationShader")},
	{"exec.raytracing", "stage", "Ray tracing / ray query", SevBlocker, "None: Metal ray tracing is not exposed through KosmicKrisp. Titles that gate RT behind a setting can run with RT disabled.", extYes("VK_KHR_ray_tracing_pipeline", "VK_KHR_ray_query")},

	// ---- types
	{"cap.float64", "types", "64-bit float arithmetic (Float64)", SevMajor, "Lower f64 to f32 in the recompiler (precision loss) or use double-float emulation; Metal has no f64 at all.", featYes("shaderFloat64")},
	{"cap.int64", "types", "64-bit integer arithmetic (Int64)", SevMajor, "Split into 2x32-bit operations in the recompiler.", featYes("shaderInt64")},
	{"cap.int16", "types", "16-bit integers (Int16)", SevMinor, "Widen to 32-bit.", featYes("shaderInt16")},
	{"cap.int8", "types", "8-bit integers (Int8)", SevMinor, "Widen to 32-bit.", featYes("shaderInt8")},
	{"cap.float16", "types", "16-bit float arithmetic (Float16)", SevMinor, "Compute at f32 (changes rounding slightly).", featYes("shaderFloat16")},
	{"cap.storage_16bit", "types", "16-bit storage access", SevMinor, "Pack/unpack 32-bit words in the shader.", featYes("storageBuffer16BitAccess")},
	{"cap.storage_8bit", "types", "8-bit storage access", SevMinor, "Pack/unpack 32-bit words in the shader.", featYes("storageBuffer8BitAccess")},

	// ---- atomics
	{"atomics.int64.any", "atomics", "Int64Atomics capability", SevBlocker, "See atomics.int64.buffer.", anyFeat("shaderBufferInt64Atomics")},
	{"atomics.int64.buffer", "atomics", "64-bit integer atomics on buffers", SevBlocker, "Emulate with a 32-bit compare-exchange on a (hi,lo) pair guarded by a lock word, or split max/min into a two-pass reduction. UE5 Nanite software rasteriser depends on 64-bit atomic max for visibility buffers.", featYes("shaderBufferInt64Atomics")},
	{"atomics.int64.shared", "atomics", "64-bit integer atomics on workgroup memory", SevBlocker, "Emulate through a 32-bit CAS loop on the high word with the low word re-read.", featYes("shaderSharedInt64Atomics")},
	{"atomics.int64.image", "atomics", "64-bit integer atomics on images", SevBlocker, "Redirect the image to a buffer + 32-bit pair emulation, or drop to a 32-bit visibility format.", featYes("shaderImageInt64Atomics")},
	{"atomics.image.int32", "atomics", "32-bit integer atomics on storage images", SevMajor, "Use a storage buffer instead of the image.", func(d *Device) (string, string) {
		if has, known := d.FormatHas("VK_FORMAT_R32_UINT", "STORAGE_IMAGE_ATOMIC"); known && has {
			return Yes, "R32_UINT STORAGE_IMAGE_ATOMIC"
		} else if known {
			return No, "R32_UINT lacks STORAGE_IMAGE_ATOMIC"
		}
		return Unknown, "no format data"
	}},
	{"atomics.float.minmax", "atomics", "Float atomic min/max", SevMajor, "CAS loop on the integer bit pattern.", featYes("shaderBufferFloat32AtomicMinMax")},

	// ---- subgroup
	{"subgroup.stage.vertex_pipeline", "subgroup", "Subgroup operations outside fragment/compute stages", SevBlocker, "Subgroup ops are only advertised for fragment+compute. Rewrite lane operations of VS/GS/tess shaders to be lane-agnostic, or move the shader to a compute pre-pass.", func(d *Device) (string, string) {
		if d == nil {
			return Unknown, "no device data"
		}
		if d.SubgroupSt["VK_SHADER_STAGE_VERTEX_BIT"] {
			return Yes, "vertex stage supports subgroup operations"
		}
		return No, fmt.Sprintf("subgroupSupportedStages=%v", keys(d.SubgroupSt))
	}},
	{"subgroup.quad_all_stages", "subgroup", "Quad subgroup operations in non-fragment/compute stages", SevMajor, "Avoid quad ops outside fragment/compute.", propYes("subgroupQuadOperationsInAllStages")},
	{"subgroup.builtins", "subgroup", "Subgroup builtins (SubgroupSize / LocalInvocationId / ...)", SevInfo, "KosmicKrisp subgroup size is fixed (see device); RDNA wave64 code assumes 64 lanes.", always("core Vulkan 1.1")},

	// ---- shader behaviour
	{"shader.demote", "shader", "Demote to helper invocation", SevMajor, "Fall back to OpKill (changes derivative results for the rest of the quad).", featYes("shaderDemoteToHelperInvocation")},
	{"shader.terminate", "shader", "OpTerminateInvocation", SevMinor, "Use OpKill.", featYes("shaderTerminateInvocation")},
	{"shader.kill", "shader", "OpKill (discard)", SevInfo, "", always("core Vulkan")},
	{"shader.clip_distance", "shader", "ClipDistance outputs", SevMinor, "Clip in the fragment shader.", featYes("shaderClipDistance")},
	{"shader.cull_distance", "shader", "CullDistance outputs", SevMinor, "Cull in the vertex shader (degenerate triangle).", featYes("shaderCullDistance")},
	{"shader.multi_viewport", "shader", "Multiple viewports (MultiViewport)", SevMajor, "Render per viewport in separate draws.", featYes("multiViewport")},
	{"shader.layer_output", "shader", "gl_Layer written outside geometry stage", SevMajor, "Replicate the draw per layer.", featYes("shaderOutputLayer")},
	{"shader.viewport_index_output", "shader", "ViewportIndex written outside geometry stage", SevMajor, "Replicate the draw per viewport.", featYes("shaderOutputViewportIndex")},
	{"shader.draw_parameters", "shader", "DrawParameters (BaseVertex, DrawIndex...)", SevMajor, "Pass through push constants/SGPR emulation.", featYes("shaderDrawParameters")},
	{"shader.clock", "shader", "Shader clock (ReadClockKHR)", SevMinor, "Replace by a constant or a per-dispatch counter.", extYes("VK_KHR_shader_clock")},
	{"shader.expect_assume", "shader", "OpAssume/OpExpect hints", SevInfo, "Lower to no-op.", always("hint only")},
	{"shader.maximal_reconvergence", "shader", "Maximal reconvergence execution mode", SevMinor, "Ignore; only matters for divergent subgroup code.", anyFeat("shaderMaximalReconvergence")},
	{"shader.derivative_group", "shader", "Compute shader derivatives", SevMajor, "Compute derivatives through quad subgroup ops.", extYes("VK_KHR_compute_shader_derivatives", "VK_NV_compute_shader_derivatives")},
	{"shader.device_group", "shader", "DeviceGroup (multi-GPU)", SevInfo, "", always("single device")},
	{"nv.legacy_ext", "shader", "NV-only legacy extension capability", SevMajor, "Rewrite with the KHR/EXT equivalent.", func(*Device) (string, string) { return No, "vendor-specific" }},

	// ---- memory / descriptors
	{"mem.buffer_device_address", "memory", "Buffer device address (PhysicalStorageBufferAddresses)", SevBlocker, "", featYes("bufferDeviceAddress")},
	{"mem.vulkan_memory_model", "memory", "Vulkan memory model", SevMajor, "Use GLSL450 memory model + coherent decorations.", featYes("vulkanMemoryModel")},
	{"mem.variable_pointers", "memory", "Variable pointers", SevMajor, "Avoid pointer selects in the recompiler.", featYes("variablePointers")},
	{"mem.workgroup_explicit_layout", "memory", "Workgroup memory explicit layout", SevMinor, "Use typed workgroup variables.", featYes("workgroupMemoryExplicitLayout")},
	{"desc.runtime_array", "descriptors", "Runtime descriptor arrays (bindless)", SevBlocker, "Bound the arrays statically.", featYes("runtimeDescriptorArray")},
	{"desc.non_uniform_indexing", "descriptors", "Non-uniform descriptor indexing", SevMajor, "Waterfall loop over the index.", anyFeat("shaderSampledImageArrayNonUniformIndexing", "shaderStorageBufferArrayNonUniformIndexing")},
	{"limits.shared_memory", "limits", "Workgroup shared memory within device limit", SevBlocker, "Split the dispatch, spill part of LDS to a global scratch buffer, or recompile the kernel with a smaller tile.", nil},
	{"limits.push_constants", "limits", "Push constant size within device limit", SevBlocker, "Move the excess to a uniform buffer.", nil},
	{"limits.descriptor_sets", "limits", "Descriptor set index within device limit", SevBlocker, "Remap sets.", nil},
	{"limits.descriptors", "limits", "Per-stage descriptor counts within device limit", SevBlocker, "Use argument-buffer style indirection.", nil},
	{"limits.subgroup_size", "limits", "Required subgroup size", SevMajor, "Compile wave64 code as wave32 pairs.", nil},

	// ---- textures
	{"tex.gather_extended", "texture", "Gather with non-constant/constOffsets", SevMinor, "Compute four texelFetch.", featYes("shaderImageGatherExtended")},
	{"tex.storage_image_multisample", "texture", "Multisampled storage images", SevMajor, "Copy MSAA surface into a 2D array (one layer per sample) and run the pass on that.", featYes("shaderStorageImageMultisample")},
	{"tex.cube_array", "texture", "Cube map arrays", SevMajor, "Emulate with a 2D array + manual face select.", featYes("imageCubeArray")},
	{"tex.storage_extended_formats", "texture", "Extended storage image formats", SevMajor, "Check image.format.* features.", featYes("shaderStorageImageExtendedFormats")},
	{"tex.storage_read_without_format", "texture", "Storage image read without format", SevMajor, "Declare the real format.", featYes("shaderStorageImageReadWithoutFormat")},
	{"tex.storage_write_without_format", "texture", "Storage image write without format", SevMajor, "Declare the real format.", featYes("shaderStorageImageWriteWithoutFormat")},
	{"sparse.residency", "texture", "Sparse residency (OpImageSparse*)", SevMajor, "Treat all texels as resident (OpImageSparseTexelsResident = true) and use the plain sample path; wrong for virtual-texture residency feedback.", anyFeat("shaderResourceResidency")},
	{"sparse.min_lod", "texture", "Min LOD clamp (MinLod)", SevMinor, "Clamp the LOD in the shader.", featYes("shaderResourceMinLod")},

	// ---- fragment
	{"frag.barycentric", "fragment", "Fragment barycentric coordinates (BaryCoord)", SevMajor, "AnyPS5 recompiler barycentricEmulation (per-vertex attribute pulling); native support would need KosmicKrisp to expose VK_KHR_fragment_shader_barycentric over Metal barycentric_coord.", extYes("VK_KHR_fragment_shader_barycentric", "VK_NV_fragment_shader_barycentric")},
	{"frag.interlock", "fragment", "Fragment shader interlock (ROV)", SevBlocker, "Metal has raster order groups but KosmicKrisp does not expose VK_EXT_fragment_shader_interlock. Fallback: run without ordering (races) or serialise per-tile.", extYes("VK_EXT_fragment_shader_interlock")},
	{"frag.vrs", "fragment", "Variable rate shading", SevMinor, "Ignore: render at 1x1 (costs performance, not correctness).", extYes("VK_KHR_fragment_shading_rate")},
	{"frag.stencil_export", "fragment", "Fragment stencil reference export", SevMajor, "AnyPS5 StencilReplay (multi-pass stencil reference replay) covers the common case.", extYes("VK_EXT_shader_stencil_export")},
	{"frag.fully_covered", "fragment", "FullyCovered builtin (conservative raster)", SevMinor, "Treat as covered.", extYes("VK_EXT_conservative_rasterization")},
	{"frag.density_map", "fragment", "Fragment density map", SevMinor, "Ignore.", extYes("VK_EXT_fragment_density_map")},
	{"frag.sample_rate_shading", "fragment", "Sample rate shading", SevMajor, "Shade per pixel.", featYes("sampleRateShading")},
	{"frag.interpolation_function", "fragment", "Interpolate at sample/offset/centroid", SevMajor, "Evaluate by barycentric reconstruction.", featYes("sampleRateShading")},
	{"frag.sample_mask_override", "fragment", "Sample mask override coverage (NV)", SevMajor, "Not available.", func(*Device) (string, string) { return No, "vendor-specific" }},
	{"xfb.transform_feedback", "stage", "Transform feedback / stream-out", SevBlocker, "Write vertex outputs from a compute prepass (vertex pulling) into the stream-out buffer.", extYes("VK_EXT_transform_feedback")},

	{"queue.async_compute", "queue", "Asynchronous compute queue (ACB)", SevMajor, "Serialise ACB submissions onto the graphics queue (AnyPS5 default); titles that spin-wait on a label written by concurrent compute work can deadlock.", func(d *Device) (string, string) {
		if d == nil {
			return Unknown, "no device data"
		}
		if d.Queues >= 2 {
			return Yes, fmt.Sprintf("%d compute-capable queues", d.Queues)
		}
		return Partial, fmt.Sprintf("%d compute-capable queue(s): no concurrent compute", d.Queues)
	}},
	{"pipe.conditional_rendering", "pipeline", "Conditional (predicated) rendering", SevMinor, "Evaluate the predicate on the CPU or skip.", extYes("VK_EXT_conditional_rendering")},
	{"mem.gds", "memory", "Global data share (GDS)", SevMajor, "Map GDS ranges to a device storage buffer (AnyPS5 maps gdsMemoryRange resources); ordered-append counters need atomics on that buffer.", func(*Device) (string, string) {
		return No, "no GDS equivalent in Vulkan/Metal"
	}},

	// ---- pipeline level
	{"pipe.multiview", "pipeline", "Multiview rendering", SevMajor, "Draw once per view.", featYes("multiview")},
	{"pipe.sample_locations", "pipeline", "Custom sample locations", SevMinor, "Use the default pattern.", extYes("VK_EXT_sample_locations")},
	{"pipe.depth_bounds", "pipeline", "Depth bounds test", SevMinor, "Ignore, or discard in the fragment shader from a depth fetch (needs depth read).", featYes("depthBounds")},
	{"pipe.depth_clamp", "pipeline", "Depth clamp", SevMinor, "Clamp gl_FragDepth in the shader.", featYes("depthClamp")},
	{"pipe.depth_bias_clamp", "pipeline", "Depth bias clamp", SevMinor, "Ignore the clamp.", featYes("depthBiasClamp")},
	{"pipe.depth_clip_control", "pipeline", "[-1,1] depth range (depth clip control)", SevMajor, "Remap depth in the vertex shader.", extYes("VK_EXT_depth_clip_control")},
	{"pipe.dual_source_blend", "pipeline", "Dual-source blending", SevMajor, "Two-pass blend.", featYes("dualSrcBlend")},
	{"pipe.polygon_line", "pipeline", "Wireframe fill mode (fillModeNonSolid)", SevMinor, "Draw lines from an index buffer expansion.", featYes("fillModeNonSolid")},
	{"pipe.wide_lines", "pipeline", "Wide lines (>1px)", SevMinor, "Expand to quads in the vertex shader.", featYes("wideLines")},
	{"pipe.conservative_raster", "pipeline", "Conservative rasterisation", SevMinor, "Expand triangles in a geometry/compute pass.", extYes("VK_EXT_conservative_rasterization")},
	{"pipe.provoking_vertex", "pipeline", "Provoking vertex mode", SevMajor, "Rotate indices for flat attributes.", extYes("VK_EXT_provoking_vertex")},
	{"pipe.primitive_restart", "pipeline", "Primitive restart", SevMajor, "Split the draw or rewrite indices.", extYes("VK_EXT_primitive_topology_list_restart", "VK_EXT_primitive_restart_index")},
	{"pipe.sample_shading", "pipeline", "Per-sample shading", SevMajor, "Shade per pixel.", featYes("sampleRateShading")},
}

func keys(m map[string]bool) []string {
	var o []string
	for k, v := range m {
		if v {
			o = append(o, k)
		}
	}
	sortStrings(o)
	return o
}

var defIndex map[string]*FeatureDef

func init() {
	defIndex = map[string]*FeatureDef{}
	for i := range staticDefs {
		defIndex[staticDefs[i].ID] = &staticDefs[i]
	}
}

// LookupFeature returns the definition for id, synthesising it for parameterised families
// (float.<mode>.<w>, atomics.float<w>.<class>.<kind>, image.format.<name>, pipe.msaa.<n>,
// pipe.format.*, subgroup.ops.<kind>, subgroup.size.<n>, cap.<Name>).
func LookupFeature(id string) *FeatureDef {
	if d, ok := defIndex[id]; ok {
		return d
	}
	var d *FeatureDef
	parts := strings.Split(id, ".")
	switch {
	case strings.HasPrefix(id, "float."):
		mode, w := parts[1], parts[2]
		prop := map[string]string{"denorm_preserve": "shaderDenormPreserveFloat", "denorm_ftz": "shaderDenormFlushToZeroFloat", "signed_zero_inf_nan_preserve": "shaderSignedZeroInfNanPreserveFloat", "rounding_rte": "shaderRoundingModeRTEFloat", "rounding_rtz": "shaderRoundingModeRTZFloat"}[mode] + w
		sev, wk := SevMinor, "Accept the numeric difference (rare to matter)."
		if mode == "signed_zero_inf_nan_preserve" {
			sev, wk = SevMajor, "Disable fast-math in the Metal compile; NaN/Inf propagation differs otherwise."
		}
		d = &FeatureDef{id, "float-controls", fmt.Sprintf("Float control %s on %s-bit floats", strings.ReplaceAll(mode, "_", " "), w), sev, wk, propYes(prop)}
	case strings.HasPrefix(id, "atomics.float"):
		// atomics.float<w>.<class>.<kind>
		w := strings.TrimPrefix(parts[1], "float")
		cls, kind := parts[2], parts[3]
		C := strings.Title(cls)
		feat := fmt.Sprintf("shader%sFloat%sAtomics", C, w)
		switch kind {
		case "add":
			feat += "Add"
		case "minmax":
			feat += "MinMax"
		}
		d = &FeatureDef{id, "atomics", fmt.Sprintf("Float%s atomics (%s) on %s", w, kind, cls), SevMajor, "Emulate with a compare-exchange loop on the integer bit pattern.", featYes(feat)}
	case strings.HasPrefix(id, "image.format."):
		name := parts[2]
		vk := vkFormatForImageFormat[name]
		d = &FeatureDef{id, "texture", "Storage image format " + name, SevMajor, "Re-type the storage image to a supported format (or copy through a buffer).", func(dv *Device) (string, string) {
			if vk == "" {
				return Unknown, "no Vulkan mapping"
			}
			has, known := dv.FormatHas(vk, "STORAGE_IMAGE")
			if !known {
				return Unknown, "no format data"
			}
			if has {
				return Yes, vk + " STORAGE_IMAGE"
			}
			return No, vk + " lacks STORAGE_IMAGE"
		}}
	case strings.HasPrefix(id, "pipe.msaa."):
		n := parts[2]
		d = &FeatureDef{id, "pipeline", n + "x MSAA", SevMajor, "Render at 1x and super-sample or resolve manually.", func(dv *Device) (string, string) {
			if dv == nil {
				return Unknown, "no device data"
			}
			want := "VK_SAMPLE_COUNT_" + n + "_BIT"
			for _, s := range dv.Lists["framebufferColorSampleCounts"] {
				if s == want {
					return Yes, want
				}
			}
			return No, want + " not in framebufferColorSampleCounts"
		}}
	case strings.HasPrefix(id, "pipe.format.color.") || strings.HasPrefix(id, "pipe.format.depth."):
		role, name := parts[2], strings.TrimPrefix(id, "pipe.format."+parts[2]+".")
		bit, sev, wk := "COLOR_ATTACHMENT", SevBlocker, "Substitute a supported format (convert on resolve)."
		if role == "depth" {
			bit = "DEPTH_STENCIL_ATTACHMENT"
			wk = "Substitute D32_SFLOAT(_S8) / D16 and convert on copy; D24 formats are not supported by Metal on Apple silicon."
		}
		d = &FeatureDef{id, "format", fmt.Sprintf("%s attachment format %s", role, name), sev, wk, func(dv *Device) (string, string) {
			has, known := dv.FormatHas(name, bit)
			if !known {
				return Unknown, "no format data"
			}
			if has {
				return Yes, "optimal tiling " + bit
			}
			return No, name + " lacks " + bit
		}}
	case strings.HasPrefix(id, "subgroup.ops."):
		kind := parts[2]
		bit := map[string]string{"basic": "BASIC", "vote": "VOTE", "ballot": "BALLOT", "shuffle": "SHUFFLE", "shuffle_relative": "SHUFFLE_RELATIVE", "arithmetic": "ARITHMETIC", "clustered": "CLUSTERED", "quad": "QUAD", "rotate": "ROTATE", "partitioned": "PARTITIONED_BIT_NV"}[kind]
		d = &FeatureDef{id, "subgroup", "Subgroup " + strings.ReplaceAll(kind, "_", " ") + " operations", SevBlocker, "Emulate through shared memory / quad-uniform rewrite.", func(dv *Device) (string, string) {
			if dv == nil {
				return Unknown, "no device data"
			}
			if bit == "" {
				return Unknown, "unmapped op class"
			}
			full := "VK_SUBGROUP_FEATURE_" + bit + "_BIT"
			if strings.HasSuffix(bit, "_NV") || bit == "PARTITIONED_BIT_NV" {
				full = "VK_SUBGROUP_FEATURE_PARTITIONED_BIT_NV"
			}
			if dv.SubgroupOp[full] {
				return Yes, full
			}
			return No, full + " not supported"
		}}
	case strings.HasPrefix(id, "subgroup.size."):
		n := parts[2]
		d = &FeatureDef{id, "subgroup", "Required subgroup size " + n, SevMajor, "Compile wave64 code as wave32 pairs (AnyPS5 hostSubgroupSize); a shader that relies on 64 lanes in one subgroup computes wrong ballots.", func(dv *Device) (string, string) {
			if dv == nil {
				return Unknown, "no device data"
			}
			var v float64
			fmt.Sscan(n, &v)
			lo, _ := dv.Limit("minSubgroupSize")
			hi, _ := dv.Limit("maxSubgroupSize")
			if v >= lo && v <= hi && hi != 0 {
				return Yes, fmt.Sprintf("subgroup size range [%v,%v]", lo, hi)
			}
			return No, fmt.Sprintf("subgroup size range [%v,%v]", lo, hi)
		}}
	case strings.HasPrefix(id, "cap."):
		d = &FeatureDef{id, "capability", "SPIR-V capability " + strings.TrimPrefix(id, "cap."), SevInfo, "Unmapped capability: add a device check in gpuscan features.go.", func(*Device) (string, string) { return Unknown, "no device mapping for this capability" }}
	default:
		d = &FeatureDef{id, "other", id, SevInfo, "", func(*Device) (string, string) { return Unknown, "unmapped feature" }}
	}
	defIndex[id] = d
	return d
}

func sortStrings(s []string) { sort.Strings(s) }
