package emitter

import (
	"strings"
	"testing"

	"ps4-recomp/pkg/lifter"
)

// Live choice sites: an override on "mov eax, [rbx+0x2390]", an override with keep_negative on
// "mov eax, ebx", and a data32 word of a constant table.
var liveImg = map[uint64][]byte{
	0x7000: {0x8b, 0x83, 0x90, 0x23, 0x00, 0x00},             // mov eax, dword ptr [rbx + 0x2390]
	0x7100: {0x89, 0xd8},                                     // mov eax, ebx
	0x7200: {0x56, 0x00, 0x00, 0x00, 0x98, 0x00, 0x00, 0x00}, // table {86, 152}
	0x7300: {0x8b, 0x03},                                     // mov eax, [rbx]
}

func liveTestPatch() Patch {
	return Patch{Key: "render_scale", Title: "Render scale", Description: "d", DefaultOption: "auto",
		Options: []PatchOption{{Name: "auto", Label: "Automatic"}, {Name: "full", Label: "720p"}},
		Sites: []PatchSite{
			{Addr: "0x7000", Bytes: "8b8390230000", Op: PatchOverride, Values: []string{"0", "keep"}},
			{Addr: "0x7100", Bytes: "89d8", Op: PatchOverride, Values: []string{"0", "keep"}, KeepNegative: true},
			{Addr: "0x7204", Bytes: "98000000", Op: PatchData32, Values: []string{"60", "keep"}},
		}}
}

func TestPatchLiveBinds(t *testing.T) {
	e := replTestEmitter(liveImg, nil, nil)
	e.lifter = lifter.NewArm64LifterMode(nil, lifter.AddrBase)
	prof := &PatchProfile{TitleID: "TEST00001", Builds: []PatchBuild{{Version: "01.007", Patches: []Patch{liveTestPatch()}}}}
	if _, err := applyChoice(e, prof, liveImg); err != nil {
		t.Fatal(err)
	}
	st := e.patch
	o, n, d := st.sites[0x7000+replBias], st.sites[0x7100+replBias], st.sites[0x7204+replBias]
	if o.kind != lifter.PatchValueOverride || o.keepNeg || o.vals[0] != 0 || o.vals[1] != lifter.PatchOverrideKeep {
		t.Errorf("override %+v", o)
	}
	if n.kind != lifter.PatchValueOverride || !n.keepNeg || string(n.fact()) != "PV1,2,n;" || string(o.fact()) != "PV0,2;" {
		t.Errorf("keep_negative override %+v %q", n, n.fact())
	}
	if d.kind != lifter.PatchValueData32 || d.vals[0] != 60 || d.vals[1] != lifter.PatchOverrideKeep || !st.seen[0x7204+replBias] {
		t.Errorf("data %+v", d)
	}
	if err := e.CheckPatchSites(); err == nil || strings.Contains(err.Error(), "0x7204") {
		t.Errorf("only the two code sites are unseen before a lift: %v", err)
	}
	c := st.tableC()
	for _, w := range []string{"{0, 2, 0x", "{1, 4, 0x", "{2, 3, 0x", "nps5_guest_patch_values[64] = {0x0ull, 0x0ull, 0x3cull};", "vals_0_0[] = {0x0ull, 0xffffffffffffffffull};", "vals_0_2[] = {0x3cull, 0xffffffffffffffffull, 0x98ull};"} {
		if !strings.Contains(c, w) {
			t.Errorf("table lacks %q:\n%s", w, c)
		}
	}
	// Refusals: no option keeps the game; a value above 32 bits; keep_negative on a data site; an
	// override on a 64-bit load; a data word that is not the build's.
	bad := []func(p *Patch){
		func(p *Patch) { p.Sites[0].Values = []string{"0", "1"} },
		func(p *Patch) { p.Sites[0].Values = []string{"0x100000000", "keep"} },
		func(p *Patch) { p.Sites[2].KeepNegative = true },
		func(p *Patch) { p.Sites[2].Bytes = "99000000" },
		func(p *Patch) { p.Sites[2].Values = []string{"60", "61"} },
		func(p *Patch) { p.Options = nil; p.DefaultOption = ""; p.On, p.Off = "a", "b" },
	}
	for i, f := range bad {
		p := liveTestPatch()
		f(&p)
		e := replTestEmitter(liveImg, nil, nil)
		e.lifter = lifter.NewArm64LifterMode(nil, lifter.AddrBase)
		prof := &PatchProfile{TitleID: "TEST00001", Builds: []PatchBuild{{Version: "01.007", Patches: []Patch{p}}}}
		if _, err := applyChoice(e, prof, liveImg); err == nil {
			t.Errorf("bad profile %d was accepted", i)
		}
	}
	// "data own value" counts as the game's option.
	p := liveTestPatch()
	p.Sites[2].Values = []string{"60", "152"}
	e2 := replTestEmitter(liveImg, nil, nil)
	e2.lifter = lifter.NewArm64LifterMode(nil, lifter.AddrBase)
	if _, err := applyChoice(e2, &PatchProfile{TitleID: "TEST00001", Builds: []PatchBuild{{Version: "01.007", Patches: []Patch{p}}}}, liveImg); err != nil {
		t.Errorf("data site with the build's own word: %v", err)
	}
	// A 64-bit load cannot be an override.
	img := map[uint64][]byte{0x7000: {0x48, 0x8b, 0x03}, 0x7100: liveImg[0x7100], 0x7200: liveImg[0x7200]}
	p = liveTestPatch()
	p.Sites[0].Bytes = "488b03"
	e3 := replTestEmitter(img, nil, nil)
	e3.lifter = lifter.NewArm64LifterMode(nil, lifter.AddrBase)
	if _, err := applyChoice(e3, &PatchProfile{TitleID: "TEST00001", Builds: []PatchBuild{{Version: "01.007", Patches: []Patch{p}}}}, img); err == nil {
		t.Errorf("override on mov rax, [rbx] accepted")
	}
}
