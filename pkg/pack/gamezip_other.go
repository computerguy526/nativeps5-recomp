// SPDX-License-Identifier: GPL-2.0-or-later

//go:build !darwin

package pack

import (
	"io/fs"
	"strings"
)

func fileCompressed(fs.FileInfo) bool   { return false }
func volumeInfo(string) (string, int64) { return "", 0 }
func onExternalVolume(dir string) bool  { return strings.HasPrefix(dir, "/Volumes/") }
