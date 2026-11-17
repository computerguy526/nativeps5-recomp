#!/usr/bin/python3
"""Compare two directories of frame_NNN.bmp dumps (AnyPS5 APS5_DUMP_FRAMES output). Stdlib only.
usage: frame_compare.py <dirA> <dirB> [--shift K] [--json OUT]
Per index i: exact-match % of pixels (all channels equal), max channel diff, PSNR over RGB, and the
best-matching B index within +-3 (by a sampled mean abs diff) to separate timing offsets from rendering
differences. A is the arm64 run, B the x86 reference."""
import array, os, struct, sys, json, math

def read_bmp(p):
    d = open(p, 'rb').read()
    off = struct.unpack('<I', d[10:14])[0]; w, h, planes, bpp = struct.unpack('<iiHH', d[18:30])
    bpp //= 8; flip = h > 0; h = abs(h); stride = (w * bpp + 3) & ~3
    rows = [d[off + y * stride: off + y * stride + w * bpp] for y in range(h)]
    if flip: rows.reverse()
    return w, h, bpp, rows

def rgb_pixels(rows, bpp):
    # sampled-free full conversion to a flat bytes of B,G,R per pixel
    if bpp == 3: return b''.join(rows)
    out = bytearray()
    for r in rows:
        v = bytearray(r); del v[3::4]; out += v      # drop alpha/X
    return bytes(out)

def compare(a, b):
    n = len(a); assert n == len(b)
    eq_px = 0; mx = 0; sq = 0
    # per-row compare: fast path for identical rows
    return None

def metrics(A, B):
    (w, h, bpp, ra), (w2, h2, bpp2, rb) = A, B
    if (w, h) != (w2, h2): return {'size_mismatch': [w, h, w2, h2]}
    eq = 0; mx = 0; sq = 0
    for ya, yb in zip(ra, rb):
        if ya == yb:
            eq += w; continue
        pa, pb = array.array('B', ya), array.array('B', yb)
        for x in range(w):
            i = x * bpp
            d0 = pa[i] - pb[i]; d1 = pa[i+1] - pb[i+1]; d2 = pa[i+2] - pb[i+2]
            if d0 == 0 and d1 == 0 and d2 == 0: eq += 1; continue
            sq += d0*d0 + d1*d1 + d2*d2
            m = max(abs(d0), abs(d1), abs(d2))
            if m > mx: mx = m
    mse = sq / (w * h * 3)
    return {'exact_pct': 100.0 * eq / (w * h), 'max_diff': mx, 'psnr_db': (99.0 if mse == 0 else 10 * math.log10(255 * 255 / mse))}

def sampled(A, step=16):
    w, h, bpp, rows = A
    return [bytes(r[x * bpp:x * bpp + 3]) for r in rows[::step] for x in range(0, w, step)]

def sad(sa, sb):
    return sum(abs(x[0]-y[0]) + abs(x[1]-y[1]) + abs(x[2]-y[2]) for x, y in zip(sa, sb)) / (3 * len(sa))

def main():
    a_dir, b_dir = sys.argv[1], sys.argv[2]
    jout = sys.argv[sys.argv.index('--json') + 1] if '--json' in sys.argv else None
    fa = sorted(f for f in os.listdir(a_dir) if f.endswith('.bmp')); fb = sorted(f for f in os.listdir(b_dir) if f.endswith('.bmp'))
    n = min(len(fa), len(fb)); res = []
    Bs = [read_bmp(os.path.join(b_dir, f)) for f in fb[:n]]; Bsm = [sampled(b) for b in Bs]
    for i in range(n):
        A = read_bmp(os.path.join(a_dir, fa[i])); sa = sampled(A)
        m = metrics(A, Bs[i]); m['i'] = i
        sads = {j: sad(sa, Bsm[j]) for j in range(max(0, i - 3), min(n, i + 4))}
        bj = min(sads, key=sads.get); m['best_j'] = bj; m['sad_same'] = round(sads[i], 2); m['sad_best'] = round(sads[bj], 2)
        if bj != i: m['best_j_metrics'] = metrics(A, Bs[bj])
        res.append(m)
        print(json.dumps(m, separators=(',', ':')), flush=True)
    if jout: json.dump(res, open(jout, 'w'), indent=1)

main()
