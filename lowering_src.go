// Package ps4recomp only carries the translator's lowering sources: embedded at build time so
// emitter.LoweringID names what this binary was built from, not what the source tree holds when it
// runs (docs/m3/iteration-speed.md).
package ps4recomp

import "embed"

// Sources is the Go source of the packages that decide what assembly a guest function becomes.
//
//go:embed pkg/lifter pkg/emitter pkg/disasm
var Sources embed.FS
