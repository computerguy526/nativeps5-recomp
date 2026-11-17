package emitter

import (
	"strings"
	"sync"

	"ps4-recomp/pkg/elfloader"
	"ps4-recomp/pkg/thunkgen"
)

// EnableAnyPS5Shims routes the imports of libSceAgc, libSceAgcDriver, libSceVideoOut, libScePad and
// libSceUserService to the generated AnyPS5 thunks (shim_aps5_<name>, pkg/runtime/modules/libSceAnyPS5)
// instead of the PS4Recomp HLE of the same names. Both the plain name and the NID of every export are
// rebound, so a retail title (NID imports) and a payload-SDK title (plain names) get the same
// function. Called once before emission (`-gpu anyps5`).
func EnableAnyPS5Shims() error {
	descs, err := thunkgen.AnyPS5Descs()
	if err != nil {
		return err
	}
	for _, d := range descs {
		name := strings.TrimPrefix(d.Shim, "shim_aps5_") // the export's own name (d.HLE is a pointer variable for indirect libraries)
		CanonicalShims[name] = d.Shim
		if nid := elfloader.CalculateNID(name); nid != "" {
			CanonicalShims[nid] = d.Shim
		}
		if nid := elfloader.CalculateNID("_" + name); nid != "" {
			CanonicalShims[nid] = d.Shim
		}
	}
	// The event queues the AnyPS5 drivers register on must be AnyPS5's own (EqueuePin): the create/wait/user-event
	// family goes to its libkernel (nps5_anyps5_equeue.c).
	for _, name := range anyps5EqueueFamily {
		shim := "shim_aps5_" + name
		CanonicalShims[name] = shim
		if nid := elfloader.CalculateNID(name); nid != "" {
			CanonicalShims[nid] = shim
		}
	}
	canonicalNIDMapOnce = sync.Once{}
	canonicalNIDMap = nil
	return nil
}

var anyps5EqueueFamily = []string{
	"sceKernelCreateEqueue", "sceKernelDeleteEqueue", "sceKernelWaitEqueue",
	"sceKernelAddUserEvent", "sceKernelAddUserEventEdge", "sceKernelTriggerUserEvent", "sceKernelDeleteUserEvent",
	"sceKernelAddAmprEvent", "sceKernelDeleteAmprEvent",
}
