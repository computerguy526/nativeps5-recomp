// SPDX-License-Identifier: GPL-2.0-or-later

package thunkgen

import (
	_ "embed"
	"fmt"
	"strings"
)

// AnyPS5 export table (docs/m2/anyps5-integration.md).
//
// anyps5_sigs.tsv lists the C signature of every export of the AnyPS5 libraries the runtime routes
// imports to (libSceAgc, libSceAgcDriver, libSceVideoOut, libScePad, libSceUserService); it is
// extracted from AnyPS5's APS5_VABI definitions by tools/anyps5_sigs.py. A descriptor is derived
// from the C types:
//
//	pointer          -> Ptr (host pointer, translated). AnyPS5 convention (docs/nps5-gpu-addr.md): a pointer
//	                    PARAMETER is a host pointer; guest addresses the library stores in or reads from the
//	                    pointed-to memory (PM4 words, Packet::addr, Shader fields, out-slots) stay guest values
//	                    because only the argument itself is translated, never the memory it points to.
//	GuestAddr names  -> GuestAddr: the parameter itself is a guest address the library keys by (anyps5GuestAddr).
//	float/double     -> Float (xmm)
//	other            -> Scalar
//	returned pointer -> RetPtr (host pointer into the guest window, translated back)
//
// The shim of export X is shim_aps5_X and calls the native function X of the AnyPS5 library.

//go:embed anyps5_sigs.tsv
var anyps5Sigs string

// AnyPS5Libs are the libraries routed to AnyPS5.
var AnyPS5Libs = []string{"libSceAgc", "libSceAgcDriver", "libSceVideoOut", "libScePad", "libSceUserService"}

// anyps5GuestAddr lists the pointer parameters that are guest addresses the library keeps as values
// ("function.parameter"). Everything else that is a pointer is translated.
var anyps5GuestAddr = map[string]bool{
	"sceAgcDriverSetTFRing.base":                     true, // ring base: GPU-visible memory
	"sceAgcDriverRegisterResource.memory":            true, // resource registry is keyed by guest address
	"sceAgcDriverAddEqEvent.udata":                   true, // handed back to the guest in the event
	"sceUserServiceRegisterEventCallback.callback":   true, // guest code
	"sceUserServiceUnregisterEventCallback.callback": true,
}

// routedIndirect: the runtime itself defines C functions with the plain names of these libraries' exports
// (PS4Recomp's HLE of VideoOut/Pad/UserService), so they cannot be linked by name next to AnyPS5's; they live
// in libnps5_gpu.dylib and are reached through dlsym (Desc.Indirect). libSceAgc and libSceAgcDriver have no
// namesake in the runtime and are linked directly.
var routedIndirect = map[string]bool{"libSceVideoOut": true, "libScePad": true, "libSceUserService": true}

// AnyPS5Shim returns the shim symbol of an AnyPS5 export.
func AnyPS5Shim(name string) string { return "shim_aps5_" + name }

type anyps5Sig struct {
	Lib, Name, Ret string
	Params         []anyps5Param
}

type anyps5Param struct{ Type, Name string }

func cIntType(t string) (string, bool) {
	t = strings.TrimSpace(strings.ReplaceAll(t, "std::", ""))
	switch t {
	case "int", "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
		"size_t", "uintptr_t", "unsigned", "unsigned int":
		return t, true
	case "bool":
		return "_Bool", true
	case "KernelEqueue":
		return "int64_t", true
	case "ShaderRegister": // struct { u32 offset; u32 value; }: one integer register in both ABIs
		return "uint64_t", true
	case "KernelCpumask", "KernelUseconds", "KernelModule", "SceUserServiceUserId", "UserServiceUserId":
		return "uint64_t", true // integer typedefs (AnyPS5 SceTypes.hpp); a wider register read is harmless
	}
	return "", false
}

func parseAnyPS5Sigs() ([]anyps5Sig, error) {
	var out []anyps5Sig
	for ln, line := range strings.Split(anyps5Sigs, "\n") {
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		f := strings.Split(line, "\t")
		if len(f) != 4 {
			return nil, fmt.Errorf("anyps5_sigs.tsv:%d: want 4 fields", ln+1)
		}
		s := anyps5Sig{Lib: f[0], Name: f[1], Ret: f[2]}
		p := strings.TrimSpace(f[3])
		if p != "" && p != "void" {
			for i, part := range strings.Split(p, ",") {
				part = strings.TrimSpace(part)
				// split "type name" at the last identifier when the declaration has a name
				name := fmt.Sprintf("arg%d", i)
				ty := part
				if k := strings.LastIndexAny(part, " *&"); k >= 0 && k+1 < len(part) {
					id := part[k+1:]
					if _, isType := cIntType(id); !isType && id != "float" && id != "double" && id != "void" && id != "const" &&
						id != "ShaderRegister" && id != "KernelEqueue" {
						ty, name = strings.TrimSpace(part[:k+1]), id
					}
				}
				s.Params = append(s.Params, anyps5Param{Type: ty, Name: name})
			}
		}
		out = append(out, s)
	}
	return out, nil
}

// AnyPS5Descs returns the descriptors of every routed export.
func AnyPS5Descs() ([]Desc, error) {
	sigs, err := parseAnyPS5Sigs()
	if err != nil {
		return nil, err
	}
	var descs []Desc
	var exports []string
	for _, s := range sigs {
		export := strings.TrimSuffix(s.Name, "_nid_postfix") // the name the guest imports; Name is the C symbol
		d := Desc{Group: "anyps5", Shim: AnyPS5Shim(export), HLE: s.Name, Pre: fmt.Sprintf("nps5_anyps5_enter(ctx, %q);", s.Name), Post: fmt.Sprintf("nps5_anyps5_leave(ctx, %q);", s.Name)}
		if routedIndirect[s.Lib] {
			d.Indirect, d.HLE = true, "aps5_fp_"+s.Name
		}
		rt := strings.TrimSpace(s.Ret)
		switch {
		case rt == "void":
			d.CRet, d.Ret = "void", RetVoid
		case strings.Contains(rt, "*"):
			d.CRet, d.Ret = "void *", RetPtr
		default:
			ct, ok := cIntType(rt)
			if !ok {
				return nil, fmt.Errorf("%s: unsupported return type %q", s.Name, rt)
			}
			d.CRet, d.Ret = ct, RetInt
		}
		ok := true
		for i, p := range s.Params {
			name := p.Name
			if name == "" || name == "register" || name == "default" {
				name = fmt.Sprintf("arg%d", i)
			}
			t := strings.TrimSpace(p.Type)
			switch {
			case t == "float" || t == "double":
				d.Args = append(d.Args, Arg{Kind: Float, CType: t, Name: name})
			case strings.Contains(t, "*") || t == "UserServiceEventCallback":
				kind := Ptr
				if anyps5GuestAddr[export+"."+p.Name] {
					kind = GuestAddr
				}
				if kind == GuestAddr {
					d.Args = append(d.Args, Arg{Kind: GuestAddr, CType: "uint64_t", Name: name})
				} else {
					d.Args = append(d.Args, Arg{Kind: Ptr, CType: "void *", Name: name})
				}
			default:
				ct, isInt := cIntType(t)
				if !isInt {
					ok = false
					break
				}
				d.Args = append(d.Args, Arg{Kind: Scalar, CType: ct, Name: name})
			}
		}
		if !ok {
			return nil, fmt.Errorf("%s: unsupported parameter in %v", s.Name, s.Params)
		}
		descs = append(descs, d)
		exports = append(exports, export)
	}
	if err := markKytyRoutes(descs, exports); err != nil {
		return nil, err
	}
	return descs, nil
}
