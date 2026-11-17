package cli

// `ps4-recomp fragment-serve`: a persistent tier-1 translator (docs/m3/iteration-speed.md). A
// process-per-miss `fragment` spends 2-3 s loading the title (ELF, companion modules,
// relocations, disassembler) for a fragment that takes 0.2 s to lift and build. The server loads
// once, listens on a unix socket and builds fragments from requests, concurrently. The runtime
// starts it on the first miss and falls back to process-per-miss if it cannot.
//
// Protocol, one request per connection: the client sends one line
//
//	entry=0x<hex>[,0x<hex>] out=<abs dir> [max-insts=N] [max-funcs=N] [timeout-ms=N]
//
// the server builds the fragment, writes <out>/result.txt exactly as `fragment` does, and
// answers "done\n". A line the server cannot parse is answered "error <text>\n".

import (
	"bufio"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"strconv"
	"strings"
	"sync/atomic"
	"syscall"
	"time"
)

func fragmentServeCommand(args []string) error {
	fs := flag.NewFlagSet("fragment-serve", flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	var manifest, sock string
	var parent, idleSec int
	fs.StringVar(&manifest, "manifest", "", "lifter_manifest.json of the translation")
	fs.StringVar(&sock, "socket", "", "unix socket path to listen on")
	fs.IntVar(&parent, "parent", 0, "exit when this process is gone")
	fs.IntVar(&idleSec, "idle-s", 1800, "exit after this many seconds without a request")
	if err := fs.Parse(args); err != nil || manifest == "" || sock == "" {
		return errors.New("usage: ps4-recomp fragment-serve --manifest <lifter_manifest.json> --socket <path> [--parent pid] [--idle-s N]")
	}
	logw = io.Discard
	env, err := prepareFragmentEnv(manifest)
	if err != nil {
		return err
	}
	_ = os.Remove(sock)
	ln, err := net.Listen("unix", sock)
	if err != nil {
		return err
	}
	_ = os.Chmod(sock, 0o600)
	defer os.Remove(sock)
	fmt.Fprintf(os.Stderr, "fragment-serve: title loaded in %d ms, listening on %s\n", env.loadedMs, sock)
	var last atomic.Int64
	last.Store(time.Now().UnixNano())
	go func() {
		for range time.Tick(time.Second) {
			if parent > 0 && syscall.Kill(parent, 0) != nil {
				_ = os.Remove(sock)
				os.Exit(0)
			}
			if time.Since(time.Unix(0, last.Load())) > time.Duration(idleSec)*time.Second {
				_ = os.Remove(sock)
				os.Exit(0)
			}
		}
	}()
	for {
		c, err := ln.Accept()
		if err != nil {
			return err
		}
		if !peerAllowed(c) {
			c.Close()
			continue
		}
		go func() {
			defer c.Close()
			line, err := bufio.NewReader(c).ReadString('\n')
			if err != nil {
				return
			}
			last.Store(time.Now().UnixNano())
			o, perr := parseServeRequest(line)
			if perr != nil {
				fmt.Fprintf(c, "error %v\n", perr)
				return
			}
			res, berr := env.build(o)
			_ = finishFragment(o, res, berr)
			last.Store(time.Now().UnixNano())
			fmt.Fprint(c, "done\n")
		}()
	}
}

func parseServeRequest(line string) (*fragOpts, error) {
	o := &fragOpts{maxInsts: 20000, maxFuncs: 16, timeout: 120 * time.Second}
	for _, f := range strings.Fields(line) {
		k, v, ok := strings.Cut(f, "=")
		if !ok {
			return nil, fmt.Errorf("bad field %q", f)
		}
		switch k {
		case "entry":
			var m multiHex
			if err := m.Set(v); err != nil {
				return nil, err
			}
			o.entries = m
		case "out":
			o.out = v
		case "max-insts", "max-funcs", "timeout-ms":
			n, err := strconv.Atoi(v)
			if err != nil {
				return nil, err
			}
			switch k {
			case "max-insts":
				o.maxInsts = n
			case "max-funcs":
				o.maxFuncs = n
			default:
				o.timeout = time.Duration(n) * time.Millisecond
			}
		}
	}
	if len(o.entries) == 0 || o.out == "" || !strings.HasPrefix(o.out, "/") {
		return nil, errors.New("entry and an absolute out are required")
	}
	if err := os.MkdirAll(o.out, 0o755); err != nil {
		return nil, err
	}
	return o, nil
}

// peerAllowed accepts only a client running as the same user as the server (LOCAL_PEERCRED). Where the peer
// credentials cannot be read the connection is refused: the socket lives in a 0700 directory, but the check is cheap.
func peerAllowed(c net.Conn) bool {
	uid, ok := peerUID(c)
	return ok && int(uid) == os.Geteuid()
}
