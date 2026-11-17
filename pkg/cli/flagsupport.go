// SPDX-License-Identifier: GPL-2.0-or-later

package cli

import "strings"

// SupportsFlag reports whether this translator documents the flag (name without the dash). The
// packager uses it to refuse a profile that asks for a flag a build of the translator lacks.
func SupportsFlag(name string) bool {
	return strings.Contains(usageText, "-"+name+" ") || strings.Contains(usageText, "-"+name+"\n")
}

// AnyPS5BuildEnv names the variable that tells -gpu anyps5 where AnyPS5's arm64 build is.
const AnyPS5BuildEnv = anyps5Env
