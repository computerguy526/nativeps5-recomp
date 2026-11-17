#!/usr/bin/env python3
"""Checksum manifest of a game dump, to prove a run left it untouched.

  dump_manifest.py create [--v1] <dump-dir> <manifest>   write the manifest (v2; --v1 writes the old format)
  dump_manifest.py verify <dump-dir> <manifest>          exit 0 if identical, 1 with one line per difference

v2, one line per entry, sorted, tab separated: type (d/f/l), size, mtime in ns, sha256 (files up to --small MiB,
default 16; larger files are compared by size and mtime; "-" otherwise; for a symlink its target), mode (octal
st_mode), uid, gid, BSD file flags, xattrs (name=sha1-of-value, sorted, "com.apple.*" ignored; "-" when none),
path relative to the dump. Directories are listed too (a file created and deleted again still shows as a changed
directory mtime), and so are symlinks to directories (not descended into). .DS_Store files are ignored.
A chmod / chown / chflags / setxattr on any entry therefore shows up.
v1 manifests (type, size, mtime, sha256, path) stay readable: verify compares only the v1 fields and says so.
Reads only; never writes into the dump.
"""
import hashlib, os, sys

import ctypes, ctypes.util
_libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
_XATTR_NOFOLLOW = 0x0001  # macOS options for list/getxattr

def _listxattr(p):
    b = os.fsencode(p)
    if hasattr(os, "listxattr"):
        return os.listxattr(p, follow_symlinks=False)
    n = _libc.listxattr(b, None, 0, _XATTR_NOFOLLOW)
    if n <= 0:
        return []
    buf = ctypes.create_string_buffer(n)
    n = _libc.listxattr(b, buf, n, _XATTR_NOFOLLOW)
    return [x.decode("utf-8", "replace") for x in buf.raw[:max(n, 0)].split(b"\0") if x]

def _getxattr(p, name):
    if hasattr(os, "getxattr"):
        return os.getxattr(p, name, follow_symlinks=False)
    b, nb = os.fsencode(p), name.encode()
    n = _libc.getxattr(b, nb, None, 0, 0, _XATTR_NOFOLLOW)
    if n < 0:
        raise OSError(ctypes.get_errno(), "getxattr")
    buf = ctypes.create_string_buffer(max(n, 1))
    n = _libc.getxattr(b, nb, buf, n, 0, _XATTR_NOFOLLOW)
    return buf.raw[:max(n, 0)]

def xattrs(p):
    try:
        names = sorted(n for n in _listxattr(p) if not n.startswith("com.apple."))
    except (OSError, AttributeError):
        return "-"
    out = []
    for n in names:
        try:
            v = _getxattr(p, n)
        except OSError:
            v = b"?"
        out.append("%s=%s" % (n, hashlib.sha1(v).hexdigest()[:16]))
    return ",".join(out) or "-"

def meta(st, p):
    return ("%o" % st.st_mode, st.st_uid, st.st_gid, getattr(st, "st_flags", 0), xattrs(p))

def scan(root, small):
    out = []
    for dirpath, dirs, files in os.walk(root):
        dirs.sort()
        rel = os.path.relpath(dirpath, root)
        st = os.lstat(dirpath)
        out.append(("d", 0, st.st_mtime_ns, "-") + meta(st, dirpath) + (rel,))
        for dn in list(dirs):  # symlinks to directories are not descended into: list them as links
            p = os.path.join(dirpath, dn)
            if os.path.islink(p):
                s2 = os.lstat(p)
                out.append(("l", 0, s2.st_mtime_ns, os.readlink(p)) + meta(s2, p) + (os.path.normpath(os.path.join(rel, dn)),))
                dirs.remove(dn)
        for n in sorted(files):
            if n == ".DS_Store":  # Finder metadata, rewritten by browsing the folder
                continue
            p = os.path.join(dirpath, n)
            st = os.lstat(p)
            r = os.path.normpath(os.path.join(rel, n))
            if os.path.islink(p):
                out.append(("l", 0, st.st_mtime_ns, os.readlink(p)) + meta(st, p) + (r,))
                continue
            h = "-"
            if st.st_size <= small:
                d = hashlib.sha256()
                with open(p, "rb") as f:
                    for b in iter(lambda: f.read(1 << 20), b""):
                        d.update(b)
                h = d.hexdigest()
            out.append(("f", st.st_size, st.st_mtime_ns, h) + meta(st, p) + (r,))
    return sorted(out, key=lambda e: e[-1])

def fmt(e):
    return "\t".join(str(x) for x in e)

def v1_view(line):
    f = line.split("\t")
    return "\t".join(f[:4] + [f[-1]])

def main(a):
    small = 16
    v1 = False
    if "--small" in a:
        i = a.index("--small"); small = int(a[i + 1]); del a[i:i + 2]
    if "--v1" in a:
        v1 = True; a.remove("--v1")
    if len(a) != 3 or a[0] not in ("create", "verify"):
        sys.stderr.write(__doc__); return 2
    cmd, root, man = a
    if not os.path.isdir(root):
        sys.stderr.write("dump_manifest: %s is not a directory\n" % root); return 2
    lines = [fmt(e) for e in scan(root, small << 20)]
    if cmd == "create":
        if os.path.realpath(man).startswith(os.path.realpath(root) + os.sep):
            sys.stderr.write("dump_manifest: refusing to write the manifest inside the dump\n"); return 2
        with open(man, "w") as f:
            f.write("# dump_manifest v%d small=%d\n" % (1 if v1 else 2, small))
            f.write("\n".join(v1_view(l) if v1 else l for l in lines) + "\n")
        print("dump_manifest: %d entries -> %s (v%d)" % (len(lines), man, 1 if v1 else 2)); return 0
    with open(man) as f:
        head = f.readline().strip()
        old = [l.rstrip("\n") for l in f if l.strip()]
    ver = None
    for v in (1, 2):
        if head == "# dump_manifest v%d small=%d" % (v, small):
            ver = v
    if ver is None:
        sys.stderr.write("dump_manifest: manifest header %r not understood (same --small as at creation?)\n" % head); return 2
    if ver == 1:
        print("dump_manifest: NOTE v1 manifest: mode, owner, flags and xattrs are NOT verified (regenerate with create)")
        lines = [v1_view(l) for l in lines]
    key = lambda l: l.split("\t")[-1]
    o = {key(l): l for l in old}; n = {key(l): l for l in lines}
    bad = 0
    for k in sorted(set(o) | set(n)):
        if k not in n: print("DUMP CHANGED: removed  %s" % k); bad += 1
        elif k not in o: print("DUMP CHANGED: added    %s" % k); bad += 1
        elif o[k] != n[k]: print("DUMP CHANGED: modified %s\n    was %s\n    now %s" % (k, o[k], n[k])); bad += 1
    if bad:
        print("dump_manifest: %d difference(s) in %s" % (bad, root)); return 1
    print("dump_manifest: %d entries unchanged" % len(lines)); return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
