// ps5nidify: make a PS5 payload-SDK ELF digestible by AnyPS5's relinker.
//
// Payload-SDK ELFs import by plain name (sceKernelWrite, strlen, ...) and have e_entry == 0
// (the CRT's _start sits at vaddr 0).  AnyPS5's libs export NID names (base64 of SHA-1(name+suffix)),
// and its relinker rejects e_entry == 0 ("Code analysis: no code entry points").  This tool
//
//  1. rewrites every undefined .dynsym name to its NID (names already containing '#' are kept),
//
//  2. sets e_entry to the address of -entry symbol (default __crt_start) when e_entry is 0.
//
//  3. adds a PT_SCE_PROCPARAM (all-zero, 0x40 bytes): AnyPS5's libc refuses to start without one.
//
// New data (stub, dynstr, procparam) lives in two new PT_LOAD segments appended past the end of the
// image and of the file; no existing section moves.  usage: ps5nidify [-entry sym] [-list] in.elf out.elf
package main

import (
	"bytes"
	"crypto/sha1"
	"encoding/binary"
	"flag"
	"fmt"
	"os"
)

var suffix = []byte{0x51, 0x8D, 0x64, 0xA6, 0x35, 0xDE, 0xD8, 0xC1, 0xE6, 0xB0, 0x39, 0xB1, 0xC3, 0xE5, 0x52, 0x30}

const b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"

// NID returns the PS4/PS5 NID of a symbol name.
func NID(name string) string {
	h := sha1.New()
	h.Write([]byte(name))
	h.Write(suffix)
	d := h.Sum(nil)[:8]
	var r [8]byte
	for i := range r {
		r[i] = d[7-i]
	}
	out := make([]byte, 0, 11)
	enc := func(t uint32, n int) {
		for k := 0; k < n; k++ {
			out = append(out, b64[(t>>(18-6*uint(k)))&63])
		}
	}
	enc(uint32(r[0])<<16|uint32(r[1])<<8|uint32(r[2]), 4)
	enc(uint32(r[3])<<16|uint32(r[4])<<8|uint32(r[5]), 4)
	enc(uint32(r[6])<<16|uint32(r[7])<<8, 3)
	return string(out)
}

type phdr struct {
	off                              int // file offset of the header itself
	typ, flags                       uint32
	offset, vaddr, paddr, filesz, mz uint64
}

func die(f string, a ...any) { fmt.Fprintf(os.Stderr, "ps5nidify: "+f+"\n", a...); os.Exit(1) }

func main() {
	entrySym := flag.String("entry", "", "use this symbol's address as e_entry (default: generate a stub that calls main, bypassing the SDK CRT)")
	list := flag.Bool("list", false, "print name -> NID for each import and exit")
	flag.Parse()
	if (!*list && flag.NArg() != 2) || (*list && flag.NArg() != 1) {
		die("usage: ps5nidify [-entry sym] [-list] in.elf [out.elf]")
	}
	b, err := os.ReadFile(flag.Arg(0))
	if err != nil {
		die("%v", err)
	}
	le := binary.LittleEndian
	if len(b) < 64 || string(b[:4]) != "\x7fELF" || b[4] != 2 || le.Uint16(b[18:]) != 62 {
		die("not an x86-64 ELF64")
	}
	phoff, phentsz, phnum := int(le.Uint64(b[32:])), int(le.Uint16(b[54:])), int(le.Uint16(b[56:]))
	var ph []phdr
	for i := 0; i < phnum; i++ {
		o := phoff + i*phentsz
		ph = append(ph, phdr{o, le.Uint32(b[o:]), le.Uint32(b[o+4:]), le.Uint64(b[o+8:]), le.Uint64(b[o+16:]), le.Uint64(b[o+24:]), le.Uint64(b[o+32:]), le.Uint64(b[o+40:])})
	}
	v2o := func(v uint64) int {
		for _, p := range ph {
			if p.typ == 1 && v >= p.vaddr && v < p.vaddr+p.filesz {
				return int(v - p.vaddr + p.offset)
			}
		}
		die("vaddr %#x not in a LOAD segment", v)
		return 0
	}
	var dynOff = -1
	for _, p := range ph {
		if p.typ == 2 {
			dynOff = int(p.offset)
		}
	}
	if dynOff < 0 {
		die("no PT_DYNAMIC")
	}
	tags := map[int64]int{} // tag -> file offset of its value slot
	vals := map[int64]uint64{}
	for o := dynOff; ; o += 16 {
		t := int64(le.Uint64(b[o:]))
		if t == 0 {
			break
		}
		if _, ok := tags[t]; !ok {
			tags[t], vals[t] = o+8, le.Uint64(b[o+8:])
		}
	}
	for _, t := range []int64{5, 6, 10} {
		if _, ok := vals[t]; !ok {
			die("missing dynamic tag %d", t)
		}
	}
	strOff, strSz, symOff := v2o(vals[5]), int(vals[10]), v2o(vals[6])
	// dynsym count: from section headers (SHT_DYNSYM); fall back to gap to DT_STRTAB / GNU_HASH.
	nsym := 0
	shoff, shentsz, shnum := int(le.Uint64(b[40:])), int(le.Uint16(b[58:])), int(le.Uint16(b[60:]))
	for i := 0; i < shnum && shoff > 0; i++ {
		o := shoff + i*shentsz
		if le.Uint32(b[o+4:]) == 11 { // SHT_DYNSYM
			nsym = int(le.Uint64(b[o+32:]) / 24)
		}
	}
	if nsym == 0 {
		end := vals[5]
		if g, ok := vals[0x6ffffef5]; ok && g > vals[6] && g < end {
			end = g
		}
		nsym = int((end - vals[6]) / 24)
	}
	cstr := func(o int) string { return string(b[o : o+bytes.IndexByte(b[o:], 0)]) }

	type imp struct {
		idx  int
		name string
		nid  string
	}
	var imps []imp
	for i := 1; i < nsym; i++ {
		o := symOff + i*24
		nameOff := int(le.Uint32(b[o:]))
		shndx := le.Uint16(b[o+6:])
		name := cstr(strOff + nameOff)
		if shndx != 0 || name == "" {
			continue
		}
		n := name
		for j := range name {
			if name[j] == '#' {
				n = ""
				break
			}
		}
		if n == "" {
			continue
		}
		imps = append(imps, imp{i, name, NID(name)})
	}
	if *list {
		for _, im := range imps {
			fmt.Printf("%s %s\n", im.name, im.nid)
		}
		return
	}
	lookup := func(name string) uint64 {
		for i := 0; i < shnum && shoff > 0; i++ {
			o := shoff + i*shentsz
			if le.Uint32(b[o+4:]) != 2 { // SHT_SYMTAB
				continue
			}
			so, ssz, link := int(le.Uint64(b[o+24:])), int(le.Uint64(b[o+32:])), int(le.Uint32(b[o+40:]))
			sto := int(le.Uint64(b[shoff+link*shentsz+24:]))
			for k := 0; k < ssz/24; k++ {
				e := so + k*24
				if cstr(sto+int(le.Uint32(b[e:]))) == name && le.Uint16(b[e+6:]) != 0 {
					return le.Uint64(b[e+8:])
				}
			}
		}
		die("symbol %q not found in .symtab (ELF stripped?)", name)
		return 0
	}

	// ---- new segments -------------------------------------------------------------------------
	const page = 0x4000
	var imageEnd uint64
	hasProc := false
	for _, p := range ph {
		if p.typ == 1 && p.vaddr+p.mz > imageEnd {
			imageEnd = p.vaddr + p.mz
		}
		if p.typ == 0x61000001 {
			hasProc = true
		}
	}
	// stub segment (code)
	codeV := (imageEnd + page - 1) &^ (page - 1)
	var stub []byte
	if le.Uint64(b[24:]) == 0 {
		if *entrySym != "" {
			le.PutUint64(b[24:], lookup(*entrySym))
		} else {
			// The SDK CRT (__crt_start) expects the elfldr's payload_args and resolves libkernel with raw
			// syscalls through its own rtld; neither exists under AnyPS5.  Emit a stub that calls
			// main(argc, argv, envp) from the AnyPS5 entry convention (rdi = &argc), then returns
			// (AnyPS5 aborts with "guest entry returned"; the relinker forbids a raw exit syscall here).
			stub = []byte{0x55, 0x48, 0x89, 0xe5, 0x48, 0x89, 0xf8, 0x8b, 0x38, 0x48, 0x8d, 0x70, 0x08, 0x48, 0x8d, 0x54, 0xfe, 0x08, 0xe8, 0, 0, 0, 0, 0x5d, 0xc3}
			rel := int64(lookup("main")) - int64(codeV+19+4)
			le.PutUint32(stub[19:], uint32(int32(rel)))
			le.PutUint64(b[24:], codeV)
		}
	}
	dataV := codeV
	if len(stub) > 0 {
		dataV = codeV + page
	}
	var data []byte
	var dtStrLen, newRelaV, newRelaSz uint64
	strV := uint64(0)
	if len(imps) > 0 {
		strV = dataV
		newTab := append([]byte{}, b[strOff:strOff+strSz]...)
		for _, im := range imps {
			le.PutUint32(b[symOff+im.idx*24:], uint32(len(newTab)))
			newTab = append(newTab, im.nid...)
			newTab = append(newTab, 0)
		}
		data = append(data, newTab...)
		le.PutUint64(b[tags[5]:], strV)
		le.PutUint64(b[tags[10]:], uint64(len(newTab)))
		dtStrLen = uint64(len(newTab))
	}
	procV := uint64(0)
	if !hasProc {
		// A minimal valid SCE process-parameter chain so that AnyPS5's libc heap initialisation
		// succeeds with default allocator: procparam{size=0x40,'ORBI',+0x38->libcparam},
		// libcparam{size=0x38,+0x30->replacement table}, replacement{size=0x78,kind=2, no hooks}.
		// The two pointers need R_X86_64_RELATIVE relocations, so .rela.dyn is copied here, extended.
		align := func() {
			for len(data)%16 != 0 {
				data = append(data, 0)
			}
		}
		align()
		procOff := len(data)
		data = append(data, make([]byte, 0x40)...)
		libcOff := len(data)
		data = append(data, make([]byte, 0x40)...)
		replOff := len(data)
		data = append(data, make([]byte, 0x80)...)
		procV = dataV + uint64(procOff)
		le.PutUint64(data[procOff:], 0x40)
		le.PutUint32(data[procOff+8:], 0x4942524f)
		le.PutUint64(data[libcOff:], 0x40)
		le.PutUint64(data[replOff:], 0x78)
		le.PutUint64(data[replOff+8:], 2)
		relaV, okA := vals[7]
		relaSz, okS := vals[8]
		if okA && okS {
			align()
			ro := v2o(relaV)
			relaOff := len(data)
			data = append(data, b[ro:ro+int(relaSz)]...)
			for _, r := range [][2]uint64{{procV + 0x38, dataV + uint64(libcOff)}, {dataV + uint64(libcOff) + 0x30, dataV + uint64(replOff)}} {
				var e [24]byte
				le.PutUint64(e[0:], r[0])
				le.PutUint64(e[8:], 8) // R_X86_64_RELATIVE
				le.PutUint64(e[16:], r[1])
				data = append(data, e[:]...)
			}
			newRelaV = dataV + uint64(relaOff)
			newRelaSz = relaSz + 48
			le.PutUint64(b[tags[7]:], newRelaV)
			le.PutUint64(b[tags[8]:], newRelaSz)
		} else {
			die("no DT_RELA/DT_RELASZ: cannot relocate process parameters")
		}
	}
	type newSeg struct {
		typ, flags    uint32
		v, filesz, al uint64
		body          []byte
	}
	var segs []newSeg
	if len(stub) > 0 {
		segs = append(segs, newSeg{1, 7, codeV, uint64(len(stub)), page, stub})
	}
	if len(data) > 0 {
		segs = append(segs, newSeg{1, 6, dataV, uint64(len(data)), page, data})
	}
	if procV != 0 {
		segs = append(segs, newSeg{0x61000001, 4, procV, 0x40, 16, nil})
	}
	var dataOff uint64
	if len(segs) > 0 {
		firstLoad := uint64(1 << 62)
		for _, p := range ph {
			if p.typ == 1 && p.offset < firstLoad {
				firstLoad = p.offset
			}
		}
		tableEnd := phoff + (phnum+len(segs))*phentsz
		if uint64(tableEnd) > firstLoad {
			die("no room for %d more program headers", len(segs))
		}
		for _, x := range b[phoff+phnum*phentsz : tableEnd] {
			if x != 0 {
				die("space after the program header table is not free")
			}
		}
		for i, sg := range segs {
			o := phoff + (phnum+i)*phentsz
			var off uint64
			if sg.typ == 1 {
				for len(b)%page != 0 {
					b = append(b, 0)
				}
				off = uint64(len(b))
				if sg.v == dataV {
					dataOff = off
				}
				b = append(b, sg.body...)
			} else { // PT_SCE_PROCPARAM: lives inside the data segment already
				off = uint64(len(b)) // patched below
			}
			le.PutUint32(b[o:], sg.typ)
			le.PutUint32(b[o+4:], sg.flags)
			le.PutUint64(b[o+8:], off)
			le.PutUint64(b[o+16:], sg.v)
			le.PutUint64(b[o+24:], sg.v)
			le.PutUint64(b[o+32:], sg.filesz)
			le.PutUint64(b[o+40:], sg.filesz)
			le.PutUint64(b[o+48:], sg.al)
		}
		// the procparam header's file offset = data segment offset + (procV - dataV)
		for i, sg := range segs {
			if sg.typ == 0x61000001 {
				for j, d := range segs {
					if d.typ == 1 && d.v == dataV {
						o := phoff + (phnum+j)*phentsz
						le.PutUint64(b[phoff+(phnum+i)*phentsz+8:], le.Uint64(b[o+8:])+procV-dataV)
					}
				}
			}
		}
		le.PutUint16(b[56:], uint16(phnum+len(segs)))
	}
	if newRelaV != 0 {
		for i := 0; i < shnum && shoff > 0; i++ {
			o := shoff + i*shentsz
			if le.Uint32(b[o+4:]) == 4 && le.Uint64(b[o+16:]) == vals[7] { // SHT_RELA .rela.dyn
				le.PutUint64(b[o+16:], newRelaV)
				le.PutUint64(b[o+24:], dataOff+newRelaV-dataV)
				le.PutUint64(b[o+32:], newRelaSz)
			}
		}
	}
	if strV != 0 {
		// keep the .dynstr section header coherent
		for i := 0; i < shnum && shoff > 0; i++ {
			o := shoff + i*shentsz
			if le.Uint32(b[o+4:]) == 3 && le.Uint64(b[o+16:]) == vals[5] {
				le.PutUint64(b[o+16:], strV)
				le.PutUint64(b[o+24:], dataOff)
				le.PutUint64(b[o+32:], dtStrLen)
			}
		}
	}
	if err := os.WriteFile(flag.Arg(1), b, 0o755); err != nil {
		die("%v", err)
	}
	fmt.Printf("ps5nidify: %d imports rewritten to NIDs, entry=%#x\n", len(imps), le.Uint64(b[24:]))
}
