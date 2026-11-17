#!/usr/bin/python3
"""Time-domain check of a captured stream against one decoded asset (stdlib only).
usage: audio_xcorr.py <capture.wav> <asset.wav> t0 [t1 ...]   (2 s segments starting at the given seconds)
Both are mono-mixed and decimated x24 (2 kHz) by block average; the normalised cross-correlation of each
capture segment is taken at every lag of the asset (FFT). An NCC near 1 means the asset's waveform is in the
capture (scaled); the lag is the asset position in seconds."""
import cmath, math, sys, array
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
D = 24

def read(p):
    import struct
    d = open(p, 'rb').read(); pos = 12; fmt = None
    while pos + 8 <= len(d):
        cid, sz = d[pos:pos+4], struct.unpack('<I', d[pos+4:pos+8])[0]
        if cid == b'fmt ': fmt = struct.unpack('<HHIIHH', d[pos+8:pos+24])
        if cid == b'data':
            body = d[pos+8:]; 
            if sz < len(body) and sz != 0xFFFFFFF0 - 36: body = body[:sz]
            break
        pos += 8 + sz + (sz & 1)
    a = array.array('f'); a.frombytes(body[:len(body)//4*4]); return a, fmt[1]

def deci(a, ch):
    n = len(a) // ch // D
    return [sum(a[(i*D+k)*ch+c] for k in range(D) for c in range(ch)) / (D*ch) for i in range(n)]

def fft(x, inv=False):
    n = len(x); x = list(x); j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit: j ^= bit; bit >>= 1
        j ^= bit
        if i < j: x[i], x[j] = x[j], x[i]
    ln = 2
    while ln <= n:
        w = cmath.exp((2j if inv else -2j) * math.pi / ln); h = ln // 2
        for s in range(0, n, ln):
            wk = 1
            for k in range(h):
                u, v = x[s+k], x[s+k+h]*wk; x[s+k] = u+v; x[s+k+h] = u-v; wk *= w
        ln <<= 1
    return [v / n for v in x] if inv else x

cap, cc = read(sys.argv[1]); ref, rc = read(sys.argv[2])
cap = deci(cap, cc); ref = deci(ref, rc); L = 2 * 48000 // D
N = 1
while N < len(ref) + L: N <<= 1
FR = fft(ref + [0] * (N - len(ref)))
cs = [0.0]
for v in ref: cs.append(cs[-1] + v * v)
for t in map(float, sys.argv[3:]):
    s = int(t * 48000 / D); seg = cap[s:s+L]
    if len(seg) < L: print(t, 'short'); continue
    m = sum(seg) / L; seg = [v - m for v in seg]; es = math.sqrt(sum(v*v for v in seg))
    if es < 1e-6: print('t=%.1f silent' % t); continue
    X = fft(seg + [0] * (N - L)); c = fft([a * b.conjugate() for a, b in zip(FR, X)], True)
    best = (-1, 0)
    for lag in range(0, len(ref) - L + 1):
        er = math.sqrt(max(cs[lag+L] - cs[lag], 1e-12)); r = c[lag].real / (es * er)
        if r > best[0]: best = (r, lag)
    print('t=%.1f  ncc=%.3f  asset_pos=%.2fs  seg_rms=%.4f' % (t, best[0], best[1] * D / 48000, es / math.sqrt(L)))
