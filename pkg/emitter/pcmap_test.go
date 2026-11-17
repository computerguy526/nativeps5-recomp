package emitter

import (
	"bytes"
	"debug/macho"
	"encoding/binary"
	"math/rand"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strings"
	"testing"

	"ps4-recomp/pkg/disasm"
	"ps4-recomp/pkg/elfloader"
	"ps4-recomp/pkg/lifter"
)

type pcRec struct {
	hostOff uint64
	guest   uint64
	flags   uint64
}

func uleb(s []byte) (uint64, []byte) {
	var v uint64
	for sh := uint(0); ; sh += 7 {
		c := s[0]
		s = s[1:]
		v |= uint64(c&0x7f) << sh
		if c&0x80 == 0 {
			return v, s
		}
	}
}

// decodePCStream mirrors the decoder in pkg/runtime/core/nps5_diag.c.
func decodePCStream(fnGuest uint64, stream []byte, n int) []pcRec {
	var out []pcRec
	var host, guest uint64 = 0, fnGuest
	for i := 0; i < n; i++ {
		b := stream[0]
		stream = stream[1:]
		var hw, fl uint64
		var gd int64
		if b != 0 {
			hw, gd = uint64(b>>4), int64(b&15)
		} else {
			fl, stream = uleb(stream)
			hw, stream = uleb(stream)
			var z uint64
			z, stream = uleb(stream)
			gd = int64(z>>1) ^ -int64(z&1)
		}
		host += hw * 4
		guest = uint64(int64(guest) + gd)
		out = append(out, pcRec{host, guest, fl})
	}
	return out
}

func TestPCStreamRoundTrip(t *testing.T) {
	rng := rand.New(rand.NewSource(1))
	for iter := 0; iter < 200; iter++ {
		fn := uint64(0x400000 + rng.Intn(1<<20))
		var ents []pcEntry
		off, g := 0, fn
		for i := 0; i < 1+rng.Intn(60); i++ {
			off += 4 * rng.Intn(40)
			switch rng.Intn(4) {
			case 0:
				g = uint64(int64(g) + int64(rng.Intn(2000)-1000)) // a jump
			default:
				g += uint64(1 + rng.Intn(15))
			}
			ents = append(ents, pcEntry{off: off, guest: g, flags: uint8(rng.Intn(3) * rng.Intn(2))})
		}
		stream, ok := encodePCStream(fn, ents)
		if !ok {
			t.Fatal("encode failed")
		}
		got := decodePCStream(fn, stream, len(ents))
		for i, e := range ents {
			if got[i].hostOff != uint64(e.off) || got[i].guest != e.guest || got[i].flags != uint64(e.flags) {
				t.Fatalf("entry %d: got %+v want %+v", i, got[i], e)
			}
		}
	}
}

func TestLeanLine(t *testing.T) {
	cases := []struct {
		in       string
		comments bool
		want     string
		keep     bool
	}{
		{"    // 0x40: PUSH RBP", false, "", false},
		{"    // 0x40: PUSH RBP", true, "    // 0x40: PUSH RBP", true},
		{"    ldr x30, [sp], #16", false, "\tldr x30,[sp],#16", true},
		{"    ldr x30, [sp], #16", true, "    ldr x30, [sp], #16", true},
		{"    strb wzr, [x17, #0x10] // df", false, "\tstrb wzr,[x17,#0x10]", true},
		{"    brk #0 // unsupported indirect jmp", false, "\tbrk #0", true},
		{"    .asciz \"a, b // c\"", false, "\t.asciz \"a, b // c\"", true},
		{"    b.ne L_fn_0x425d60_loc_0x425dc7", false, "\tb.ne Lf425d60_425dc7", true},
		{"L_fn_0x40_loc_0x40:", false, "Lf40_40:", true},
		{"    bl _fn_0x40", false, "\tbl _fn_0x40", true},
		{"1:", false, "1:", true},
	}
	for _, c := range cases {
		got, keep := leanLine(c.in, c.comments)
		if got != c.want || keep != c.keep {
			t.Errorf("leanLine(%q, %v) = %q, %v; want %q, %v", c.in, c.comments, got, keep, c.want, c.keep)
		}
	}
}

func TestAsmSizer(t *testing.T) {
	var s asmSizer
	for _, l := range []string{"\tstp x30,x26,[sp,#-16]!", "Lf1_2:", "1:", "\tmov x0,x1", "\tb 1b", "// c", ""} {
		s.line(l)
	}
	if s.off != 12 || s.irregular {
		t.Fatalf("off=%d irregular=%v", s.off, s.irregular)
	}
	// the unsupported stub: 5-byte string (with NUL: 6) then .balign 4
	s = asmSizer{}
	s.line("\tmov x0,#1")
	s.line("\t.asciz \"hello\"")
	s.line("\t.balign 4")
	s.line("\tnop")
	if s.off != 4+8+4 || s.irregular {
		t.Fatalf("stub: off=%d irregular=%v", s.off, s.irregular)
	}
	// an inline-cache site record lives in another section: no code bytes
	s = asmSizer{}
	for _, l := range []string{"\tmov x0,x1", "\t.pushsection __DATA,__nps5_ic,regular,no_dead_strip", "\t.p2align 4", "4:", "\t.quad -1,0,-1,0,0,0", "\t.popsection", "\tadrp x1,4b@PAGE"} {
		s.line(l)
	}
	if s.off != 8 || s.irregular || s.inData {
		t.Fatalf("ic record: off=%d irregular=%v", s.off, s.irregular)
	}
	s = asmSizer{}
	s.line("\t.quad 1")
	if !s.irregular {
		t.Fatal("an inline .quad must mark the function irregular")
	}
}

// pcmapImage: a function with a conditional branch, an unsupported instruction, a ud2 and a
// loop-free tail.
func pcmapTestEmitter(t *testing.T) *AsmEmitter {
	img := make([]byte, 0x100)
	copy(img[0x40:], []byte{
		0x55,             // 40 push rbp
		0x48, 0x89, 0xe5, // 41 mov rbp, rsp
		0x83, 0xff, 0x05, // 44 cmp edi, 5
		0x75, 0x04, // 47 jne 4d
		0xd9, 0xff, // 49 fcos (no lowering)
		0x0f, 0x0b, // 4b ud2
		0x48, 0x8b, 0x07, // 4d mov rax, [rdi]
		0x89, 0x07, // 50 mov [rdi], eax
		0x5d, // 52 pop rbp
		0xc3, // 53 ret
	})
	loaded := &elfloader.LoadedELF{
		MemoryImage:  elfloader.NewImage(0, img),
		ExecRanges:   []elfloader.AddrRange{{Start: 0, End: uint64(len(img))}},
		SymbolByAddr: map[uint64]elfloader.Symbol{},
		SymbolByName: map[string]elfloader.Symbol{},
	}
	d, err := disasm.NewDisassembler(loaded)
	if err != nil {
		t.Fatal(err)
	}
	l := lifter.NewArm64Lifter(map[uint64]bool{0x40: true})
	l.SetAllowUnsupported(true)
	em := NewAsmEmitter(loaded, d, l)
	em.PCMap = true
	em.PCMapStrict = true
	return em
}

func assembleTest(t *testing.T, src, obj string) {
	t.Helper()
	args := []string{"-c", src, "-o", obj}
	if runtime.GOARCH == "arm64" {
		args = append([]string{"-target", "arm64-apple-darwin", "-mcpu=apple-m1"}, args...)
	}
	if out, err := exec.Command("clang", args...).CombinedOutput(); err != nil || len(out) > 0 {
		t.Fatalf("clang %s: %v\n%s", src, err, out)
	}
}

func machoSection(t *testing.T, obj, seg, sect string) []byte {
	t.Helper()
	f, err := macho.Open(obj)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	s := f.Section(sect)
	if s == nil || s.Seg != seg {
		t.Fatalf("%s: no section %s,%s", obj, seg, sect)
	}
	b, err := s.Data()
	if err != nil {
		t.Fatal(err)
	}
	return b
}

// The lean and the commented assembly assemble to the same code, and the compact pc map agrees
// with where the assembler really put each guest instruction (read back through labels placed
// before every instruction in the commented variant).
func TestLeanAssemblyMatchesCommentedAndPCMap(t *testing.T) {
	if runtime.GOOS != "darwin" {
		t.Skip("needs the Apple arm64 assembler")
	}
	if _, err := exec.LookPath("clang"); err != nil {
		t.Skip("no clang")
	}
	dir := t.TempDir()
	var objs [2]string
	var srcs [2]string
	for i, comments := range []bool{false, true} {
		em := pcmapTestEmitter(t)
		em.Comments = comments
		srcs[i] = filepath.Join(dir, map[bool]string{false: "lean", true: "com"}[comments]+".s")
		if err := em.emitSingleAsmChunk(srcs[i], 0, []uint64{0x40}); err != nil {
			t.Fatal(err)
		}
		if len(em.Facts.Unsupported) == 0 || em.Facts.Unsupported[0].Addr != 0x49 || em.Facts.UD2Stubs != 1 {
			t.Fatalf("facts: %+v ud2=%d", em.Facts.Unsupported, em.Facts.UD2Stubs)
		}
		if em.Facts.GuestInsts != 10 || len(em.Facts.Blocks) < 2 || len(em.Warnings) != 0 {
			t.Fatalf("facts: insts=%d blocks=%v warnings=%v", em.Facts.GuestInsts, em.Facts.Blocks, em.Warnings)
		}
		objs[i] = strings.TrimSuffix(srcs[i], ".s") + ".o"
		assembleTest(t, srcs[i], objs[i])
	}
	lean, _ := os.ReadFile(srcs[0])
	com, _ := os.ReadFile(srcs[1])
	if bytes.Contains(lean, []byte("//")) || bytes.Contains(lean, []byte("    ")) {
		t.Errorf("lean assembly has comments or space indentation:\n%s", lean)
	}
	if !bytes.Contains(com, []byte("// 0x49: ")) || len(com) <= len(lean) {
		t.Errorf("commented assembly lacks the guest comments")
	}
	if a, b := machoSection(t, objs[0], "__TEXT", "__recomp_guest"), machoSection(t, objs[1], "__TEXT", "__recomp_guest"); !bytes.Equal(a, b) {
		t.Fatal("lean and commented assembly produce different code")
	}

	// ground truth: a symbol before every instruction of the commented variant
	re := regexp.MustCompile(`(?m)^    // 0x([0-9a-f]+): `)
	n := 0
	truth := re.ReplaceAllStringFunc(string(com), func(m string) string {
		n++
		a := re.FindStringSubmatch(m)[1]
		return "Xtruth_" + a + ":\n" + m
	})
	tsrc := filepath.Join(dir, "truth.s")
	os.WriteFile(tsrc, []byte(truth), 0o644)
	assembleTest(t, tsrc, filepath.Join(dir, "truth.o"))
	f, err := macho.Open(filepath.Join(dir, "truth.o"))
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	var fnAddr uint64
	want := map[uint64]uint64{}
	for _, s := range f.Symtab.Syms {
		if s.Name == "_fn_0x40" {
			fnAddr = s.Value
		}
		if a, ok := strings.CutPrefix(s.Name, "Xtruth_"); ok {
			var g uint64
			for _, c := range a {
				g = g*16 + uint64(strings.IndexRune("0123456789abcdef", c))
			}
			want[g] = s.Value
		}
	}
	data := machoSection(t, objs[0], "__DATA", "__nps5_pcmap")
	nameLen := binary.LittleEndian.Uint32(data[20:])
	streamLen := binary.LittleEndian.Uint32(data[24:])
	nent := int(binary.LittleEndian.Uint32(data[28:]))
	hostLen := binary.LittleEndian.Uint32(data[16:])
	if nent != n || binary.LittleEndian.Uint64(data[8:]) != 0x40 || hostLen == 0 {
		t.Fatalf("header: nent=%d (want %d) hostlen=%d", nent, n, hostLen)
	}
	stream := data[32+nameLen : 32+nameLen+streamLen]
	recs := decodePCStream(0x40, stream, nent)
	flagged := 0
	for _, r := range recs {
		w, ok := want[r.guest]
		if !ok || w-fnAddr != r.hostOff {
			t.Errorf("guest 0x%x: pc map says host +0x%x, assembler says +0x%x (known=%v)", r.guest, r.hostOff, w-fnAddr, ok)
		}
		if r.flags != 0 {
			flagged++
			if r.guest != 0x49 || r.flags != 1 {
				t.Errorf("unexpected flags %+v", r)
			}
		}
	}
	if flagged != 1 {
		t.Errorf("flagged entries = %d, want the one unsupported fcos", flagged)
	}
}
