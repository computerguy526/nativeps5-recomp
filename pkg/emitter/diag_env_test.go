package emitter

import "os"

// The runtime test harnesses link core/nps5_diag.c; keep them from creating runs/ bundles.
func init() { os.Setenv("NPS5_NO_BUNDLE", "1") }
