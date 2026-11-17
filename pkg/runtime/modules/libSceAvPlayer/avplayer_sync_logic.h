// SPDX-License-Identifier: GPL-2.0-or-later
// Video frame selection and timing statistics of the AvPlayer HLE (host-testable, no ffmpeg/guest deps).
// Build/run the test: tools/avsync_unit.sh
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

// Which queued frame is shown at clock `now_ms`: the LAST one whose timestamp is due (ts <= now + tol_ms);
// every earlier due frame is stale (the guest polls once per present, so two due frames mean we are over a
// frame late) and is skipped. Returns the index to deliver (the caller drops the `index` frames before it)
// or -1 = nothing due, keep holding (the guest keeps showing its last picture).
// The old rule delivered ONE frame per poll, up to 40 ms EARLY and never skipped, so a decode stall was
// followed by a fast-forward burst at the poll rate (60 Hz for 30 fps video) = judder.
static inline int nps5_avsync_pick(const uint64_t *ts, size_t n, uint64_t now_ms, uint64_t tol_ms) {
    int pick = -1;
    for (size_t i = 0; i < n; ++i) {
        if (ts[i] <= now_ms + tol_ms) pick = (int)i; else break;
    }
    return pick;
}

// Welford running mean / variance.
struct Nps5AvRunStat {
    uint64_t n = 0; double mean = 0, m2 = 0, mn = 1e300, mx = -1e300;
    void add(double v) {
        n++; double d = v - mean; mean += d / (double)n; m2 += d * (v - mean);
        if (v < mn) mn = v; if (v > mx) mx = v;
    }
    double stddev() const { return n > 1 ? std::sqrt(m2 / (double)(n - 1)) : 0.0; }
};

struct Nps5AvSyncStats {
    uint64_t polls = 0, delivered = 0, dropped = 0, empty_polls = 0, held_early = 0;
    Nps5AvRunStat deliver_interval_ms;  // time between two delivered frames
    Nps5AvRunStat poll_interval_ms;     // time between two guest GetVideoData calls
    Nps5AvRunStat lateness_ms;          // clock at delivery minus frame timestamp (negative = early)
    Nps5AvRunStat convert_ms;           // yuv->NV12 copy into the guest buffer per frame
    Nps5AvRunStat decode_lead_ms;       // frame timestamp minus clock when decode finished (negative = decoded late)
    double min_reuse_age_ms = 1e300;    // shortest time between the guest fetching a buffer and the decoder refilling it
    uint64_t reuse_under_50ms = 0;
};
