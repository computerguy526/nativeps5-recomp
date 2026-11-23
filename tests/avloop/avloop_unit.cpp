// Stress test of the AvPlayer HLE loop path without a guest (looping clip: demuxer EOF -> seek -> flush marker -> the
// decoder thread flushes its codec). The loop restart used to flush the codec from the DEMUXER thread while the decoder
// thread was inside send/receive: ffmpeg's frame-thread assert 'fctx->async_lock' aborted the process (and with one
// thread a corrupted buffer pool crashed in avcodec_default_get_buffer2). The test includes ps4_avplayer.cpp, stubs the
// runtime services it calls (guest memory = one mmap), generates a short clip with ffmpeg at test time (no game data),
// plays it with loop on for NPS5_AVLOOP_COUNT passes (default 50), 4 decode threads, fetched-frame hold > 0, polling
// like a 60 Hz game, and fails on a crash, a stall, or frames that go backwards inside a pass.
// Run by tools/avloop_unit.sh (links ffmpeg through pkg-config; skips, exit 0, if ffmpeg/libx264 is missing).
#include "../../pkg/runtime/modules/libSceAvPlayer/ps4_avplayer.cpp"
#include <sys/mman.h>
#include <unistd.h>

static const size_t kMem = 256ull << 20;
static GuestContext g_ctx;
static std::atomic<uint64_t> g_bump{0x10000};
extern "C" {
uint64_t g_recomp_call_model = 0;
recomp_fn_t **g_dispatch_l1[DISPATCH_LEVEL_SIZE];
int nps5_guest_is_ps5(void) { return 1; }
int ps4_vfs_resolve(const char *, char *, size_t) { return -1; } // the path is a host path
GuestContext *recomp_create_thread_context(GuestContext *, uint64_t) { return &g_ctx; }
void recomp_free_thread_context(GuestContext *) {}
RecompThread *recomp_init_system_thread(GuestContext *, const char *) { return nullptr; }
void recomp_cleanup_system_thread(RecompThread *) {}
uint64_t recomp_vm_alloc(GuestContext *, size_t size) {
  uint64_t a = g_bump.fetch_add((size + 0xfff) & ~0xfffull);
  return a + size <= kMem ? a : (uint64_t)-1;
}
int recomp_vm_free(GuestContext *, uint64_t, size_t) { return 0; } // bump allocator; the arena is freed at exit
uintptr_t recomp_dispatch_miss(GuestContext *, uint64_t, uint32_t) { abort(); }
}

int main() {
  const char *cnt = getenv("NPS5_AVLOOP_COUNT");
  const int loops = cnt && atoi(cnt) > 0 ? atoi(cnt) : 50;
  char dir[] = "/tmp/avloop_unit.XXXXXX";
  if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
  char clip[300], cmd[1200];
  snprintf(clip, sizeof clip, "%s/clip.mp4", dir);
  // 24 frames at 60 fps (0.4 s) of 320x180 H.264, a keyframe every 4 frames (frequent flushes with frames in flight)
  snprintf(cmd, sizeof cmd, "ffmpeg -v error -y -f lavfi -i testsrc2=size=320x180:rate=60 -frames:v 24 -an -c:v libx264 -g 4 -pix_fmt yuv420p '%s' >/dev/null 2>&1", clip);
  if (system(cmd) != 0 || access(clip, R_OK) != 0) {
    printf("avloop_unit: SKIP (ffmpeg with libx264 not available)\n");
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir); system(cmd);
    return 0;
  }
  setenv("NPS5_AVPLAYER_DEC_THREADS", "4", 1);
  setenv("NPS5_AVPLAYER_HOLD", "2", 1);
  void *mem = mmap(nullptr, kMem, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (mem == MAP_FAILED) { perror("mmap"); return 1; }
  memset(&g_ctx, 0, sizeof g_ctx);
  g_ctx.mem_base = (uint8_t *)mem;
  g_ctx.mem_size = kMem;

  // the player logs every pass to stderr ("looping back"): count them in a file
  char logp[300]; snprintf(logp, sizeof logp, "%s/stderr.log", dir);
  fflush(stderr);
  if (!freopen(logp, "w", stderr)) { perror("freopen"); return 1; }
  setvbuf(stderr, nullptr, _IOLBF, 0);
  auto passes = [&]() -> int {
    char c[400]; snprintf(c, sizeof c, "grep -c 'looping back' '%s'", logp);
    FILE *f = popen(c, "r"); int n = 0; if (f) { if (fscanf(f, "%d", &n) != 1) n = 0; pclose(f); } return n;
  };

  int fails = 0;
  uint64_t delivered = 0, bad_ts = 0;
  int done = 0;
  {
    AvPlayerInitData init{};
    init.num_output_video_framebuffers = 8;
    init.auto_start = true; // READY -> enable the streams and start, like a title that sets no event callback
    AvPlayer *p = new AvPlayer(&g_ctx, init);
    p->set_looping(true);
    p->set_av_sync_mode(AVPLAYER_AV_SYNC_MODE_NONE);
    if (p->add_source(clip) != 0) { fprintf(stdout, "FAIL add_source\n"); return 1; }
    { // sceAvPlayerGetStreamInfoEx record of the clip (320x180 @ 60, PS5 pitch 256): what the frames will carry
      AvPlayerStreamInfoEx ex{};
      const bool got = p->get_stream_info_ex(0, &ex);
      const AvPlayerVideoEx &v = ex.details.video;
      if (!got || ex.type != AVPLAYER_STREAM_TYPE_VIDEO || v.width != 320 || v.height != 192 || v.pitch != 512 ||
          v.crop_right_offset != 192 || v.crop_bottom_offset != 12 || v.frame_rate != 60.0 || v.luma_bit_depth != 8 || ex.this_size != 104 ||
          ex.duration < 300 || ex.duration > 500 || p->get_stream_info_ex(1, &ex)) {
        printf("FAIL stream info ex: got %d type %u %ux%u pitch %u crop r%u b%u fps %.2f depth %u duration %llu\n", got, ex.type,
               v.width, v.height, v.pitch, v.crop_right_offset, v.crop_bottom_offset, v.frame_rate, v.luma_bit_depth,
               (unsigned long long)ex.duration);
        fails++;
      }
    }
    const auto t0 = std::chrono::steady_clock::now();
    bool started = false;
    int poll = 0;
    for (;;) {
      const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      if (++poll % 30 == 0) done = passes();
      if (done >= loops) break;
      if (el > 20.0 + loops * 0.6) { printf("FAIL stalled: %d passes, %llu frames after %.0f s\n", done, (unsigned long long)delivered, el); fails++; break; }
      if (!started && p->is_playing()) started = true;
      AvPlayerFrameInfo fi{};
      if (started && p->get_video_data(&fi)) {
        delivered++;
        if (fi.timestamp > 1000) bad_ts++; // the clip is 0.4 s long: the clock of a pass restarts at its marker
      }
      usleep(16667);
    }
    printf("avloop_unit: %d passes, %llu frames delivered\n", done, (unsigned long long)delivered);
    if (!fails && delivered < 20) { printf("FAIL too few frames (%llu)\n", (unsigned long long)delivered); fails++; }
    if (bad_ts) { printf("FAIL %llu frames with a timestamp beyond the clip\n", (unsigned long long)bad_ts); fails++; }
    delete p; // stops the workers: must not hang or crash either
  }
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir); system(cmd);
  printf(fails ? "avloop_unit: %d FAILURES\n" : "avloop_unit: all passed\n", fails);
  return fails ? 1 : 0;
}
