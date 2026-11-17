package main

import (
	"crypto/sha1"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"sort"
)

// ShaderInfo is the per-shader result of analysing one SPIR-V module. Everything is derived by
// walking the binary directly (no spirv-dis needed), so it is fast and deterministic.
type ShaderInfo struct {
	ID           string            `json:"id"`                   // sha1 prefix of the SPIR-V words (content address)
	Stage        string            `json:"stage"`                // execution model of the first entry point
	Models       []string          `json:"executionModels"`      // all entry-point models
	Words        int               `json:"words"`                // module size in 32-bit words
	Version      string            `json:"spirvVersion"`         // e.g. 1.6
	Capabilities []string          `json:"capabilities"`         // OpCapability names
	Extensions   []string          `json:"extensions,omitempty"` // OpExtension names
	Modes        []string          `json:"executionModes,omitempty"`
	Features     []string          `json:"features"` // feature ids this shader needs
	SharedBytes  uint32            `json:"sharedMemoryBytes,omitempty"`
	PushBytes    uint32            `json:"pushConstantBytes,omitempty"`
	Descriptors  map[string]uint32 `json:"descriptors,omitempty"` // type -> count (arrays expanded)
	MaxSet       int               `json:"maxDescriptorSet,omitempty"`
	Unbounded    bool              `json:"unboundedDescriptorArray,omitempty"`
	ImageFormats []string          `json:"storageImageFormats,omitempty"`
	LocalSize    []uint32          `json:"localSize,omitempty"`
	Origin       string            `json:"origin,omitempty"` // where the module came from (cache | journal)

	// need records detail per feature id, used for the report (not serialised per shader).
	need map[string]bool
	// stageIdx is the numeric execution model of the first entry point.
	stageIdx uint32
	// subgroup operations observed per stage.
}

func (s *ShaderInfo) add(f string) {
	if s.need == nil {
		s.need = map[string]bool{}
	}
	s.need[f] = true
}

type spvType struct {
	op      uint16
	width   uint32 // int/float width, vector count, array length id
	elem    uint32
	members []uint32
	sc      uint32 // pointer storage class
	img     [7]uint32
	length  uint32
}

// ParseSPIRV words from little-endian bytes.
func WordsFromBytes(b []byte) ([]uint32, bool) {
	if len(b) < 20 || len(b)%4 != 0 {
		return nil, false
	}
	w := make([]uint32, len(b)/4)
	for i := range w {
		w[i] = binary.LittleEndian.Uint32(b[i*4:])
	}
	if w[0] != 0x07230203 {
		return nil, false
	}
	return w, true
}

func spvID(w []uint32) string {
	h := sha1.New()
	var buf [4]byte
	for _, x := range w {
		binary.LittleEndian.PutUint32(buf[:], x)
		h.Write(buf[:])
	}
	return hex.EncodeToString(h.Sum(nil))[:16]
}

func litString(w []uint32) (string, int) {
	var out []byte
	for i, x := range w {
		for k := 0; k < 4; k++ {
			c := byte(x >> (8 * k))
			if c == 0 {
				return string(out), i + 1
			}
			out = append(out, c)
		}
	}
	return string(out), len(w)
}

// AnalyzeSPIRV analyses one module.
func AnalyzeSPIRV(w []uint32) (*ShaderInfo, error) {
	if len(w) < 5 || w[0] != 0x07230203 {
		return nil, fmt.Errorf("not a SPIR-V module")
	}
	s := &ShaderInfo{ID: spvID(w), Words: len(w), Version: fmt.Sprintf("%d.%d", (w[1]>>16)&0xff, (w[1]>>8)&0xff)}
	types := map[uint32]*spvType{}
	consts := map[uint32]uint32{}
	ptrType := map[uint32]uint32{} // pointer-valued id -> its pointer type id
	vars := map[uint32]uint32{}    // variable id -> pointer type id
	decor := map[uint32]map[uint32][]uint32{}
	mdecor := map[uint32]map[uint32]map[uint32][]uint32{} // struct -> member -> decoration -> ops
	caps := map[uint32]bool{}
	exts := map[string]bool{}
	models := map[uint32]bool{}
	var firstModel uint32
	haveModel := false
	modes := map[string]bool{}
	type atomicUse struct {
		kind   string // op family
		result uint32 // result type id
		ptr    uint32
	}
	var atomics []atomicUse
	subgroupOps := map[string]bool{}
	i := 5
	for i < len(w) {
		wc := int(w[i] >> 16)
		op := uint16(w[i] & 0xffff)
		if wc == 0 || i+wc > len(w) {
			return nil, fmt.Errorf("malformed instruction at word %d", i)
		}
		a := w[i+1 : i+wc]
		switch op {
		case 17: // OpCapability
			caps[a[0]] = true
		case 10: // OpExtension
			n, _ := litString(a)
			exts[n] = true
		case 15: // OpEntryPoint
			models[a[0]] = true
			if !haveModel {
				firstModel, haveModel = a[0], true
			}
		case 16: // OpExecutionMode entry mode ...
			name := execModeName(a[1])
			if (a[1] == 4459 || a[1] == 4460 || a[1] == 4461 || a[1] == 4462 || a[1] == 4463) && len(a) > 2 {
				name = fmt.Sprintf("%s.%d", name, a[2])
				s.add(fmt.Sprintf("float.%s.%d", floatModeID(a[1]), a[2]))
			}
			if (a[1] == 26 || a[1] == 5270 || a[1] == 5298 || a[1] == 0) && len(a) > 2 {
				name = fmt.Sprintf("%s.%d", name, a[2])
			}
			modes[name] = true
			switch a[1] {
			case 17:
				if len(a) >= 5 {
					s.LocalSize = []uint32{a[2], a[3], a[4]}
				}
			case 35: // SubgroupSize
				if len(a) > 2 {
					modes[fmt.Sprintf("SubgroupSize.%d", a[2])] = true
					s.add(fmt.Sprintf("subgroup.size.%d", a[2]))
				}
			case 5366, 5367, 5368, 5369, 5370, 5371:
				s.add("frag.interlock")
			case 5027:
				s.add("frag.stencil_export")
			case 6023:
				s.add("shader.maximal_reconvergence")
			case 5289, 5290:
				s.add("shader.derivative_group")
			}
		case 21: // OpTypeInt result width signed
			types[a[0]] = &spvType{op: op, width: a[1]}
		case 22: // OpTypeFloat
			types[a[0]] = &spvType{op: op, width: a[1]}
		case 23, 24: // vector / matrix: result comp count
			types[a[0]] = &spvType{op: op, elem: a[1], width: a[2]}
		case 25: // OpTypeImage result sampled dim depth arrayed ms sampled format
			t := &spvType{op: op, elem: a[1]}
			for k := 0; k < 7 && 2+k < len(a); k++ {
				t.img[k] = a[2+k]
			}
			types[a[0]] = t
		case 26, 27: // sampler, sampledimage
			t := &spvType{op: op}
			if op == 27 {
				t.elem = a[1]
			}
			types[a[0]] = t
		case 28: // OpTypeArray result elem length
			types[a[0]] = &spvType{op: op, elem: a[1], length: a[2]}
		case 29:
			types[a[0]] = &spvType{op: op, elem: a[1]}
		case 30: // OpTypeStruct
			types[a[0]] = &spvType{op: op, members: append([]uint32(nil), a[1:]...)}
		case 32: // OpTypePointer result sc type
			types[a[0]] = &spvType{op: op, sc: a[1], elem: a[2]}
		case 43: // OpConstant type id value
			if len(a) >= 3 {
				consts[a[1]] = a[2]
			}
		case 59: // OpVariable type id sc
			vars[a[1]] = a[0]
			ptrType[a[1]] = a[0]
		case 65, 66, 67, 60: // access chains / ImageTexelPointer: type id
			ptrType[a[1]] = a[0]
		case 71: // OpDecorate target decoration ops
			if decor[a[0]] == nil {
				decor[a[0]] = map[uint32][]uint32{}
			}
			decor[a[0]][a[1]] = append([]uint32(nil), a[2:]...)
			if a[1] == 11 && len(a) > 2 {
				builtinFeature(s, a[2])
			}
		case 72: // OpMemberDecorate struct member decoration ops
			m := mdecor[a[0]]
			if m == nil {
				m = map[uint32]map[uint32][]uint32{}
				mdecor[a[0]] = m
			}
			if m[a[1]] == nil {
				m[a[1]] = map[uint32][]uint32{}
			}
			m[a[1]][a[2]] = append([]uint32(nil), a[3:]...)
			if a[2] == 11 && len(a) > 3 {
				builtinFeature(s, a[3])
			}
		case 227, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 5614, 5615, 6035: // atomics with result
			atomics = append(atomics, atomicUse{kind: atomicKind(op), result: a[0], ptr: a[2]})
		case 228: // OpAtomicStore pointer ...
			atomics = append(atomics, atomicUse{kind: "store", result: 0, ptr: a[0]})
		case 252:
			s.add("shader.kill")
		case 5380, 5381:
			s.add("shader.demote")
		case 4416:
			s.add("shader.terminate")
		case 305, 306, 307, 308, 309, 310, 311, 312, 313, 314, 315, 316, 320:
			s.add("sparse.residency")
		}
		if op >= 333 && op <= 366 || op == 4431 || op == 4421 || op == 4422 || op == 4432 {
			if k := subgroupKind(op, a); k != "" {
				subgroupOps[k] = true
			}
		}
		i += wc
	}
	// capabilities
	for c := range caps {
		s.Capabilities = append(s.Capabilities, capName(c))
		if f := capFeature(c); f != "" {
			s.add(f)
		}
	}
	sort.Strings(s.Capabilities)
	for e := range exts {
		s.Extensions = append(s.Extensions, e)
	}
	sort.Strings(s.Extensions)
	for m := range modes {
		s.Modes = append(s.Modes, m)
	}
	sort.Strings(s.Modes)
	for m := range models {
		s.Models = append(s.Models, modelName(m))
		if f := modelFeature(m); f != "" {
			s.add(f)
		}
	}
	sort.Strings(s.Models)
	s.Stage, s.stageIdx = modelName(firstModel), firstModel
	if !haveModel {
		s.Stage = "none"
	}
	// subgroup operations x stage
	for k := range subgroupOps {
		s.add("subgroup.ops." + k)
		switch firstModel {
		case 0, 1, 2, 3, 5364, 5365, 5267, 5268:
			if firstModel != 5364 && firstModel != 5365 && firstModel != 5267 && firstModel != 5268 {
				s.add("subgroup.stage.vertex_pipeline")
			}
		}
		if k == "quad" && firstModel != 4 && firstModel != 5 {
			s.add("subgroup.quad_all_stages")
		}
	}
	// atomics
	pointee := func(ptrID uint32) (storage uint32, elem *spvType) {
		pt := types[ptrType[ptrID]]
		if pt == nil || pt.op != 32 {
			return 0, nil
		}
		return pt.sc, types[pt.elem]
	}
	for _, at := range atomics {
		sc, et := pointee(at.ptr)
		if et == nil && at.result != 0 {
			et = types[at.result]
		}
		if et == nil {
			continue
		}
		cls := "buffer"
		switch sc {
		case 4:
			cls = "shared"
		case 11:
			cls = "image"
		}
		switch et.op {
		case 21:
			if et.width == 64 {
				s.add("atomics.int64." + cls)
			} else if cls == "image" {
				s.add("atomics.image.int32")
			}
		case 22:
			s.add(fmt.Sprintf("atomics.float%d.%s.%s", et.width, cls, at.kind))
		}
	}
	// sizes
	var sizeOf func(id uint32) uint32
	sizeOf = func(id uint32) uint32 {
		t := types[id]
		if t == nil {
			return 0
		}
		switch t.op {
		case 21, 22:
			return (t.width + 7) / 8
		case 23:
			return sizeOf(t.elem) * t.width
		case 24:
			return sizeOf(t.elem) * t.width
		case 28:
			n := consts[t.length]
			stride := sizeOf(t.elem)
			if d, ok := decor[id][6]; ok && len(d) > 0 {
				stride = d[0]
			}
			return n * stride
		case 30:
			var end uint32
			var off uint32
			for mi, m := range t.members {
				sz := sizeOf(m)
				if d, ok := mdecor[id][uint32(mi)][35]; ok && len(d) > 0 {
					off = d[0]
				}
				if off+sz > end {
					end = off + sz
				}
				off += sz
			}
			return end
		case 32:
			return 8
		}
		return 0
	}
	s.Descriptors = map[string]uint32{}
	fmts := map[string]bool{}
	var vids []uint32
	for id := range vars {
		vids = append(vids, id)
	}
	sort.Slice(vids, func(a, b int) bool { return vids[a] < vids[b] })
	for _, id := range vids {
		pt := types[vars[id]]
		if pt == nil || pt.op != 32 {
			continue
		}
		count := uint32(1)
		el := types[pt.elem]
		for el != nil && (el.op == 28 || el.op == 29) {
			if el.op == 29 {
				s.Unbounded = true
				count = 0
				s.add("desc.runtime_array")
			} else if count != 0 {
				count *= consts[el.length]
			}
			el = types[el.elem]
		}
		var eid uint32 = pt.elem
		for {
			t := types[eid]
			if t != nil && (t.op == 28 || t.op == 29) {
				eid = t.elem
				continue
			}
			break
		}
		if el == nil {
			continue
		}
		switch pt.sc {
		case 4: // Workgroup
			s.SharedBytes += sizeOf(pt.elem)
		case 9: // PushConstant
			if n := sizeOf(pt.elem); n > s.PushBytes {
				s.PushBytes = n
			}
		case 0, 2, 12: // UniformConstant, Uniform, StorageBuffer
			if _, ok := decor[id][33]; !ok && pt.sc == 0 {
				continue
			}
			set := 0
			if d := decor[id][34]; len(d) > 0 {
				set = int(d[0])
			}
			if set > s.MaxSet {
				s.MaxSet = set
			}
			kind := ""
			switch el.op {
			case 25:
				dim, sampled, format := el.img[0], el.img[4], el.img[5]
				switch {
				case sampled == 2 && dim == 5:
					kind = "storageTexelBuffer"
				case sampled == 2:
					kind = "storageImage"
					if format != 0 {
						fmts[imageFormatName(format)] = true
					}
				case dim == 5:
					kind = "uniformTexelBuffer"
				case dim == 6:
					kind = "inputAttachment"
				default:
					kind = "sampledImage"
				}
			case 26:
				kind = "sampler"
			case 27:
				kind = "combinedImageSampler"
			case 30:
				if pt.sc == 12 || decor[el0(types, eid)][3] != nil {
					kind = "storageBuffer"
				} else {
					kind = "uniformBuffer"
				}
			}
			if kind != "" {
				s.Descriptors[kind] += count
			}
		}
	}
	for f := range fmts {
		s.ImageFormats = append(s.ImageFormats, f)
		s.add("image.format." + f)
	}
	sort.Strings(s.ImageFormats)
	if len(s.Descriptors) == 0 {
		s.Descriptors = nil
	}
	for f := range s.need {
		s.Features = append(s.Features, f)
	}
	sort.Strings(s.Features)
	return s, nil
}

func el0(types map[uint32]*spvType, id uint32) uint32 { return id }

func atomicKind(op uint16) string {
	switch op {
	case 234, 235, 232, 233, 6035:
		return "add"
	case 236, 237, 238, 239, 5614, 5615:
		return "minmax"
	}
	return "other"
}

func floatModeID(m uint32) string {
	switch m {
	case 4459:
		return "denorm_preserve"
	case 4460:
		return "denorm_ftz"
	case 4461:
		return "signed_zero_inf_nan_preserve"
	case 4462:
		return "rounding_rte"
	}
	return "rounding_rtz"
}

func subgroupKind(op uint16, a []uint32) string {
	switch op {
	case 333:
		return "basic"
	case 334, 335, 336:
		return "vote"
	case 337, 338, 339, 340, 341, 342, 343, 344, 4421, 4422, 4432:
		return "ballot"
	case 345, 346:
		return "shuffle"
	case 347, 348:
		return "shuffle_relative"
	case 365, 366:
		return "quad"
	case 4431:
		return "rotate"
	}
	if op >= 349 && op <= 364 {
		if len(a) > 3 && a[3] == 3 {
			return "clustered"
		}
		return "arithmetic"
	}
	return ""
}

func builtinFeature(s *ShaderInfo, b uint32) {
	switch b {
	case 5286, 5287, 4992, 4993, 4994, 4995, 4996, 4997, 4998:
		s.add("frag.barycentric")
	case 4432, 4444:
		s.add("frag.vrs")
	case 5014:
		s.add("frag.stencil_export")
	case 4440:
		s.add("pipe.multiview")
	case 5264:
		s.add("frag.fully_covered")
	case 36, 38, 40, 41:
		s.add("subgroup.builtins")
	}
}

func execModeName(m uint32) string {
	if n, ok := execModes[m]; ok {
		return n
	}
	return fmt.Sprintf("ExecutionMode(%d)", m)
}

func modelName(m uint32) string {
	switch m {
	case 0:
		return "Vertex"
	case 1:
		return "TessellationControl"
	case 2:
		return "TessellationEvaluation"
	case 3:
		return "Geometry"
	case 4:
		return "Fragment"
	case 5:
		return "GLCompute"
	case 6:
		return "Kernel"
	case 5267:
		return "TaskNV"
	case 5268:
		return "MeshNV"
	case 5313:
		return "RayGenerationKHR"
	case 5314:
		return "IntersectionKHR"
	case 5315:
		return "AnyHitKHR"
	case 5316:
		return "ClosestHitKHR"
	case 5317:
		return "MissKHR"
	case 5318:
		return "CallableKHR"
	case 5364:
		return "TaskEXT"
	case 5365:
		return "MeshEXT"
	}
	return fmt.Sprintf("ExecutionModel(%d)", m)
}

func modelFeature(m uint32) string {
	switch m {
	case 5364, 5267:
		return "exec.task"
	case 5365, 5268:
		return "exec.mesh"
	case 3:
		return "exec.geometry"
	case 1, 2:
		return "exec.tessellation"
	case 5313, 5314, 5315, 5316, 5317, 5318:
		return "exec.raytracing"
	}
	return ""
}

func capName(c uint32) string {
	if n := capNames[c]; n != "" {
		return n
	}
	return fmt.Sprintf("Capability(%d)", c)
}

// capFeature maps a SPIR-V capability to the feature id the device is checked against. Capabilities
// without an entry become "cap.<Name>" features of unknown support so nothing is silently dropped.
func capFeature(c uint32) string {
	n := capName(c)
	if f, ok := capFeatureByName[n]; ok {
		return f
	}
	if capImplied[n] {
		return ""
	}
	return "cap." + n
}

// capImplied lists capabilities every Vulkan device must have (or that are implied by others).
var capImplied = map[string]bool{"SignedZeroInfNanPreserve": true, "DenormPreserve": true, "DenormFlushToZero": true, "RoundingModeRTE": true, "RoundingModeRTZ": true, "FloatControls2": true, "GroupNonUniformVote": true, "GroupNonUniformArithmetic": true, "GroupNonUniformBallot": true, "GroupNonUniformShuffle": true, "GroupNonUniformShuffleRelative": true, "GroupNonUniformClustered": true, "GroupNonUniformQuad": true, "SubgroupVoteKHR": true, "Shader": true, "Matrix": true, "Linkage": false, "Sampled1D": true, "Image1D": true, "SampledBuffer": true, "ImageBuffer": true, "ImageQuery": true,
	"DerivativeControl": true, "ImageBasic": true, "Int64": false, "InputAttachment": true, "ImageRect": true, "SampledRect": true, "TessellationPointSize": true, "GeometryPointSize": true,
	"UniformBufferArrayDynamicIndexing": true, "SampledImageArrayDynamicIndexing": true, "StorageBufferArrayDynamicIndexing": true, "StorageImageArrayDynamicIndexing": true,
	"GroupNonUniform": true, "SubgroupBallotKHR": true, "StorageImageReadWithoutFormat": false, "StorageImageWriteWithoutFormat": false,
	"InputAttachmentArrayDynamicIndexing": true, "UniformTexelBufferArrayDynamicIndexing": true, "StorageTexelBufferArrayDynamicIndexing": true, "VulkanMemoryModel": false,
	"VulkanMemoryModelDeviceScope": false}

var capFeatureByName = map[string]string{
	"Geometry": "exec.geometry", "Tessellation": "exec.tessellation", "Float16": "cap.float16", "Float64": "cap.float64", "Int64": "cap.int64",
	"Int64Atomics": "atomics.int64.any", "Int16": "cap.int16", "Int8": "cap.int8", "ImageGatherExtended": "tex.gather_extended",
	"StorageImageMultisample": "tex.storage_image_multisample", "ImageMSArray": "tex.storage_image_multisample", "ClipDistance": "shader.clip_distance",
	"CullDistance": "shader.cull_distance", "ImageCubeArray": "tex.cube_array", "SampledCubeArray": "tex.cube_array", "SampleRateShading": "frag.sample_rate_shading",
	"SparseResidency": "sparse.residency", "MinLod": "sparse.min_lod", "StorageImageExtendedFormats": "tex.storage_extended_formats",
	"InterpolationFunction": "frag.interpolation_function", "TransformFeedback": "xfb.transform_feedback", "GeometryStreams": "xfb.transform_feedback",
	"StorageImageReadWithoutFormat": "tex.storage_read_without_format", "StorageImageWriteWithoutFormat": "tex.storage_write_without_format",
	"MultiViewport": "shader.multi_viewport", "ShaderLayer": "shader.layer_output", "ShaderViewportIndex": "shader.viewport_index_output",
	"ShaderViewportIndexLayerEXT": "shader.viewport_index_output", "MultiView": "pipe.multiview", "DrawParameters": "shader.draw_parameters",
	"StorageBuffer16BitAccess": "cap.storage_16bit", "UniformAndStorageBuffer16BitAccess": "cap.storage_16bit", "StoragePushConstant16": "cap.storage_16bit",
	"StorageInputOutput16": "cap.storage_16bit", "StorageBuffer8BitAccess": "cap.storage_8bit", "UniformAndStorageBuffer8BitAccess": "cap.storage_8bit",
	"StoragePushConstant8": "cap.storage_8bit", "VariablePointersStorageBuffer": "mem.variable_pointers", "VariablePointers": "mem.variable_pointers",
	"RuntimeDescriptorArray": "desc.runtime_array", "ShaderNonUniform": "desc.non_uniform_indexing", "SampledImageArrayNonUniformIndexing": "desc.non_uniform_indexing",
	"StorageBufferArrayNonUniformIndexing": "desc.non_uniform_indexing", "StorageImageArrayNonUniformIndexing": "desc.non_uniform_indexing",
	"UniformBufferArrayNonUniformIndexing": "desc.non_uniform_indexing", "UniformTexelBufferArrayNonUniformIndexing": "desc.non_uniform_indexing",
	"StorageTexelBufferArrayNonUniformIndexing": "desc.non_uniform_indexing", "InputAttachmentArrayNonUniformIndexing": "desc.non_uniform_indexing",
	"RayTracingKHR": "exec.raytracing", "RayTracingNV": "exec.raytracing", "RayQueryKHR": "exec.raytracing", "FragmentFullyCoveredEXT": "frag.fully_covered",
	"FragmentBarycentricKHR": "frag.barycentric", "FragmentDensityEXT": "frag.density_map", "FragmentShaderSampleInterlockEXT": "frag.interlock",
	"FragmentShaderPixelInterlockEXT": "frag.interlock", "FragmentShaderShadingRateInterlockEXT": "frag.interlock", "FragmentShadingRateKHR": "frag.vrs",
	"ShadingRateNV": "frag.vrs", "MeshShadingEXT": "exec.mesh", "MeshShadingNV": "exec.mesh", "DemoteToHelperInvocation": "shader.demote",
	"StencilExportEXT": "frag.stencil_export", "Int64ImageEXT": "atomics.int64.image", "ShaderClockKHR": "shader.clock",
	"SampleMaskOverrideCoverageNV": "frag.sample_mask_override", "GroupNonUniformRotateKHR": "subgroup.ops.rotate", "ExpectAssumeKHR": "shader.expect_assume",
	"WorkgroupMemoryExplicitLayoutKHR": "mem.workgroup_explicit_layout", "WorkgroupMemoryExplicitLayout8BitAccessKHR": "mem.workgroup_explicit_layout",
	"WorkgroupMemoryExplicitLayout16BitAccessKHR": "mem.workgroup_explicit_layout", "PhysicalStorageBufferAddresses": "mem.buffer_device_address",
	"VulkanMemoryModel": "mem.vulkan_memory_model", "VulkanMemoryModelDeviceScope": "mem.vulkan_memory_model", "DeviceGroup": "shader.device_group",
	"GroupNonUniformPartitionedNV": "subgroup.ops.partitioned", "ShaderEnqueue": "cap.ShaderEnqueue",
}

var imageFormatNames = map[uint32]string{
	1: "Rgba32f", 2: "Rgba16f", 3: "R32f", 4: "Rgba8", 5: "Rgba8Snorm", 6: "Rg32f", 7: "Rg16f", 8: "R11fG11fB10f", 9: "R16f", 10: "Rgba16", 11: "Rgb10A2", 12: "Rg16",
	13: "Rg8", 14: "R16", 15: "R8", 16: "Rgba16Snorm", 17: "Rg16Snorm", 18: "Rg8Snorm", 19: "R16Snorm", 20: "R8Snorm", 21: "Rgba32i", 22: "Rgba16i", 23: "Rgba8i", 24: "R32i",
	25: "Rg32i", 26: "Rg16i", 27: "Rg8i", 28: "R16i", 29: "R8i", 30: "Rgba32ui", 31: "Rgba16ui", 32: "Rgba8ui", 33: "R32ui", 34: "Rgb10a2ui", 35: "Rg32ui", 36: "Rg16ui",
	37: "Rg8ui", 38: "R16ui", 39: "R8ui", 40: "R64ui", 41: "R64i",
}

func imageFormatName(f uint32) string {
	if n, ok := imageFormatNames[f]; ok {
		return n
	}
	return fmt.Sprintf("ImageFormat%d", f)
}

// vkFormatForImageFormat maps a SPIR-V storage image format to the Vulkan format name used in
// vulkaninfo's format table.
var vkFormatForImageFormat = map[string]string{
	"Rgba32f": "VK_FORMAT_R32G32B32A32_SFLOAT", "Rgba16f": "VK_FORMAT_R16G16B16A16_SFLOAT", "R32f": "VK_FORMAT_R32_SFLOAT", "Rgba8": "VK_FORMAT_R8G8B8A8_UNORM",
	"Rgba8Snorm": "VK_FORMAT_R8G8B8A8_SNORM", "Rg32f": "VK_FORMAT_R32G32_SFLOAT", "Rg16f": "VK_FORMAT_R16G16_SFLOAT", "R11fG11fB10f": "VK_FORMAT_B10G11R11_UFLOAT_PACK32",
	"R16f": "VK_FORMAT_R16_SFLOAT", "Rgba16": "VK_FORMAT_R16G16B16A16_UNORM", "Rgb10A2": "VK_FORMAT_A2B10G10R10_UNORM_PACK32", "Rg16": "VK_FORMAT_R16G16_UNORM",
	"Rg8": "VK_FORMAT_R8G8_UNORM", "R16": "VK_FORMAT_R16_UNORM", "R8": "VK_FORMAT_R8_UNORM", "Rgba16Snorm": "VK_FORMAT_R16G16B16A16_SNORM", "Rg16Snorm": "VK_FORMAT_R16G16_SNORM",
	"Rg8Snorm": "VK_FORMAT_R8G8_SNORM", "R16Snorm": "VK_FORMAT_R16_SNORM", "R8Snorm": "VK_FORMAT_R8_SNORM", "Rgba32i": "VK_FORMAT_R32G32B32A32_SINT", "Rgba16i": "VK_FORMAT_R16G16B16A16_SINT",
	"Rgba8i": "VK_FORMAT_R8G8B8A8_SINT", "R32i": "VK_FORMAT_R32_SINT", "Rg32i": "VK_FORMAT_R32G32_SINT", "Rg16i": "VK_FORMAT_R16G16_SINT", "Rg8i": "VK_FORMAT_R8G8_SINT",
	"R16i": "VK_FORMAT_R16_SINT", "R8i": "VK_FORMAT_R8_SINT", "Rgba32ui": "VK_FORMAT_R32G32B32A32_UINT", "Rgba16ui": "VK_FORMAT_R16G16B16A16_UINT", "Rgba8ui": "VK_FORMAT_R8G8B8A8_UINT",
	"R32ui": "VK_FORMAT_R32_UINT", "Rgb10a2ui": "VK_FORMAT_A2B10G10R10_UINT_PACK32", "Rg32ui": "VK_FORMAT_R32G32_UINT", "Rg16ui": "VK_FORMAT_R16G16_UINT", "Rg8ui": "VK_FORMAT_R8G8_UINT",
	"R16ui": "VK_FORMAT_R16_UINT", "R8ui": "VK_FORMAT_R8_UINT", "R64ui": "VK_FORMAT_R64_UINT", "R64i": "VK_FORMAT_R64_SINT",
}
