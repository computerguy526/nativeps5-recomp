#!/usr/bin/env python3
"""Per-frame mean brightness and distinct-colour count; flags dark frames.
usage: frame_stats.py DIR [--ref DIR|--ref-mean M] [--step N] [--dark-ratio R] [--csv OUT]

DIR holds PNG or BMP frames (APS5_DUMP_FRAMES / autoplay / screenshot captures).
Brightness is the mean Rec.601 luma (0..255) over every --step'th pixel in x and y.
The reference mean defaults to the mean over work/ppsa30552/ref/frames (about 104).
A frame is flagged DARK below R x reference (default 0.75), and the run is
flagged when the median frame is dark. Exit status 1 when the run is flagged.
Distinct colours are counted over the sampled pixels (24-bit RGB).
Stdlib only."""
import argparse
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from framelib import list_frames, read_frame  # noqa: E402

DEFAULT_REF = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "work", "ppsa30552", "ref", "frames")


def stats(path, step):
    w, h, rgb = read_frame(path)
    rs, gs, bs = [], [], []
    colours = set()
    total = 0.0
    n = 0
    stride = w * 3
    for y in range(0, h, step):
        row = rgb[y * stride:(y + 1) * stride]
        r, g, b = row[0::3 * step], row[1::3 * step], row[2::3 * step]
        for rv, gv, bv in zip(r, g, b):
            total += 0.299 * rv + 0.587 * gv + 0.114 * bv
            colours.add((rv << 16) | (gv << 8) | bv)
            n += 1
    return w, h, total / max(n, 1), len(colours)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir")
    ap.add_argument("--ref", default=None, help="reference frame directory (default work/ppsa30552/ref/frames)")
    ap.add_argument("--ref-mean", type=float, default=None)
    ap.add_argument("--step", type=int, default=4)
    ap.add_argument("--dark-ratio", type=float, default=0.75)
    ap.add_argument("--csv", default=None)
    a = ap.parse_args()

    ref = a.ref_mean
    if ref is None:
        refdir = a.ref or DEFAULT_REF
        files = list_frames(refdir) if os.path.isdir(refdir) else []
        if files:
            ref = statistics.mean(stats(f, a.step)[2] for f in files)
        else:
            ref = 104.0
            print("reference frames not found, using 104", file=sys.stderr)
    limit = ref * a.dark_ratio
    files = list_frames(a.dir)
    if not files:
        print("no frames in", a.dir)
        return 2
    rows = []
    for i, f in enumerate(files):
        w, h, mean, colours = stats(f, a.step)
        rows.append((i, os.path.basename(f), w, h, mean, colours, mean < limit))
    print("index name size mean_luma colours flag   (reference mean %.1f, dark below %.1f)" % (ref, limit))
    for i, name, w, h, mean, colours, dark in rows:
        print("%5d %-24s %dx%d %7.1f %8d %s" % (i, name, w, h, mean, colours, "DARK" if dark else ""))
    means = [r[4] for r in rows]
    med = statistics.median(means)
    print("frames %d  mean %.1f  median %.1f  min %.1f  max %.1f  dark frames %d  median/ref %.2f  -> %s" % (
        len(rows), statistics.mean(means), med, min(means), max(means), sum(r[6] for r in rows), med / ref,
        "DARK RUN" if med < limit else "ok"))
    if a.csv:
        with open(a.csv, "w") as fh:
            fh.write("index,name,mean_luma,colours,dark\n")
            for i, name, w, h, mean, colours, dark in rows:
                fh.write("%d,%s,%.2f,%d,%d\n" % (i, name, mean, colours, dark))
    return 1 if med < limit else 0


if __name__ == "__main__":
    sys.exit(main())
