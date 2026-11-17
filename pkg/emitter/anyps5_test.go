package emitter

import (
	"testing"

	"ps4-recomp/pkg/elfloader"
)

func TestEnableAnyPS5Shims(t *testing.T) {
	saved := make(map[string]string, len(CanonicalShims))
	for k, v := range CanonicalShims {
		saved[k] = v
	}
	defer func() {
		CanonicalShims = saved
		canonicalNIDMap = nil
	}()
	for _, name := range []string{"scePadSetVibrationMode", "scePadGetTriggerEffectState", "scePadSetTriggerEffect", "scePadIsRemoteController"} {
		if s, ok := LookupShim(name); !ok || s != "shim_"+name {
			t.Errorf("default binding of %s: %q %v", name, s, ok)
		}
	}
	// default: the PS4Recomp HLE serves sceVideoOutOpen (by NID too)
	if s, ok := LookupShim("Up36PTk687E#libSceVideoOut#libSceVideoOut"); !ok || s != "shim_sceVideoOutOpen" {
		t.Fatalf("default binding: %q %v", s, ok)
	}
	if err := EnableAnyPS5Shims(); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"sceVideoOutOpen", "sceAgcDriverSubmitDcb", "scePadReadState", "sceUserServiceInitialize",
		// the four Pad imports the host HLE (libSceHlesys) answers when there is no pad backend
		"scePadSetVibrationMode", "scePadGetTriggerEffectState", "scePadSetTriggerEffect", "scePadIsRemoteController"} {
		want := "shim_aps5_" + name
		if s, ok := LookupShim(name); !ok || s != want {
			t.Errorf("%s by name: %q %v", name, s, ok)
		}
		if s, ok := LookupShim(elfloader.CalculateNID(name) + "#libX#libX"); !ok || s != want {
			t.Errorf("%s by NID: %q %v", name, s, ok)
		}
	}
	// libraries that are not routed keep their binding
	if s, ok := LookupShim("sceKernelUsleep"); ok && s == "shim_aps5_sceKernelUsleep" {
		t.Error("kernel routed to AnyPS5")
	}
}
