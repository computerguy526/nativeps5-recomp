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

// NPS5_AVPLAYER_VT: unset/0 = software, 1 = hardware decode + download into a system frame (control),
// 2 = hardware decode + map the CVPixelBuffer and copy once into the guest buffer.
// Test hooks re-read the environment on every call (the unit test switches modes inside one process).
static inline int avplayer_vt_mode() {
    const char *t = getenv("NPS5_AVPLAYER_VT");
    if (!t || !t[0] || t[0] == '0') return 0;
    return t[0] == '2' ? 2 : 1;
}
static inline bool avplayer_vt_enabled() { return avplayer_vt_mode() != 0; }

static enum AVPixelFormat avplayer_vt_get_format(AVCodecContext *, const enum AVPixelFormat *fmts) {
    // NPS5_AVPLAYER_VT_FORCE_SW=1 (TEST ONLY): behave as if the hardware refused at get_format time (software format)
    const char *force_sw = getenv("NPS5_AVPLAYER_VT_FORCE_SW");
    const bool refuse = force_sw && force_sw[0] == '1';
    for (const enum AVPixelFormat *p = fmts; !refuse && *p != AV_PIX_FMT_NONE; ++p)
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

// A decoded frame that is not a VideoToolbox frame while the codec was opened for VideoToolbox: ffmpeg's hardware init failed
// inside the decoder (get_format fell back to a software format), and the context, opened with one thread because the
// hardware was expected to do the work, now decodes in software on that single thread. The caller takes the software
// fallback (reopen with frame threads) instead of running on silently.
static inline bool avplayer_vt_sw_frame(bool vt_active, const AVFrame *f) {
    return vt_active && f->format != AV_PIX_FMT_VIDEOTOOLBOX;
}

// Same as avplayer_vt_to_system, but mode 2 maps the CVPixelBuffer (av_hwframe_map READ locks the base address, no download
// allocation or copy); the caller then copies once from the mapped planes. The result replaces `f` in place; a mapping that
// fails falls back to the download. `tmp` is scratch.
static inline int avplayer_vt_to_system(AVFrame *f, AVFrame *tmp);
static inline int avplayer_vt_convert(AVFrame *f, AVFrame *tmp, int mode) {
    if (f->format != AV_PIX_FMT_VIDEOTOOLBOX) return 0;
    if (mode == 2) {
        av_frame_unref(tmp);
        tmp->format = AV_PIX_FMT_NV12;
        tmp->width = f->width;
        tmp->height = f->height;
        int r = av_hwframe_map(tmp, f, AV_HWFRAME_MAP_READ);
        if (r >= 0) r = av_frame_copy_props(tmp, f);
        if (r >= 0 && tmp->format == AV_PIX_FMT_NV12 && tmp->data[0] && tmp->data[1]) {
            av_frame_unref(f); // the mapping holds its own reference to the hardware frame
            av_frame_move_ref(f, tmp);
            return 0;
        }
        av_frame_unref(tmp);
    }
    return avplayer_vt_to_system(f, tmp);
}

// Turns a VideoToolbox frame into a system-memory frame in place (timestamps, crop and color fields are kept).
// Frames of any other format are left alone. Returns 0 or a negative AVERROR; on error `f` is unreferenced.
static inline int avplayer_vt_to_system(AVFrame *f, AVFrame *tmp) {
    if (f->format != AV_PIX_FMT_VIDEOTOOLBOX) return 0;
    av_frame_unref(tmp);
    tmp->format = AV_PIX_FMT_NV12;
    int r = av_hwframe_transfer_data(tmp, f, 0);
    if (r >= 0) r = av_frame_copy_props(tmp, f);
    if (r < 0) {
        av_frame_unref(tmp);
        av_frame_unref(f);
        return r;
    }
    av_frame_unref(f);
    av_frame_move_ref(f, tmp);
    return 0;
}

// Opens `*cc` for `par`: on VideoToolbox when `want_vt` and the hardware accepts it, otherwise on the software decoder
// (`sw_threads` > 1 = frame+slice threads). Frees and replaces `*cc`. *is_vt reports what ended up open. false = nothing open.
static inline bool avplayer_open_video_codec(AVCodecContext **cc, const AVCodec *dec, const AVCodecParameters *par,
                                             bool want_vt, int sw_threads, bool *is_vt) {
    if (*cc) avcodec_free_context(cc);
    *is_vt = false;
    for (int attempt = want_vt ? 0 : 1; attempt < 2; ++attempt) {
        AVCodecContext *c = avcodec_alloc_context3(dec);
        if (!c) return false;
        avcodec_parameters_to_context(c, par);
        const bool vt = attempt == 0 && avplayer_vt_attach(c);
        if (!vt && sw_threads > 1 && sw_threads <= 16) {
            c->thread_count = sw_threads;
            c->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        }
        if (avcodec_open2(c, dec, nullptr) >= 0) {
            *cc = c;
            *is_vt = vt;
            return true;
        }
        avcodec_free_context(&c);
    }
    return false;
}

// Mid-stream failure tracking: 3 errors in a row (decode or frame transfer) while on the hardware = fall back to software
// from the next keyframe. NPS5_AVPLAYER_VT_FAIL_AT=<n> (TEST ONLY) makes every hardware frame from the n-th on report an error.
struct AvplayerVtGuard {
    int consecutive = 0;
    long hw_frames = 0;
    bool inject_error() { // call once per hardware frame
        const char *t = getenv("NPS5_AVPLAYER_VT_FAIL_AT");
        const long at = t && t[0] ? atol(t) : -1;
        return at >= 0 && hw_frames++ >= at;
    }
    void ok() { consecutive = 0; }
    bool failed() { return ++consecutive >= 3; } // true = switch to software now
};

#endif
