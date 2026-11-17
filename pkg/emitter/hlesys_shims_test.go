package emitter

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"

	"ps4-recomp/pkg/elfloader"
)

// Every m2-hlesys name binds by plain name and by NID, and has a C definition in the runtime
// (a bound name without a definition would be a link error in every translated title).
func TestHlesysShimsBindByNameAndNID(t *testing.T) {
	for _, n := range hlesysShims {
		want := "shim_" + n
		if got, ok := LookupShim(n); !ok || got != want {
			t.Errorf("LookupShim(%q) = %q,%v want %q", n, got, ok, want)
		}
		nid := elfloader.CalculateNID(n)
		if nid == "" {
			t.Errorf("no NID for %q", n)
			continue
		}
		if got, ok := LookupShim(nid + "#A#B"); !ok || got != want {
			t.Errorf("LookupShim(NID %s of %q) = %q,%v want %q", nid, n, got, ok, want)
		}
	}
}

// NIDs as they appear in the PPSA30552 import table (names only; no game bytes).
func TestHlesysShimsTitleNIDs(t *testing.T) {
	cases := []struct{ name, nid string }{
		{"pthread_attr_setsolosched_np", "2+pVfgiEd7A"},
	}
	for _, c := range cases {
		if got := elfloader.CalculateNID(c.name); got != c.nid {
			t.Errorf("NID(%s) = %s want %s", c.name, got, c.nid)
		}
		if got, ok := LookupShim(c.nid + "#H#I"); !ok || got != "shim_"+c.name {
			t.Errorf("LookupShim(%s) = %q,%v", c.nid, got, ok)
		}
	}
}

func TestHlesysShimsAreDefinedInRuntime(t *testing.T) {
	root := filepath.Join("..", "runtime")
	defs := map[string]bool{}
	re := regexp.MustCompile(`(?m)^(?:static\s+)?void\s+(shim_[A-Za-z0-9_]+)\s*\(\s*GuestContext`)
	_ = filepath.Walk(root, func(p string, info os.FileInfo, err error) error {
		if err != nil || info.IsDir() {
			return nil
		}
		if ext := filepath.Ext(p); ext != ".c" && ext != ".m" && ext != ".h" {
			return nil
		}
		b, rerr := os.ReadFile(p)
		if rerr != nil {
			return nil
		}
		for _, m := range re.FindAllStringSubmatch(string(b), -1) {
			if !strings.HasSuffix(p, ".h") {
				defs[m[1]] = true
			}
		}
		return nil
	})
	for _, n := range hlesysShims {
		if !defs["shim_"+n] {
			t.Errorf("shim_%s has no definition under pkg/runtime", n)
		}
	}
}

// The five libSceSaveData / thirteen AudioOut2 / thirteen Ampr names the title imports.
func TestHlesysTitleImportsBound(t *testing.T) {
	for _, n := range []string{
		"sceSaveDataCreateTransactionResource", "sceSaveDataMount3", "sceSaveDataPrepare", "sceSaveDataCommit", "sceSaveDataUmount2",
		"sceAudioOut2PortDestroy", "sceAudioOut2Initialize", "sceAudioOut2PortSetAttributes", "sceAudioOut2PortGetState",
		"sceAudioOut2ContextAdvance", "sceAudioOut2ContextPush", "sceAudioOut2UserCreate", "sceAudioOut2PortCreate",
		"sceAudioOut2ContextResetParam", "sceAudioOut2ContextQueryMemory", "sceAudioOut2ContextCreate",
		"sceAudioOut2ContextDestroy", "sceAudioOut2UserDestroy",
		"sceAmprCommandBufferConstructor", "sceAmprCommandBufferDestructor", "sceAmprCommandBufferReset",
		"sceAmprCommandBufferSetBuffer", "sceAmprCommandBufferClearBuffer", "sceAmprCommandBufferGetSize",
		"sceAmprCommandBufferGetCurrentOffset", "sceAmprCommandBufferWriteKernelEventQueue_04_00",
		"sceAmprAprCommandBufferConstructor", "sceAmprAprCommandBufferDestructor", "sceAmprAprCommandBufferReadFile",
		"sceAmprMeasureCommandSizeWriteKernelEventQueue_04_00", "sceAmprMeasureCommandSizeReadFile",
		"sceAjmBatchInitialize", "sceAjmBatchJobInitialize", "sceAjmBatchStart", "sceAjmBatchJobDecode", "sceAjmBatchJobClearContext",
		"sceKernelSetPrtAperture", "sceKernelIsTrinityMode", "sceCoredumpWriteUserString", "sceKernelAddAmprEvent",
		"sceCoredumpSetUserDataType", "sceKernelAprResolveFilepathsToIdsAndFileSizes",
		"sceKernelAprSubmitCommandBufferAndGetResult", "sceKernelAprWaitCommandBuffer",
	} {
		if _, ok := LookupShim(n); !ok {
			t.Errorf("%s is not bound", n)
		}
		if _, ok := LookupShim(elfloader.CalculateNID(n) + "#X#Y"); !ok {
			t.Errorf("NID of %s is not bound", n)
		}
	}
}

// astro-hle2: every Astro stub name has a C definition and binds by name and by NID.
func TestAstroStubsDefinedAndBound(t *testing.T) {
	defs := map[string]bool{}
	re := regexp.MustCompile(`(?m)(?:void\s+shim_(\w+)\s*\(GuestContext|^(?:FIXED|J_CTOR|J_THIS|J_CONST)\((\w+))`)
	// The sce::Json names of the list are implemented in libSceJson2/ps4_json2.c (astro-boot).
	for _, f := range []string{"../runtime/modules/libSceAstroStubs/nps5_astro_stubs.c", "../runtime/modules/libSceJson2/ps4_json2.c"} {
		src, err := os.ReadFile(f)
		if err != nil {
			t.Fatal(err)
		}
		for _, m := range re.FindAllStringSubmatch(string(src), -1) {
			defs[m[1]+m[2]] = true
		}
	}
	for _, n := range astroStubShims {
		if !defs[n] {
			t.Errorf("%s has no definition in nps5_astro_stubs.c", n)
		}
		if _, ok := LookupShim(n); !ok {
			t.Errorf("%s is not bound", n)
		}
		if _, ok := LookupShim(elfloader.CalculateNID(n) + "#X#Y"); !ok {
			t.Errorf("NID of %s is not bound", n)
		}
	}
}
