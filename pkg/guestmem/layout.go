// SPDX-License-Identifier: GPL-2.0-or-later

// Package guestmem is the translate-time side of the guest memory model (docs/m1/memory.md):
// the addressing mode, the guest virtual layout, the per-title mode selection scan and the
// memory manifest the runtime is built against.
package guestmem

import "fmt"

// Mode is how a guest address becomes a host address.
type Mode int

const (
	// Base: host = guest + HostBase. The guest sees the console layout, including addresses the
	// host can never map itself (GPU carveout, dyld shared cache).
	Base Mode = iota
	// Identity: host == guest. Guest memory must live inside the identity arena.
	Identity
)

func (m Mode) String() string {
	if m == Identity {
		return "identity"
	}
	return "base"
}

// Host and guest constants. See docs/m1/memory.md for the measurements behind them.
const (
	// PageSize is both the PS5 guest page and the arm64 macOS host page.
	PageSize = 0x4000

	// BaseHostBase is the preferred host bias in base mode: 2^40, so host = guest | 1<<40 for
	// every 40-bit guest address, and the 1 TiB reservation [2^40, 2^41) lies above everything
	// macOS places itself (carveout ends 0x70_0000_0000). The runtime may fall back to another
	// multiple of 2^40 when this one is occupied: x28 is loaded at run time, so nothing the
	// translator emits depends on the value.
	BaseHostBase = 0x100_0000_0000
	// BaseGuestSpan is the guest address space reserved in base mode: all 40 bits.
	BaseGuestSpan = 0x100_0000_0000

	// IdentityLo and IdentityHi bound the only host range that can be reserved at a fixed
	// address on arm64 macOS and still keeps guest pointers under 40 bits.
	IdentityLo = 0x70_0000_0000
	IdentityHi = 0xFC_0000_0000

	// Console layout, relative to the layout origin (0 in base mode, IdentityLo in identity).
	NullGuardSize  = 0x40_0000      // [0, 4 MiB) is never mapped
	ConsoleExeBase = 0x40_0000      // where the console maps a relocatable executable
	DynOffset      = 0x2_0000_0000  // first address the kernel hands to an unhinted mapping
	StackTopOffset = 0x7_EEFF_C000  // top of the main thread stack
	StackSize      = 0x400_0000     // 64 MiB main stack, committed lazily by the host; no grow-on-fault
	PoolOffset     = 0x10_0000_0000 // user area: memory pools, large fixed maps
	GuestEnd       = 0xFC_0000_0000 // end of the PS5 application area

	// CarveoutLo/Hi is the guest range no identity-mapped process can serve on arm64 macOS
	// (shared cache, randomised 12 GiB windows, 384 GiB GPU carveout).
	CarveoutLo = 0x1_8000_0000
	CarveoutHi = 0x70_0000_0000
)

// Layout is the guest virtual layout of one title for one mode. All fields are guest addresses
// except HostBase.
type Layout struct {
	Mode     Mode
	HostBase uint64 // preferred host bias; 0 in identity mode
	// Origin is the guest address of the layout's zero: 0 (base) or IdentityLo (identity).
	Origin uint64
	// GuestLo..GuestHi is the valid guest address window. Below GuestLo is the null guard.
	GuestLo uint64
	GuestHi uint64
	// ExeBase is where a relocatable main executable (linked at 0) is placed.
	ExeBase uint64
	// DynLo is the lowest address for mappings the guest requests without a fixed address.
	DynLo uint64
	// PoolLo is where memory pools and other large user-area reservations start.
	PoolLo   uint64
	StackTop uint64
	StackLen uint64
}

// LayoutFor returns the guest layout for mode.
func LayoutFor(mode Mode) Layout {
	var org, hostBase, hi uint64
	switch mode {
	case Identity:
		org, hi = IdentityLo, IdentityHi
	default:
		hostBase, hi = BaseHostBase, GuestEnd
	}
	l := Layout{
		Mode:     mode,
		HostBase: hostBase,
		Origin:   org,
		GuestLo:  org + NullGuardSize,
		GuestHi:  hi,
		ExeBase:  org + ConsoleExeBase,
		DynLo:    org + DynOffset,
		PoolLo:   org + PoolOffset,
		StackTop: org + StackTopOffset,
		StackLen: StackSize,
	}
	return l
}

// ReserveLo and ReserveLen give the host range the runtime must own for this layout.
func (l Layout) ReserveLo() uint64 {
	if l.Mode == Identity {
		return IdentityLo
	}
	return l.HostBase
}

func (l Layout) ReserveLen() uint64 {
	if l.Mode == Identity {
		return IdentityHi - IdentityLo
	}
	return BaseGuestSpan
}

// ExeBias returns the displacement to apply to a main executable whose lowest mapped address is
// minVAddr. A relocatable image (linked below the console executable base) moves to ExeBase; a
// fixed-address image stays in base mode and cannot be served in identity mode.
func (l Layout) ExeBias(minVAddr uint64, relocatable bool) (uint64, error) {
	floor := minVAddr &^ (PageSize - 1)
	if relocatable {
		if floor >= l.ExeBase {
			return 0, nil // already linked at or above the console base (base mode only reaches here)
		}
		return l.ExeBase - floor, nil
	}
	if l.Mode == Identity {
		return 0, fmt.Errorf("executable is linked at fixed address 0x%x and has no relocations: "+
			"it cannot be moved into the identity arena 0x%x-0x%x; title needs base addressing (-addr-mode base)",
			minVAddr, uint64(IdentityLo), uint64(IdentityHi))
	}
	if floor >= l.GuestHi {
		return 0, fmt.Errorf("executable is linked at fixed address 0x%x at or beyond the end of the guest address space 0x%x "+
			"and has no relocations", minVAddr, l.GuestHi)
	}
	if floor < l.GuestLo {
		return 0, fmt.Errorf("executable is linked at fixed address 0x%x inside the null guard [0, 0x%x) and has no relocations",
			minVAddr, l.GuestLo)
	}
	return 0, nil
}

// Contains reports whether [addr, addr+n) is inside the valid guest window.
func (l Layout) Contains(addr, n uint64) bool {
	return addr >= l.GuestLo && addr <= l.GuestHi && n <= l.GuestHi-addr
}
