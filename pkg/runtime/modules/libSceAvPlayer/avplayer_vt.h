// SPDX-License-Identifier: GPL-2.0-or-later
// H.264 on VideoToolbox for AvPlayer (NPS5_AVPLAYER_VT=1, default off; docs/m3/cpulean.md).
//
// FFmpeg's own VideoToolbox hardware decoder does the work, so the demuxer, the packet queue, the reorder logic,
// flushing on loop/jump and the PTS delivery stay exactly as they are for the software path. The decoder hands out
// AV_PIX_FMT_VIDEOTOOLBOX frames; avplayer_vt_to_system() downloads each into an NV12 system-memory frame (the format
// the frame copy already consumes) and moves it over the original, so the timestamps and crop fields survive.
// Anything that cannot run on the hardware (no device, a profile VideoToolbox rejects, a build of FFmpeg without
// VideoToolbox) leaves the codec on its software decoder: get_format never offers a format the device did not open for,
// and a failed attach returns false before avcodec_open2.
#ifndef NPS5_AVPLAYER_VT_H
#define NPS5_AVPLAYER_VT_H

#include <cstdlib>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

static inline bool avplayer_vt_enabled() {
    static const bool on = [] { const char *t = getenv("NPS5_AVPLAYER_VT"); return t && t[0] && t[0] != '0'; }();
    return on;
}

static enum AVPixelFormat avplayer_vt_get_format(AVCodecContext *, const enum AVPixelFormat *fmts) {
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == AV_PIX_FMT_VIDEOTOOLBOX) return *p;
    // VideoToolbox was dropped from the list (the hardware refused the stream): software decoding
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(*p);
        if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
    }
    return AV_PIX_FMT_NONE;
}

// Call before avcodec_open2. Only H.264 is routed to the hardware. Returns true when the codec context is set up for it.
static inline bool avplayer_vt_attach(AVCodecContext *c) {
    if (!avplayer_vt_enabled() || !c || c->codec_id != AV_CODEC_ID_H264) return false;
    AVBufferRef *dev = nullptr;
    if (av_hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0) < 0 || !dev) return false;
    c->hw_device_ctx = dev;
    c->get_format = avplayer_vt_get_format;
    return true;
}

// Turns a VideoToolbox frame into a system-memory frame in place (timestamps, crop and color fields are kept).
// Frames of any other format are left alone. Returns 0 or a negative AVERROR; on error `f` is unreferenced.
static inline int avplayer_vt_to_system(AVFrame *f, AVFrame *tmp) {
    if (f->format != AV_PIX_FMT_VIDEOTOOLBOX) return 0;
    av_frame_unref(tmp);
    tmp->format = AV_PIX_FMT_NV12;
    int r = av_hwframe_transfer_data(tmp, f, 0);
    if (r >= 0) r = av_frame_copy_props(tmp, f);
    if (r < 0) { av_frame_unref(tmp); av_frame_unref(f); return r; }
    av_frame_unref(f);
    av_frame_move_ref(f, tmp);
    return 0;
}

#endif
