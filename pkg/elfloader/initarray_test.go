package elfloader

import (
	"encoding/binary"
	"testing"
)

// The legacy ".ctors behind a LEA in DT_INIT" heuristic must not run when the dynamic table
// has DT_PREINIT_ARRAY: the loop bound after the array is not a constructor.
func TestPreinitArrayNotPolluted(t *testing.T) {
	local := make([]byte, 0x300)
	put := func(off int, v uint64) { binary.LittleEndian.PutUint64(local[off:], v) }
	// DT_INIT at 0x10: lea r15,[rip+X] (-> 0x200) ; lea r12,[rip+Y] (-> 0x208)
	code := []byte{0x4c, 0x8d, 0x3d, 0, 0, 0, 0, 0x4c, 0x8d, 0x25, 0, 0, 0, 0}
	binary.LittleEndian.PutUint32(code[3:], uint32(0x200-(0x10+7)))
	binary.LittleEndian.PutUint32(code[10:], uint32(0x208-(0x10+14)))
	copy(local[0x10:], code)
	put(0x1f8, ^uint64(0)) // __CTOR_LIST__ sentinel
	put(0x200, 0x100)      // the one real preinit entry
	put(0x208, 0x20)       // next dynamic-table word: points into code but is not a constructor
	l := &LoadedELF{
		local:       local,
		ExecRanges:  []AddrRange{{Start: 0, End: 0x1f0}},
		DynamicTags: map[int64]uint64{DT_INIT: 0x10, DT_PREINIT_ARRAY: 0x200, DT_PREINIT_ARRAYSZ: 8},
	}
	parseInitArray(l)
	if len(l.InitArray) != 1 || l.InitArray[0] != 0x100 {
		t.Fatalf("InitArray = %#x, want [0x100]", l.InitArray)
	}
}
