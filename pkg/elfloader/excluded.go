package elfloader

import (
	"path/filepath"
	"strings"
)

// ExcludedModuleMatch reports whether file name or path matches one -exclude-module value
// (case-insensitive; the value may omit the .prx/.sprx extension).
func ExcludedModuleMatch(path string, excludes []string) bool {
	base := strings.ToLower(filepath.Base(path))
	stem := strings.TrimSuffix(strings.TrimSuffix(base, ".prx"), ".sprx")
	for _, e := range excludes {
		e = strings.ToLower(strings.TrimSpace(filepath.Base(e)))
		if e == "" {
			continue
		}
		if e == base || e == stem || strings.TrimSuffix(strings.TrimSuffix(e, ".prx"), ".sprx") == stem {
			return true
		}
	}
	return false
}

// IsExcludedImport returns the excluded module that exports symName's NID.
func (l *LoadedELF) IsExcludedImport(symName string) (string, bool) {
	if l == nil || len(l.ExcludedImports) == 0 || symName == "" {
		return "", false
	}
	mod, ok := l.ExcludedImports[NIDPrefix(symName)]
	return mod, ok
}

// MarkExcludedExports records the exports of a module that is not translated. NIDs that some
// linked module (satisfied) also exports stay with that module.
func (l *LoadedELF) MarkExcludedExports(module string, exports []Symbol) {
	if l.ExcludedImports == nil {
		l.ExcludedImports = make(map[string]string)
	}
	for _, e := range exports {
		if nid := NIDPrefix(e.Name); nid != "" {
			l.ExcludedImports[nid] = module
		}
	}
}

// ClearExcluded removes NIDs a linked module exports, so a duplicate export keeps resolving.
func (l *LoadedELF) ClearExcluded(linked []Symbol) {
	for _, e := range linked {
		delete(l.ExcludedImports, NIDPrefix(e.Name))
	}
}
