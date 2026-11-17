//go:build darwin

package cli

import (
	"net"
	"os"
	"path/filepath"
	"testing"
)

func TestPeerUID(t *testing.T) {
	sock := filepath.Join(t.TempDir(), "s")
	ln, err := net.Listen("unix", sock)
	if err != nil {
		t.Skip(err)
	}
	defer ln.Close()
	done := make(chan net.Conn, 1)
	go func() { c, _ := ln.Accept(); done <- c }()
	cl, err := net.Dial("unix", sock)
	if err != nil {
		t.Fatal(err)
	}
	defer cl.Close()
	srv := <-done
	defer srv.Close()
	if uid, ok := peerUID(srv); !ok || int(uid) != os.Geteuid() {
		t.Fatalf("peerUID = %d, %v; want %d", uid, ok, os.Geteuid())
	}
	if !peerAllowed(srv) {
		t.Fatal("same-user peer refused")
	}
}
