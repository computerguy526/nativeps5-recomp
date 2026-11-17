//go:build darwin

package cli

import (
	"net"
	"syscall"
	"unsafe"
)

// peerUID returns the effective uid of the process on the other end of a unix socket connection (LOCAL_PEERCRED).
func peerUID(c net.Conn) (uint32, bool) {
	uc, ok := c.(*net.UnixConn)
	if !ok {
		return 0, false
	}
	raw, err := uc.SyscallConn()
	if err != nil {
		return 0, false
	}
	var uid uint32
	got := false
	_ = raw.Control(func(fd uintptr) {
		// struct xucred { u_int cr_version; uid_t cr_uid; short cr_ngroups; gid_t cr_groups[16]; }
		var cred [4 + 4 + 4 + 64]byte
		l := uint32(len(cred))
		const solLocal, localPeerCred = 0, 1
		_, _, e := syscall.Syscall6(syscall.SYS_GETSOCKOPT, fd, solLocal, localPeerCred, uintptr(unsafe.Pointer(&cred[0])), uintptr(unsafe.Pointer(&l)), 0)
		if e == 0 {
			uid = *(*uint32)(unsafe.Pointer(&cred[4]))
			got = true
		}
	})
	return uid, got
}
