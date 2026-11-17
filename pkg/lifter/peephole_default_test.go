package lifter

// The text-shape tests document the lowerings before the peephole pass, so new lifters in this test
// binary have it off; the tests that run lowerings on the host (exec, FEX, memory corpus) and
// arm64_peephole_test.go turn it on with SetPeephole(true).
func init() { peepholeDefault = false }
