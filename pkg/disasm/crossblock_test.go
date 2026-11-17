package disasm

import (
	"encoding/binary"
	"testing"
)

// Duff-style entry (Syobon 0x22c8a1): the table load and add are in a block before the loop
// head that holds the jmp.
//
//	0x100: and ebx, 3
//	0x103: lea r14, [rip+table]
//	0x10a: mov r10d, ebx
//	0x10d: movsxd r10, [r14+4*r10]
//	0x111: add r10, r14
//	0x114: jmp 0x120
//	0x120: dec eax
//	0x122: jmp r10
//	cases at 0x130, 0x131, 0x132, 0x133: ret
func TestCrossBlockJumpTable(t *testing.T) {
	img := make([]byte, 0x2000)
	copy(img[0x100:], []byte{
		0x83, 0xe3, 0x03, // and ebx, 3
		0x4c, 0x8d, 0x35, 0, 0, 0, 0, // lea r14, [rip+table]
		0x41, 0x89, 0xda, // mov r10d, ebx
		0x4f, 0x63, 0x14, 0x96, // movsxd r10, [r14+4*r10]
		0x4d, 0x01, 0xf2, // add r10, r14
		0xeb, 0x0a, // jmp 0x120
	})
	binary.LittleEndian.PutUint32(img[0x106:], uint32(int32(0x1000-0x10a)))
	copy(img[0x120:], []byte{0xff, 0xc8, 0x41, 0xff, 0xe2}) // dec eax; jmp r10
	copy(img[0x130:], []byte{0xc3, 0xc3, 0xc3, 0xc3})
	for i := 0; i < 4; i++ {
		binary.LittleEndian.PutUint32(img[0x1000+4*i:], uint32(int32(0x130+i-0x1000)))
	}
	d, err := NewDisassembler(switchTestELF(img))
	if err != nil {
		t.Fatal(err)
	}
	if err := d.AnalyzeReachable([]uint64{0x100}); err != nil {
		t.Fatal(err)
	}
	fn := d.Functions[0x100]
	if fn == nil {
		t.Fatal("no function")
	}
	got := fn.JumpTables[0x122]
	if len(got) != 4 || got[0] != 0x130 || got[3] != 0x133 {
		t.Fatalf("jump table targets = %#x (blocks %v)", got, fn.BlockOrder)
	}
	if tb := fn.Tables[0x122]; tb.TableAddr != 0x1000 || tb.Count != 4 {
		t.Fatalf("table = %+v", tb)
	}
}
