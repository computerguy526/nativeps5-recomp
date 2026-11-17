#!/usr/bin/env python3
"""Flicker / stretch detector over a directory of frames.
usage: glitch_scan.py DIR [--thresh T] [--agree A] [--step N] [--flag-pct P] [--csv OUT]

For each frame i with neighbours i-1 and i+1, count the pixels that differ from BOTH
neighbours by more than T (max channel difference) while the two neighbours agree with
each other within A. Steady camera motion changes a pixel the same way in both
directions only by chance, and moves the neighbours apart, so it does not count;
a one-frame flicker or a stretched triangle that appears for a single frame does.
Frames with more than P percent such pixels are flagged. Exit status 1 when any is flagged.
Frames must be consecutive captures. Stdlib only."""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from framelib import list_frames, read_frame  # noqa: E402


def sample(path, step):
    w, h, rgb = read_frame(path)
    stride = w * 3
    rows = []
    for y in range(0, h, step):
        row = rgb[y * stride:(y + 1) * stride]
        rows.append((row[0::3 * step], row[1::3 * step], row[2::3 * step]))
    return w, h, rows


def glitch(prev, cur, nxt, thresh, agree):
    bad = 0
    n = 0
    for (pr, pg, pb), (cr, cg, cb), (nr, ng, nb) in zip(prev, cur, nxt):
        for a0, a1, a2, c0, c1, c2, b0, b1, b2 in zip(pr, pg, pb, cr, cg, cb, nr, ng, nb):
            n += 1
            if (abs(a0 - b0) <= agree and abs(a1 - b1) <= agree and abs(a2 - b2) <= agree
                    and max(abs(c0 - a0), abs(c1 - a1), abs(c2 - a2)) > thresh
                    and max(abs(c0 - b0), abs(c1 - b1), abs(c2 - b2)) > thresh):
                bad += 1
    return bad, n


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir")
    ap.add_argument("--thresh", type=int, default=24, help="min difference from both neighbours")
    ap.add_argument("--agree", type=int, default=8, help="max difference between the neighbours")
    ap.add_argument("--step", type=int, default=2)
    ap.add_argument("--flag-pct", type=float, default=1.0)
    ap.add_argument("--csv", default=None)
    a = ap.parse_args()
    files = list_frames(a.dir)
    if len(files) < 3:
        print("need at least 3 frames in", a.dir)
        return 2
    frames = [sample(files[0], a.step), sample(files[1], a.step)]
    results = []
    flagged = 0
    print("index name glitch_px sampled pct flag   (thresh %d, agree %d, step %d)" % (a.thresh, a.agree, a.step))
    for i in range(1, len(files) - 1):
        nxt = sample(files[i + 1], a.step)
        w, h, cur = frames[1]
        if (frames[0][0], frames[0][1]) != (w, h) or (nxt[0], nxt[1]) != (w, h):
            print("%5d %-24s size change, skipped" % (i, os.path.basename(files[i])))
        else:
            bad, n = glitch(frames[0][2], cur, nxt[2], a.thresh, a.agree)
            pct = 100.0 * bad / max(n, 1)
            flag = pct > a.flag_pct
            flagged += flag
            results.append((i, os.path.basename(files[i]), bad, n, pct, flag))
            print("%5d %-24s %8d %8d %6.3f %s" % (i, os.path.basename(files[i]), bad, n, pct, "GLITCH" if flag else ""))
        frames = [frames[1], nxt]
    pcts = [r[4] for r in results]
    print("frames scanned %d  flagged %d  max %.3f %%  mean %.3f %%" % (len(results), flagged, max(pcts, default=0), sum(pcts) / max(len(pcts), 1)))
    if a.csv:
        with open(a.csv, "w") as fh:
            fh.write("index,name,glitch_px,sampled,pct,flag\n")
            for r in results:
                fh.write("%d,%s,%d,%d,%.4f,%d\n" % r)
    return 1 if flagged else 0


if __name__ == "__main__":
    sys.exit(main())
