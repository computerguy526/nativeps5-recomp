// SPDX-License-Identifier: GPL-2.0-or-later

// Command gen writes the generated thunk sources into pkg/runtime.
// Run from the repository root: go run ./pkg/thunkgen/gen
package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"ps4-recomp/pkg/thunkgen"
)

func main() {
	root := filepath.Join("pkg", "runtime")
	if len(os.Args) > 1 {
		root = os.Args[1]
	}
	hdr, err := thunkgen.Header("NPS5_LIBKERNEL_HLE_H", thunkgen.Libkernel)
	if err == nil {
		err = os.WriteFile(filepath.Join(root, thunkgen.LibkernelHeader), []byte(hdr), 0o644)
	}
	for group, descs := range thunkgen.Groups(thunkgen.Libkernel) {
		if err != nil {
			break
		}
		var src string
		if src, err = thunkgen.Thunks(filepath.Base(thunkgen.LibkernelHeader), descs); err == nil {
			err = os.WriteFile(filepath.Join(root, thunkgen.LibkernelThunkFile(group)), []byte(src), 0o644)
		}
	}
	if err == nil {
		err = genAnyPS5(root)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "thunkgen:", err)
		os.Exit(1)
	}
}

// genAnyPS5 writes the thunks of the AnyPS5 exports (-gpu anyps5 builds only; the file compiles to
// nothing unless NPS5_HAVE_ANYPS5_AGC is set).
func genAnyPS5(root string) error {
	descs, err := thunkgen.AnyPS5Descs()
	if err != nil {
		return err
	}
	dir := filepath.Join(root, "modules", "libSceAnyPS5")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	hdr, err := thunkgen.Header("NPS5_ANYPS5_HLE_H", descs)
	if err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(dir, "nps5_anyps5_hle.h"), []byte(hdr), 0o644); err != nil {
		return err
	}
	src, err := thunkgen.Thunks("nps5_anyps5_hle.h", descs)
	if err != nil {
		return err
	}
	src = strings.Replace(src, "#include \"nps5_thunk.h\"", "#include \"nps5_anyps5_config.h\"\n#if NPS5_HAVE_ANYPS5_AGC\n#include \"nps5_thunk.h\"", 1) + "\n#endif // NPS5_HAVE_ANYPS5_AGC\n"
	src = strings.Replace(src, "#include \"nps5_anyps5_hle.h\"", "#include <dlfcn.h>\n#include <stdio.h>\n#include \"nps5_anyps5_hle.h\"", 1)
	routes, err := thunkgen.KytyRouteDefs(descs)
	if err != nil {
		return err
	}
	src = strings.Replace(src, "#include \"nps5_anyps5_hle.h\"", "#include \"nps5_anyps5_hle.h\"\n#include \"nps5_kyty.h\"", 1)
	src = strings.TrimSuffix(src, "\n#endif // NPS5_HAVE_ANYPS5_AGC\n") + "\n" + thunkgen.IndirectDefs("nps5_anyps5_bind_exports", descs) + "\n" + routes + "\n#endif // NPS5_HAVE_ANYPS5_AGC\n"
	return os.WriteFile(filepath.Join(dir, "nps5_anyps5_thunks.c"), []byte(src), 0o644)
}
