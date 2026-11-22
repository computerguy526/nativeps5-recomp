// SPDX-License-Identifier: GPL-2.0-or-later
//
// Small system-library entry points a PS5 title imports at boot (m2-hlesys), each either a real
// (if trivial) implementation or a stub that logs once and returns a documented error. Values
// and error codes follow AnyPS5 (core/libs/prx/{libkernel,libScePad}; GPL-2.0-only) where it has
// evidence for them. (UserService / SystemService / PlayGo / Acm entry points of the title are in
// m2-hlestubs' libSceNpOnlineStubs.)

#include "nps5_diag.h"
#include "nps5_thunk.h"
#include "recomp_runtime.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RET32(ctx, v) do { (ctx)->rax = (uint64_t)(int64_t)(int32_t)(v); SHIM_RETURN(); } while (0)

static void hs_once(const char *name, const char *what) {
  static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
  static char seen[48][64];
  static int n;
  pthread_mutex_lock(&lk);
  for (int i = 0; i < n; i++) {
    if (!strcmp(seen[i], name)) { pthread_mutex_unlock(&lk); return; }
  }
  if (n < 48) { snprintf(seen[n], sizeof(seen[n]), "%s", name); n++; }
  pthread_mutex_unlock(&lk);
  NPS5_LOG(NPS5_S_HLE, NPS5_L_WARN, "%s: %s", name, what);
}

// ---- libkernel -------------------------------------------------------------------------------

// int sceKernelSetPrtAperture(int index, void *addr, size_t len): three apertures; recorded only
// (the AGC path does not map PRT yet).
static struct { uint64_t addr, len; } g_prt[3];
void shim_sceKernelSetPrtAperture(GuestContext *ctx) {
  int idx = (int)ctx->rdi;
  if (idx < 0 || idx >= 3) RET32(ctx, (int32_t)0x80020016);
  g_prt[idx].addr = ctx->rsi;
  g_prt[idx].len = ctx->rdx;
  hs_once("sceKernelSetPrtAperture", "PRT aperture recorded, not mapped");
  RET32(ctx, 0);
}

// int sceKernelIsTrinityMode(void): the host presents a base PS5, not the Pro ("Trinity") mode.
void shim_sceKernelIsTrinityMode(GuestContext *ctx) { RET32(ctx, 0); }

// Coredumps are never generated on the host (same class as libSceNoopStubs' coredump hooks).
void shim_sceCoredumpWriteUserString(GuestContext *ctx) {
  hs_once("sceCoredumpWriteUserString", "no coredump is generated on the host; ignored");
  RET32(ctx, 0);
}
void shim_sceCoredumpSetUserDataType(GuestContext *ctx) {
  hs_once("sceCoredumpSetUserDataType", "no coredump is generated on the host; ignored");
  RET32(ctx, 0);
}

// int pthread_attr_setsolosched_np(pthread_attr_t *attr, int value): the console accepts 0 and
// 16 and answers ENOTSUP (45) for anything else; the value does not change host scheduling.
void shim_pthread_attr_setsolosched_np(GuestContext *ctx) {
  if (!ctx->rdi) RET32(ctx, 22);
  int v = (int)ctx->rsi;
  if (v != 0 && v != 16) RET32(ctx, 45);
  if (v) hs_once("pthread_attr_setsolosched_np", "solo scheduling is not modelled; ignored");
  RET32(ctx, 0);
}

// ---- libScePad (adaptive triggers / vibration mode) --------------------------------------------
// Host fallback for a build without a pad backend (`-gpu off`): there is no controller, so there is nothing to drive.
// With `-gpu anyps5` the four imports below are rebound to AnyPS5's libScePad (shim_aps5_<name>, emitter.EnableAnyPS5Shims),
// which sends the vibration mode, the trigger effects and the state report to the SDL controller (DualSense through HIDAPI,
// rumble-only triggers on Xbox, nothing on a Switch Pro); docs/m3/controller.md.

#define PAD_ERR_INVALID_ARG ((int32_t)0x80920001)

void shim_scePadSetVibrationMode(GuestContext *ctx) {
  int mode = (int)ctx->rsi;
  if (mode <= 0 || mode > 5) RET32(ctx, PAD_ERR_INVALID_ARG);
  hs_once("scePadSetVibrationMode", "mode recorded only; host rumble follows scePadSetVibration");
  RET32(ctx, 0);
}

void shim_scePadGetTriggerEffectState(GuestContext *ctx) {
  void *info = NPS5_G2H(ctx, ctx->rsi);
  if (!info) RET32(ctx, PAD_ERR_INVALID_ARG);
  memset(info, 0, 8); // PadTriggerEffectStateInformation: int32_t state[2], both triggers report "no effect active"
  hs_once("scePadGetTriggerEffectState", "adaptive triggers are not emulated; reports no effect");
  RET32(ctx, 0);
}

void shim_scePadSetTriggerEffect(GuestContext *ctx) {
  if (!ctx->rsi) RET32(ctx, PAD_ERR_INVALID_ARG);
  hs_once("scePadSetTriggerEffect", "adaptive triggers are not emulated; effect ignored");
  RET32(ctx, 0);
}

void shim_scePadIsRemoteController(GuestContext *ctx) {
  uint8_t *remote = (uint8_t *)NPS5_G2H(ctx, ctx->rsi);
  if (!remote) RET32(ctx, PAD_ERR_INVALID_ARG);
  *remote = 0;
  RET32(ctx, 0);
}

// ---- _umtx_op (libkernel, FreeBSD umtx: the futex of the PS5 kernel; Unity PS5 PS5Util.prx) ------------------
// int _umtx_op(void *obj, int op, u_long val, void *uaddr, void *uaddr2) as FreeBSD sys/kern/kern_umtx.c:
//   UMTX_OP_WAIT (2)               sleep while the u_long at obj equals val
//   UMTX_OP_WAIT_UINT (11) and
//   UMTX_OP_WAIT_UINT_PRIVATE (15) the same for a u_int
//   UMTX_OP_WAKE (3) and
//   UMTX_OP_WAKE_PRIVATE (16)      wake up to val waiters on obj
// Timeout of the waits: uaddr == 0 -> uaddr2 is a relative struct timespec (or NULL = forever); otherwise uaddr is
// the size of the struct _umtx_time at uaddr2 {timespec, u_int flags (UMTX_ABSTIME 1), u_int clockid}. A wait returns
// 0 when woken or when the value differs at entry; a timeout is -1/ETIMEDOUT (60). Waits may end spuriously, as on
// FreeBSD (callers re-check their word). The other operations (umtx mutexes, rwlocks, semaphores) answer -1/EINVAL
// and are logged once: none of the titles seen so far uses them.
// Waiters on the host: a hashed bucket (mutex + condition + wake sequence) per guest address; a wake bumps the
// bucket's sequence under its mutex and broadcasts, so a wake between the value check and the sleep is never lost.
#include <errno.h>
#include <time.h>

#define UMTX_BUCKETS 64
typedef struct {
  pthread_mutex_t lk;
  pthread_cond_t cv;
  uint64_t seq;
} UmtxBucket;
static UmtxBucket g_umtx[UMTX_BUCKETS];
static pthread_once_t g_umtx_once = PTHREAD_ONCE_INIT;
static void umtx_init(void) {
  for (int i = 0; i < UMTX_BUCKETS; i++) {
    pthread_mutex_init(&g_umtx[i].lk, NULL);
    pthread_cond_init(&g_umtx[i].cv, NULL);
  }
}
static UmtxBucket *umtx_bucket(uint64_t guest_addr) {
  pthread_once(&g_umtx_once, umtx_init);
  return &g_umtx[(guest_addr >> 2) * 0x9E3779B97F4A7C15ULL >> 58];
}

#define UMTX_ETIMEDOUT 60 // FreeBSD
#define UMTX_EINVAL 22
#define UMTX_EFAULT 14

static void umtx_fail(GuestContext *ctx, int err) {
  set_guest_errno(ctx, err);
  ctx->rax = (uint64_t)(int64_t)-1;
}

// Absolute CLOCK_REALTIME deadline for pthread_cond_timedwait from the umtx timeout arguments; false = no timeout.
static bool umtx_deadline(GuestContext *ctx, uint64_t uaddr, uint64_t uaddr2, struct timespec *out, bool *bad) {
  *bad = false;
  if (!uaddr2) return false;
  const int64_t *ts = (const int64_t *)NPS5_G2H(ctx, uaddr2);
  if (!ts) { *bad = true; return false; }
  int64_t sec = ts[0], nsec = ts[1];
  if (sec < 0 || nsec < 0 || nsec >= 1000000000) { *bad = true; return false; }
  bool abs = false;
  int clockid = 0; // CLOCK_REALTIME
  if (uaddr != 0) { // struct _umtx_time
    if (uaddr < 24) { *bad = true; return false; }
    const uint32_t *f = (const uint32_t *)(ts + 2);
    abs = (f[0] & 1) != 0;
    clockid = (int)f[1];
  }
  struct timespec now_rt;
  clock_gettime(CLOCK_REALTIME, &now_rt);
  if (!abs) {
    out->tv_sec = now_rt.tv_sec + sec;
    out->tv_nsec = now_rt.tv_nsec + nsec;
  } else {
    // FreeBSD clock ids: 0 REALTIME, 4 MONOTONIC (also 11/12 MONOTONIC_PRECISE/FAST). Translate a monotonic
    // deadline into a realtime one.
    struct timespec now_c = now_rt;
    if (clockid != 0) clock_gettime(CLOCK_MONOTONIC, &now_c);
    int64_t dsec = sec - now_c.tv_sec, dnsec = nsec - now_c.tv_nsec;
    out->tv_sec = now_rt.tv_sec + dsec;
    out->tv_nsec = now_rt.tv_nsec + dnsec;
    while (out->tv_nsec < 0) { out->tv_nsec += 1000000000; out->tv_sec--; }
  }
  while (out->tv_nsec >= 1000000000) { out->tv_nsec -= 1000000000; out->tv_sec++; }
  return true;
}

void shim__umtx_op(GuestContext *ctx) {
  const uint64_t obj = ctx->rdi;
  const int op = (int)ctx->rsi;
  const uint64_t val = ctx->rdx, uaddr = ctx->rcx, uaddr2 = ctx->r8;
  switch (op) {
  case 2: case 11: case 15: { // WAIT, WAIT_UINT, WAIT_UINT_PRIVATE
    const bool wide = op == 2;
    const void *w = obj ? NPS5_G2H(ctx, obj) : NULL;
    if (!w) { umtx_fail(ctx, UMTX_EFAULT); SHIM_RETURN(); }
    struct timespec dl;
    bool bad;
    const bool timed = umtx_deadline(ctx, uaddr, uaddr2, &dl, &bad);
    if (bad) { umtx_fail(ctx, UMTX_EINVAL); SHIM_RETURN(); }
    UmtxBucket *b = umtx_bucket(obj);
    pthread_mutex_lock(&b->lk);
    const uint64_t cur = wide ? __atomic_load_n((const uint64_t *)w, __ATOMIC_SEQ_CST)
                              : __atomic_load_n((const uint32_t *)w, __ATOMIC_SEQ_CST);
    const uint64_t want = wide ? val : (uint32_t)val;
    int rc = 0;
    if (cur == want) {
      const uint64_t seq = b->seq;
      while (b->seq == seq && rc == 0) rc = timed ? pthread_cond_timedwait(&b->cv, &b->lk, &dl) : pthread_cond_wait(&b->cv, &b->lk);
    }
    pthread_mutex_unlock(&b->lk);
    if (rc == ETIMEDOUT) { umtx_fail(ctx, UMTX_ETIMEDOUT); SHIM_RETURN(); }
    RET32(ctx, 0);
  }
  case 3: case 16: { // WAKE, WAKE_PRIVATE
    UmtxBucket *b = umtx_bucket(obj);
    pthread_mutex_lock(&b->lk);
    b->seq++;
    pthread_cond_broadcast(&b->cv); // every waiter of the bucket re-checks: waking more than val is allowed
    pthread_mutex_unlock(&b->lk);
    RET32(ctx, 0);
  }
  default: {
    char what[64];
    snprintf(what, sizeof what, "operation %d not implemented: EINVAL", op);
    hs_once("_umtx_op", what);
    umtx_fail(ctx, UMTX_EINVAL);
    SHIM_RETURN();
  }
  }
}
