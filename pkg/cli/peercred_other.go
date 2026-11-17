//go:build !darwin

package cli

import "net"

func peerUID(c net.Conn) (uint32, bool) { return 0, false }
