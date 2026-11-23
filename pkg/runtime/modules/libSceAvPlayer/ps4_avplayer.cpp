// SPDX-License-Identifier: GPL-2.0-or-later
#include "ps4_avplayer.h"
#include "ps4_vfs.h"
#include "ps4_threading.h"
#include "recomp_runtime.h"
#include "avplayer_sync_logic.h"
#include <mutex>
#include <set>
class AvPlayer;
static std::mutex g_live_mu;
static std::set<AvPlayer *> g_live_players;
extern "C" int nps5_guest_is_ps5(void); // kernel/memory/ps4_direct_mem.c

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#define AVPLAYER_LOG(fmt, ...) \
    fprintf(stderr, "[libSceAvPlayer] " fmt "\n", ##__VA_ARGS__)

static inline uint32_t align_up(uint32_t val, uint32_t alignment) {
    return (val + alignment - 1) & ~(alignment - 1);
}

static inline void *recomp_guest_to_host(const GuestContext *ctx, uint64_t gaddr) {
    if (!ctx || !RECOMP_MEM_OK(ctx) || gaddr == 0) return nullptr;
    if (gaddr >= (uintptr_t)ctx->mem_base && gaddr < (uintptr_t)ctx->mem_base + ctx->mem_size) {
        return (void *)gaddr;
    }
    if (gaddr < ctx->mem_size) {
        return (void *)(ctx->mem_base + gaddr);
    }
    return nullptr;
}

static inline uint64_t recomp_host_to_guest(const GuestContext *ctx, const void *hptr) {
    if (!ctx || !RECOMP_MEM_OK(ctx) || !hptr) return 0;
    uintptr_t p = (uintptr_t)hptr;
    uintptr_t base = (uintptr_t)ctx->mem_base;
    if (p >= base && p < base + ctx->mem_size) {
        return (uint64_t)(p - base);
    }
    return (uint64_t)p;
}

static uint64_t call_guest_allocate(GuestContext *ctx, uint64_t fn_addr, uint64_t obj_ptr, uint32_t align, uint32_t size) {
    if (!fn_addr) return 0;
    uint64_t saved_rdi = ctx->rdi;
    uint64_t saved_rsi = ctx->rsi;
    uint64_t saved_rdx = ctx->rdx;
    uint64_t saved_rax = ctx->rax;

    ctx->rdi = obj_ptr;
    ctx->rsi = align;
    ctx->rdx = size;
    recomp_call_guest(ctx, fn_addr);
    uint64_t res = ctx->rax;

    ctx->rdi = saved_rdi;
    ctx->rsi = saved_rsi;
    ctx->rdx = saved_rdx;
    ctx->rax = saved_rax;
    return res;
}

static void call_guest_deallocate(GuestContext *ctx, uint64_t fn_addr, uint64_t obj_ptr, uint64_t mem) {
    if (!fn_addr || !mem) return;
    uint64_t saved_rdi = ctx->rdi;
    uint64_t saved_rsi = ctx->rsi;
    uint64_t saved_rax = ctx->rax;

    ctx->rdi = obj_ptr;
    ctx->rsi = mem;
    recomp_call_guest(ctx, fn_addr);

    ctx->rdi = saved_rdi;
    ctx->rsi = saved_rsi;
    ctx->rax = saved_rax;
}

// Row pitch of the NV12 frame buffers handed to the guest.
// PS5 images: a multiple of 256 (the row pitch of a linear R8 texture on the PS5 GPU; AnyPS5 and KytyPS5 use
// the same rule). ASTRO BOT copies pitch * height * 3 / 2 bytes of each frame straight into the memory of a
// linear texture as wide as the STREAM (880 for its tutorial clips): with the old 64-pixel pitch (896) every
// row landed 128 bytes early and the picture was a horizontal smear. The columns between the picture and the
// pitch are reported as crop_right_offset. PS4 images keep the 64-pixel pitch.
// NPS5_AVPLAYER_PITCH_ALIGN=<power of two> overrides (64 = the old layout); NPS5_AVPLAYER_LOG=1 logs the
// stream and the first frames of every source (sizes, pitch, crops, buffer address).
static uint32_t video_pitch_alignment() {
    static const uint32_t value = [] {
        const char *text = getenv("NPS5_AVPLAYER_PITCH_ALIGN");
        const unsigned long v = text ? strtoul(text, nullptr, 0) : 0;
        if (v >= 16 && v <= 4096 && (v & (v - 1)) == 0) return (uint32_t)v;
        return nps5_guest_is_ps5() ? 256u : 64u;
    }();
    return value;
}
static uint32_t video_pitch(uint32_t width) { return align_up(width, video_pitch_alignment()); }
static bool avplayer_verbose() {
    static const bool on = [] { const char *t = getenv("NPS5_AVPLAYER_LOG"); return t && t[0] && t[0] != '0'; }();
    return on;
}

// Frame delivery (see avplayer_sync_logic.h). NPS5_AVPLAYER_SYNC_LEGACY=1 restores the old rule (one frame per
// poll, up to 40 ms early, no skipping, a fetched buffer recycled at the next fetch). NPS5_AVPLAYER_SYNC_TOL_MS
// (default 8 = half a 60 Hz poll) is how early a frame may be shown; NPS5_AVPLAYER_HOLD (default 2, capped to
// half the buffers minus one) is how many fetched frames stay untouched by the decoder (the guest copies a frame by DMA
// AFTER sceAvPlayerGetVideoData returned; recycling the buffer at the next fetch let the decoder overwrite it
// under the copy = flicker; Jetsku/KytyPS5 PR 106). NPS5_AVPLAYER_VTRACE=1 logs every frame + a summary.
static bool avsync_legacy() {
    static const bool on = [] { const char *t = getenv("NPS5_AVPLAYER_SYNC_LEGACY"); return t && t[0] && t[0] != '0'; }();
    return on;
}
static bool avplayer_vtrace() {
    static const bool on = [] { const char *t = getenv("NPS5_AVPLAYER_VTRACE"); return t && t[0] && t[0] != '0'; }();
    return on;
}
static uint64_t avsync_tol_ms() {
    static const uint64_t v = [] { const char *t = getenv("NPS5_AVPLAYER_SYNC_TOL_MS"); return t && t[0] ? (uint64_t)strtoul(t, nullptr, 0) : 8ull; }();
    return v;
}
static size_t avsync_hold_frames() {
    static const size_t v = [] { const char *t = getenv("NPS5_AVPLAYER_HOLD"); return t && t[0] ? (size_t)strtoul(t, nullptr, 0) : 2ul; }();
    return v;
}
static uint64_t mono_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Largest NV12 frame buffer a stream may need; bigger (or empty) dimensions fail the open.
static const uint64_t kMaxVideoFrameBytes = 256ull << 20;
static uint64_t nv12_frame_bytes(uint32_t width, uint32_t height) {
    const uint64_t pitch = video_pitch(width);
    const uint64_t aligned_height = align_up(height, 16);
    return pitch * aligned_height * 3 / 2;
}

// Returns false (nothing delivered) when the frame does not fit the destination buffer.
static bool copy_nv12_data(uint8_t *dst, uint64_t max_size, const AVFrame *src, bool planar_chroma = false) {
    const uint32_t dst_pitch = video_pitch((uint32_t)src->width);
    const uint32_t dst_height = align_up((uint32_t)src->height, 16);
    const uint32_t width = (uint32_t)src->width;
    const uint32_t chroma_rows = ((uint32_t)src->height + 1) / 2;
    const uint32_t chroma_bytes = (width + 1) & ~1u;

    if (width == 0 || src->height <= 0 || (uint32_t)src->height > 0x7fff0000u || width > 0x7fff0000u ||
        nv12_frame_bytes(width, (uint32_t)src->height) > max_size) {
        return false;
    }

    uint8_t *luma_dst = dst;
    uint8_t *chroma_dst = dst + (size_t)dst_pitch * dst_height;

    // Picture rows, each padded to the pitch with its last pixel (a bilinear tap at the right edge then reads
    // picture colour, not old buffer contents).
    for (uint32_t y = 0; y < (uint32_t)src->height; ++y) {
        uint8_t *row = luma_dst + (size_t)y * dst_pitch;
        std::memcpy(row, src->data[0] + (size_t)y * src->linesize[0], width);
        if (dst_pitch > width) std::memset(row + width, row[width - 1], dst_pitch - width);
    }
    for (uint32_t y = 0; y < chroma_rows; ++y) {
        uint8_t *row = chroma_dst + (size_t)y * dst_pitch;
        if (planar_chroma) {
            // yuv420p: interleave U and V (one pass, instead of a 4K swscale conversion per frame)
            const uint8_t *u = src->data[1] + (size_t)y * src->linesize[1];
            const uint8_t *v = src->data[2] + (size_t)y * src->linesize[2];
            for (uint32_t x = 0; x < chroma_bytes / 2; ++x) { row[2 * x] = u[x]; row[2 * x + 1] = v[x]; }
        } else {
            std::memcpy(row, src->data[1] + (size_t)y * src->linesize[1], chroma_bytes);
        }
        for (uint32_t x = chroma_bytes; x + 1 < dst_pitch; x += 2) {
            row[x] = row[chroma_bytes - 2];
            row[x + 1] = row[chroma_bytes - 1];
        }
    }
    // Rows below the picture repeat its last row.
    for (uint32_t y = (uint32_t)src->height; y < dst_height; ++y) {
        std::memcpy(luma_dst + (size_t)y * dst_pitch, luma_dst + (size_t)(src->height - 1) * dst_pitch, dst_pitch);
    }
    for (uint32_t y = chroma_rows; y < dst_height / 2; ++y) {
        std::memcpy(chroma_dst + (size_t)y * dst_pitch, chroma_dst + (size_t)(chroma_rows - 1) * dst_pitch, dst_pitch);
    }
    return true;
}

static AvPlayerStreamType codec_type_to_stream_type(AVMediaType codec_type) {
    switch (codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        return AVPLAYER_STREAM_TYPE_VIDEO;
    case AVMEDIA_TYPE_AUDIO:
        return AVPLAYER_STREAM_TYPE_AUDIO;
    case AVMEDIA_TYPE_SUBTITLE:
        return AVPLAYER_STREAM_TYPE_TIMED_TEXT;
    default:
        return AVPLAYER_STREAM_TYPE_UNKNOWN;
    }
}

static uint64_t timestamp_to_millis(int64_t timestamp, AVRational time_base) {
    if (timestamp == AV_NOPTS_VALUE || timestamp <= 0 || time_base.num <= 0 || time_base.den <= 0) {
        return 0;
    }
    int64_t millis = av_rescale_q(timestamp, time_base, AVRational{1, 1000});
    return millis > 0 ? (uint64_t)millis : 0;
}

static uint64_t frame_timestamp_millis(const AVFrame *frame, AVRational time_base) {
    int64_t ts = frame->best_effort_timestamp;
    if (ts == AV_NOPTS_VALUE) ts = frame->pts;
    if (ts == AV_NOPTS_VALUE) ts = frame->pkt_dts;
    return timestamp_to_millis(ts, time_base);
}

struct GuestBuffer {
    uint64_t guest_addr = 0;
    uint8_t *host_ptr = nullptr;
    uint64_t size = 0;
    bool is_texture = false;
    bool vm_allocated = false;
    uint64_t fetch_ns = 0; // when the guest last fetched this buffer (0 = never); NPS5_AVPLAYER_VTRACE

    GuestBuffer() = default;
    GuestBuffer(uint64_t gaddr, uint8_t *hptr, uint64_t sz, bool tex, bool vm)
        : guest_addr(gaddr), host_ptr(hptr), size(sz), is_texture(tex), vm_allocated(vm) {}

    GuestBuffer(const GuestBuffer &) = delete;
    GuestBuffer &operator=(const GuestBuffer &) = delete;

    GuestBuffer(GuestBuffer &&o) noexcept
        : guest_addr(o.guest_addr), host_ptr(o.host_ptr), size(o.size),
          is_texture(o.is_texture), vm_allocated(o.vm_allocated), fetch_ns(o.fetch_ns) {
        o.guest_addr = 0;
        o.host_ptr = nullptr;
        o.size = 0;
    }

    GuestBuffer &operator=(GuestBuffer &&o) noexcept {
        if (this != &o) {
            guest_addr = o.guest_addr;
            host_ptr = o.host_ptr;
            size = o.size;
            is_texture = o.is_texture;
            vm_allocated = o.vm_allocated;
            fetch_ns = o.fetch_ns;
            o.guest_addr = 0;
            o.host_ptr = nullptr;
            o.size = 0;
        }
        return *this;
    }
};

template <typename T>
class SafeQueue {
public:
    void push(T &&item) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(std::move(item));
        m_cv.notify_one();
    }

    std::optional<T> pop() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty()) return std::nullopt;
        T val = std::move(m_queue.front());
        m_queue.pop_front();
        return val;
    }

    bool try_pop(T &out) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty()) return false;
        out = std::move(m_queue.front());
        m_queue.pop_front();
        return true;
    }

    template <typename Predicate>
    bool pop_if(Predicate pred, T &out) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty()) return false;
        if (!pred(m_queue.front())) return false;
        out = std::move(m_queue.front());
        m_queue.pop_front();
        return true;
    }

    // Deliver the last element whose timestamp is due, move the earlier (stale) ones to `dropped`; false = none due.
    template <typename TsFn>
    bool pop_due_latest(TsFn ts_of, uint64_t now_ms, uint64_t tol_ms, T &out, std::vector<T> &dropped) {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<uint64_t> ts; ts.reserve(m_queue.size());
        for (const T &e : m_queue) ts.push_back(ts_of(e));
        int k = nps5_avsync_pick(ts.data(), ts.size(), now_ms, tol_ms);
        if (k < 0) return false;
        for (int i = 0; i < k; ++i) { dropped.push_back(std::move(m_queue.front())); m_queue.pop_front(); }
        out = std::move(m_queue.front());
        m_queue.pop_front();
        return true;
    }

    bool wait_pop(const std::atomic<bool> &stop_flag, T &out) {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (m_queue.empty() && !stop_flag.load()) {
            m_cv.wait_for(lock, std::chrono::milliseconds(10));
        }
        if (m_queue.empty() || stop_flag.load()) return false;
        out = std::move(m_queue.front());
        m_queue.pop_front();
        return true;
    }

    bool wait_pop_timeout(T &out, uint32_t timeout_ms) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_queue.empty()) {
            m_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms));
        }
        if (m_queue.empty()) return false;
        out = std::move(m_queue.front());
        m_queue.pop_front();
        return true;
    }

    size_t size() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.size();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        while (!m_queue.empty()) m_queue.pop_front();
        m_cv.notify_all();
    }

    void notify_all() {
        m_cv.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<T> m_queue;
};

struct DecodedVideoFrame {
    GuestBuffer buffer;
    AvPlayerFrameInfoEx info{};
    bool looped = false; // the first frame of a new pass of a looping source
};

struct DecodedAudioFrame {
    GuestBuffer buffer;
    AvPlayerFrameInfo info{};
};

struct StreamDesc {
    uint32_t ffmpeg_index;
    AvPlayerStreamInfo info;
};

enum class AvState {
    Unknown,
    Initial,
    AddingSource,
    Ready,
    Play,
    Pause,
    Stop,
    EndOfFile,
    Buffering,
    Error,
};

class AvPlayer {
public:
    AvPlayer(GuestContext *ctx, const AvPlayerInitData &data)
        : m_init_data(data), m_main_ctx(ctx) {
        if (ctx) {
            m_proc_ctx = ctx->process_ctx ? ctx->process_ctx : ctx;
            m_controller_ctx = recomp_create_thread_context(m_proc_ctx, 1024 * 1024);
        }
        if (m_init_data.default_language && ctx) {
            const char *lang = (const char *)recomp_guest_to_host(ctx, m_init_data.default_language);
            if (lang) {
                strncpy(m_default_language, lang, sizeof(m_default_language) - 1);
            }
        }
        m_state.store(AvState::Initial);
        { std::lock_guard<std::mutex> lk(g_live_mu); g_live_players.insert(this); }
        m_controller_stop.store(false);
        m_controller_thread = std::thread([this]() {
            controller_worker();
        });
    }

    ~AvPlayer() {
        { std::lock_guard<std::mutex> lk(g_live_mu); g_live_players.erase(this); }
        stop_playback_internal(m_main_ctx, false);
        m_controller_stop.store(true);
        m_event_queue.notify_all();
        if (m_controller_thread.joinable()) {
            m_controller_thread.join();
        }
        if (m_controller_ctx) {
            recomp_free_thread_context(m_controller_ctx);
            m_controller_ctx = nullptr;
        }
        if (m_warning_guest && m_proc_ctx) recomp_vm_free(m_proc_ctx, m_warning_guest, 64);
    }

    // ue2boot: tear down on the CALLING guest thread's context. The destructor uses m_main_ctx (the context of the
    // thread that called Init); when Close comes from another thread, the guest deallocate callbacks then run on a
    // context that its own thread is using at the same time (PPSA07631: UE closes the logo movie from a task
    // thread -> the init thread's registers were clobbered -> wcslen(0x40000000) SIGBUS). Opt-in:
    // NPS5_AVPLAYER_CLOSE_CALLER_CTX=1.
    void stop_on_caller(GuestContext *ctx) { stop_playback_internal(ctx, false); }

    void post_init(const AvPlayerPostInitData &data) {
        m_post_init_data = data;
    }

    int32_t add_source(const char *path) {
        if (!path || path[0] == '\0') return ORBIS_AVPLAYER_ERROR_INVALID_PARAMS;
        m_state.store(AvState::AddingSource);

        char host_path[1024] = {0};
        const char *open_path = path;
        if (ps4_vfs_resolve(path, host_path, sizeof(host_path)) == 0) {
            open_path = host_path;
        }

        AVFormatContext *context = avformat_alloc_context();
        if (!context) {
            AVPLAYER_LOG("Failed to allocate AVFormatContext for %s", path);
            m_state.store(AvState::Error);
            return ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
        }

        if (avformat_open_input(&context, open_path, nullptr, nullptr) < 0) {
            AVPLAYER_LOG("Failed to open video file %s (open_path: %s)", path, open_path);
            avformat_free_context(context);
            m_state.store(AvState::Error);
            return ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
        }

        if (avformat_find_stream_info(context, nullptr) < 0) {
            AVPLAYER_LOG("Failed to find stream info for %s", path);
            avformat_close_input(&context);
            m_state.store(AvState::Error);
            return ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
        }

        m_fmt_ctx = context;
        m_streams.clear();
        for (uint32_t i = 0; i < m_fmt_ctx->nb_streams; ++i) {
            AVStream *stream = m_fmt_ctx->streams[i];
            auto codec_type = stream->codecpar->codec_type;
            if (codec_type != AVMEDIA_TYPE_VIDEO && codec_type != AVMEDIA_TYPE_AUDIO) {
                continue;
            }
            StreamDesc desc{};
            desc.ffmpeg_index = i;
            desc.info.type = codec_type_to_stream_type(codec_type);
            desc.info.start_time = 0;
            desc.info.duration = timestamp_to_millis(stream->duration, stream->time_base);
            if (desc.info.duration == 0 && m_fmt_ctx->duration > 0) {
                desc.info.duration = (uint64_t)(m_fmt_ctx->duration / (AV_TIME_BASE / 1000));
            }

            AVDictionaryEntry *lang_entry = av_dict_get(stream->metadata, "language", nullptr, 0);
            if (desc.info.type == AVPLAYER_STREAM_TYPE_VIDEO) {
                desc.info.details.video.width = align_up((uint32_t)stream->codecpar->width, 16);
                desc.info.details.video.height = align_up((uint32_t)stream->codecpar->height, 16);
                if (stream->codecpar->height > 0) {
                    desc.info.details.video.aspect_ratio = (float)stream->codecpar->width / (float)stream->codecpar->height;
                }
                if (lang_entry && lang_entry->value) {
                    strncpy(desc.info.details.video.language_code, lang_entry->value, 3);
                }
            } else if (desc.info.type == AVPLAYER_STREAM_TYPE_AUDIO) {
                desc.info.details.audio.channel_count = (uint16_t)stream->codecpar->ch_layout.nb_channels;
                desc.info.details.audio.sample_rate = (uint32_t)stream->codecpar->sample_rate;
                desc.info.details.audio.size = 0;
                if (lang_entry && lang_entry->value) {
                    strncpy(desc.info.details.audio.language_code, lang_entry->value, 3);
                }
            }
            m_streams.push_back(desc);
        }

        m_state.store(AvState::Ready);
        m_event_queue.push(AVPLAYER_EVENT_STATE_READY);
        AVPLAYER_LOG("Source opened successfully: %s, found %zu supported streams", path, m_streams.size());
        return ORBIS_OK;
    }

    int32_t get_stream_count() const {
        return (int32_t)m_streams.size();
    }

    bool get_stream_info(uint32_t stream_index, AvPlayerStreamInfo *info) const {
        if (!info || stream_index >= m_streams.size()) return false;
        *info = m_streams[stream_index].info;
        return true;
    }

    // The Ex record of a stream: the video fields the decoded frames will carry (16-aligned size, pitch, crops of the
    // padding, 8-bit planes, range) and the frame rate (double, frames per second), as AnyPS5's createStreamInfoEx.
    bool get_stream_info_ex(uint32_t stream_index, AvPlayerStreamInfoEx *info) const {
        if (!info || stream_index >= m_streams.size()) return false;
        const StreamDesc &d = m_streams[stream_index];
        memset(info, 0, sizeof(*info));
        info->this_size = sizeof(*info);
        info->type = (uint32_t)d.info.type;
        info->duration = d.info.duration;
        const AVStream *st = m_fmt_ctx ? m_fmt_ctx->streams[d.ffmpeg_index] : nullptr;
        if (d.info.type == AVPLAYER_STREAM_TYPE_VIDEO) {
            AvPlayerVideoEx &v = info->details.video;
            v.width = d.info.details.video.width;
            v.height = d.info.details.video.height;
            v.aspect_ratio = d.info.details.video.aspect_ratio;
            memcpy(v.language_code, d.info.details.video.language_code, 4);
            if (st) {
                const uint32_t w = (uint32_t)st->codecpar->width, h = (uint32_t)st->codecpar->height;
                v.pitch = video_pitch(w);
                v.crop_right_offset = v.pitch - w;
                v.crop_bottom_offset = v.height > h ? v.height - h : 0;
                const AVRational fr = av_guess_frame_rate(m_fmt_ctx, const_cast<AVStream *>(st), nullptr);
                v.frame_rate = fr.num > 0 && fr.den > 0 ? av_q2d(fr) : 0.0; // AnyPS5 fills only this one
                v.video_full_range_flag = st->codecpar->color_range == AVCOL_RANGE_JPEG;
            }
            v.luma_bit_depth = 8;
            v.chroma_bit_depth = 8;
        } else if (d.info.type == AVPLAYER_STREAM_TYPE_AUDIO) {
            AvPlayerAudioEx &a = info->details.audio;
            a.channel_count = d.info.details.audio.channel_count;
            a.sample_rate = d.info.details.audio.sample_rate;
            a.size = d.info.details.audio.size;
            memcpy(a.language_code, d.info.details.audio.language_code, 4);
        }
        return true;
    }

    bool enable_stream(uint32_t stream_index) {
        if (stream_index >= m_streams.size()) return false;
        const auto &s = m_streams[stream_index];
        if (s.info.type == AVPLAYER_STREAM_TYPE_VIDEO) {
            m_video_stream_idx = (int)s.ffmpeg_index;
            AVPLAYER_LOG("Enabled video stream #%u (ffmpeg index %u)", stream_index, s.ffmpeg_index);
        } else if (s.info.type == AVPLAYER_STREAM_TYPE_AUDIO) {
            m_audio_stream_idx = (int)s.ffmpeg_index;
            AVPLAYER_LOG("Enabled audio stream #%u (ffmpeg index %u)", stream_index, s.ffmpeg_index);
        }
        return true;
    }

    bool start_playback(GuestContext *ctx) {
        std::unique_lock<std::mutex> lock(m_state_mutex);
        if (m_video_stream_idx < 0 && m_audio_stream_idx < 0) {
            AVPLAYER_LOG("Cannot start playback: no streams enabled");
            return false;
        }

        if (m_state.load() == AvState::Play || m_state.load() == AvState::Pause) {
            stop_playback_internal(ctx, false);
        }

        uint32_t max_fb = (uint32_t)std::min(std::max(2, m_init_data.num_output_video_framebuffers), 16);

        if (m_video_stream_idx >= 0) {
            AVStream *stream = m_fmt_ctx->streams[m_video_stream_idx];
            const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
            if (!decoder) {
                AVPLAYER_LOG("Could not find video decoder for codec id %d", stream->codecpar->codec_id);
                return false;
            }
            m_video_codec_ctx = avcodec_alloc_context3(decoder);
            avcodec_parameters_to_context(m_video_codec_ctx, stream->codecpar);
            // The intro clip is 3840x2160 H.264 at 60 fps: one decode thread needed 19 ms (p50) .. 28 ms (p90) per frame,
            // so the picture ran behind the clock (up to 1.9 s late) and the game's load made it worse. Frame threading
            // costs a few frames of output delay, which the PTS delivery absorbs. NPS5_AVPLAYER_DEC_THREADS=<n> (1 = old).
            if (!avsync_legacy()) {
                const char *t = getenv("NPS5_AVPLAYER_DEC_THREADS");
                int n = t && t[0] ? atoi(t) : 4;
                if (n > 1 && n <= 16) {
                    m_video_codec_ctx->thread_count = n;
                    m_video_codec_ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
                }
            }
            if (avcodec_open2(m_video_codec_ctx, decoder, nullptr) < 0) {
                AVPLAYER_LOG("Failed to open video codec");
                return false;
            }

            uint32_t pitch = video_pitch((uint32_t)m_video_codec_ctx->width);
            uint32_t height = align_up((uint32_t)m_video_codec_ctx->height, 16);
            const uint32_t vw = (uint32_t)std::max(m_video_codec_ctx->width, 0), vh = (uint32_t)std::max(m_video_codec_ctx->height, 0);
            const uint64_t frame_size64 = (vw && vh && vw <= 0x7fff0000u && vh <= 0x7fff0000u) ? nv12_frame_bytes(vw, vh) : 0;
            if (frame_size64 == 0 || frame_size64 > kMaxVideoFrameBytes) {
                AVPLAYER_LOG("Unusable video dimensions %dx%d", m_video_codec_ctx->width, m_video_codec_ctx->height);
                return false;
            }
            const uint32_t frame_size = (uint32_t)frame_size64;
            m_logged_video_frames = 0;
            if (avplayer_verbose()) {
                AVPLAYER_LOG("video stream: codec %s %dx%d pix_fmt %d -> stream info %ux%u, frame buffers %u x %u bytes (pitch %u, height %u, NV12), ps5 %d",
                             decoder->name, m_video_codec_ctx->width, m_video_codec_ctx->height, (int)m_video_codec_ctx->pix_fmt,
                             align_up((uint32_t)m_video_codec_ctx->width, 16), height, max_fb, frame_size, pitch, height, (int)nps5_guest_is_ps5());
            }

            for (uint32_t i = 0; i < max_fb; ++i) {
                GuestBuffer buf = allocate_buffer(ctx, 0x100, frame_size, true);
                if (!buf.host_ptr) {
                    AVPLAYER_LOG("Failed to allocate video framebuffer %u", i);
                    return false;
                }
                m_free_video_buffers.push(std::move(buf));
            }
        }

        if (m_audio_stream_idx >= 0) {
            AVStream *stream = m_fmt_ctx->streams[m_audio_stream_idx];
            const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
            if (!decoder) {
                AVPLAYER_LOG("Could not find audio decoder for codec id %d", stream->codecpar->codec_id);
                return false;
            }
            m_audio_codec_ctx = avcodec_alloc_context3(decoder);
            avcodec_parameters_to_context(m_audio_codec_ctx, stream->codecpar);
            if (avcodec_open2(m_audio_codec_ctx, decoder, nullptr) < 0) {
                AVPLAYER_LOG("Failed to open audio codec");
                return false;
            }

            uint32_t max_channels = 8;
            uint32_t audio_buf_size = max_channels * sizeof(int32_t) * 1024;
            uint32_t num_audio_bufs = std::max(max_fb * 4, 16u);
            for (uint32_t i = 0; i < num_audio_bufs; ++i) {
                GuestBuffer buf = allocate_buffer(ctx, 0x10, audio_buf_size, false);
                if (!buf.host_ptr) {
                    AVPLAYER_LOG("Failed to allocate audio buffer %u", i);
                    return false;
                }
                m_free_audio_buffers.push(std::move(buf));
            }
        }

        m_is_eof.store(false);
        m_is_paused.store(false);
        m_workers_stop.store(false);
        m_eof_stop_event_sent.store(false);
        m_total_pause_duration = std::chrono::steady_clock::duration(0);
        m_start_time = std::chrono::steady_clock::now();
        m_state.store(AvState::Play);

        m_demuxer_thread = std::thread([this]() { demuxer_worker(); });
        if (m_video_stream_idx >= 0) {
            m_video_decoder_thread = std::thread([this]() { video_decoder_worker(); });
        }
        if (m_audio_stream_idx >= 0) {
            m_audio_decoder_thread = std::thread([this]() { audio_decoder_worker(); });
        }

        m_event_queue.push(AVPLAYER_EVENT_STATE_PLAY);
        AVPLAYER_LOG("Playback started successfully (video_stream=%d, audio_stream=%d)", m_video_stream_idx, m_audio_stream_idx);
        return true;
    }

    bool stop_playback(GuestContext *ctx) {
        std::unique_lock<std::mutex> lock(m_state_mutex);
        m_eof_stop_event_sent.store(true);
        return stop_playback_internal(ctx, true);
    }

    void pause_playback() {
        if (m_state.load() == AvState::Play) {
            m_pause_time = std::chrono::steady_clock::now();
            m_is_paused.store(true);
            m_state.store(AvState::Pause);
            m_event_queue.push(AVPLAYER_EVENT_STATE_PAUSE);
            AVPLAYER_LOG("Playback paused");
        }
    }

    void resume_playback() {
        if (m_state.load() == AvState::Pause) {
            m_total_pause_duration += (std::chrono::steady_clock::now() - m_pause_time);
            m_is_paused.store(false);
            m_state.store(AvState::Play);
            m_event_queue.push(AVPLAYER_EVENT_STATE_PLAY);
            AVPLAYER_LOG("Playback resumed");
        }
    }

    void set_looping(bool loop) {
        m_is_looping.store(loop);
    }

    uint64_t current_time() {
        if (m_state.load() != AvState::Play && m_state.load() != AvState::Pause) {
            return 0;
        }
        auto now = m_is_paused.load() ? m_pause_time : std::chrono::steady_clock::now();
        if (m_audio_stream_idx >= 0 && m_last_audio_time.time_since_epoch().count() > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_audio_time - m_total_pause_duration).count();
            return m_last_audio_ts + (elapsed > 0 ? (uint64_t)elapsed : 0);
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_start_time - m_total_pause_duration).count();
        return m_clock_base_ms.load() + (elapsed > 0 ? (uint64_t)elapsed : 0);
    }

    // sceAvPlayerJumpToTime: honoured while a source plays or is paused (ready/playing/paused on the console).
    bool jump_to(uint64_t ms) {
        const AvState st = m_state.load();
        if (st != AvState::Play && st != AvState::Pause) return false;
        if (m_is_eof.load()) return false; // the demuxer has finished this source (not looping)
        // A paused player's frame buffers all wait in the ready queue and the decoder waits for one: give them back so
        // it reaches the jump marker (frames decoded before the marker are dropped there).
        if (m_video_codec_ctx) m_jump_inflight.store(true);
        DecodedVideoFrame old;
        while (m_ready_video_frames.try_pop(old)) m_free_video_buffers.push(std::move(old.buffer));
        m_jump_request.store((int64_t)ms);
        AVPLAYER_LOG("JumpToTime %llu ms", (unsigned long long)ms);
        return true;
    }

    bool is_playing() { return m_state.load() == AvState::Play && is_active(); }

    bool is_active() {
        AvState st = m_state.load();
        if (st == AvState::Initial || st == AvState::Stop || st == AvState::Error) {
            return false;
        }
        if (!m_is_eof.load()) {
            return true;
        }
        return (m_ready_video_frames.size() > 0 || m_ready_audio_frames.size() > 0 ||
                m_video_packets.size() > 0 || m_audio_packets.size() > 0);
    }

    // Give a fetched buffer back to the decoder. Not before the guest is done with it: the guest copies a frame by
    // DMA after GetVideoData returned, so the last avsync_hold_frames() fetched buffers stay out of the pool.
    void release_fetched_buffer(GuestBuffer &&buf) {
        m_held_video_bufs.push_back(std::move(buf));
        const size_t cap = avsync_legacy() ? 0 : std::min(avsync_hold_frames(), (size_t)(std::max(2, m_init_data.num_output_video_framebuffers) / 2 - 1));
        while (m_held_video_bufs.size() > cap) {
            m_free_video_buffers.push(std::move(m_held_video_bufs.front()));
            m_held_video_bufs.pop_front();
        }
    }

    void log_video_summary() {
        std::lock_guard<std::mutex> lk(m_stats_mutex);
        if (!avplayer_vtrace() || m_vstats.polls == 0) return;
        const Nps5AvSyncStats &s = m_vstats;
        AVPLAYER_LOG("[avvid] SUMMARY mode=%s polls=%llu delivered=%llu dropped=%llu empty_polls=%llu held_early=%llu | deliver_interval_ms mean %.2f sd %.2f min %.1f max %.1f | poll_interval_ms mean %.2f sd %.2f max %.1f | lateness_ms mean %.2f sd %.2f min %.1f max %.1f | decode_lead_ms mean %.1f min %.1f | convert_ms mean %.2f max %.1f | min_reuse_age_ms %.1f reuse<50ms %llu",
                     avsync_legacy() ? "legacy" : "pts", (unsigned long long)s.polls, (unsigned long long)s.delivered, (unsigned long long)s.dropped,
                     (unsigned long long)s.empty_polls, (unsigned long long)s.held_early,
                     s.deliver_interval_ms.mean, s.deliver_interval_ms.stddev(), s.deliver_interval_ms.mn, s.deliver_interval_ms.mx,
                     s.poll_interval_ms.mean, s.poll_interval_ms.stddev(), s.poll_interval_ms.mx,
                     s.lateness_ms.mean, s.lateness_ms.stddev(), s.lateness_ms.mn, s.lateness_ms.mx,
                     s.decode_lead_ms.mean, s.decode_lead_ms.mn, s.convert_ms.mean, s.convert_ms.mx, s.min_reuse_age_ms < 1e299 ? s.min_reuse_age_ms : -1.0,
                     (unsigned long long)s.reuse_under_50ms);
        m_vstats = Nps5AvSyncStats{};
        m_last_poll_ns = m_last_deliver_ns = 0;
    }

    // SCE_AVPLAYER_ERROR_WAR_LOOPING_BACK (0x806A00A1), sent as an AVPLAYER_EVENT_WARNING_ID when the first frame of a
    // new pass reaches the guest (as AnyPS5 Source::present). The Unity PS5 player keeps SetLooping(1) on every clip and
    // takes this warning as the clip's end (VideoPlayer.loopPointReached); timestamps restart at 0 as on the console.
    void note_looped_frame() {
        AVPLAYER_LOG("loop point: WARNING_ID 0x806a00a1 to the game");
        m_warning_codes.push((int32_t)0x806A00A1u);
        m_event_queue.push(AVPLAYER_EVENT_WARNING_ID);
    }

    bool get_video_data_ex(AvPlayerFrameInfoEx *info) {
        if (!info || !is_active()) return false;
        if (m_is_paused.load()) {
            // Paused: nothing, except the first frame after a jump (the picture at the new position).
            if (!m_jump_frame_pending.load()) return false;
            DecodedVideoFrame frame;
            if (!m_ready_video_frames.try_pop(frame)) return false;
            m_jump_frame_pending.store(false);
            if (frame.looped) note_looped_frame();
            if (m_curr_video_buf.has_value()) {
                release_fetched_buffer(std::move(*m_curr_video_buf));
                m_curr_video_buf.reset();
            }
            *info = frame.info;
            frame.buffer.fetch_ns = mono_ns();
            m_curr_video_buf = std::move(frame.buffer);
            return true;
        }

        const uint64_t now_ms = current_time();
        const uint64_t t_ns = mono_ns();
        if (avplayer_vtrace()) {
            std::lock_guard<std::mutex> lk(m_stats_mutex);
            m_vstats.polls++;
            if (m_last_poll_ns) m_vstats.poll_interval_ms.add((double)(t_ns - m_last_poll_ns) / 1e6);
            m_last_poll_ns = t_ns;
        }
        DecodedVideoFrame frame;
        std::vector<DecodedVideoFrame> stale;
        bool ok;
        if (avsync_legacy()) {
            ok = m_ready_video_frames.pop_if([now_ms, this](const DecodedVideoFrame &f) {
                if (m_sync_mode == AVPLAYER_AV_SYNC_MODE_DEFAULT) {
                    return f.info.timestamp <= now_ms + 40;
                }
                return true;
            }, frame);
        } else {
            // Every sync mode: the clock paces the video. The guest of this title sets AV_SYNC_MODE_NONE (no audio stream
            // is played by the AvPlayer), and the old code then handed out one frame per poll as fast as it decoded: the
            // clip ran 14% fast against real time (717 ms ahead after 8 s) or, decode-bound, up to 1.9 s behind.
            ok = m_ready_video_frames.pop_due_latest([](const DecodedVideoFrame &f) { return f.info.timestamp; }, now_ms, avsync_tol_ms(), frame, stale);
        }

        bool looped = ok && frame.looped;
        for (auto &f : stale) {
            looped = looped || f.looped;
            if (avplayer_vtrace()) AVPLAYER_LOG("[avvid] drop pts=%llu clock=%llu", (unsigned long long)f.info.timestamp, (unsigned long long)now_ms);
            f.buffer.fetch_ns = 0;
            m_free_video_buffers.push(std::move(f.buffer));
        }

        if (looped) note_looped_frame();
        if (!ok) {
            if (avplayer_vtrace()) {
                std::lock_guard<std::mutex> lk(m_stats_mutex);
                m_vstats.empty_polls++;
                if (m_ready_video_frames.size() > 0) m_vstats.held_early++;
            }
            return false;
        }

        if (m_curr_video_buf.has_value()) {
            release_fetched_buffer(std::move(*m_curr_video_buf));
            m_curr_video_buf.reset();
        }

        *info = frame.info;
        frame.buffer.fetch_ns = t_ns;
        m_curr_video_buf = std::move(frame.buffer);
        if (avplayer_vtrace()) {
            std::lock_guard<std::mutex> lk(m_stats_mutex);
            m_vstats.delivered++;
            m_vstats.dropped += stale.size();
            if (m_last_deliver_ns) m_vstats.deliver_interval_ms.add((double)(t_ns - m_last_deliver_ns) / 1e6);
            m_last_deliver_ns = t_ns;
            const double late = (double)now_ms - (double)frame.info.timestamp;
            m_vstats.lateness_ms.add(late);
            AVPLAYER_LOG("[avvid] fetch t_ms=%.2f pts=%llu clock=%llu late_ms=%.0f buf=0x%llx dropped=%zu ready=%zu",
                         (double)t_ns / 1e6, (unsigned long long)frame.info.timestamp, (unsigned long long)now_ms, late,
                         (unsigned long long)frame.info.p_data, stale.size(), m_ready_video_frames.size());
        }
        return true;
    }

    bool get_video_data(AvPlayerFrameInfo *info) {
        if (!info) return false;
        AvPlayerFrameInfoEx ex{};
        if (!get_video_data_ex(&ex)) return false;
        info->p_data = ex.p_data;
        info->timestamp = ex.timestamp;
        info->details.video.width = ex.details.video.width;
        info->details.video.height = ex.details.video.height;
        info->details.video.aspect_ratio = ex.details.video.aspect_ratio;
        memcpy(info->details.video.language_code, ex.details.video.language_code, 4);
        return true;
    }

    bool get_audio_data(AvPlayerFrameInfo *info) {
        if (!info || !is_active() || m_is_paused.load()) return false;
        DecodedAudioFrame frame;
        bool ok = false;
        if (m_is_eof.load() && m_audio_packets.size() == 0) {
            ok = m_ready_audio_frames.try_pop(frame);
        } else {
            ok = m_ready_audio_frames.wait_pop_timeout(frame, 5);
        }
        if (!ok) return false;

        if (m_curr_audio_buf.has_value()) {
            m_free_audio_buffers.push(std::move(*m_curr_audio_buf));
            m_curr_audio_buf.reset();
        }

        *info = frame.info;
        m_last_audio_ts = frame.info.timestamp;
        m_last_audio_time = std::chrono::steady_clock::now();
        m_curr_audio_buf = std::move(frame.buffer);
        return true;
    }

    void set_av_sync_mode(AvPlayerAvSyncMode sync_mode) {
        m_sync_mode = sync_mode;
        if (avplayer_vtrace()) AVPLAYER_LOG("[avvid] sceAvPlayerSetAvSyncMode %d", (int)sync_mode);
    }

private:
    GuestBuffer allocate_buffer(GuestContext *ctx, uint32_t align, uint32_t size, bool is_texture) {
        uint64_t gaddr = 0;
        bool is_vm = false;
        if (is_texture && m_init_data.memory_replacement.allocate_texture && ctx) {
            gaddr = call_guest_allocate(ctx, m_init_data.memory_replacement.allocate_texture,
                                        m_init_data.memory_replacement.object_ptr, align, size);
        } else if (!is_texture && m_init_data.memory_replacement.allocate && ctx) {
            gaddr = call_guest_allocate(ctx, m_init_data.memory_replacement.allocate,
                                        m_init_data.memory_replacement.object_ptr, align, size);
        }

        uint8_t *hptr = nullptr;
        if (gaddr && ctx) {
            hptr = (uint8_t *)recomp_guest_to_host(ctx, gaddr);
        }

        if (!hptr && m_proc_ctx) {
            gaddr = recomp_vm_alloc(m_proc_ctx, size);
            if (gaddr != (uint64_t)-1) {
                hptr = (uint8_t *)recomp_guest_to_host(m_proc_ctx, gaddr);
                is_vm = true;
            } else {
                gaddr = 0;
            }
        }

        return GuestBuffer(gaddr, hptr, size, is_texture, is_vm);
    }

    void free_buffer(GuestContext *ctx, GuestBuffer &buf) {
        if (!buf.guest_addr) return;
        if (buf.vm_allocated && m_proc_ctx) {
            recomp_vm_free(m_proc_ctx, buf.guest_addr, buf.size);
        } else if (buf.is_texture && m_init_data.memory_replacement.deallocate_texture && ctx) {
            call_guest_deallocate(ctx, m_init_data.memory_replacement.deallocate_texture,
                                  m_init_data.memory_replacement.object_ptr, buf.guest_addr);
        } else if (!buf.is_texture && m_init_data.memory_replacement.deallocate && ctx) {
            call_guest_deallocate(ctx, m_init_data.memory_replacement.deallocate,
                                  m_init_data.memory_replacement.object_ptr, buf.guest_addr);
        }
        buf.guest_addr = 0;
        buf.host_ptr = nullptr;
    }

    bool stop_playback_internal(GuestContext *ctx, bool emit_event = true) {
        m_workers_stop.store(true);

        m_video_packets.notify_all();
        m_audio_packets.notify_all();
        m_free_video_buffers.notify_all();
        m_free_audio_buffers.notify_all();
        m_ready_video_frames.notify_all();
        m_ready_audio_frames.notify_all();

        if (m_demuxer_thread.joinable()) m_demuxer_thread.join();
        if (m_video_decoder_thread.joinable()) m_video_decoder_thread.join();
        if (m_audio_decoder_thread.joinable()) m_audio_decoder_thread.join();

        while (auto pkt = m_video_packets.pop()) {
            AVPacket *p = *pkt;
            av_packet_free(&p);
        }
        while (auto pkt = m_audio_packets.pop()) {
            AVPacket *p = *pkt;
            av_packet_free(&p);
        }

        while (auto buf = m_free_video_buffers.pop()) {
            free_buffer(ctx, *buf);
        }
        while (auto buf = m_free_audio_buffers.pop()) {
            free_buffer(ctx, *buf);
        }
        while (auto frame = m_ready_video_frames.pop()) {
            free_buffer(ctx, frame->buffer);
        }
        while (auto frame = m_ready_audio_frames.pop()) {
            free_buffer(ctx, frame->buffer);
        }

        log_video_summary();
        while (!m_held_video_bufs.empty()) { free_buffer(ctx, m_held_video_bufs.front()); m_held_video_bufs.pop_front(); }
        if (m_curr_video_buf.has_value()) {
            free_buffer(ctx, *m_curr_video_buf);
            m_curr_video_buf.reset();
        }
        if (m_curr_audio_buf.has_value()) {
            free_buffer(ctx, *m_curr_audio_buf);
            m_curr_audio_buf.reset();
        }

        if (m_video_codec_ctx) {
            avcodec_free_context(&m_video_codec_ctx);
            m_video_codec_ctx = nullptr;
        }
        if (m_audio_codec_ctx) {
            avcodec_free_context(&m_audio_codec_ctx);
            m_audio_codec_ctx = nullptr;
        }
        if (m_sws_ctx) {
            sws_freeContext(m_sws_ctx);
            m_sws_ctx = nullptr;
        }
        if (m_swr_ctx) {
            swr_free(&m_swr_ctx);
            m_swr_ctx = nullptr;
        }

        m_state.store(AvState::Stop);
        if (emit_event) {
            m_event_queue.push(AVPLAYER_EVENT_STATE_STOP);
        }
        return true;
    }

    void demuxer_worker() {
        AVPLAYER_LOG("Demuxer worker started");
        while (!m_workers_stop.load()) {
            const int64_t jump = m_jump_request.exchange(-1); // before the read-ahead limit: a paused player has full queues
            if (jump >= 0) {
                AVPacket *old = nullptr; // packets of the old position are not decoded at all
                while (m_video_packets.try_pop(old)) av_packet_free(&old);
                while (m_audio_packets.try_pop(old)) av_packet_free(&old);
                const int64_t ts = jump * (AV_TIME_BASE / 1000);
                if (avformat_seek_file(m_fmt_ctx, -1, INT64_MIN, ts, ts, 0) < 0) avformat_seek_file(m_fmt_ctx, -1, 0, 0, 0, 0);
                if (m_video_codec_ctx) { AVPacket *mk = av_packet_alloc(); mk->stream_index = -2; mk->pts = jump; m_video_packets.push(std::move(mk)); }
                else { m_clock_base_ms.store((uint64_t)jump); m_start_time = std::chrono::steady_clock::now(); m_total_pause_duration = std::chrono::steady_clock::duration(0); m_pause_time = m_start_time; }
                if (m_audio_codec_ctx) { AVPacket *mk = av_packet_alloc(); mk->stream_index = -2; mk->pts = jump; m_audio_packets.push(std::move(mk)); }
                continue;
            }
            if (m_video_packets.size() > 30 || m_audio_packets.size() > 60) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            AVPacket *pkt = av_packet_alloc();
            int ret = av_read_frame(m_fmt_ctx, pkt);
            if (ret < 0) {
                av_packet_free(&pkt);
                if (m_is_looping.load()) {
                    AVPLAYER_LOG("Demuxer EOF reached, looping back to start");
                    avformat_seek_file(m_fmt_ctx, -1, 0, 0, 0, 0);
                    // A codec context may only be touched by its own decoder thread (a flush from here, while that
                    // thread sat in send/receive, tripped ffmpeg's frame-thread assert 'fctx->async_lock' and
                    // aborted the process): queue a flush marker (stream_index -1) behind the old packets instead.
                    // The video decoder flushes, drops the frames of the old pass and restarts the clock.
                    if (m_video_codec_ctx) { AVPacket *mk = av_packet_alloc(); mk->stream_index = -1; m_video_packets.push(std::move(mk)); }
                    else { m_clock_base_ms.store(0); m_start_time = std::chrono::steady_clock::now(); m_total_pause_duration = std::chrono::steady_clock::duration(0); }
                    if (m_audio_codec_ctx) { AVPacket *mk = av_packet_alloc(); mk->stream_index = -1; m_audio_packets.push(std::move(mk)); }
                    continue;
                }
                AVPLAYER_LOG("Demuxer EOF reached");
                m_is_eof.store(true);
                m_video_packets.notify_all();
                m_audio_packets.notify_all();
                break;
            }

            if (pkt->stream_index == m_video_stream_idx) {
                m_video_packets.push(std::move(pkt));
            } else if (pkt->stream_index == m_audio_stream_idx) {
                m_audio_packets.push(std::move(pkt));
            } else {
                av_packet_free(&pkt);
            }
        }
        AVPLAYER_LOG("Demuxer worker ended");
    }

    void video_decoder_worker() {
        AVPLAYER_LOG("Video decoder worker started");
        AVFrame *raw_frame = av_frame_alloc();
        AVFrame *nv12_frame = av_frame_alloc();

        while (!m_workers_stop.load()) {
            if (m_video_packets.size() == 0 && m_is_eof.load()) {
                break;
            }
            AVPacket *pkt = nullptr;
            if (!m_video_packets.wait_pop(m_workers_stop, pkt) || !pkt) {
                if (m_is_eof.load() && m_video_packets.size() == 0) break;
                continue;
            }

            if (pkt->stream_index == -1 || pkt->stream_index == -2) { // loop (-1) or jump (-2, pts = target ms) marker
                const bool jump = pkt->stream_index == -2;
                const uint64_t target = jump ? (uint64_t)pkt->pts : 0;
                av_packet_free(&pkt);
                avcodec_flush_buffers(m_video_codec_ctx);
                DecodedVideoFrame old;
                while (m_ready_video_frames.try_pop(old)) m_free_video_buffers.push(std::move(old.buffer));
                m_skip_before_ms.store(target);
                m_clock_base_ms.store(target);
                m_start_time = std::chrono::steady_clock::now();
                m_total_pause_duration = std::chrono::steady_clock::duration(0);
                m_pause_time = m_start_time; // paused: the frozen clock reads the target
                if (jump) {
                    m_jump_frame_pending.store(true);
                    m_jump_inflight.store(false);
                } else {
                    m_next_frame_looped = true;
                }
                continue;
            }
            int ret = avcodec_send_packet(m_video_codec_ctx, pkt);
            av_packet_free(&pkt);
            if (ret < 0) continue;

            while (ret >= 0 && !m_workers_stop.load()) {
                ret = avcodec_receive_frame(m_video_codec_ctx, raw_frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) break;

                GuestBuffer buf;
                if (!m_free_video_buffers.wait_pop(m_workers_stop, buf)) break;
                if (avplayer_vtrace() && buf.fetch_ns) {
                    const double age = (double)(mono_ns() - buf.fetch_ns) / 1e6;
                    std::lock_guard<std::mutex> lk(m_stats_mutex);
                    if (age < m_vstats.min_reuse_age_ms) m_vstats.min_reuse_age_ms = age;
                    if (age < 50.0) m_vstats.reuse_under_50ms++;
                }
                buf.fetch_ns = 0;

                uint32_t width = align_up((uint32_t)raw_frame->width, 16);
                uint32_t pitch = video_pitch((uint32_t)raw_frame->width);
                uint32_t height = align_up((uint32_t)raw_frame->height, 16);

                bool copied = false;
                const uint64_t conv_t0 = avplayer_vtrace() ? mono_ns() : 0;
                if (!avsync_legacy() && (raw_frame->format == AV_PIX_FMT_YUV420P || raw_frame->format == AV_PIX_FMT_YUVJ420P) &&
                    raw_frame->data[2] && !raw_frame->crop_left && !raw_frame->crop_top) {
                    copied = copy_nv12_data(buf.host_ptr, buf.size, raw_frame, true);
                } else if (raw_frame->format != AV_PIX_FMT_NV12) {
                    if (!m_sws_ctx) {
                        m_sws_ctx = sws_getContext(raw_frame->width, raw_frame->height, (AVPixelFormat)raw_frame->format,
                                                   raw_frame->width, raw_frame->height, AV_PIX_FMT_NV12,
                                                   SWS_BILINEAR, nullptr, nullptr, nullptr);
                    }
                    nv12_frame->format = AV_PIX_FMT_NV12;
                    nv12_frame->width = raw_frame->width;
                    nv12_frame->height = raw_frame->height;
                    av_frame_get_buffer(nv12_frame, 0);
                    sws_scale(m_sws_ctx, raw_frame->data, raw_frame->linesize, 0, raw_frame->height,
                              nv12_frame->data, nv12_frame->linesize);
                    copied = copy_nv12_data(buf.host_ptr, buf.size, nv12_frame);
                    av_frame_unref(nv12_frame);
                } else {
                    copied = copy_nv12_data(buf.host_ptr, buf.size, raw_frame);
                }
                if (conv_t0) { std::lock_guard<std::mutex> lk(m_stats_mutex); m_vstats.convert_ms.add((double)(mono_ns() - conv_t0) / 1e6); }
                if (!copied) {
                    AVPLAYER_LOG("Dropping video frame %dx%d: it does not fit the frame buffer", raw_frame->width, raw_frame->height);
                    m_free_video_buffers.push(std::move(buf));
                    av_frame_unref(raw_frame);
                    continue;
                }

                AVStream *st = m_fmt_ctx->streams[m_video_stream_idx];
                uint64_t ts = frame_timestamp_millis(raw_frame, st->time_base);
                if (ts < m_skip_before_ms.load()) { // decoded from the key frame before a jump target
                    m_free_video_buffers.push(std::move(buf));
                    av_frame_unref(raw_frame);
                    continue;
                }

                DecodedVideoFrame frame{};
                frame.buffer = std::move(buf);
                frame.info.p_data = frame.buffer.guest_addr;
                frame.info.timestamp = ts;
                frame.looped = m_next_frame_looped;
                m_next_frame_looped = false;
                frame.info.details.video.width = width;
                frame.info.details.video.height = height;
                frame.info.details.video.pitch = pitch;
                // What the buffer holds beyond the picture (the guest shows pitch - left - right by
                // height - top - bottom).
                frame.info.details.video.crop_left_offset = (uint32_t)raw_frame->crop_left;
                frame.info.details.video.crop_right_offset = (uint32_t)raw_frame->crop_right + pitch - (uint32_t)raw_frame->width;
                frame.info.details.video.crop_top_offset = (uint32_t)raw_frame->crop_top;
                frame.info.details.video.crop_bottom_offset = (uint32_t)raw_frame->crop_bottom + height - (uint32_t)raw_frame->height;
                if (avplayer_verbose() && m_logged_video_frames < 3) {
                    m_logged_video_frames++;
                    AVPLAYER_LOG("video frame: guest 0x%llx ts %llu ms, %ux%u pitch %u crop l%u r%u t%u b%u, source %dx%d fmt %d",
                                 (unsigned long long)frame.info.p_data, (unsigned long long)ts, width, height, pitch,
                                 frame.info.details.video.crop_left_offset, frame.info.details.video.crop_right_offset,
                                 frame.info.details.video.crop_top_offset, frame.info.details.video.crop_bottom_offset,
                                 raw_frame->width, raw_frame->height, raw_frame->format);
                }
                frame.info.details.video.aspect_ratio = (raw_frame->height > 0) ? ((float)raw_frame->width / (float)raw_frame->height) : 1.777f;
                frame.info.details.video.luma_bit_depth = 8;
                frame.info.details.video.chroma_bit_depth = 8;

                if (avplayer_vtrace()) {
                    const double lead = (double)ts - (double)current_time();
                    { std::lock_guard<std::mutex> lk(m_stats_mutex); m_vstats.decode_lead_ms.add(lead); }
                    AVPLAYER_LOG("[avvid] decoded t_ms=%.2f pts=%llu lead_ms=%.0f", (double)mono_ns() / 1e6, (unsigned long long)ts, lead);
                }
                if (m_jump_inflight.load()) { // decoded before the jump marker: stale, and a paused guest would never free it
                    m_free_video_buffers.push(std::move(frame.buffer));
                    av_frame_unref(raw_frame);
                    continue;
                }
                m_ready_video_frames.push(std::move(frame));
                av_frame_unref(raw_frame);
            }
        }

        av_frame_free(&raw_frame);
        av_frame_free(&nv12_frame);
        AVPLAYER_LOG("Video decoder worker ended");
    }

    void audio_decoder_worker() {
        AVPLAYER_LOG("Audio decoder worker started");
        AVFrame *raw_frame = av_frame_alloc();

        while (!m_workers_stop.load()) {
            if (m_audio_packets.size() == 0 && m_is_eof.load()) {
                break;
            }
            AVPacket *pkt = nullptr;
            if (!m_audio_packets.wait_pop(m_workers_stop, pkt) || !pkt) {
                if (m_is_eof.load() && m_audio_packets.size() == 0) break;
                continue;
            }

            if (pkt->stream_index == -1 || pkt->stream_index == -2) { av_packet_free(&pkt); avcodec_flush_buffers(m_audio_codec_ctx); continue; }
            int ret = avcodec_send_packet(m_audio_codec_ctx, pkt);
            av_packet_free(&pkt);
            if (ret < 0 && ret != AVERROR(EAGAIN)) {
                AVPLAYER_LOG("avcodec_send_packet error %d", ret);
                continue;
            }

            while (ret >= 0 && !m_workers_stop.load()) {
                ret = avcodec_receive_frame(m_audio_codec_ctx, raw_frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) {
                    AVPLAYER_LOG("avcodec_receive_frame error %d", ret);
                    break;
                }

                GuestBuffer buf;
                if (!m_free_audio_buffers.wait_pop(m_workers_stop, buf)) break;

                const AVFrame *target_frame = raw_frame;
                AVFrame *converted_frame = nullptr;

                if (raw_frame->format != AV_SAMPLE_FMT_S16) {
                    converted_frame = av_frame_alloc();
                    converted_frame->best_effort_timestamp = raw_frame->best_effort_timestamp;
                    converted_frame->pts = raw_frame->pts;
                    converted_frame->pkt_dts = raw_frame->pkt_dts < 0 ? 0 : raw_frame->pkt_dts;
                    converted_frame->format = AV_SAMPLE_FMT_S16;
                    av_channel_layout_copy(&converted_frame->ch_layout, &raw_frame->ch_layout);
                    converted_frame->sample_rate = raw_frame->sample_rate;

                    if (!m_swr_ctx) {
                        SwrContext *swr = nullptr;
                        AVChannelLayout in_layout{};
                        AVChannelLayout out_layout{};
                        av_channel_layout_copy(&in_layout, &raw_frame->ch_layout);
                        av_channel_layout_copy(&out_layout, &raw_frame->ch_layout);
                        swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_S16, raw_frame->sample_rate,
                                            &in_layout, (AVSampleFormat)raw_frame->format, raw_frame->sample_rate,
                                            0, nullptr);
                        av_channel_layout_uninit(&in_layout);
                        av_channel_layout_uninit(&out_layout);
                        m_swr_ctx = swr;
                        swr_init(m_swr_ctx);
                    }

                    int conv_res = swr_convert_frame(m_swr_ctx, converted_frame, raw_frame);
                    if (conv_res < 0) {
                        AVPLAYER_LOG("swr_convert_frame error %d", conv_res);
                        av_frame_free(&converted_frame);
                        m_free_audio_buffers.push(std::move(buf));
                        av_frame_unref(raw_frame);
                        continue;
                    }
                    target_frame = converted_frame;
                }

                uint32_t channels = (uint32_t)target_frame->ch_layout.nb_channels;
                uint32_t bytes = channels * (uint32_t)target_frame->nb_samples * sizeof(int16_t);
                if (bytes <= buf.size && target_frame->data[0]) {
                    memcpy(buf.host_ptr, target_frame->data[0], bytes);
                }

                AVStream *st = m_fmt_ctx->streams[m_audio_stream_idx];
                uint64_t ts = frame_timestamp_millis(raw_frame, st->time_base);

                DecodedAudioFrame frame{};
                frame.buffer = std::move(buf);
                frame.info.p_data = frame.buffer.guest_addr;
                frame.info.timestamp = ts;
                frame.info.details.audio.channel_count = (uint16_t)channels;
                frame.info.details.audio.sample_rate = (uint32_t)target_frame->sample_rate;
                frame.info.details.audio.size = bytes;

                m_ready_audio_frames.push(std::move(frame));

                if (converted_frame) {
                    av_frame_free(&converted_frame);
                }
                av_frame_unref(raw_frame);
            }
        }

        av_frame_free(&raw_frame);
        AVPLAYER_LOG("Audio decoder worker ended");
    }

    void controller_worker() {
        AVPLAYER_LOG("Controller worker started");
        RecompThread *sys_thr = nullptr;
        if (m_controller_ctx) {
            sys_thr = recomp_init_system_thread(m_controller_ctx, "AvController");
        }
        while (!m_controller_stop.load()) {
            AvPlayerEvents ev;
            if (m_event_queue.wait_pop(m_controller_stop, ev)) {
                if (ev == AVPLAYER_EVENT_STATE_READY && (m_init_data.auto_start || !m_init_data.event_replacement.event_callback)) {
                    // AutoPlay logic
                    int v_idx = -1;
                    int a_idx = -1;
                    for (size_t i = 0; i < m_streams.size(); ++i) {
                        if (m_streams[i].info.type == AVPLAYER_STREAM_TYPE_VIDEO && v_idx == -1) {
                            v_idx = (int)i;
                        }
                        if (m_streams[i].info.type == AVPLAYER_STREAM_TYPE_AUDIO && a_idx == -1) {
                            a_idx = (int)i;
                        }
                    }
                    if (v_idx >= 0) enable_stream((uint32_t)v_idx);
                    if (a_idx >= 0) enable_stream((uint32_t)a_idx);
                    start_playback(m_main_ctx);
                }
                uint64_t event_data = 0;
                if (ev == AVPLAYER_EVENT_WARNING_ID) {
                    int32_t code = 0;
                    m_warning_codes.try_pop(code);
                    if (!m_warning_guest && m_proc_ctx) {
                        const uint64_t g = recomp_vm_alloc(m_proc_ctx, 64);
                        m_warning_guest = g != (uint64_t)-1 ? g : 0;
                    }
                    int32_t *slot = m_warning_guest ? (int32_t *)recomp_guest_to_host(m_proc_ctx, m_warning_guest) : nullptr;
                    if (slot) {
                        *slot = code;
                        event_data = m_warning_guest;
                    }
                }
                if (m_init_data.event_replacement.event_callback && m_controller_ctx) {
                    m_controller_ctx->rdi = m_init_data.event_replacement.object_ptr;
                    m_controller_ctx->rsi = (uint64_t)ev;
                    m_controller_ctx->rdx = 0;
                    m_controller_ctx->rcx = event_data; // eventData: the int32 warning code for WARNING_ID
                    recomp_call_guest(m_controller_ctx, m_init_data.event_replacement.event_callback);
                }
            } else {
                if (!m_controller_stop.load() && m_state.load() == AvState::Play && m_is_eof.load()) {
                    if (!is_active() && !m_eof_stop_event_sent.exchange(true)) {
                        m_state.store(AvState::Stop);
                        AVPLAYER_LOG("Natural EOF reached and all frames consumed, emitting STOP event to game");
                        m_event_queue.push(AVPLAYER_EVENT_STATE_STOP);
                    }
                }
            }
        }
        if (sys_thr) {
            recomp_cleanup_system_thread(sys_thr);
        }
        AVPLAYER_LOG("Controller worker ended");
    }

    AvPlayerInitData m_init_data{};
    AvPlayerPostInitData m_post_init_data{};
    GuestContext *m_main_ctx = nullptr;
    GuestContext *m_proc_ctx = nullptr;
    GuestContext *m_controller_ctx = nullptr;
    char m_default_language[8] = {0};

    AVFormatContext *m_fmt_ctx = nullptr;
    std::vector<StreamDesc> m_streams;
    int m_video_stream_idx = -1;
    int m_audio_stream_idx = -1;

    AVCodecContext *m_video_codec_ctx = nullptr;
    AVCodecContext *m_audio_codec_ctx = nullptr;
    SwsContext *m_sws_ctx = nullptr;
    uint32_t m_logged_video_frames = 0; // NPS5_AVPLAYER_LOG: frames logged for the current source
    SwrContext *m_swr_ctx = nullptr;

    SafeQueue<AVPacket *> m_video_packets;
    SafeQueue<AVPacket *> m_audio_packets;
    SafeQueue<GuestBuffer> m_free_video_buffers;
    SafeQueue<GuestBuffer> m_free_audio_buffers;
    SafeQueue<DecodedVideoFrame> m_ready_video_frames;
    SafeQueue<DecodedAudioFrame> m_ready_audio_frames;

    std::optional<GuestBuffer> m_curr_video_buf;
    std::deque<GuestBuffer> m_held_video_bufs; // fetched, not yet given back to the decoder (guest DMA copy window)
    std::mutex m_stats_mutex;
    Nps5AvSyncStats m_vstats;
    uint64_t m_last_poll_ns = 0, m_last_deliver_ns = 0;
    std::optional<GuestBuffer> m_curr_audio_buf;

    std::chrono::steady_clock::time_point m_start_time;
    std::chrono::steady_clock::time_point m_pause_time;
    std::chrono::steady_clock::duration m_total_pause_duration{0};
    uint64_t m_last_audio_ts = 0;
    std::chrono::steady_clock::time_point m_last_audio_time{};

    std::atomic<bool> m_is_paused{false};
    // sceAvPlayerJumpToTime: the demuxer takes m_jump_request (ms, -1 = none), seeks and queues a jump marker
    // (stream_index -2, pts = target); the decoders flush there, the clock restarts at the target, frames before it
    // are dropped, and one frame is handed out even while paused (the guest sees where it jumped to).
    std::atomic<int64_t> m_jump_request{-1};
    std::atomic<uint64_t> m_skip_before_ms{0};
    std::atomic<uint64_t> m_clock_base_ms{0};
    std::atomic<bool> m_jump_frame_pending{false};
    std::atomic<bool> m_jump_inflight{false}; // JumpToTime called, its marker not yet reached by the video decoder
    std::atomic<bool> m_is_looping{false};
    std::atomic<bool> m_is_eof{false};
    std::atomic<bool> m_eof_stop_event_sent{false};
    std::atomic<bool> m_workers_stop{false};
    std::atomic<bool> m_controller_stop{false};
    std::atomic<AvState> m_state{AvState::Initial};
    AvPlayerAvSyncMode m_sync_mode = AVPLAYER_AV_SYNC_MODE_DEFAULT;

    std::mutex m_state_mutex;
    SafeQueue<AvPlayerEvents> m_event_queue;
    // AVPLAYER_EVENT_WARNING_ID payloads (int32 codes), one per queued WARNING_ID event, in order; the controller writes
    // the code to m_warning_guest (guest memory, allocated on first use) and passes its address as eventData.
    SafeQueue<int32_t> m_warning_codes;
    uint64_t m_warning_guest = 0;
    bool m_next_frame_looped = false; // video decoder thread only

    std::thread m_demuxer_thread;
    std::thread m_video_decoder_thread;
    std::thread m_audio_decoder_thread;
    std::thread m_controller_thread;
};

// ==========================================
// Host Shims
// ==========================================

// Number of players currently playing (state Play, not drained): the [framerate] real-time-speed block freezes while > 0.
extern "C" int nps5_avplayer_playing_count(void) {
    std::lock_guard<std::mutex> lk(g_live_mu);
    int n = 0;
    for (AvPlayer *p : g_live_players) n += p->is_playing() ? 1 : 0;
    return n;
}

void shim_sceAvPlayerInit(GuestContext *ctx) {
    if (!ctx->rdi) {
        ctx->rax = 0;
        SHIM_RETURN();
    }
    const AvPlayerInitData *data = (const AvPlayerInitData *)recomp_guest_to_host(ctx, ctx->rdi);
    if (!data) {
        ctx->rax = 0;
        SHIM_RETURN();
    }

    AVPLAYER_LOG("sceAvPlayerInit called (auto_start=%d, num_fb=%d, event_cb=0x%llx)",
                 data->auto_start, data->num_output_video_framebuffers,
                 (unsigned long long)data->event_replacement.event_callback);

    AvPlayer *player = new AvPlayer(ctx, *data);
    ctx->rax = (uint64_t)player;
    SHIM_RETURN();
}

void shim_sceAvPlayerInitEx(GuestContext *ctx) {
    if (!ctx->rdi || !ctx->rsi) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_AVPLAYER_ERROR_INVALID_PARAMS;
        SHIM_RETURN();
    }
    const AvPlayerInitDataEx *p_data = (const AvPlayerInitDataEx *)recomp_guest_to_host(ctx, ctx->rdi);
    uint64_t *out_handle = (uint64_t *)recomp_guest_to_host(ctx, ctx->rsi);
    if (!p_data || !out_handle) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_AVPLAYER_ERROR_INVALID_PARAMS;
        SHIM_RETURN();
    }

    AvPlayerInitData data{};
    data.memory_replacement = p_data->memory_replacement;
    data.file_replacement = p_data->file_replacement;
    data.event_replacement = p_data->event_replacement;
    data.default_language = p_data->default_language;
    data.debug_level = p_data->debug_level;
    data.num_output_video_framebuffers = p_data->num_output_video_framebuffers;
    data.auto_start = p_data->auto_start;

    AvPlayer *player = new AvPlayer(ctx, data);
    *out_handle = (uint64_t)player;
    ctx->rax = 0;
    SHIM_RETURN();
}

void shim_sceAvPlayerPostInit(GuestContext *ctx) {
    AvPlayer *player = (AvPlayer *)ctx->rdi;
    const AvPlayerPostInitData *data = (const AvPlayerPostInitData *)recomp_guest_to_host(ctx, ctx->rsi);
    if (!player || !data) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_AVPLAYER_ERROR_INVALID_PARAMS;
        SHIM_RETURN();
    }
    player->post_init(*data);
    ctx->rax = 0;
    SHIM_RETURN();
}

void shim_sceAvPlayerAddSource(GuestContext *ctx) {
    AvPlayer *player = (AvPlayer *)ctx->rdi;
    const char *path = (const char *)recomp_guest_to_host(ctx, ctx->rsi);
    if (!player || !path) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_AVPLAYER_ERROR_INVALID_PARAMS;
        SHIM_RETURN();
    }
    AVPLAYER_LOG("sceAvPlayerAddSource: %s", path);
    ctx->rax = (uint64_t)(int64_t)player->add_source(path);
    SHIM_RETURN();
}

void shim_sceAvPlayerAddSourceEx(GuestContext *ctx) {
    AvPlayer *player = (AvPlayer *)ctx->rdi;
    AvPlayerUriType uri_type = (AvPlayerUriType)ctx->rsi;
    const AvPlayerSourceDetails *details = (const AvPlayerSourceDetails *)recomp_guest_to_host(ctx, ctx->rdx);
    if (!player || !details || uri_type != AVPLAYER_URI_TYPE_SOURCE) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_AVPLAYER_ERROR_INVALID_PARAMS;
        SHIM_RETURN();
    }
    const char *path = (const char *)recomp_guest_to_host(ctx, details->uri.name);
    if (!path) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_AVPLAYER_ERROR_INVALID_PARAMS;
        SHIM_RETURN();
    }
    ctx->rax = (uint64_t)(int64_t)player->add_source(path);
    SHIM_RETURN();
}

void shim_sceAvPlayerStreamCount(GuestContext *ctx) {
    AvPlayer *player = (AvPlayer *)ctx->rdi;
    if (!player) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_AVPLAYER_ERROR_INVALID_PARAMS;
        SHIM_RETURN();
    }
    ctx->rax = (uint64_t)(int64_t)player->get_stream_count();
    SHIM_RETURN();
}

void shim_sceAvPlayerGetStreamInfo(GuestContext *ctx) {
    AvPlayer *player = (AvPlayer *)ctx->rdi;
    uint32_t stream_id = (uint32_t)ctx->rsi;
    AvPlayerStreamInfo *info = (AvPlayerStreamInfo *)recomp_guest_to_host(ctx, ctx->rdx);
    if (!player || !info) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_AVPLAYER_ERROR_INVALID_PARAMS;
        SHIM_RETURN();
    }
    // The guest's SceAvPlayerStreamInfo is 32 bytes {type, reserved, details[16], duration}: ASTRO BOT
    // keeps it right below its stack protector and AnyPS5 asserts the same size. Our struct has a
    // trailing start_time (shadPS4's PS4 layout); never write it to the guest.
    AvPlayerStreamInfo local;
    if (player->get_stream_info(stream_id, &local)) {
        // Stream type numbers differ by console: PS4 video 0 / audio 1 / timed text 2 (our
        // internal values); a PS5 title sees video 1 / audio 2 (AnyPS5 Player.cpp; ASTRO BOT sizes
        // its YUV buffers from the type-1 stream's width x height).
        if (nps5_guest_is_ps5()) {
            if (local.type == AVPLAYER_STREAM_TYPE_VIDEO) local.type = (AvPlayerStreamType)1;
            else if (local.type == AVPLAYER_STREAM_TYPE_AUDIO) local.type = (AvPlayerStreamType)2;
            else local.type = (AvPlayerStreamType)3;
        }
        memcpy(info, &local, 32);
        ctx->rax = 0;
    } else {
        ctx->rax = (uint64_t)(int64_t)ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
    }
    SHIM_RETURN();
}

// int32_t sceAvPlayerGetStreamInfoEx(SceAvPlayerHandle, uint32_t streamID, SceAvPlayerStreamInfoEx *info): the Unity PS5
// player (VideoPlayer) asks for this record of every stream before it enables one (PPSA28997's intro video).
void shim_sceAvPlayerGetStreamInfoEx(GuestContext *ctx) {
    AvPlayer *player = (AvPlayer *)ctx->rdi;
    uint32_t stream_id = (uint32_t)ctx->rsi;
    AvPlayerStreamInfoEx *info = (AvPlayerStreamInfoEx *)recomp_guest_to_host(ctx, ctx->rdx);
    if (!player || !info) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_AVPLAYER_ERROR_INVALID_PARAMS;
        SHIM_RETURN();
    }
    AvPlayerStreamInfoEx local;
    if (!player->get_stream_info_ex(stream_id, &local)) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
        SHIM_RETURN();
    }
    if (nps5_guest_is_ps5()) { // stream type numbers as in shim_sceAvPlayerGetStreamInfo
        if (local.type == AVPLAYER_STREAM_TYPE_VIDEO) local.type = 1;
        else if (local.type == AVPLAYER_STREAM_TYPE_AUDIO) local.type = 2;
        else local.type = 3;
    }
    memcpy(info, &local, sizeof(local));
    ctx->rax = 0;
    SHIM_RETURN();
}

void shim_sceAvPlayerEnableStream(GuestContext *ctx) {
    AvPlayer *player = (AvPlayer *)ctx->rdi;
    uint32_t stream_id = (uint32_t)ctx->rsi;
    if (!player) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_AVPLAYER_ERROR_INVALID_PARAMS;
        SHIM_RETURN();
    }
    if (player->enable_stream(stream_id)) {
        ctx->rax = 0;
    } else {
        ctx->rax = (uint64_t)(int64_t)ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
    }
    SHIM_RETURN();
}

void shim_sceAvPlayerDisableStream(GuestContext *ctx) {
    ctx->rax = 0;
    SHIM_RETURN();
}

void shim_sceAvPlayerChangeStream(GuestContext *ctx) {
    ctx->rax = 0;
    SHIM_RETURN();
}

void shim_sceAvPlayerStart(GuestContext *ctx) {
    AvPlayer *player = (AvPlayer *)ctx->rdi;
    if (!player) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_AVPLAYER_ERROR_INVALID_PARAMS;
        SHIM_RETURN();
    }
    if (player->start_playback(ctx)) {
        ctx->rax = 0;
    } else {
        ctx->rax = (uint64_t)(int64_t)ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
    }
    SHIM_RETURN();
}

void shim_sceAvPlayerStop(GuestContext *ctx) {
    AvPlayer *player = (AvPlayer *)ctx->rdi;
    if (!player) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_AVPLAYER_ERROR_INVALID_PARAMS;
        SHIM_RETURN();
    }
    player->stop_playback(ctx);
    ctx->rax = 0;
    SHIM_RETURN();
}

void shim_sceAvPlayerPause(GuestContext *ctx) {
    AvPlayer *player = (AvPlayer *)ctx->rdi;
    if (!player) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_AVPLAYER_ERROR_INVALID_PARAMS;
        SHIM_RETURN();
    }
    player->pause_playback();
    ctx->rax = 0;
    SHIM_RETURN();
}

void shim_sceAvPlayerResume(GuestContext *ctx) {
    AvPlayer *player = (AvPlayer *)ctx->rdi;
    if (!player) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_AVPLAYER_ERROR_INVALID_PARAMS;
        SHIM_RETURN();
    }
    player->resume_playback();
    ctx->rax = 0;
    SHIM_RETURN();
}

void shim_sceAvPlayerSetLooping(GuestContext *ctx) {
    AvPlayer *player = (AvPlayer *)ctx->rdi;
    bool loop = (bool)ctx->rsi;
    if (!player) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_AVPLAYER_ERROR_INVALID_PARAMS;
        SHIM_RETURN();
    }
    player->set_looping(loop);
    ctx->rax = 0;
    SHIM_RETURN();
}

void shim_sceAvPlayerCurrentTime(GuestContext *ctx) {
    AvPlayer *player = (AvPlayer *)ctx->rdi;
    if (!player) {
        ctx->rax = 0;
        SHIM_RETURN();
    }
    ctx->rax = player->current_time();
    SHIM_RETURN();
}

void shim_sceAvPlayerIsActive(GuestContext *ctx) {
    AvPlayer *player = (AvPlayer *)ctx->rdi;
    if (!player) {
        ctx->rax = 0;
        SHIM_RETURN();
    }
    ctx->rax = player->is_active() ? 1 : 0;
    SHIM_RETURN();
}

void shim_sceAvPlayerGetAudioData(GuestContext *ctx) {
    AvPlayer *player = (AvPlayer *)ctx->rdi;
    AvPlayerFrameInfo *info = (AvPlayerFrameInfo *)recomp_guest_to_host(ctx, ctx->rsi);
    if (!player || !info) {
        ctx->rax = 0;
        SHIM_RETURN();
    }
    ctx->rax = player->get_audio_data(info) ? 1 : 0;
    SHIM_RETURN();
}

void shim_sceAvPlayerGetVideoData(GuestContext *ctx) {
    AvPlayer *player = (AvPlayer *)ctx->rdi;
    AvPlayerFrameInfo *info = (AvPlayerFrameInfo *)recomp_guest_to_host(ctx, ctx->rsi);
    if (!player || !info) {
        ctx->rax = 0;
        SHIM_RETURN();
    }
    ctx->rax = player->get_video_data(info) ? 1 : 0;
    SHIM_RETURN();
}

void shim_sceAvPlayerGetVideoDataEx(GuestContext *ctx) {
    AvPlayer *player = (AvPlayer *)ctx->rdi;
    AvPlayerFrameInfoEx *info = (AvPlayerFrameInfoEx *)recomp_guest_to_host(ctx, ctx->rsi);
    if (!player || !info) {
        ctx->rax = 0;
        SHIM_RETURN();
    }
    ctx->rax = player->get_video_data_ex(info) ? 1 : 0;
    SHIM_RETURN();
}

void shim_sceAvPlayerClose(GuestContext *ctx) {
    AvPlayer *player = (AvPlayer *)ctx->rdi;
    if (!player) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_AVPLAYER_ERROR_INVALID_PARAMS;
        SHIM_RETURN();
    }
    AVPLAYER_LOG("sceAvPlayerClose called");
    {
        static int caller_ctx = -1;
        if (caller_ctx < 0) { const char *e = getenv("NPS5_AVPLAYER_CLOSE_CALLER_CTX"); caller_ctx = (e && e[0] == '1') ? 1 : 0; }
        if (caller_ctx == 1) player->stop_on_caller(ctx);
    }
    delete player;
    ctx->rax = 0;
    SHIM_RETURN();
}

void shim_sceAvPlayerSetAvSyncMode(GuestContext *ctx) {
    AvPlayer *player = (AvPlayer *)ctx->rdi;
    AvPlayerAvSyncMode mode = (AvPlayerAvSyncMode)ctx->rsi;
    if (!player) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_AVPLAYER_ERROR_INVALID_PARAMS;
        SHIM_RETURN();
    }
    player->set_av_sync_mode(mode);
    ctx->rax = 0;
    SHIM_RETURN();
}

void shim_sceAvPlayerSetLogCallback(GuestContext *ctx) {
    ctx->rax = 0;
    SHIM_RETURN();
}

// int32_t sceAvPlayerJumpToTime(SceAvPlayerHandle, uint64_t jumpTimeMsec): Unity's VideoPlayer prepares a clip with
// Start, Pause, JumpToTime and waits for the frame at the new position (PPSA28997: it closed the clip after 10 s when
// the old 0-returning stub never moved).
void shim_sceAvPlayerJumpToTime(GuestContext *ctx) {
    AvPlayer *player = (AvPlayer *)ctx->rdi;
    if (!player) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_AVPLAYER_ERROR_INVALID_PARAMS;
        SHIM_RETURN();
    }
    ctx->rax = player->jump_to(ctx->rsi) ? 0 : (uint64_t)(int64_t)ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
    SHIM_RETURN();
}

void shim_sceAvPlayerSetTrickSpeed(GuestContext *ctx) {
    ctx->rax = 0;
    SHIM_RETURN();
}

void shim_sceAvPlayerPrintf(GuestContext *ctx) {
    ctx->rax = 0;
    SHIM_RETURN();
}

void shim_sceAvPlayerVprintf(GuestContext *ctx) {
    ctx->rax = 0;
    SHIM_RETURN();
}
