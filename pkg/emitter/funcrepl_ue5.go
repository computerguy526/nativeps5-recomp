package emitter

import (
	"fmt"
	"strings"
)

// HLE malloc profiles: -hle-malloc ue5-binned3.
//
// FMallocBinned3 is replaced as a whole class (docs/m3/function-hle.md): its constructor stores the
// vtable pointer and nothing else; Malloc, Realloc, Free, QuantizeSize, GetAllocationSize, Trim, the
// thread-cache hooks and the destructor become host shims over a native allocator. The functions it
// leaves alone (the FMalloc base-class defaults TryMalloc, TryRealloc, MallocZeroed, ... and the
// stubs that return a constant) only call back through the vtable or touch no state, so they run
// translated and end up in the native Malloc / Realloc.
//
// A profile is one exact binary: every function is identified by the SHA-256 of its bytes, and the
// vtable slots that reach it are checked. Addresses are link-time addresses (before the module bias)
// and were located statically; the method is in docs/m3/function-hle.md.

// UE5Binned3Modes are the values -hle-malloc accepts.
var UE5Binned3Modes = []string{"ue5-binned3"}

const (
	shimUeCtor     = "shim_ue5_malloc_ctor"
	shimUeMalloc   = "shim_ue5_malloc_malloc"
	shimUeRealloc  = "shim_ue5_malloc_realloc"
	shimUeFree     = "shim_ue5_malloc_free"
	shimUeQuantize = "shim_ue5_malloc_quantize"
	shimUeGetSize  = "shim_ue5_malloc_getsize"
	shimUeNop      = "shim_ue5_malloc_nop"
)

// ue5Binned3Profiles are the titles with a known FMallocBinned3.
var ue5Binned3Profiles = []*ReplacementSet{
	{
		// UE 5.6 PS5 build, MallocBinned3.cpp; GMalloc at 0x6c800a8 (+0x400000), object 0x2420 bytes in .bss.
		Name: "ue5-binned3/PPSA30552",
		Funcs: []Replacement{
			{Name: "FMallocBinned3::FMallocBinned3", Addr: 0x9c2b0, Size: 2715, SHA256: "737055248ce6974f300d60699f5badcba39871e4f995b0a7dbd926a99426d498", Shim: shimUeCtor},
			{Name: "FMallocBinned3::~FMallocBinned3 (D1)", Addr: 0x9d8a0, Size: 965, SHA256: "97fb00fd774f6bb4a6c69e019b08b496ae0ad64ceddba27a5e191e77ffb40566", Shim: shimUeNop},
			{Name: "FMallocBinned3::Malloc", Addr: 0xa11c0, Size: 216, SHA256: "3296f9e69672b15fa7682af7638771a5d4052c626b5a84e0022f0e2793ad7c6b", Shim: shimUeMalloc},
			{Name: "FMallocBinned3::Realloc", Addr: 0xa12a0, Size: 576, SHA256: "4f09c3b75d74dea1acf24010cae5e076878794fef27769ac514ab0613f291d28", Shim: shimUeRealloc},
			{Name: "FMallocBinned3::Free", Addr: 0xa14e0, Size: 204, SHA256: "598b25076ba177e63e00da0e8314de60b33ca6464e6000acf51a79363c259530", Shim: shimUeFree},
			{Name: "FMallocBinned3::QuantizeSize", Addr: 0xa15b0, Size: 220, SHA256: "d1a29e6333c920960d511f40d18b8ff67cac0c0c847df490bfb09d02f0c3f061", Shim: shimUeQuantize},
			{Name: "FMallocBinned3::GetAllocationSize", Addr: 0xa1690, Size: 65, SHA256: "2493c49bc9fef222663421ca6a8de8808a27a73c60cc3890c84b97dbf9179944", Shim: shimUeGetSize},
			{Name: "FMallocBinned3::Trim", Addr: 0x9f300, Size: 15, SHA256: "8e13e2b96d28b571dfd267147ea9f9c6bd443145cc79f5a7f4fe7595e4b32d9b", Shim: shimUeNop},
			{Name: "FMallocBinned3::SetupTLSCachesOnCurrentThread", Addr: 0xa16e0, Size: 82, SHA256: "dba27b73ea9bfce6585015f5ae995119a28e9895dafa44384740114bfa2b6795", Shim: shimUeNop},
			{Name: "FMallocBinned3::MarkTLSCachesAsUsedOnCurrentThread", Addr: 0xa1740, Size: 57, SHA256: "f0fccb23d3e83af767f42832e9453625f9eab5a50ae4751d6ed2206b4c642698", Shim: shimUeNop},
			{Name: "FMallocBinned3::MarkTLSCachesAsUnusedOnCurrentThread", Addr: 0xa1780, Size: 57, SHA256: "57db9a4958ddace4a0c55a153586a0f89e1358919520e2acc21c90d2da31261c", Shim: shimUeNop},
			{Name: "FMallocBinned3::ClearAndDisableTLSCachesOnCurrentThread", Addr: 0xa17c0, Size: 179, SHA256: "6958ac9df222532cd83989d3ba7854d8f94e209f6d54134b59a7a59d957e0d5f", Shim: shimUeNop},
		},
		VTableChecks: []VTableCheck{
			{0x5dddad0, 0, 0x9d8a0}, // ~FMallocBinned3 (D1)
			{0x5dddad0, 6, 0xa11c0}, // Malloc
			{0x5dddad0, 8, 0xa12a0}, // Realloc
			{0x5dddad0, 10, 0xa14e0},
			{0x5dddad0, 13, 0xa15b0},
			{0x5dddad0, 14, 0xa1690},
			{0x5dddad0, 15, 0x9f300},
			{0x5dddad0, 16, 0xa16e0},
			{0x5dddad0, 17, 0xa1740},
			{0x5dddad0, 18, 0xa1780},
			{0x5dddad0, 19, 0xa17c0},
		},
		Params: []ReplParam{{Key: "ue5m.vtable", Addr: 0x5dddad0}},
	},
}

// ApplyUE5Binned3 applies the table for mode (-hle-malloc) that matches the image, trying every
// profile of the mode with the check of ApplyReplacements; fragment as there. It applies the first
// that holds. When none does, the error says why each failed.
func (e *AsmEmitter) ApplyUE5Binned3(mode string, fragment bool) (*ReplReport, error) {
	if mode != "ue5-binned3" {
		return nil, fmt.Errorf("-hle-malloc %q: unknown mode (supported: %s)", mode, strings.Join(UE5Binned3Modes, ", "))
	}
	var why []string
	for _, p := range ue5Binned3Profiles {
		rep, err := e.ApplyReplacements(p, fragment)
		if err == nil {
			return rep, nil
		}
		why = append(why, fmt.Sprintf("%s: %v", p.Name, err))
	}
	return nil, fmt.Errorf("-hle-malloc %s: no profile matches this binary (%d known)\n  %s\n  (add one, or pass the functions with -hle-replace; docs/m3/function-hle.md)", mode, len(ue5Binned3Profiles), strings.Join(why, "\n  "))
}
