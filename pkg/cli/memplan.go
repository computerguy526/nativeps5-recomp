package cli

import (
	"fmt"
	"path/filepath"

	"ps4-recomp/pkg/elfloader"
	"ps4-recomp/pkg/emitter"
	"ps4-recomp/pkg/guestmem"
)

// memPlan is the guest memory decision of one translation.
type memPlan struct {
	layout  guestmem.Layout
	reason  string
	exeBias uint64
	scan    guestmem.Scan
}

// planGuestMemory scans the title (executable and every companion module, at their linked
// addresses), resolves cfg.AddrMode against the evidence, and moves the executable to its place
// in the chosen layout. cfg.AddrMode is rewritten to the resolved mode ("auto" never reaches
// the lifter).
func planGuestMemory(cfg *Config, loaded *elfloader.LoadedELF) (*memPlan, error) {
	relocatable := loaded.MinVAddr&^(guestmem.PageSize-1) == 0
	scan := guestmem.ScanTitle(loaded, relocatable)
	for _, ref := range elfloader.DiscoverCompanionModules(cfg.ElfPath, cfg.AppDir, loaded.MemoryImage) {
		if elfloader.ExcludedModuleMatch(ref.Path, cfg.ExcludeModules) {
			continue
		}
		mod, err := elfloader.LoadELF(ref.Path)
		if err != nil {
			continue // reported when the module is linked
		}
		scan.Merge(guestmem.ScanTitle(mod, true))
	}
	mode, reason, err := guestmem.Select(cfg.AddrMode, scan)
	if err != nil {
		return nil, err
	}
	layout := guestmem.LayoutFor(mode)
	bias, err := layout.ExeBias(loaded.MinVAddr, relocatable)
	if err != nil {
		return nil, err
	}
	if err := loaded.ApplyBias(bias); err != nil {
		return nil, fmt.Errorf("failed to place the executable at 0x%x: %w", layout.ExeBase, err)
	}
	if !layout.Contains(loaded.MinVAddr, loaded.MaxVAddr-loaded.MinVAddr) {
		return nil, fmt.Errorf("executable 0x%x-0x%x does not fit the %s guest window 0x%x-0x%x",
			loaded.MinVAddr, loaded.MaxVAddr, mode, layout.GuestLo, layout.GuestHi)
	}
	cfg.AddrMode = mode.String()
	return &memPlan{layout: layout, reason: reason, exeBias: bias, scan: scan}, nil
}

// manifest describes the placed title for the runtime and for tools.
func (p *memPlan) manifest(loaded *elfloader.LoadedELF, modules []emitter.GuestModule) guestmem.Manifest {
	var regions []guestmem.Region
	for _, r := range loaded.MemoryImage.Regions() {
		name := filepath.Base(loaded.FileName)
		for _, m := range modules {
			if r.Addr >= m.StartAddr&^(guestmem.PageSize-1) && r.Addr < m.EndAddr {
				name = m.FileName
			}
		}
		regions = append(regions, guestmem.NewRegion(name, r.Addr, uint64(len(r.Data))))
	}
	imageEnd := (loaded.MemoryImage.End() + guestmem.PageSize - 1) &^ (guestmem.PageSize - 1)
	return guestmem.NewManifest(p.layout, p.reason, p.exeBias, imageEnd, regions, p.scan)
}
