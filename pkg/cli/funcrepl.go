package cli

import (
	"fmt"
	"io"
	"strings"

	"ps4-recomp/pkg/emitter"
)

// applyFuncRepl binds the function-level replacements the user asked for (-hle-malloc,
// -hle-replace) on em: docs/m3/function-hle.md. fragment is true for a tier-1 fragment emitter.
// A requested replacement that cannot be applied is an error: the user asked for it, and a title
// translated without it is not the one they meant to run.
//
// A tier-1 fragment continues a translation that already passed -hle-replace-unverified, so it
// takes the same table without the opt-in.
func applyFuncRepl(em *emitter.AsmEmitter, hleMalloc, hleReplace string, fragment bool, logw io.Writer) error {
	return applyFuncReplMode(em, hleMalloc, hleReplace, fragment, fragment, logw)
}

// applyFuncReplMode is applyFuncRepl with -hle-replace-unverified given explicitly.
func applyFuncReplMode(em *emitter.AsmEmitter, hleMalloc, hleReplace string, allowUnverified, fragment bool, logw io.Writer) error {
	if hleMalloc != "" {
		rep, err := em.ApplyUE5Binned3(hleMalloc, fragment)
		if err != nil {
			return err
		}
		logRepl(logw, rep)
	}
	if hleReplace != "" {
		set, err := emitter.LoadReplacementFile(hleReplace)
		if err != nil {
			return fmt.Errorf("-hle-replace: %w", err)
		}
		set.AllowUnverified = allowUnverified
		rep, err := em.ApplyReplacements(set, fragment)
		if err != nil {
			return fmt.Errorf("-hle-replace %s: %w", hleReplace, err)
		}
		logRepl(logw, rep)
	}
	return nil
}

// applyPatches binds the game patch profile the user asked for (-patches) on em.
func applyPatches(em *emitter.AsmEmitter, path string, logw io.Writer) error {
	if path == "" {
		return nil
	}
	p, err := emitter.LoadPatchProfile(path)
	if err != nil {
		return fmt.Errorf("-patches: %w", err)
	}
	rep, err := em.ApplyPatchProfile(p)
	if err != nil {
		return fmt.Errorf("-patches %s: %w", path, err)
	}
	fmt.Fprintf(logw, "             Game patches %s: %d run-time settings (%s) at %d sites\n", rep.Profile, len(rep.Keys), strings.Join(rep.Keys, ", "), rep.Sites)
	return nil
}

func logRepl(logw io.Writer, rep *emitter.ReplReport) {
	names := make([]string, 0, len(rep.Replaced))
	for _, r := range rep.Replaced {
		n := r.Name
		if i := strings.LastIndex(n, "::"); i >= 0 {
			n = n[i+2:]
		}
		names = append(names, n)
	}
	unv := ""
	if rep.Unverified > 0 {
		unv = fmt.Sprintf(" (%d not verified by hash)", rep.Unverified)
	}
	fmt.Fprintf(logw, "             Function replacements %s: %d guest functions run as host shims%s: %s\n", rep.Set, len(rep.Replaced), unv, strings.Join(names, ", "))
}
