package uecvar

import (
	"encoding/binary"
	"math"
	"testing"
)

// fakeMem is a flat byte image starting at address 0.
type fakeMem []byte

func (f fakeMem) Bytes(addr, n uint64) []byte {
	if addr+n > uint64(len(f)) {
		return nil
	}
	return f[addr : addr+n]
}

type asm struct {
	b    []byte
	base uint64
}

func (a *asm) pc() uint64    { return a.base + uint64(len(a.b)) }
func (a *asm) raw(b ...byte) { a.b = append(a.b, b...) }
func (a *asm) u32(v uint32)  { a.b = binary.LittleEndian.AppendUint32(a.b, v) }
func (a *asm) rip(prefix []byte, target uint64) { // <prefix> modrm(rip) disp32 -> target
	a.raw(prefix...)
	a.u32(uint32(int64(target) - int64(a.pc()+4)))
}

func u16le(s string) []byte {
	var b []byte
	for _, c := range s {
		b = append(b, byte(c), 0)
	}
	return append(b, 0, 0)
}

func build(t *testing.T) (fakeMem, map[uint64]string) {
	mem := make(fakeMem, 0x10000)
	copy(mem[0x8000:], u16le("r.IntVar"))
	copy(mem[0x8100:], u16le("r.FloatVar"))
	copy(mem[0x8200:], u16le("r.RefVar"))
	copy(mem[0x8300:], u16le("some help"))
	binary.LittleEndian.PutUint32(mem[0x8400:], math.Float32bits(0.75))
	binary.LittleEndian.PutUint32(mem[0x9000:], 1234) // the Ref global's initial value
	a := &asm{base: 0x1000}
	// TAutoConsoleVariable<int>(name, 5, help, 0x20): mov rax,[rdi]; lea rsi,name; lea rcx,help; mov edx,5; mov r8d,0x20; call [rax+8]
	a.raw(0x48, 0x8b, 0x07)
	a.rip([]byte{0x48, 0x8d, 0x35}, 0x8000)
	a.rip([]byte{0x48, 0x8d, 0x0d}, 0x8300)
	a.raw(0xba, 5, 0, 0, 0)
	a.raw(0x41, 0xb8, 0x20, 0, 0, 0)
	a.raw(0xff, 0x50, 0x08)
	a.rip([]byte{0x48, 0x8d, 0x1d}, 0xa000) // lea rbx,[object]
	a.raw(0x48, 0x89, 0xc6)                 // mov rsi,rax
	a.raw(0x48, 0x89, 0xdf)                 // mov rdi,rbx
	a.raw(0xe8, 0, 0, 0, 0)                 // call FAutoConsoleObject::FAutoConsoleObject
	a.raw(0xc3)
	// TAutoConsoleVariable<float>(name, 0.75f, help, 0x40): VEX-free form: movss xmm0,[rip+k]
	a.raw(0xcc, 0xcc)
	a.rip([]byte{0xf3, 0x0f, 0x10, 0x05}, 0x8400)
	a.rip([]byte{0x48, 0x8d, 0x35}, 0x8100)
	a.rip([]byte{0x48, 0x8d, 0x15}, 0x8300) // lea rdx,help
	a.raw(0xb9, 0x40, 0, 0, 0)              // mov ecx,0x40
	a.raw(0xff, 0x50, 0x10)                 // call [rax+0x10]
	a.rip([]byte{0x48, 0x8d, 0x1d}, 0xa100)
	a.raw(0x48, 0x89, 0xc6, 0x48, 0x89, 0xdf, 0xe8, 0, 0, 0, 0, 0xc3)
	// FAutoConsoleVariableRef(int&): lea rdx,global; lea rsi,name; lea rcx,help; mov r8d,1; call [rax+0x30]
	a.raw(0xcc)
	a.rip([]byte{0x48, 0x8d, 0x15}, 0x9000)
	a.rip([]byte{0x48, 0x8d, 0x35}, 0x8200)
	a.rip([]byte{0x48, 0x8d, 0x0d}, 0x8300)
	a.raw(0x41, 0xb8, 1, 0, 0, 0)
	a.raw(0xff, 0x50, 0x30)
	a.rip([]byte{0x48, 0x8d, 0x1d}, 0xa200)
	a.raw(0x48, 0x89, 0xc6, 0x48, 0x89, 0xdf, 0xe8, 0, 0, 0, 0, 0xc3)
	copy(mem[0x1000:], a.b)
	names := UTF16Names(mem, []Range{{0x8000, 0x8400}})
	return mem, names
}

func TestUTF16Names(t *testing.T) {
	_, names := build(t)
	if names[0x8000] != "r.IntVar" || names[0x8100] != "r.FloatVar" || names[0x8200] != "r.RefVar" {
		t.Fatalf("names: %v", names)
	}
	if _, ok := names[0x8300]; ok {
		t.Errorf("'some help' is not a cvar name")
	}
}

func TestXref(t *testing.T) {
	mem, names := build(t)
	cv := Xref(mem, Range{0x1000, 0x2000}, names)
	FillRefDefaults(mem, cv)
	got := map[string]Cvar{}
	for _, c := range cv {
		got[c.Name] = c
	}
	if len(got) != 3 {
		t.Fatalf("found %d: %+v", len(got), cv)
	}
	i := got["r.IntVar"]
	if i.Type != "int" || i.Kind != KindTACV || i.Default != "5" || i.Flags != 0x20 || i.Storage != 0xa000 || i.Slot != 8 {
		t.Errorf("int: %+v", i)
	}
	f := got["r.FloatVar"]
	if f.Type != "float" || f.Default != "0.75" || f.Flags != 0x40 || f.Storage != 0xa100 {
		t.Errorf("float: %+v", f)
	}
	r := got["r.RefVar"]
	if r.Kind != KindRef || r.Type != "int" || r.Storage != 0x9000 || r.Default != "1234" || r.Flags != 1 {
		t.Errorf("ref: %+v", r)
	}
}

func TestXrefIgnoresNonRegistration(t *testing.T) {
	mem := make(fakeMem, 0x10000)
	copy(mem[0x8000:], u16le("r.Lookup"))
	a := &asm{base: 0x1000}
	a.rip([]byte{0x48, 0x8d, 0x3d}, 0x8000) // lea rdi,name (a lookup by name, not a registration)
	a.raw(0xe8, 0, 0, 0, 0, 0xc3)
	copy(mem[0x1000:], a.b)
	if cv := Xref(mem, Range{0x1000, 0x2000}, map[uint64]string{0x8000: "r.Lookup"}); len(cv) != 0 {
		t.Errorf("found %+v", cv)
	}
}

func TestFindRIPRefs(t *testing.T) {
	a := &asm{base: 0x1000}
	a.rip([]byte{0x8b, 0x05}, 0x9000) // mov eax,[rip+g]
	a.raw(0x90)
	a.rip([]byte{0x83, 0x3d}, 0x9000-1) // cmp dword [rip+x], imm8 : target 0x9000-1 is not ours
	a.raw(0)
	a.rip([]byte{0x83, 0x3d}, 0x9000) // cmp dword [rip+g], 0 (imm8 after the disp)
	a.raw(0)
	refs := FindRIPRefs(a.b, 0x1000, 0x9000)
	if len(refs) != 2 || refs[0].Addr != 0x1000 {
		t.Errorf("refs: %+v", refs)
	}
}

// UE5.5 (PPSA07631) shape: a direct constructor call per variable, the default stored through a
// frame pointer: lea r14,obj; lea rsi,name; lea rcx,help; mov rdx,r13; mov r8d,0x20;
// mov dword [r13],0x80; mov rdi,r14; call ctor. And the Ref form with rdx = lea global.
func TestXrefDirectCtor(t *testing.T) {
	mem := make(fakeMem, 0x10000)
	copy(mem[0x8000:], u16le("r.Direct.Int"))
	copy(mem[0x8100:], u16le("r.Direct.Ref"))
	copy(mem[0x8300:], u16le("help"))
	binary.LittleEndian.PutUint32(mem[0x9000:], 7)
	a := &asm{base: 0x1000}
	a.rip([]byte{0x4c, 0x8d, 0x35}, 0xa000)      // lea r14,[obj]
	a.rip([]byte{0x48, 0x8d, 0x35}, 0x8000)      // lea rsi,name
	a.rip([]byte{0x48, 0x8d, 0x0d}, 0x8300)      // lea rcx,help
	a.raw(0x4c, 0x89, 0xea)                      // mov rdx,r13
	a.raw(0x41, 0xb8, 0x20, 0, 0, 0)             // mov r8d,0x20
	a.raw(0x41, 0xc7, 0x45, 0x00, 0x80, 0, 0, 0) // mov dword [r13+0],0x80
	a.raw(0x4c, 0x89, 0xf7)                      // mov rdi,r14
	a.raw(0xe8, 0x00, 0x01, 0, 0)                // call ctor
	a.raw(0xcc)
	a.rip([]byte{0x4c, 0x8d, 0x35}, 0xa100) // lea r14,[obj2]
	a.rip([]byte{0x48, 0x8d, 0x35}, 0x8100) // lea rsi,name2
	a.rip([]byte{0x48, 0x8d, 0x15}, 0x9000) // lea rdx,[global]
	a.rip([]byte{0x48, 0x8d, 0x0d}, 0x8300)
	a.raw(0x41, 0xb8, 0x20, 0, 0, 0)
	a.raw(0x4c, 0x89, 0xf7)
	a.raw(0xe8, 0x00, 0x02, 0, 0)
	copy(mem[0x1000:], a.b)
	cv := Xref(mem, Range{0x1000, 0x2000}, map[uint64]string{0x8000: "r.Direct.Int", 0x8100: "r.Direct.Ref"})
	FillRefDefaults(mem, cv)
	got := map[string]Cvar{}
	for _, c := range cv {
		got[c.Name] = c
	}
	i := got["r.Direct.Int"]
	if i.Kind != KindTACV || i.Storage != 0xa000 || i.Slot != SlotDirect || i.Flags != 0x20 || i.Default != "128" || i.Ctor == 0 {
		t.Errorf("direct int: %+v", i)
	}
	r := got["r.Direct.Ref"]
	if r.Kind != KindRef || r.Storage != 0x9000 || r.Default != "7" || r.Slot != SlotDirect {
		t.Errorf("direct ref: %+v", r)
	}
}
