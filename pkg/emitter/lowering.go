package emitter

import (
	"crypto/sha256"
	"encoding/hex"
	"io/fs"
	"os"
	"strings"
	"sync"

	ps4recomp "ps4-recomp"
)

var (
	loweringOnce sync.Once
	loweringID   string
)

// LoweringID identifies the translator's lowering: what the lifter, the emitter and the decoder
// turn a function into. Cached chunks and tier-1 fragments are valid only for the id they were
// made with. It is the hash of the non-test Go sources of pkg/lifter, pkg/emitter and pkg/disasm
// that were embedded when this binary was built; NPS5_LOWERING_ID overrides it (tests, forcing
// a rebuild).
func LoweringID() string {
	loweringOnce.Do(func() {
		if v := os.Getenv("NPS5_LOWERING_ID"); v != "" {
			loweringID = v
			return
		}
		h := sha256.New()
		_ = fs.WalkDir(ps4recomp.Sources, ".", func(p string, d fs.DirEntry, err error) error {
			if err != nil || d.IsDir() || !strings.HasSuffix(p, ".go") || strings.HasSuffix(p, "_test.go") {
				return nil
			}
			b, rerr := ps4recomp.Sources.ReadFile(p)
			if rerr != nil {
				return nil
			}
			h.Write([]byte(p + "\x00"))
			h.Write(b)
			return nil
		})
		loweringID = hex.EncodeToString(h.Sum(nil))[:16]
	})
	return loweringID
}
