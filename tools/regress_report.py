#!/usr/bin/python3
"""Helper for tools/regress.sh (stdlib only; run with python3 -I).

  regress_report.py title  --title T --run-dir D --refs R [--out metrics.json]
        Evaluate one homebrew-title run.  D holds timing.json, run.err, frames/ (APS5 dump BMPs),
        audio/ (NPS5_AUDIO_CAPTURE output) and w/runs (diagnostics bundles).  R is the title's reference
        directory (thresholds.json + reference frames).  Prints a JSON document {status, checks[], metrics{}}.
  regress_report.py final  --results results.tsv --out runs/regress-<ts>.json [--ref-results ref.tsv] ...
        Merge check results into the final JSON and print the PASS/FAIL table (exit 1 on any failure).
  regress_report.py bmpcmp A.bmp B.bmp
        Pixel comparison of two BMPs (exact %, max channel diff, PSNR) as JSON; exit 0 iff identical.
"""
import array, json, math, os, re, struct, sys

# ------------------------------------------------------------------ BMP / pixel comparison
def read_bmp(path):
    d = open(path, 'rb').read()
    off = struct.unpack('<I', d[10:14])[0]
    w, h, _planes, bpp = struct.unpack('<iiHH', d[18:30])
    bpp //= 8
    flip = h > 0
    h = abs(h)
    stride = (w * bpp + 3) & ~3
    rows = [d[off + y * stride: off + y * stride + w * bpp] for y in range(h)]
    if flip:
        rows.reverse()
    return w, h, bpp, rows


def valid_bmp(path):
    """True when the file is a complete BMP. A run killed by the 80 s SIGTERM can leave a zero-byte or half-written last frame
    dump; such a frame is ignored (it must not crash the report into a false FAIL)."""
    try:
        sz = os.path.getsize(path)
        if sz < 30:
            return False
        with open(path, 'rb') as f:
            hd = f.read(30)
        if hd[:2] != b'BM':
            return False
        off = struct.unpack('<I', hd[10:14])[0]
        w, h, _planes, bpp = struct.unpack('<iiHH', hd[18:30])
        if w <= 0 or h == 0 or bpp not in (24, 32):
            return False
        return sz >= off + (((w * (bpp // 8)) + 3) & ~3) * abs(h)
    except (OSError, struct.error):
        return False


def frame_files(fdir):
    """Sorted *.bmp names in fdir that are complete."""
    if not os.path.isdir(fdir):
        return []
    return sorted(f for f in os.listdir(fdir) if f.endswith('.bmp') and valid_bmp(os.path.join(fdir, f)))


def rgb_rows(img):
    w, h, bpp, rows = img
    if bpp == 3:
        return rows
    out = []
    for r in rows:                      # drop the 4th (alpha / X) byte
        v = bytearray(r)
        del v[3::4]
        out.append(bytes(v))
    return out


def compare_bmp(pa, pb, near=16):
    """exact_pct: pixels with all channels equal; near_pct: pixels with every channel within +-near (tolerates the
    phase jitter of animated content and tiny filtering differences); max_diff / PSNR over the whole frame."""
    A, B = read_bmp(pa), read_bmp(pb)
    if (A[0], A[1]) != (B[0], B[1]):
        return {'size_mismatch': [A[0], A[1], B[0], B[1]], 'exact_pct': 0.0, 'near_pct': 0.0, 'max_diff': 255, 'psnr_db': 0.0}
    w, h = A[0], A[1]
    ra, rb = rgb_rows(A), rgb_rows(B)
    eq = 0
    nr = 0
    mx = 0
    sq = 0
    for ya, yb in zip(ra, rb):
        if ya == yb:
            eq += w
            nr += w
            continue
        a, b = array.array('B', ya), array.array('B', yb)
        for x in range(w):
            i = x * 3
            d0, d1, d2 = a[i] - b[i], a[i + 1] - b[i + 1], a[i + 2] - b[i + 2]
            if d0 == 0 and d1 == 0 and d2 == 0:
                eq += 1
                nr += 1
                continue
            sq += d0 * d0 + d1 * d1 + d2 * d2
            m = max(abs(d0), abs(d1), abs(d2))
            if m <= near:
                nr += 1
            if m > mx:
                mx = m
    mse = sq / (w * h * 3)
    return {'exact_pct': round(100.0 * eq / (w * h), 4), 'near_pct': round(100.0 * nr / (w * h), 4), 'max_diff': mx,
            'psnr_db': 99.0 if mse == 0 else round(10 * math.log10(255 * 255 / mse), 2)}


def signature(path, step=8):
    w, h, bpp, rows = read_bmp(path)
    return [rows[y][x * bpp:x * bpp + 3] for y in range(0, h, step) for x in range(0, w, step)]


def sig_dist(a, b):
    if len(a) != len(b):
        return 1e9
    return sum(abs(p[0] - q[0]) + abs(p[1] - q[1]) + abs(p[2] - q[2]) for p, q in zip(a, b))


# ------------------------------------------------------------------ audio
def audio_stats(wav_path, win_ms=100, sil=1e-4):
    """float32 stereo 48 kHz WAV (NPS5_AUDIO_CAPTURE). Non-silent = a 100 ms window with peak > -80 dBFS."""
    d = open(wav_path, 'rb').read()
    if d[:4] != b'RIFF':
        return None
    pos, fmt, data = 12, None, b''
    while pos + 8 <= len(d):
        cid, sz = d[pos:pos + 4], struct.unpack('<I', d[pos + 4:pos + 8])[0]
        if cid == b'fmt ':
            fmt = struct.unpack('<HHIIHH', d[pos + 8:pos + 24])
        elif cid == b'data':
            data = d[pos + 8:] if (sz == 0 or pos + 8 + sz > len(d)) else d[pos + 8:pos + 8 + sz]
            break
        pos += 8 + sz + (sz & 1)
    if fmt is None or fmt[0] != 3 or fmt[4] * 8 // fmt[1] != 32 and False:
        pass
    ch, rate = (fmt[1], fmt[2]) if fmt else (2, 48000)
    n = len(data) // 4
    samples = array.array('f')
    samples.frombytes(data[:n * 4])
    win = int(rate * win_ms / 1000) * ch
    nwin = n // win
    if nwin == 0:
        return {'seconds': 0.0, 'windows': 0, 'nonsilent_pct': 0.0, 'peak': 0.0, 'rms_db': -200.0}
    ns = 0
    peak = 0.0
    sq = 0.0
    for k in range(nwin):
        ch_ = samples[k * win:(k + 1) * win]
        p = max(max(ch_), -min(ch_))
        if p > sil:
            ns += 1
        if p > peak:
            peak = p
        sq += sum(x * x for x in ch_[::8]) * 8
    rms = math.sqrt(sq / (nwin * win)) if sq > 0 else 0.0
    return {'seconds': round(n / ch / rate, 2), 'windows': nwin, 'nonsilent_pct': round(100.0 * ns / nwin, 2),
            'peak': round(peak, 4), 'rms_db': round(20 * math.log10(rms), 1) if rms > 0 else -200.0}


# ------------------------------------------------------------------ title evaluation
DEFAULTS = {'max_gap_s': 1.0, 'tail_slack_s': 1.5, 'min_frames': 0, 'min_fps': 0.0, 'dump_every': 1,
            'min_audio_nonsilent_pct': None, 'frame0_exact': None, 'checkpoints': [], 'search_top': 6}


def check(name, ok, value, thr, note=''):
    return {'name': name, 'status': 'PASS' if ok else 'FAIL', 'value': value, 'threshold': thr, 'note': note}


def title_eval(title, run_dir, refs):
    cfg = dict(DEFAULTS)
    cpath = os.path.join(refs, 'thresholds.json')
    if os.path.exists(cpath):
        cfg.update(json.load(open(cpath)))
    tim = json.load(open(os.path.join(run_dir, 'timing.json')))
    checks, met = [], {'thresholds_file': cpath if os.path.exists(cpath) else None}
    # ---- run / crash
    rc = tim.get('rc')
    crash = None
    cands = [os.path.join(run_dir, 'w', 'runs', 'latest-run', 'crash.txt')]
    for p in cands:
        if os.path.exists(p) and os.path.getsize(p) > 0:
            txt = open(p, errors='replace').read()
            m = re.search(r'^signal: (\d+)', txt, re.M)
            note = re.search(r'^note: (.*)$', txt, re.M)
            reason = re.search(r'^reason: (.*)$', txt, re.M)
            if m and m.group(1) != '15':
                crash = {'signal': m.group(1), 'reason': reason.group(1) if reason else '',
                         'note': note.group(1)[:160] if note else '', 'file': p}
    met['rc'] = rc
    met['crash'] = crash
    alive = rc == 124
    why = []
    if not alive:
        why.append('rc=%s (expected 124: alive until the harness timeout)' % rc)
    if crash:
        why.append('crash.txt signal %s %s' % (crash['signal'], crash['note'] or crash['reason']))
    checks.append(check('run', alive and not crash, rc, 124, '; '.join(why)))
    # ---- flips from the dump log
    rel = []
    err = os.path.join(run_dir, 'run.err')
    if os.path.exists(err):
        for line in open(err, errors='replace'):
            m = re.match(r'\[gpu\] frame_(\d+)\.bmp: .* at ([0-9.]+) s', line)
            if m:
                rel.append((int(m.group(1)), float(m.group(2))))
    every = int(cfg['dump_every'])
    n = len(rel)
    presented = n * every
    met['dumps'] = n
    met['dump_every'] = every
    met['frames_presented_est'] = presented
    checks.append(check('frames', presented >= cfg['min_frames'], presented, '>= %d' % cfg['min_frames'],
                        '' if n else 'no frame was ever presented'))
    gaps = [rel[i + 1][1] - rel[i][1] for i in range(n - 1)]
    max_gap = max(gaps) if gaps else 0.0
    t_first, t_end = tim.get('t_first_dump'), tim.get('t_end')
    tail = None
    if n and t_first and t_end:
        tail = (t_end - t_first) - rel[-1][1]
        # the log has 0.1 s resolution and the first-dump timestamp is polled at 50 ms
    met['max_flip_gap_s'] = round(max_gap, 2)
    met['tail_gap_s'] = round(tail, 2) if tail is not None else None
    met['first_flip_after_s'] = round(t_first - tim['t_start'], 2) if (t_first and tim.get('t_start')) else None
    if n:
        worst = max(max_gap, (tail - cfg['tail_slack_s'] + cfg['max_gap_s']) if tail is not None else 0)
        checks.append(check('max_gap', worst <= cfg['max_gap_s'] + 0.1, round(max(max_gap, tail or 0), 2),
                            '<= %.1f s' % cfg['max_gap_s'],
                            'longest gap between presented frames (%d-frame samples) %.1f s, stall at the end %s s' % (
                                every, max_gap, met['tail_gap_s'])))
        span = rel[-1][1] - rel[0][1]
        fps = (n - 1) * every / span if span > 0 else 0.0
        met['fps'] = round(fps, 1)
        if cfg['min_fps']:
            checks.append(check('fps', fps >= cfg['min_fps'], round(fps, 1), '>= %.1f' % cfg['min_fps']))
    else:
        checks.append(check('max_gap', False, None, '<= %.1f s' % cfg['max_gap_s'], 'no flips'))
    # ---- audio
    wavs = sorted(f for f in os.listdir(os.path.join(run_dir, 'audio'))
                  if f.endswith('.wav')) if os.path.isdir(os.path.join(run_dir, 'audio')) else []
    if wavs:
        a = audio_stats(os.path.join(run_dir, 'audio', wavs[0]))
        met['audio'] = dict(a or {}, file=wavs[0])
        thr = cfg['min_audio_nonsilent_pct']
        if thr is not None and a:
            checks.append(check('audio', a['nonsilent_pct'] >= thr, a['nonsilent_pct'], '>= %.0f %%' % thr,
                                'non-silent 100 ms windows; %.1f s captured, peak %.3f' % (a['seconds'], a['peak'])))
    else:
        met['audio'] = None
        if cfg['min_audio_nonsilent_pct'] is not None and not tim.get('capture', True):
            c = check('audio', True, None, '>= %.0f %%' % cfg['min_audio_nonsilent_pct'], 'runtime of this rev has no NPS5_AUDIO_CAPTURE: not measured')
            c['status'] = 'SKIP'
            checks.append(c)
        elif cfg['min_audio_nonsilent_pct'] is not None:
            checks.append(check('audio', False, None, '>= %.0f %%' % cfg['min_audio_nonsilent_pct'],
                                'no audio capture produced (runtime without NPS5_AUDIO_CAPTURE, or no AudioOut port opened)'))
    # ---- reference frames
    fdir = os.path.join(run_dir, 'frames')
    got = frame_files(fdir)
    f0 = cfg['frame0_exact']
    if f0:
        gp = os.path.join(run_dir, 'frame0', 'frame_000.bmp')
        rp = os.path.join(refs, f0)
        if valid_bmp(gp) and valid_bmp(rp):
            r = compare_bmp(gp, rp)
            met['frame0'] = r
            checks.append(check('frame0', r['exact_pct'] == 100.0 and r['max_diff'] == 0, r['exact_pct'], '100 % (max diff 0)',
                                'vs %s, max channel diff %d, PSNR %s' % (f0, r['max_diff'], r['psnr_db'])))
        else:
            checks.append(check('frame0', False, None, '100 %', 'frame_000.bmp missing (or truncated) in this run' if not valid_bmp(gp)
                                else 'reference %s missing' % rp))
    cps = cfg['checkpoints']
    if cps:
        # Presentation rate is environmental (the same revision ran at 60 and at 120 frames per second on this machine, display
        # refresh/power state), and the scripted pad is wall-clock, so "frame k" is not a fixed scene.  Each reference frame is
        # therefore searched in the whole run: candidates are ranked by a coarse signature and the best few compared in full.
        sigs = {}
        for f in got:
            sigs[f] = signature(os.path.join(fdir, f))
        res = []
        for cp in cps:
            i, tol = cp['index'], cp['min_near_pct']
            rp = os.path.join(refs, 'frame_%03d.bmp' % i)
            if not os.path.exists(rp):
                res.append({'index': i, 'error': 'no reference', 'min_near_pct': tol})
                continue
            if not sigs:
                res.append({'index': i, 'error': 'no frame was presented', 'min_near_pct': tol})
                continue
            rs = signature(rp)
            ranked = sorted(sigs, key=lambda f: sig_dist(sigs[f], rs))[:cfg.get('search_top', 6)]
            best = None
            for f in ranked:
                r = compare_bmp(os.path.join(fdir, f), rp)
                r['frame'] = f
                if best is None or r['near_pct'] > best['near_pct']:
                    best = r
            best.update(index=i, min_near_pct=tol)
            res.append(best)
        met['checkpoints'] = res
        bad = [r for r in res if 'error' in r or r['near_pct'] < r['min_near_pct']]
        worst = min((r.get('near_pct', 0.0) for r in res), default=100.0)
        checks.append(check('frames_ref', not bad, worst, 'each >= its tolerance (see metrics)',
                            '; '.join('ref #%d %s' % (r['index'], r['error'] if 'error' in r else '%.1f%% < %.1f%%' % (r['near_pct'], r['min_near_pct']))
                                      for r in bad) if bad else '%d reference frames each found in the run, worst %.2f%% of pixels within +-16' % (len(res), worst)))
    status = 'PASS' if all(c['status'] in ('PASS', 'SKIP') for c in checks) else 'FAIL'
    return {'title': title, 'status': status, 'checks': checks, 'metrics': met}


# ------------------------------------------------------------------ final table / json
def read_results(path):
    rows = []
    if path and os.path.exists(path):
        for line in open(path):
            p = line.rstrip('\n').split('\t')
            if len(p) >= 4:
                rows.append({'check': p[0], 'status': p[1], 'seconds': int(float(p[2])), 'detail': p[3],
                             'metrics': p[4] if len(p) > 4 else ''})
    return rows


def expand(rows):
    """One flat list of (name, status, seconds, detail); title rows expand into their sub-checks."""
    out = []
    for r in rows:
        mp = r['metrics']
        if mp and os.path.exists(mp):
            m = json.load(open(mp))
            if 'checks' in m:
                out.append((r['check'], m['status'], r['seconds'], 'build/run %s' % ('ok' if m['status'] == 'PASS' else 'see below')))
                for c in m['checks']:
                    out.append(('  ' + r['check'] + '.' + c['name'], c['status'], None, '%s (need %s)%s' % (
                        c['value'], c['threshold'], ' ' + c['note'] if c['note'] else '')))
                continue
        out.append((r['check'], r['status'], r['seconds'], r['detail']))
    return out


def table(flat, title):
    w = max([len(r[0]) for r in flat] + [8])
    lines = [title, '%-*s  %-6s %6s  %s' % (w, 'CHECK', 'RESULT', 'SECS', 'DETAIL')]
    for n, s, t, d in flat:
        lines.append('%-*s  %-6s %6s  %s' % (w, n, s, '' if t is None else t, d))
    return '\n'.join(lines)


def final(args):
    opt = dict(zip(args[::2], args[1::2]))
    rows = read_results(opt['--results'])
    flat = expand(rows)
    doc = {'timestamp': opt.get('--ts'), 'mode': opt.get('--mode'), 'rev': opt.get('--rev'), 'subject': opt.get('--subject'),
           'jobs': opt.get('--jobs'), 'wall_seconds': int(opt.get('--wall', 0)), 'checks': [], 'titles': {}}
    ok = True
    for r in rows:
        e = {k: r[k] for k in ('check', 'status', 'seconds', 'detail')}
        mp = r['metrics']
        if mp and os.path.exists(mp):
            m = json.load(open(mp))
            if 'checks' in m:
                doc['titles'][r['check']] = m
                e['status'] = m['status']
        doc['checks'].append(e)
        if e['status'] not in ('PASS', 'SKIP'):
            ok = False
    doc['status'] = 'PASS' if ok else 'FAIL'
    print(table(flat, 'regress: %s  mode=%s  rev=%s' % (opt.get('--subject', ''), opt.get('--mode', ''), opt.get('--rev', ''))))
    if '--ref-results' in opt and os.path.exists(opt['--ref-results']):
        rr = read_results(opt['--ref-results'])
        rflat = expand(rr)
        print()
        print(table(rflat, 'reference: %s  rev=%s' % (opt.get('--ref-label', ''), opt.get('--ref-rev', ''))))
        a = {n: s for n, s, _t, _d in flat}
        b = {n: s for n, s, _t, _d in rflat}
        diff = [(n, a.get(n, '-'), b.get(n, '-')) for n in sorted(set(a) | set(b)) if a.get(n) != b.get(n)]
        print('\ndiff (subject vs reference): ' + ('no check changed status' if not diff else ''))
        for n, x, y in diff:
            print('  %-34s %-5s (subject)   %-5s (reference)' % (n.strip(), x, y))
        doc['ref'] = {'label': opt.get('--ref-label'), 'rev': opt.get('--ref-rev'), 'checks': [
            {k: r[k] for k in ('check', 'status', 'seconds', 'detail')} for r in rr], 'diff': [
            {'check': n.strip(), 'subject': x, 'reference': y} for n, x, y in diff],
            'titles': {r['check']: json.load(open(r['metrics'])) for r in rr if r['metrics'] and os.path.exists(r['metrics'])
                       and 'checks' in json.load(open(r['metrics']))}}
    print('\nRESULT: %s  (%d s)' % (doc['status'], doc['wall_seconds']))
    json.dump(doc, open(opt['--out'], 'w'), indent=1)
    print('wrote ' + opt['--out'])
    return 0 if ok else 1


def refs_update(o):
    import shutil
    run_dir, refs = o['--run-dir'], o['--refs']
    os.makedirs(refs, exist_ok=True)
    cpath = os.path.join(refs, 'thresholds.json')
    cfg = json.load(open(cpath)) if os.path.exists(cpath) else {}
    tim = json.load(open(os.path.join(run_dir, 'timing.json')))
    fdir = os.path.join(run_dir, 'frames')
    have = sorted(int(f[6:9]) for f in frame_files(fdir) if f.startswith('frame_'))
    cps = cfg.get('checkpoints') or [{'index': i, 'min_near_pct': float(o.get('--tol', 80))}
                                    for i in have[3::max(1, len(have) // 6)][:6]]
    cfg['checkpoints'] = cps
    cfg.setdefault('dump_every', 1)
    n = 0
    for cp in cps:
        src = os.path.join(fdir, 'frame_%03d.bmp' % cp['index'])
        if os.path.exists(src):
            shutil.copy(src, os.path.join(refs, 'frame_%03d.bmp' % cp['index']))
            n += 1
    cfg['refs_meta'] = o.get('--meta', '')
    json.dump(cfg, open(cpath, 'w'), indent=1)
    print('refs-update: %d frames -> %s' % (n, refs))


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else ''
    if cmd == 'title':
        o = dict(zip(sys.argv[2::2], sys.argv[3::2]))
        res = title_eval(o['--title'], o['--run-dir'], o['--refs'])
        s = json.dumps(res, indent=1)
        if '--out' in o:
            open(o['--out'], 'w').write(s)
        print(s if '--quiet' not in sys.argv else res['status'])
        sys.exit(0 if res['status'] == 'PASS' else 1)
    elif cmd == 'refs-update':
        refs_update(dict(zip(sys.argv[2::2], sys.argv[3::2])))
    elif cmd == 'final':
        sys.exit(final(sys.argv[2:]))
    elif cmd == 'bmpcmp':
        r = compare_bmp(sys.argv[2], sys.argv[3])
        print(json.dumps(r))
        sys.exit(0 if r['exact_pct'] == 100.0 else 1)
    elif cmd == 'audio':
        print(json.dumps(audio_stats(sys.argv[2])))
    else:
        print(__doc__)
        sys.exit(2)
