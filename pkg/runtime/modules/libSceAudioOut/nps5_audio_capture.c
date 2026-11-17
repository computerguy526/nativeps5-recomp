// Per-port PCM capture writer; see nps5_audio_capture.h.
#include "nps5_audio_capture.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define ACAP_MAX 32

struct nps5_acap {
  FILE *wav, *csv;
  pthread_mutex_t m; // guards the pending buffers and counters below
  float *pcm;        // pending frames (stereo)
  size_t pcm_frames, pcm_cap;
  uint64_t *grains;  // pending (index, first_frame, arrival_us) triples
  size_t grain_n, grain_cap;
  uint64_t total_frames; // appended so far
  uint64_t grain_idx;
  uint64_t written_frames; // writer only
  int failed;
};

static pthread_mutex_t g_reg = PTHREAD_MUTEX_INITIALIZER; // registry + all file I/O
static nps5_acap *g_caps[ACAP_MAX];
static int g_thread_started, g_atexit_set;

const char *nps5_acap_dir(void) {
  const char *d = getenv("NPS5_AUDIO_CAPTURE");
  return (d && *d) ? d : NULL;
}

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static int write_header(FILE *f, uint64_t frames) {
  uint8_t h[44];
  uint64_t bytes = frames * 8;
  if (bytes > 0xFFFFFFF0ULL - 36) bytes = 0xFFFFFFF0ULL - 36;
  memcpy(h, "RIFF", 4); put32(h + 4, (uint32_t)(36 + bytes));
  memcpy(h + 8, "WAVEfmt ", 8); put32(h + 16, 16);
  h[20] = 3; h[21] = 0; /* IEEE float */ h[22] = 2; h[23] = 0;
  put32(h + 24, 48000); put32(h + 28, 48000 * 8);
  h[32] = 8; h[33] = 0; h[34] = 32; h[35] = 0;
  memcpy(h + 36, "data", 4); put32(h + 40, (uint32_t)bytes);
  return fseek(f, 0, SEEK_SET) == 0 && fwrite(h, 1, 44, f) == 44;
}

// g_reg held.
static void drain_locked(nps5_acap *c) {
  pthread_mutex_lock(&c->m);
  float *pcm = c->pcm; size_t pn = c->pcm_frames;
  uint64_t *gr = c->grains; size_t gn = c->grain_n;
  c->pcm = NULL; c->pcm_frames = c->pcm_cap = 0;
  c->grains = NULL; c->grain_n = c->grain_cap = 0;
  pthread_mutex_unlock(&c->m);
  if (!c->failed && (pn || gn || c->written_frames == 0)) {
    if (pn) {
      if (fseek(c->wav, 0, SEEK_END) != 0 || fwrite(pcm, 8, pn, c->wav) != pn) c->failed = 1;
      c->written_frames += pn;
    }
    for (size_t i = 0; i < gn; i++)
      fprintf(c->csv, "%llu,%llu,%llu\n", (unsigned long long)gr[i * 3], (unsigned long long)gr[i * 3 + 1],
              (unsigned long long)gr[i * 3 + 2]);
    if (!write_header(c->wav, c->written_frames)) c->failed = 1;
    fflush(c->wav);
    fflush(c->csv);
  }
  free(pcm);
  free(gr);
}

void nps5_acap_flush_all(void) {
  pthread_mutex_lock(&g_reg);
  for (int i = 0; i < ACAP_MAX; i++)
    if (g_caps[i]) drain_locked(g_caps[i]);
  pthread_mutex_unlock(&g_reg);
}

static void *writer_main(void *arg) {
  (void)arg;
  for (;;) {
    struct timespec ts = {0, 100 * 1000 * 1000};
    nanosleep(&ts, NULL);
    nps5_acap_flush_all();
  }
  return NULL;
}

nps5_acap *nps5_acap_open(const char *dir, int handle, int type) {
  if (!dir || !*dir) return NULL;
  mkdir(dir, 0777);
  char path[1100];
  nps5_acap *c = calloc(1, sizeof *c);
  if (!c) return NULL;
  pthread_mutex_init(&c->m, NULL);
  snprintf(path, sizeof path, "%s/port%d_type%d.wav", dir, handle, type);
  c->wav = fopen(path, "wb");
  snprintf(path, sizeof path, "%s/port%d_type%d.grains.csv", dir, handle, type);
  c->csv = fopen(path, "w");
  if (!c->wav || !c->csv || !write_header(c->wav, 0)) {
    fprintf(stderr, "[AudioOut] NPS5_AUDIO_CAPTURE: cannot write in %s: %s\n", dir, strerror(errno));
    if (c->wav) fclose(c->wav);
    if (c->csv) fclose(c->csv);
    free(c);
    return NULL;
  }
  fprintf(c->csv, "# grain,first_frame,arrival_us\n");
  pthread_mutex_lock(&g_reg);
  int slot = -1;
  for (int i = 0; i < ACAP_MAX; i++)
    if (!g_caps[i]) { slot = i; break; }
  if (slot < 0) {
    pthread_mutex_unlock(&g_reg);
    fclose(c->wav); fclose(c->csv); free(c);
    return NULL;
  }
  g_caps[slot] = c;
  if (!g_atexit_set) { g_atexit_set = 1; atexit(nps5_acap_flush_all); }
  if (!g_thread_started) {
    pthread_t t;
    if (pthread_create(&t, NULL, writer_main, NULL) == 0) { pthread_detach(t); g_thread_started = 1; }
  }
  pthread_mutex_unlock(&g_reg);
  return c;
}

void nps5_acap_append(nps5_acap *c, const float *stereo, uint32_t frames, uint64_t arrival_us) {
  if (!c || !frames) return;
  pthread_mutex_lock(&c->m);
  if (c->pcm_frames + frames > c->pcm_cap) {
    size_t nc = c->pcm_cap ? c->pcm_cap * 2 : 65536;
    while (nc < c->pcm_frames + frames) nc *= 2;
    float *n = realloc(c->pcm, nc * 8);
    if (!n) { pthread_mutex_unlock(&c->m); return; }
    c->pcm = n; c->pcm_cap = nc;
  }
  if (c->grain_n + 1 > c->grain_cap) {
    size_t nc = c->grain_cap ? c->grain_cap * 2 : 1024;
    uint64_t *n = realloc(c->grains, nc * 24);
    if (!n) { pthread_mutex_unlock(&c->m); return; }
    c->grains = n; c->grain_cap = nc;
  }
  memcpy(c->pcm + c->pcm_frames * 2, stereo, (size_t)frames * 8);
  c->pcm_frames += frames;
  c->grains[c->grain_n * 3] = c->grain_idx++;
  c->grains[c->grain_n * 3 + 1] = c->total_frames;
  c->grains[c->grain_n * 3 + 2] = arrival_us;
  c->grain_n++;
  c->total_frames += frames;
  pthread_mutex_unlock(&c->m);
}

void nps5_acap_close(nps5_acap *c) {
  if (!c) return;
  pthread_mutex_lock(&g_reg);
  drain_locked(c);
  for (int i = 0; i < ACAP_MAX; i++)
    if (g_caps[i] == c) g_caps[i] = NULL;
  pthread_mutex_unlock(&g_reg);
  fclose(c->wav);
  fclose(c->csv);
  pthread_mutex_destroy(&c->m);
  free(c);
}
