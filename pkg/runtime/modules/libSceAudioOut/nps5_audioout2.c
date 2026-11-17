// SPDX-License-Identifier: GPL-2.0-or-later
//
// libSceAudioOut2 (the PS5 audio output API) on top of the legacy AudioOut port of this runtime.
//
// An AudioOut2 context owns ports; every port carries PCM for one grain (the context's
// num_grains frames) that the title points at through the data attribute (id 0) before each
// sceAudioOut2ContextPush. A push mixes the ports of the context down to one stereo float grain
// (1/2/6/8/12 channel ports are folded to stereo, s16 and float PCM both accepted) and submits it
// to a private main port of ps4_audioout.c, so CoreAudio output, the headless clock device
// (PS4_AUDIO_NULL / NPS5_AUDIO_NULL), the WAV dump (PS4_AUDIO_DUMP) and the blocking semantics of
// sceAudioOutOutput are shared: a blocking push returns when the device has room for the next
// grain, a non-blocking push on a full queue returns QUEUE_FULL.
//
// Behaviour of attributes, port formats, error codes and the fold gains follows AnyPS5
// (core/libs/prx/libSceAudioOut/src/AudioOut2*.cpp, GPL-2.0-only), reimplemented in C.

#include "nps5_diag.h"
#include "nps5_thunk.h"
#include "ps4_audioout_internal.h"
#include "recomp_runtime.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define AO2_OK 0
#define AO2_ERR_INVALID_ARGUMENT ((int32_t)0x80260502)
#define AO2_ERR_INVALID_HANDLE ((int32_t)0x80260503)
#define AO2_ERR_QUEUE_FULL ((int32_t)0x80260507)
#define AO2_ERR_INVALID_PARAM ((int32_t)0x80268001)
#define AO2_ERR_UNSUPPORTED_FORMAT ((int32_t)0x8026800E)

#define AO2_RATE 48000u
#define AO2_DEFAULT_GRAIN 256u
#define AO2_MAX_CTX 8
#define AO2_MAX_PORTS 128
#define AO2_MAX_CH 12
#define AO2_QUEUED_DATA_MAX 16
#define AO2_ADV_MAX 4
#define AO2_MASTER_GAIN 0.5f
#define AO2_PORT_PAD_SPEAKER 3
#define AO2_PORT_PAD_VIBRATION 6
#define AO2_PADSPK_GAIN 0.355f

typedef struct {
  uint32_t max_ports, max_object_ports, guarantee_object_ports, queue_depth, num_grains, flags;
  uint32_t reserved[10];
} Ao2ContextParam;
typedef struct {
  uint16_t port_type, pad;
  uint32_t data_format, sampling_freq, flags;
  uint64_t user_handle;
  uint32_t reserved[10];
} Ao2PortParam;
typedef struct {
  uint32_t attribute_id;
  int32_t reserved;
  uint64_t value; // guest address
  uint64_t value_size;
} Ao2Attribute;
typedef struct {
  uint16_t output;
  uint8_t num_channels, pad1;
  int16_t volume;
  uint16_t reroute_counter;
  uint32_t flags, pad2;
  uint64_t reserved[6];
} Ao2PortState;
_Static_assert(sizeof(Ao2ContextParam) == 64, "context param");
_Static_assert(sizeof(Ao2PortParam) == 64, "port param");
_Static_assert(sizeof(Ao2Attribute) == 24, "attribute");
_Static_assert(sizeof(Ao2PortState) == 64, "port state");

typedef struct {
  void *pcm; // one grain, malloc'ed
} Ao2Queued;

typedef struct {
  int used;
  int ctx_idx;
  uint16_t type;
  uint32_t data_format, channels;
  int is_int16;
  float volume[AO2_MAX_CH];
  Ao2Queued queued[AO2_QUEUED_DATA_MAX];
  int nqueued;
  uint64_t last_buf, repeat_ptr; // guest addresses
} Ao2Port;

typedef struct {
  int used;
  uint32_t grain, queue_depth, max_ports;
  int out_port;         // legacy AudioOut handle
  uint32_t chunk;       // frames per legacy output
  float *adv[AO2_ADV_MAX];
  int nadv;
  uint64_t pushes, advances;
} Ao2Ctx;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Ao2Ctx g_ctx[AO2_MAX_CTX];
static Ao2Port g_ports[AO2_MAX_PORTS];
static uint64_t g_next_user = 1;

static void ao2_once(const char *what, const char *detail) {
  static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
  static char seen[32][48];
  static int n;
  pthread_mutex_lock(&lk);
  for (int i = 0; i < n; i++) {
    if (!strcmp(seen[i], what)) { pthread_mutex_unlock(&lk); return; }
  }
  if (n < 32) { snprintf(seen[n], sizeof(seen[n]), "%s", what); n++; }
  pthread_mutex_unlock(&lk);
  NPS5_LOG(NPS5_S_AUDIO, NPS5_L_WARN, "audioout2: %s: %s", what, detail);
}

#define RET32(ctx, v) do { (ctx)->rax = (uint64_t)(int64_t)(int32_t)(v); SHIM_RETURN(); } while (0)

// ---- folding to stereo (gains from AnyPS5's STEREO_FOLDS)
#define FG 0.7071f
#define FG2 (FG * FG)
typedef struct { uint32_t ch; float l[AO2_MAX_CH]; float r[AO2_MAX_CH]; } Fold;
static const Fold k_folds[] = {
    {1, {1.0f}, {1.0f}},
    {2, {1.0f, 0.0f}, {0.0f, 1.0f}},
    {6, {1, 0, FG, 0, FG, 0}, {0, 1, FG, 0, 0, FG}},
    {8, {1, 0, FG, 0, FG, 0, FG, 0}, {0, 1, FG, 0, 0, FG, 0, FG}},
    {12, {1, 0, FG, 0, FG, 0, FG, 0, FG, 0, FG2, 0}, {0, 1, FG, 0, 0, FG, 0, FG, 0, FG, 0, FG2}},
};
static const Fold *fold_for(uint32_t ch) {
  for (size_t i = 0; i < sizeof(k_folds) / sizeof(k_folds[0]); i++) if (k_folds[i].ch == ch) return &k_folds[i];
  return NULL;
}

static Ao2Ctx *ctx_from_handle(uint64_t h) {
  if (h == 0 || h > AO2_MAX_CTX || !g_ctx[h - 1].used) return NULL;
  return &g_ctx[h - 1];
}
static Ao2Port *port_from_handle(uint64_t h) {
  if (h == 0 || h > AO2_MAX_PORTS || !g_ports[h - 1].used) return NULL;
  return &g_ports[h - 1];
}

static size_t port_grain_bytes(const Ao2Ctx *c, const Ao2Port *p) {
  return (size_t)c->grain * p->channels * (p->is_int16 ? 2 : 4);
}

// ---- API

void shim_sceAudioOut2Initialize(GuestContext *ctx) { RET32(ctx, AO2_OK); }
void shim_sceAudioOut2InitializeEx(GuestContext *ctx) { RET32(ctx, AO2_OK); }

void shim_sceAudioOut2UserCreate(GuestContext *ctx) {
  uint64_t *out = (uint64_t *)NPS5_G2H(ctx, ctx->rsi);
  if (!out) RET32(ctx, AO2_ERR_INVALID_ARGUMENT);
  pthread_mutex_lock(&g_lock);
  *out = g_next_user++;
  pthread_mutex_unlock(&g_lock);
  RET32(ctx, AO2_OK);
}
void shim_sceAudioOut2UserCreateEx(GuestContext *ctx) {
  // (userId, attributes, handle*): the handle is the third argument
  uint64_t *out = (uint64_t *)NPS5_G2H(ctx, ctx->rdx);
  if (!out) RET32(ctx, AO2_ERR_INVALID_ARGUMENT);
  pthread_mutex_lock(&g_lock);
  *out = g_next_user++;
  pthread_mutex_unlock(&g_lock);
  RET32(ctx, AO2_OK);
}
void shim_sceAudioOut2UserDestroy(GuestContext *ctx) { RET32(ctx, AO2_OK); }

void shim_sceAudioOut2ContextResetParam(GuestContext *ctx) {
  Ao2ContextParam *p = (Ao2ContextParam *)NPS5_G2H(ctx, ctx->rdi);
  if (!p) RET32(ctx, AO2_ERR_INVALID_ARGUMENT);
  memset(p, 0, sizeof(*p));
  p->max_ports = 8;
  p->queue_depth = 1;
  p->num_grains = AO2_DEFAULT_GRAIN;
  RET32(ctx, AO2_OK);
}

void shim_sceAudioOut2ContextQueryMemory(GuestContext *ctx) {
  const Ao2ContextParam *p = (const Ao2ContextParam *)NPS5_G2H(ctx, ctx->rdi);
  uint64_t *size = (uint64_t *)NPS5_G2H(ctx, ctx->rsi);
  if (!p) RET32(ctx, AO2_ERR_INVALID_ARGUMENT);
  if (!size) RET32(ctx, AO2_ERR_INVALID_PARAM);
  *size = 61196u + 2912u * (uint64_t)p->max_ports;
  RET32(ctx, AO2_OK);
}

void shim_sceAudioOut2ContextCreate(GuestContext *ctx) {
  const Ao2ContextParam *p = (const Ao2ContextParam *)NPS5_G2H(ctx, ctx->rdi);
  uint64_t *out = (uint64_t *)NPS5_G2H(ctx, ctx->rcx); // (params, buffer, bufferSize, ctx*)
  if (!p || !out) RET32(ctx, AO2_ERR_INVALID_ARGUMENT);
  uint32_t grain = p->num_grains ? p->num_grains : AO2_DEFAULT_GRAIN;
  uint32_t chunk = grain <= 2048 ? grain : 2048;
  if (grain > 12000 || grain % chunk != 0) RET32(ctx, AO2_ERR_INVALID_ARGUMENT);
  pthread_mutex_lock(&g_lock);
  int idx = -1;
  for (int i = 0; i < AO2_MAX_CTX; i++) if (!g_ctx[i].used) { idx = i; break; }
  if (idx < 0) {
    pthread_mutex_unlock(&g_lock);
    ao2_once("ContextCreate", "more than 8 contexts");
    RET32(ctx, AO2_ERR_INVALID_ARGUMENT);
  }
  Ao2Ctx *c = &g_ctx[idx];
  memset(c, 0, sizeof(*c));
  c->grain = grain;
  c->chunk = chunk;
  c->queue_depth = p->queue_depth ? p->queue_depth : 1;
  c->max_ports = p->max_ports;
  int32_t h = ps4_audioout_port_open(0xFF, PS4_AUDIO_OUT_PORT_MAIN, 0, chunk, AO2_RATE, PS4_AUDIO_OUT_FMT_FLOAT_STEREO);
  if (h <= 0) {
    pthread_mutex_unlock(&g_lock);
    ao2_once("ContextCreate", "cannot open the output port");
    RET32(ctx, AO2_ERR_INVALID_ARGUMENT);
  }
  c->out_port = h;
  c->used = 1;
  pthread_mutex_unlock(&g_lock);
  *out = (uint64_t)idx + 1;
  RET32(ctx, AO2_OK);
}

static void port_release(Ao2Port *p) {
  for (int i = 0; i < p->nqueued; i++) free(p->queued[i].pcm);
  memset(p, 0, sizeof(*p));
}

void shim_sceAudioOut2ContextDestroy(GuestContext *ctx) {
  pthread_mutex_lock(&g_lock);
  Ao2Ctx *c = ctx_from_handle(ctx->rdi);
  if (!c) { pthread_mutex_unlock(&g_lock); RET32(ctx, AO2_ERR_INVALID_HANDLE); }
  int idx = (int)(c - g_ctx);
  for (int i = 0; i < AO2_MAX_PORTS; i++) if (g_ports[i].used && g_ports[i].ctx_idx == idx) port_release(&g_ports[i]);
  for (int i = 0; i < c->nadv; i++) free(c->adv[i]);
  int out = c->out_port;
  memset(c, 0, sizeof(*c));
  pthread_mutex_unlock(&g_lock);
  ps4_audioout_port_close(out);
  RET32(ctx, AO2_OK);
}

void shim_sceAudioOut2ContextSetAttributes(GuestContext *ctx) {
  pthread_mutex_lock(&g_lock);
  Ao2Ctx *c = ctx_from_handle(ctx->rdi);
  pthread_mutex_unlock(&g_lock);
  if (!c) RET32(ctx, AO2_ERR_INVALID_HANDLE);
  if (!ctx->rsi && ctx->rdx) RET32(ctx, AO2_ERR_INVALID_ARGUMENT);
  if (ctx->rdx) ao2_once("ContextSetAttributes", "context attributes are not modelled and are ignored");
  RET32(ctx, AO2_OK);
}

void shim_sceAudioOut2PortCreate(GuestContext *ctx) {
  const Ao2PortParam *pp = (const Ao2PortParam *)NPS5_G2H(ctx, ctx->rsi);
  uint64_t *out = (uint64_t *)NPS5_G2H(ctx, ctx->rdx);
  pthread_mutex_lock(&g_lock);
  Ao2Ctx *c = ctx_from_handle(ctx->rdi);
  if (!c) { pthread_mutex_unlock(&g_lock); RET32(ctx, AO2_ERR_INVALID_HANDLE); }
  if (!pp || !out) { pthread_mutex_unlock(&g_lock); RET32(ctx, AO2_ERR_INVALID_ARGUMENT); }
  uint32_t channels = (pp->data_format >> 8) & 0xF;
  uint32_t stype = pp->data_format & 0x7F;
  const Fold *f = fold_for(channels);
  if (pp->port_type == 0) {
    if ((pp->data_format & 0x80u) != 0 && channels != 8) {
      *out = (uint64_t)-1;
      pthread_mutex_unlock(&g_lock);
      RET32(ctx, AO2_ERR_INVALID_PARAM);
    }
    if (!f || channels == 6 || stype > 1) {
      *out = (uint64_t)-1;
      pthread_mutex_unlock(&g_lock);
      RET32(ctx, AO2_ERR_UNSUPPORTED_FORMAT);
    }
  }
  if (!f || stype > 1) {
    pthread_mutex_unlock(&g_lock);
    ao2_once("PortCreate", "unsupported data format");
    *out = (uint64_t)-1;
    RET32(ctx, AO2_ERR_UNSUPPORTED_FORMAT);
  }
  int idx = -1;
  for (int i = 0; i < AO2_MAX_PORTS; i++) if (!g_ports[i].used) { idx = i; break; }
  if (idx < 0) {
    pthread_mutex_unlock(&g_lock);
    RET32(ctx, AO2_ERR_INVALID_ARGUMENT);
  }
  Ao2Port *p = &g_ports[idx];
  memset(p, 0, sizeof(*p));
  p->used = 1;
  p->ctx_idx = (int)(c - g_ctx);
  p->type = pp->port_type;
  p->data_format = pp->data_format;
  p->channels = channels;
  p->is_int16 = stype == 1;
  for (int i = 0; i < AO2_MAX_CH; i++) p->volume[i] = 1.0f;
  *out = (uint64_t)idx + 1;
  pthread_mutex_unlock(&g_lock);
  if (pp->port_type == AO2_PORT_PAD_VIBRATION) ao2_once("PortCreate(vibration)", "haptic audio is dropped; the host has no controller haptics");
  RET32(ctx, AO2_OK);
}

void shim_sceAudioOut2PortDestroy(GuestContext *ctx) {
  pthread_mutex_lock(&g_lock);
  Ao2Port *p = port_from_handle(ctx->rdi);
  if (!p) { pthread_mutex_unlock(&g_lock); RET32(ctx, AO2_ERR_INVALID_HANDLE); }
  port_release(p);
  pthread_mutex_unlock(&g_lock);
  RET32(ctx, AO2_OK);
}

void shim_sceAudioOut2PortGetState(GuestContext *ctx) {
  Ao2PortState *st = (Ao2PortState *)NPS5_G2H(ctx, ctx->rsi);
  pthread_mutex_lock(&g_lock);
  Ao2Port *p = port_from_handle(ctx->rdi);
  uint32_t ch = p ? p->channels : 0;
  pthread_mutex_unlock(&g_lock);
  if (!p) RET32(ctx, AO2_ERR_INVALID_HANDLE);
  if (!st) RET32(ctx, AO2_ERR_INVALID_ARGUMENT);
  memset(st, 0, sizeof(*st));
  st->output = 1; // main
  st->num_channels = (uint8_t)ch;
  st->volume = 127;
  RET32(ctx, AO2_OK);
}

// attribute ids: 0 = PCM data pointer (value -> 8-byte cell holding the guest pointer),
// 1 = one float gain per channel. Others are logged and ignored.
void shim_sceAudioOut2PortSetAttributes(GuestContext *ctx) {
  const Ao2Attribute *at = (const Ao2Attribute *)NPS5_G2H(ctx, ctx->rsi);
  uint32_t n = (uint32_t)ctx->rdx;
  pthread_mutex_lock(&g_lock);
  Ao2Port *p = port_from_handle(ctx->rdi);
  if (!p) { pthread_mutex_unlock(&g_lock); RET32(ctx, AO2_ERR_INVALID_HANDLE); }
  if (!at && n) { pthread_mutex_unlock(&g_lock); RET32(ctx, AO2_ERR_INVALID_ARGUMENT); }
  Ao2Ctx *c = &g_ctx[p->ctx_idx];
  for (uint32_t i = 0; i < n; i++) {
    const void *val = NPS5_G2H(ctx, at[i].value);
    if (!val) continue;
    if (at[i].attribute_id == 0 && at[i].value_size == 8) {
      uint64_t g;
      memcpy(&g, val, 8);
      size_t bytes = port_grain_bytes(c, p);
      if (g && RECOMP_MEM_OK(ctx) && g < ctx->mem_size && bytes <= ctx->mem_size - g) {
        void *copy = malloc(bytes);
        memcpy(copy, NPS5_G2H(ctx, g), bytes);
        if (p->nqueued == AO2_QUEUED_DATA_MAX) {
          free(p->queued[0].pcm);
          memmove(&p->queued[0], &p->queued[1], sizeof(Ao2Queued) * (AO2_QUEUED_DATA_MAX - 1));
          p->nqueued--;
        }
        p->queued[p->nqueued++].pcm = copy;
        p->repeat_ptr = (p->last_buf == 0) ? g : 0;
        p->last_buf = g;
      }
    } else if (at[i].attribute_id == 1 && p->channels && at[i].value_size == p->channels * 4u) {
      memcpy(p->volume, val, at[i].value_size);
    } else {
      ao2_once("PortSetAttributes", "attribute id not modelled; ignored");
    }
  }
  pthread_mutex_unlock(&g_lock);
  RET32(ctx, AO2_OK);
}

// Mixes the ports of context idx into out (grain frames, stereo float, master gain and clamp
// applied). Lock held. Consumes one queued grain per port.
static void mix_context(GuestContext *ctx, Ao2Ctx *c, float *out) {
  memset(out, 0, (size_t)c->grain * 2 * sizeof(float));
  int idx = (int)(c - g_ctx);
  for (int pi = 0; pi < AO2_MAX_PORTS; pi++) {
    Ao2Port *p = &g_ports[pi];
    if (!p->used || p->ctx_idx != idx || p->channels == 0) continue;
    if (p->type == AO2_PORT_PAD_VIBRATION) {
      if (p->nqueued) { free(p->queued[0].pcm); memmove(&p->queued[0], &p->queued[1], sizeof(Ao2Queued) * (size_t)(p->nqueued - 1)); p->nqueued--; }
      continue;
    }
    const void *data = NULL;
    void *owned = NULL;
    if (p->nqueued) {
      owned = p->queued[0].pcm;
      data = owned;
      memmove(&p->queued[0], &p->queued[1], sizeof(Ao2Queued) * (size_t)(p->nqueued - 1));
      p->nqueued--;
    } else if (p->repeat_ptr) {
      size_t bytes = port_grain_bytes(c, p);
      if (RECOMP_MEM_OK(ctx) && p->repeat_ptr < ctx->mem_size && bytes <= ctx->mem_size - p->repeat_ptr)
        data = NPS5_G2H(ctx, p->repeat_ptr);
    }
    if (!data) continue;
    const Fold *f = fold_for(p->channels);
    float gain = p->type == AO2_PORT_PAD_SPEAKER ? AO2_PADSPK_GAIN : 1.0f;
    for (uint32_t fr = 0; fr < c->grain; fr++) {
      float l = 0, r = 0;
      for (uint32_t k = 0; k < p->channels; k++) {
        size_t i = (size_t)fr * p->channels + k;
        float s = p->is_int16 ? ((const int16_t *)data)[i] / 32768.0f : ((const float *)data)[i];
        s *= p->volume[k];
        l += s * f->l[k];
        r += s * f->r[k];
      }
      out[fr * 2] += l * gain;
      out[fr * 2 + 1] += r * gain;
    }
    free(owned);
  }
  for (uint32_t i = 0; i < c->grain * 2; i++) {
    float v = out[i] * AO2_MASTER_GAIN;
    out[i] = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
  }
}

void shim_sceAudioOut2ContextAdvance(GuestContext *ctx) {
  pthread_mutex_lock(&g_lock);
  Ao2Ctx *c = ctx_from_handle(ctx->rdi);
  if (!c) { pthread_mutex_unlock(&g_lock); RET32(ctx, AO2_ERR_INVALID_HANDLE); }
  if (c->nadv < AO2_ADV_MAX) {
    float *g = (float *)malloc((size_t)c->grain * 2 * sizeof(float));
    mix_context(ctx, c, g);
    c->adv[c->nadv++] = g;
  } else {
    ao2_once("ContextAdvance", "more than 4 advanced grains pending; the newest is dropped");
  }
  c->advances++;
  pthread_mutex_unlock(&g_lock);
  RET32(ctx, AO2_OK);
}

void shim_sceAudioOut2ContextPush(GuestContext *ctx) {
  int blocking = (uint32_t)ctx->rsi != 0;
  pthread_mutex_lock(&g_lock);
  Ao2Ctx *c = ctx_from_handle(ctx->rdi);
  if (!c) { pthread_mutex_unlock(&g_lock); RET32(ctx, AO2_ERR_INVALID_HANDLE); }
  int out = c->out_port;
  uint32_t grain = c->grain, chunk = c->chunk;
  if (!blocking && ps4_audioout_port_writable(out) <= 0) {
    pthread_mutex_unlock(&g_lock);
    RET32(ctx, AO2_ERR_QUEUE_FULL);
  }
  float *buf;
  if (c->nadv) {
    buf = c->adv[0];
    memmove(&c->adv[0], &c->adv[1], sizeof(float *) * (size_t)(c->nadv - 1));
    c->nadv--;
  } else {
    buf = (float *)malloc((size_t)grain * 2 * sizeof(float));
    mix_context(ctx, c, buf);
  }
  c->pushes++;
  pthread_mutex_unlock(&g_lock);
  // The (possibly blocking) device wait happens outside the module lock.
  for (uint32_t off = 0; off < grain; off += chunk) ps4_audioout_port_output(out, buf + (size_t)off * 2);
  free(buf);
  RET32(ctx, AO2_OK);
}

void shim_sceAudioOut2ContextGetQueueLevel(GuestContext *ctx) {
  uint32_t *level = (uint32_t *)NPS5_G2H(ctx, ctx->rsi);
  uint32_t *avail = (uint32_t *)NPS5_G2H(ctx, ctx->rdx);
  pthread_mutex_lock(&g_lock);
  Ao2Ctx *c = ctx_from_handle(ctx->rdi);
  if (!c) { pthread_mutex_unlock(&g_lock); RET32(ctx, AO2_ERR_INVALID_HANDLE); }
  uint32_t depth = c->queue_depth;
  int out = c->out_port;
  pthread_mutex_unlock(&g_lock);
  int32_t w = ps4_audioout_port_writable(out);
  uint32_t free_slots = w < 0 ? 0 : ((uint32_t)w > depth ? depth : (uint32_t)w);
  if (level) *level = depth - free_slots;
  if (avail) *avail = free_slots;
  RET32(ctx, AO2_OK);
}

void shim_sceAudioOut2GetSystemState(GuestContext *ctx) {
  float *loud = (float *)NPS5_G2H(ctx, ctx->rdi);
  if (!loud) RET32(ctx, AO2_ERR_INVALID_ARGUMENT);
  *loud = 0.0f;
  RET32(ctx, AO2_OK);
}

void shim_sceAudioOut2Terminate(GuestContext *ctx) {
  for (int i = 0; i < AO2_MAX_CTX; i++) {
    pthread_mutex_lock(&g_lock);
    int used = g_ctx[i].used;
    pthread_mutex_unlock(&g_lock);
    if (used) {
      uint64_t save_rdi = ctx->rdi, save_rsp = ctx->rsp;
      ctx->rdi = (uint64_t)i + 1;
      shim_sceAudioOut2ContextDestroy(ctx);
      ctx->rdi = save_rdi;
      ctx->rsp = save_rsp;
    }
  }
  RET32(ctx, AO2_OK);
}
