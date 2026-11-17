#!/usr/bin/python3
"""Analyse an NPS5_AUDIO_CAPTURE port WAV (float32 stereo 48 kHz) + its grains CSV. Stdlib only.

usage: audio_analyze.py <port.wav> [--assets DIR] [--json OUT]
  * RMS per 100 ms window (dBFS), % non-silent windows, peak, clipping, DC
  * coarse spectrum (radix-2 FFT, 4096 pts) of the 4 loudest windows + band energies
  * timing from <port>.grains.csv: frames vs wall time, grain gaps (underrun candidates)
  * --assets DIR: decoded asset WAVs (48 kHz stereo/mono 16/32-bit or float): which asset's 20 Hz
    RMS envelope correlates with the capture, where, and at what level ratio.
"""
import array, cmath, math, os, struct, sys, json

RATE = 48000
SIL = 1e-4  # -80 dBFS


def read_wav(path):
    with open(path, 'rb') as f:
        d = f.read()
    assert d[:4] == b'RIFF' and d[8:12] == b'WAVE'
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(d):
        cid, sz = d[pos:pos + 4], struct.unpack('<I', d[pos + 4:pos + 8])[0]
        body = d[pos + 8:pos + 8 + sz]
        if cid == b'fmt ':
            fmt = struct.unpack('<HHIIHH', body[:16])
        elif cid == b'data':
            data = d[pos + 8:] if sz >= len(d) - pos - 8 or sz == 0xFFFFFFF0 - 36 else body
            break
        pos += 8 + sz + (sz & 1)
    tag, ch, rate, _, _, bits = fmt
    if tag == 3 and bits == 32:
        a = array.array('f'); a.frombytes(data[:len(data) // 4 * 4])
    elif tag == 1 and bits == 16:
        a = array.array('h'); a.frombytes(data[:len(data) // 2 * 2]); a = array.array('f', [x / 32768.0 for x in a])
    else:
        raise SystemExit('unsupported wav %s tag=%s bits=%s' % (path, tag, bits))
    return a, ch, rate


def to_mono(a, ch):
    if ch == 1:
        return a
    n = len(a) // ch
    return array.array('f', [sum(a[i * ch:i * ch + ch]) / ch for i in range(n)])


def fft(x):
    n = len(x)
    if n == 1:
        return x
    ev, od = fft(x[0::2]), fft(x[1::2])
    t = [cmath.exp(-2j * math.pi * k / n) * od[k] for k in range(n // 2)]
    return [ev[k] + t[k] for k in range(n // 2)] + [ev[k] - t[k] for k in range(n // 2)]


def spectrum(mono, start, n=4096):
    seg = [mono[start + i] * (0.5 - 0.5 * math.cos(2 * math.pi * i / n)) for i in range(n)]
    X = fft(seg)
    mag = [abs(X[k]) / (n / 4) for k in range(n // 2)]
    return mag


BANDS = [(20, 250), (250, 1000), (1000, 4000), (4000, 12000), (12000, 24000)]


def band_energy(mag, n=4096):
    hz = RATE / n
    e = [sum(m * m for k, m in enumerate(mag) if lo <= k * hz < hi) for lo, hi in BANDS]
    t = sum(e) or 1.0
    return [x / t for x in e]


def envelope(mono, hop):
    out = []
    for i in range(0, len(mono) - hop + 1, hop):
        s = 0.0
        for v in mono[i:i + hop]:
            s += v * v
        out.append(math.sqrt(s / hop))
    return out


def ncc_best(cap, ref):
    """best normalised correlation of ref sliding over cap (both envelopes); returns (r, lag)."""
    if len(ref) > len(cap) or len(ref) < 20:
        return (0.0, 0)
    mr = sum(ref) / len(ref)
    rr = [x - mr for x in ref]
    nr = math.sqrt(sum(x * x for x in rr)) or 1.0
    best = (-1.0, 0)
    for lag in range(0, len(cap) - len(ref) + 1):
        seg = cap[lag:lag + len(ref)]
        ms = sum(seg) / len(seg)
        num = den = 0.0
        for a, b in zip(seg, rr):
            num += (a - ms) * b
            den += (a - ms) * (a - ms)
        r = num / (math.sqrt(den) * nr) if den > 0 else 0.0
        if r > best[0]:
            best = (r, lag)
    return best


NB = 24
EDGES = [60 * (12000 / 60) ** (i / NB) for i in range(NB + 1)]


def logband_frames(mono, hop_s=0.25):
    """one 24-band log-energy vector per hop (4096-pt Hann FFT); None for silent frames."""
    hop = int(RATE * hop_s); out = []
    hz = RATE / 4096
    idx = [(max(1, int(EDGES[b] / hz)), max(2, int(EDGES[b + 1] / hz))) for b in range(NB)]
    for st in range(0, len(mono) - 4096 + 1, hop):
        mag = spectrum(mono, st)
        if sum(m * m for m in mag) < 1e-7:
            out.append(None); continue
        out.append([math.log10(sum(m * m for m in mag[lo:hi]) + 1e-12) for lo, hi in idx])
    return out


def centre(frames):
    ok = [f for f in frames if f]
    if not ok: return frames
    mean = [sum(f[b] for f in ok) / len(ok) for b in range(NB)]
    res = []
    for f in frames:
        if not f: res.append(None); continue
        v = [f[b] - mean[b] for b in range(NB)]
        n = math.sqrt(sum(x * x for x in v)) or 1.0
        res.append([x / n for x in v])
    return res


def match_music(cap_mono, assets):
    """spectral-shape match of each 0.25 s capture frame against every asset frame."""
    cap = logband_frames(cap_mono)
    refs = {}
    for name, m in assets.items():
        refs[name] = logband_frames(m)
    allf = [f for fr in refs.values() for f in fr if f]
    mean = [sum(f[b] for f in allf) / len(allf) for b in range(NB)]
    def norm(f):
        if not f: return None
        v = [f[b] - mean[b] for b in range(NB)]
        n = math.sqrt(sum(x * x for x in v)) or 1.0
        return [x / n for x in v]
    refs = {k: [norm(f) for f in v] for k, v in refs.items()}
    cap = [norm(f) for f in cap]
    flat = [(k, j, f) for k, v in refs.items() for j, f in enumerate(v) if f]
    best = []
    for f in cap:
        if not f: best.append(None); continue
        b = (-2, None, 0)
        for k, j, g in flat:
            sim = sum(x * y for x, y in zip(f, g))
            if sim > b[0]: b = (sim, k, j)
        best.append(b)
    return best


def main():
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    wav = args[0]
    assets = args[args.index('--assets') + 1] if '--assets' in args else None
    jout = args[args.index('--json') + 1] if '--json' in args else None
    a, ch, rate = read_wav(wav)
    assert ch == 2 and rate == RATE, (ch, rate)
    frames = len(a) // 2
    mono = array.array('f', [(a[2 * i] + a[2 * i + 1]) * 0.5 for i in range(frames)])
    R = {'file': wav, 'frames': frames, 'seconds': frames / RATE}
    peak = max(max(a), -min(a)) if len(a) else 0.0
    R['peak'] = peak
    R['peak_dbfs'] = 20 * math.log10(peak) if peak > 0 else None
    R['clipped_samples'] = sum(1 for v in a if v >= 0.999 or v <= -0.999)
    R['dc_offset'] = sum(mono) / max(frames, 1)
    R['nan_samples'] = sum(1 for v in a if v != v)
    win = RATE // 10
    env = envelope(mono, win)
    ns = [e for e in env if e > SIL]
    R['windows_100ms'] = len(env)
    R['nonsilent_pct'] = 100.0 * len(ns) / max(len(env), 1)
    allsq = sum(v * v for v in mono)
    R['rms_overall_dbfs'] = 10 * math.log10(allsq / max(frames, 1)) if allsq > 0 else None
    R['rms_nonsilent_dbfs'] = 20 * math.log10(math.sqrt(sum(e * e for e in ns) / len(ns))) if ns else None
    # RMS over time, 5 s buckets
    per = int(5 / 0.1)
    R['rms_dbfs_per_5s'] = [round(20 * math.log10(max(math.sqrt(sum(e * e for e in env[i:i + per]) / len(env[i:i + per])), 1e-9)), 1)
                            for i in range(0, len(env), per)]
    R['nonsilent_pct_per_5s'] = [round(100.0 * sum(1 for e in env[i:i + per] if e > SIL) / len(env[i:i + per]))
                                 for i in range(0, len(env), per)]
    # longest all-silent run (windows)
    run = best = 0
    for e in env:
        run = run + 1 if e <= SIL else 0
        best = max(best, run)
    R['longest_silence_s'] = best * 0.1
    # zero-sample runs inside the stream (>= 5 ms of exact zeros): dropouts / underrun-style holes
    holes = 0; z = 0
    for v in mono:
        if v == 0.0:
            z += 1
        else:
            if z >= 240: holes += 1
            z = 0
    R['exact_zero_runs_ge_5ms'] = holes
    # spectrum of the 4 loudest windows (distinct by >= 2 s)
    order = sorted(range(len(env)), key=lambda i: -env[i])
    picks = []
    for i in order:
        if all(abs(i - j) >= 20 for j in picks) and i * win + 4096 <= frames:
            picks.append(i)
        if len(picks) == 4:
            break
    R['spectra'] = []
    for i in sorted(picks):
        mag = spectrum(mono, i * win)
        top = sorted(range(2, len(mag)), key=lambda k: -mag[k])[:3]
        cent = sum(k * m for k, m in enumerate(mag)) / (sum(mag) or 1) * RATE / 4096
        R['spectra'].append({'t_s': i * 0.1, 'rms_dbfs': round(20 * math.log10(max(env[i], 1e-9)), 1),
                             'top_hz': [round(k * RATE / 4096) for k in top],
                             'centroid_hz': round(cent), 'band_frac': [round(x, 3) for x in band_energy(mag)],
                             'bands_hz': BANDS})
    # timing
    csvp = wav[:-4] + '.grains.csv'
    if os.path.exists(csvp):
        rows = [tuple(int(x) for x in l.split(',')) for l in open(csvp) if l[0] != '#' and l.strip()]
        if len(rows) > 1:
            g = rows[1][1] - rows[0][1] if len(rows) > 1 else 0
            t0, t1 = rows[0][2], rows[-1][2]
            wall = (t1 - t0) / 1e6
            gaps = [(rows[i][2] - rows[i - 1][2]) for i in range(1, len(rows))]
            gus = g * 1e6 / RATE
            R['timing'] = {
                'grains': len(rows), 'grain_frames': g, 'grain_ms': gus / 1000,
                'wall_first_to_last_s': wall, 'audio_s_between_first_and_last': (rows[-1][1] - rows[0][1]) / RATE,
                'audio_per_wall': ((rows[-1][1] - rows[0][1]) / RATE) / wall if wall > 0 else None,
                'gap_ms_median': sorted(gaps)[len(gaps) // 2] / 1000, 'gap_ms_max': max(gaps) / 1000,
                'gaps_gt_2x_grain': sum(1 for x in gaps if x > 2 * gus),
                'gaps_gt_4x_grain': sum(1 for x in gaps if x > 4 * gus),
                'gaps_gt_50ms': sum(1 for x in gaps if x > 50000),
                'worst_gaps_ms_at_audio_s': [(round(gaps[i] / 1000, 1), round(rows[i + 1][1] / RATE, 2))
                                             for i in sorted(range(len(gaps)), key=lambda i: -gaps[i])[:5]],
                # buffer model: audio queued by the guest minus audio the clock has consumed since first grain
                'min_lead_ms': min((rows[i][1] + g) / RATE * 1000 - (rows[i][2] - t0) / 1000 for i in range(len(rows))),
                'frames_vs_wall_ratio_total': frames / RATE / wall if wall > 0 else None,
            }
    # asset match
    if assets:
        cap_env = envelope(mono, RATE // 20)
        res = []
        for fn in sorted(os.listdir(assets)):
            if not fn.endswith('.wav'):
                continue
            try:
                b, bc, br = read_wav(os.path.join(assets, fn))
            except Exception as e:
                continue
            bm = to_mono(b, bc)
            if br != RATE:
                continue
            ref = envelope(bm, RATE // 20)
            arms = math.sqrt(sum(v * v for v in bm) / max(len(bm), 1))
            r, lag = ncc_best(cap_env, ref)
            seg = cap_env[lag:lag + len(ref)]
            crms = math.sqrt(sum(v * v for v in seg) / max(len(seg), 1))
            res.append({'asset': fn, 'secs': len(bm) / RATE, 'ncc': round(r, 3), 'at_s': lag / 20.0,
                        'level_ratio_capture_over_asset': round(crms / arms, 3) if arms > 0 else None})
        res.sort(key=lambda x: -x['ncc'])
        R['asset_matches_top'] = res[:8]
        R['asset_matches_all'] = res
    if assets and '--music' in args:
        am = {}
        for fn in sorted(os.listdir(assets)):
            if fn.endswith('.wav'):
                try:
                    b, bc, br = read_wav(os.path.join(assets, fn))
                    if br == RATE: am[fn] = to_mono(b, bc)
                except Exception:
                    pass
        best = match_music(mono, am)
        buckets = []
        for i in range(0, len(best), 20):  # 5 s
            seg = [b for b in best[i:i + 20] if b]
            if not seg:
                buckets.append({'t_s': i * 0.25, 'asset': None}); continue
            cnt = {}
            for sim, k, j in seg: cnt[k] = cnt.get(k, 0) + 1
            top = max(cnt, key=cnt.get)
            sel = [(i + n, b) for n, b in enumerate(best[i:i + 20]) if b and b[1] == top]
            # offsets (asset pos - capture pos) in frames; stable offsets mean the same passage plays 1:1
            offs = sorted(b[2] - n for n, b in sel)
            med = offs[len(offs) // 2]
            stable = sum(1 for o in offs if abs(o - med) <= 1)
            buckets.append({'t_s': i * 0.25, 'asset': top, 'frames_of_20': len(seg), 'agree': cnt[top],
                            'mean_cos': round(sum(b[0] for _, b in sel) / len(sel), 3),
                            'offset_stable': stable, 'asset_pos_s': round((med + i) * 0.25, 2)})
        R['music_match_per_5s'] = buckets
    if jout:
        json.dump(R, open(jout, 'w'), indent=1)
    print(json.dumps(R, indent=None, separators=(',', ':')))


main()
