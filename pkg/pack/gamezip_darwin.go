// SPDX-License-Identifier: GPL-2.0-or-later

package pack

import (
	"io/fs"
	"strings"
	"syscall"
)

const ufCompressed = 0x20 // UF_COMPRESSED

func fileCompressed(fi fs.FileInfo) bool {
	st, ok := fi.Sys().(*syscall.Stat_t)
	return ok && st.Flags&ufCompressed != 0
}

func cstr(b []int8) string {
	var s strings.Builder
	for _, c := range b {
		if c == 0 {
			break
		}
		s.WriteByte(byte(c))
	}
	return s.String()
}

// volumeInfo returns the file system type name and the free bytes of the volume holding dir.
func volumeInfo(dir string) (string, int64) {
	var st syscall.Statfs_t
	if syscall.Statfs(dir, &st) != nil {
		return "", 0
	}
	return cstr(st.Fstypename[:]), int64(st.Bavail) * int64(st.Bsize)
}

// onExternalVolume is true for a path under /Volumes or on a volume mounted there.
func onExternalVolume(dir string) bool {
	if strings.HasPrefix(dir, "/Volumes/") {
		return true
	}
	var st syscall.Statfs_t
	return syscall.Statfs(dir, &st) == nil && strings.HasPrefix(cstr(st.Mntonname[:]), "/Volumes/")
}
