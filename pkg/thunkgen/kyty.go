// SPDX-License-Identifier: GPL-2.0-or-later

package thunkgen

import (
	_ "embed"
	"fmt"
	"sort"
	"strings"

	"ps4-recomp/pkg/elfloader"
)

// NPS5_GPU=kyty routing (docs/m3/kyty-seam.md).
//
// kyty_nids.tsv lists the graphics exports ASTRO BOT imports from libSceAgc, libSceAgcDriver and libSceVideoOut
// (nid, prx, Kyty implementation name, binding): the export table of AstroHybrid's kyty_gpu.dll (kytyGpuNids.inc
// after its patch 0005, the libkernel memory NIDs left out: guest memory stays with the runtime). The thunk of each
// calls through a function pointer instead of the AnyPS5 function, statically initialised with that function, so
// the default path is the same load-and-branch a dylib call is; nps5_kyty.c rebinds the pointers when the switch
// is on.

//go:embed kyty_nids.tsv
var kytyNids string

// KytyRoute is one line of kyty_nids.tsv.
type KytyRoute struct{ NID, Lib, Function, Binding string }

// KytyRoutes returns the routed exports by NID.
func KytyRoutes() (map[string]KytyRoute, error) {
	out := map[string]KytyRoute{}
	for ln, line := range strings.Split(kytyNids, "\n") {
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		f := strings.Split(line, "\t")
		if len(f) != 4 {
			return nil, fmt.Errorf("kyty_nids.tsv:%d: want 4 fields", ln+1)
		}
		if _, dup := out[f[0]]; dup {
			return nil, fmt.Errorf("kyty_nids.tsv:%d: duplicate NID %s", ln+1, f[0])
		}
		out[f[0]] = KytyRoute{NID: f[0], Lib: f[1], Function: f[2], Binding: f[3]}
	}
	return out, nil
}

// markKytyRoutes sets the Route fields of the descriptors whose export NID is routed. Every routed NID must have
// exactly one descriptor.
func markKytyRoutes(descs []Desc, exports []string) error {
	routes, err := KytyRoutes()
	if err != nil {
		return err
	}
	seen := map[string]string{}
	for i := range descs {
		d := &descs[i]
		nid := elfloader.CalculateNID(exports[i])
		r, ok := routes[nid]
		if !ok {
			continue
		}
		if prev, dup := seen[nid]; dup {
			return fmt.Errorf("kyty route %s: both %s and %s have this NID", nid, prev, exports[i])
		}
		seen[nid] = exports[i]
		d.RouteNID, d.RouteLib = nid, r.Lib
		if !d.Indirect {
			d.RouteTarget = d.HLE
			d.HLE = "aps5_rt_" + d.HLE
		}
	}
	for nid, r := range routes {
		if _, ok := seen[nid]; !ok {
			return fmt.Errorf("kyty route %s (%s %s): no AnyPS5 export has this NID", nid, r.Lib, r.Function)
		}
	}
	return nil
}

// KytyRouteDefs renders, for the routed descriptors: the definitions of the function pointers of the directly
// linked ones, one adapter per route that calls the kyty entry with the guest's untranslated pointer values
// (KYTY_GPU_POINTERS_GUEST), and the table nps5_kyty_routes (sorted by NID).
//
// The adapter has the pointer's prototype, so the thunk calls it like the AnyPS5 function: scalar and float
// parameters arrive marshalled (also the ones the guest passed on its stack) and are passed on; pointer parameters
// are re-read from the guest registers; a pointer result is given the bias the thunk takes off again. The kyty
// entry is called with the same C prototype, which is what makes stack-passed narrow integers land where an arm64
// callee reads them (the Apple ABI packs them; a uniform 8-byte-slot call would not).
func KytyRouteDefs(descs []Desc) (string, error) {
	var routed []*Desc
	for i := range descs {
		if descs[i].RouteNID != "" {
			routed = append(routed, &descs[i])
		}
	}
	sort.Slice(routed, func(i, j int) bool { return routed[i].RouteNID < routed[j].RouteNID })
	var b strings.Builder
	b.WriteString("// ---- NPS5_GPU=kyty routes (pkg/thunkgen/kyty.go, nps5_kyty.c) ----\n")
	for idx, d := range routed {
		var types, params, args []string
		usesCtx := false
		g := 0
		var unused []string
		for i, a := range d.Args {
			v := fmt.Sprintf("a%d", i)
			sep := " "
			if strings.HasSuffix(a.CType, "*") {
				sep = ""
			}
			types = append(types, a.CType)
			params = append(params, a.CType+sep+v)
			switch a.Kind {
			case Float:
				args = append(args, v)
				continue
			case Scalar, GuestAddr:
				args = append(args, v)
			case Ptr:
				args = append(args, fmt.Sprintf("(%s)(uintptr_t)%s", a.CType, guestArg(g)))
				unused = append(unused, v)
				usesCtx = true
			default:
				return "", fmt.Errorf("kyty route %s: argument kind %d of %s is not supported", d.RouteNID, a.Kind, d.Shim)
			}
			g++
		}
		if len(types) == 0 {
			types, params = []string{"void"}, []string{"void"}
		}
		sig := strings.Join(types, ", ")
		if d.RouteTarget != "" {
			fmt.Fprintf(&b, "%s (*%s)(%s) = %s;\n", d.CRet, d.HLE, sig, d.RouteTarget)
		}
		fmt.Fprintf(&b, "static %s kyty_raw_%d(%s) {\n", d.CRet, idx, strings.Join(params, ", "))
		if usesCtx || d.Ret == RetPtr {
			b.WriteString("  GuestContext *ctx = nps5_kyty_ctx();\n")
		}
		for _, u := range unused {
			fmt.Fprintf(&b, "  (void)%s;\n", u)
		}
		call := fmt.Sprintf("((%s (*)(%s))nps5_kyty_entry(%d))(%s)", d.CRet, sig, idx, strings.Join(args, ", "))
		switch d.Ret {
		case RetVoid:
			fmt.Fprintf(&b, "  %s;\n", call)
		case RetInt, RetGuestAddr:
			fmt.Fprintf(&b, "  return %s;\n", call)
		case RetPtr:
			fmt.Fprintf(&b, "  void *r = %s;\n  return r ? (void *)(ctx->mem_base + (uintptr_t)r) : NULL;\n", call)
		default:
			return "", fmt.Errorf("kyty route %s: result kind %d of %s is not supported", d.RouteNID, d.Ret, d.Shim)
		}
		b.WriteString("}\n")
	}
	b.WriteString("\nconst Nps5KytyRoute nps5_kyty_routes[] = {\n")
	for idx, d := range routed {
		name := d.RouteTarget
		if name == "" {
			name = strings.TrimPrefix(d.HLE, "aps5_fp_")
		}
		fmt.Fprintf(&b, "  {%q, %q, %q, (void **)&%s, (void *)kyty_raw_%d},\n", d.RouteNID, name, d.RouteLib, d.HLE, idx)
	}
	fmt.Fprintf(&b, "};\nconst unsigned nps5_kyty_route_count = %d;\n", len(routed))
	return b.String(), nil
}
