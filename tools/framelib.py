"""Stdlib-only frame reader shared by frame_stats.py and glitch_scan.py.
read_frame(path) -> (w, h, rgb) with rgb a bytes object of R,G,B per pixel (alpha dropped).
Supports 8-bit non-interlaced PNG (gray, RGB, RGBA, palette) and 24/32-bit BMP."""
import os
import re
import struct
import zlib


def _unfilter(raw, h, stride, bpp):
    out = bytearray(h * stride)
    prev = bytearray(stride)
    pos = 0
    for y in range(h):
        ft = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        if ft == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif ft == 3:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif ft == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        elif ft != 0:
            raise ValueError("bad PNG filter %d" % ft)
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return out


def read_png(path):
    d = open(path, "rb").read()
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG: " + path)
    pos = 8
    idat = bytearray()
    plte = b""
    while pos < len(d):
        n, typ = struct.unpack(">I4s", d[pos:pos + 8])
        body = d[pos + 8:pos + 8 + n]
        pos += 12 + n
        if typ == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif typ == b"PLTE":
            plte = body
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
    if depth != 8 or interlace != 0:
        raise ValueError("only 8-bit non-interlaced PNG supported: " + path)
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    stride = w * ch
    px = _unfilter(zlib.decompress(bytes(idat)), h, stride, ch)
    if ctype == 2:
        return w, h, bytes(px)
    rgb = bytearray(w * h * 3)
    if ctype == 6:
        rgb[0::3], rgb[1::3], rgb[2::3] = px[0::4], px[1::4], px[2::4]
    elif ctype == 0 or ctype == 4:
        g = px[0::ch]
        rgb[0::3] = rgb[1::3] = rgb[2::3] = g
    else:
        idx = px
        for i, v in enumerate(idx):
            rgb[i * 3:i * 3 + 3] = plte[v * 3:v * 3 + 3]
    return w, h, bytes(rgb)


def read_bmp(path):
    d = open(path, "rb").read()
    off = struct.unpack("<I", d[10:14])[0]
    w, h, _, bpp = struct.unpack("<iiHH", d[18:30])
    bpp //= 8
    flip = h > 0
    h = abs(h)
    stride = (w * bpp + 3) & ~3
    rows = [d[off + y * stride:off + y * stride + w * bpp] for y in range(h)]
    if flip:
        rows.reverse()
    out = bytearray()
    for r in rows:
        v = bytearray(r)
        if bpp == 4:
            del v[3::4]
        v[0::3], v[2::3] = v[2::3], v[0::3]     # BGR -> RGB
        out += v
    return w, h, bytes(out)


def read_frame(path):
    return read_png(path) if path.lower().endswith(".png") else read_bmp(path)


def list_frames(directory):
    names = [n for n in os.listdir(directory) if n.lower().endswith((".png", ".bmp"))]
    names.sort(key=lambda n: [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", n)])
    return [os.path.join(directory, n) for n in names]
