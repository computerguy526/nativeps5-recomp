import struct, sys
src, dst = sys.argv[1], sys.argv[2]
d = open(src,'rb').read()
magic, ver, mode, endian, attr, keytype, hsz, msz, fsz, nseg, flags = struct.unpack_from('<IBBBBIHHQHH', d, 0)
assert magic in (0x1D3D154F, 0xEEF51454), hex(magic)
segs = [struct.unpack_from('<QQQQ', d, 0x20 + i*32) for i in range(nseg)]
eoff = 0x20 + nseg*32
e = d[eoff:eoff+64]
assert e[:4] == b'\x7fELF'
(etype, mach, ver2, entry, phoff, shoff, eflags, ehsz, phes, phnum, shes, shnum, shstr) = struct.unpack_from('<HHIQQQIHHHHHH', e, 16)
ph = [struct.unpack_from('<IIQQQQQQ', d, eoff+phoff+i*phes) for i in range(phnum)]
print(f"self: nseg={nseg} flags={flags:#x} elf type={etype:#x} entry={entry:#x} phnum={phnum} shnum={shnum}")
end = max(p[2]+p[5] for p in ph)
out = bytearray(max(end, eoff+phoff+phnum*phes))
out[0:eoff+phoff+phnum*phes-eoff] = d[eoff:eoff+phoff+phnum*phes]
for i,(pf,off,fs,ms) in enumerate(segs):
    blocked = bool(pf & 0x800); idx = (pf>>20)&0xfff
    print(f" seg{i}: props={pf:#x} blocked={blocked} id={idx} off={off:#x} fsz={fs:#x} msz={ms:#x} comp={bool(pf&8)} enc={bool(pf&2)}")
    if blocked and pf & 0xA == 0:
        p = ph[idx]
        n = min(fs, p[5])
        out[p[2]:p[2]+n] = d[off:off+n]
for i,p in enumerate(ph):
    print(f" phdr{i}: type={p[0]:#x} flags={p[1]:#x} off={p[2]:#x} va={p[3]:#x} fsz={p[5]:#x} msz={p[6]:#x}")
if shnum and shoff + shnum*shes > len(out):
    # section headers not present in SELF payload: strip them
    struct.pack_into('<Q', out, 40, 0); struct.pack_into('<HHH', out, 58, shes, 0, 0)
    print('stripped absent section headers')
open(dst,'wb').write(out)
