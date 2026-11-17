// SPDX-License-Identifier: GPL-2.0-or-later

package guestmem

import (
	"encoding/json"
	"fmt"
	"strings"
)

// ManifestVersion is bumped when the runtime's view of the layout changes incompatibly; the
// runtime refuses a generated config with a different version (nps5_mem.h NPS5_MEM_ABI).
const ManifestVersion = 1

// Region is one image region as loaded by the runtime.
type Region struct {
	Name string `json:"name,omitempty"`
	Addr hex    `json:"addr"`
	Size hex    `json:"size"`
}

// Manifest is memory_manifest.json: everything a tool needs to turn a host address of a run
// into a guest address and to know what the translation assumed.
type Manifest struct {
	Version    int    `json:"version"`
	AddrMode   string `json:"addr_mode"`
	ModeReason string `json:"mode_reason"`
	// HostBase is the preferred host bias (host = guest + host_base). In base mode the runtime
	// may pick another one at launch; the bias in use is in the run log and crash report.
	HostBase   hex      `json:"host_base"`
	ReserveLo  hex      `json:"host_reserve_lo"`
	ReserveLen hex      `json:"host_reserve_len"`
	PageSize   hex      `json:"page_size"`
	GuestLo    hex      `json:"guest_lo"`
	GuestHi    hex      `json:"guest_hi"`
	ExeBase    hex      `json:"exe_base"`
	ExeBias    hex      `json:"exe_bias"`
	ImageEnd   hex      `json:"image_end"`
	DynLo      hex      `json:"dyn_lo"`
	PoolLo     hex      `json:"pool_lo"`
	StackTop   hex      `json:"stack_top"`
	StackLen   hex      `json:"stack_len"`
	Regions    []Region `json:"image_regions"`
	Scan       Scan     `json:"scan"`
}

// hex marshals as a 0x-prefixed string: JSON numbers above 2^53 do not survive every reader.
type hex uint64

func (h hex) MarshalJSON() ([]byte, error) { return []byte(fmt.Sprintf("\"0x%x\"", uint64(h))), nil }

func (h *hex) UnmarshalJSON(b []byte) error {
	var s string
	if err := json.Unmarshal(b, &s); err != nil {
		return err
	}
	var v uint64
	if _, err := fmt.Sscanf(strings.TrimPrefix(s, "0x"), "%x", &v); err != nil {
		return err
	}
	*h = hex(v)
	return nil
}

// NewManifest fills a manifest from the layout and the placed image.
func NewManifest(l Layout, reason string, exeBias, imageEnd uint64, regions []Region, scan Scan) Manifest {
	imageEnd = (imageEnd + PageSize - 1) &^ (PageSize - 1) // the runtime refuses a config that is not page aligned
	return Manifest{
		Version: ManifestVersion, AddrMode: l.Mode.String(), ModeReason: reason,
		HostBase: hex(l.HostBase), ReserveLo: hex(l.ReserveLo()), ReserveLen: hex(l.ReserveLen()),
		PageSize: PageSize, GuestLo: hex(l.GuestLo), GuestHi: hex(l.GuestHi),
		ExeBase: hex(l.ExeBase), ExeBias: hex(exeBias), ImageEnd: hex(imageEnd),
		DynLo: hex(l.DynLo), PoolLo: hex(l.PoolLo), StackTop: hex(l.StackTop), StackLen: hex(l.StackLen),
		Regions: regions, Scan: scan,
	}
}

// JSON renders the manifest.
func (m Manifest) JSON() ([]byte, error) {
	b, err := json.MarshalIndent(m, "", "  ")
	if err != nil {
		return nil, err
	}
	return append(b, '\n'), nil
}

// CConfig renders the Nps5MemConfig definition the generated main.c carries (nps5_mem.h).
func (m Manifest) CConfig() string {
	mode := "NPS5_ADDR_BASE"
	if m.AddrMode == Identity.String() {
		mode = "NPS5_ADDR_IDENTITY"
	}
	var b strings.Builder
	b.WriteString("// Guest memory model of this title (memory_manifest.json, docs/m1/memory.md).\n")
	b.WriteString("const Nps5MemConfig nps5_mem_config = {\n")
	fmt.Fprintf(&b, "    .abi = %d,\n    .addr_mode = %s,\n", m.Version, mode)
	fmt.Fprintf(&b, "    .host_base = 0x%xULL,\n    .guest_lo = 0x%xULL,\n    .guest_hi = 0x%xULL,\n", uint64(m.HostBase), uint64(m.GuestLo), uint64(m.GuestHi))
	fmt.Fprintf(&b, "    .image_end = 0x%xULL,\n    .dyn_lo = 0x%xULL,\n    .pool_lo = 0x%xULL,\n", uint64(m.ImageEnd), uint64(m.DynLo), uint64(m.PoolLo))
	fmt.Fprintf(&b, "    .stack_top = 0x%xULL,\n    .stack_len = 0x%xULL,\n};\n", uint64(m.StackTop), uint64(m.StackLen))
	return b.String()
}

// NewRegion describes one image region.
func NewRegion(name string, addr, size uint64) Region {
	return Region{Name: name, Addr: hex(addr), Size: hex(size)}
}
