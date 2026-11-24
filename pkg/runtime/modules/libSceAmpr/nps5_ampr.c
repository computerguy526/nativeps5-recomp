// SPDX-License-Identifier: GPL-2.0-or-later
//
// libSceAmpr command buffers and the libkernel APR (asynchronous package reader) service, the
// asynchronous file I/O path of PS5 titles (Unreal Engine's IoStore/pak reader among them).
//
// Model. The guest owns a 0x18-byte sce::Ampr::CommandBuffer object and a memory block it hands
// over with sceAmprCommandBufferSetBuffer. Recording appends fixed-size records to that block; the
// layout of the object is the ABI the title sees (type, recording flags, offset, command count,
// size, base), the record encoding is ours (and is smaller than the host-encoding of AnyPS5, so a
// title that sizes its buffer with sceAmprMeasureCommandSize* or by the console's constants fits).
// sceKernelAprSubmitCommandBuffer* copies the recorded bytes, numbers the submission and hands it
// to one worker thread that executes the records in order: file reads go through the runtime VFS
// into guest memory (pointer translation by NPS5_G2H), WriteKernelEventQueue posts an EVFILT_AMPR
// event. sceKernelAprWaitCommandBuffer blocks until the submission has completed.
//
// Layout and semantics follow AnyPS5 (core/libs/prx/libSceAmpr, libkernel/Apr; GPL-2.0-only),
// reimplemented here in C against this runtime's VFS, equeue and memory model.
//
// Signatures (System V, guest registers), as AnyPS5 / the SDK:
//   int sceAmprCommandBufferConstructor(CommandBuffer *)
//   int sceAmprAprCommandBufferConstructor(CommandBuffer *, u64 *gatherState, u64 *scatterState)
//   int sceAmprAprCommandBufferReadFile(CommandBuffer *, u64 *gather, u64 *scatter, u32 fileId,
//                                       void *dst, u64 size, u64 offset)           (offset on stack)
//   int sceAmprCommandBufferWriteKernelEventQueue_04_00(CommandBuffer *, equeue, i32 ident,
//                                       u64 data, u64 atStart)
//   int sceKernelAprResolveFilepathsToIds[AndFileSizes](const char **paths, u32 n, u32 *ids,
//                                       [u64 *sizes,] u32 *errorIndex)
//   int sceKernelAprSubmitCommandBuffer[AndGetId|AndGetResult](cb, u32 priority, ...)
//   int sceKernelAprWaitCommandBuffer(u32 id)

#include "nps5_diag.h"
#include "nps5_thunk.h"
#include "ps4_equeue.h"
#include "ps4_vfs.h"
#include "nps5_segfile.h"
#include "recomp_runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "nps5_kio.h"

#define AMPR_EPERM ((int32_t)0x80020001)
#define AMPR_EFAULT ((int32_t)0x8002000E)
#define AMPR_EBUSY ((int32_t)0x80020010)
#define AMPR_EINVAL ((int32_t)0x80020016)
#define AMPR_EBADF ((int32_t)0x80020009)

#define AMPR_FILTER (-25) /* EVFILT_AMPR */
#define APR_INVALID_FILE_ID 0xFFFFFFFFu

enum { BUF_GENERIC = 0, BUF_APR = 1 };
enum { REC_READFILE = 1, REC_EQUEUE = 6 };
#define REC_BYTES 24u /* both record kinds */
#define REC_SCATTER_GATHER_VALID 1u
#define REC_HAS_WAIT 4u /* the buffer holds a wait: its submission runs on its own thread */

// The guest-visible object (0x18 bytes).
typedef struct {
  uint16_t type;
  uint16_t recording;
  uint32_t offset;
  uint32_t num_commands;
  uint32_t size;
  uint64_t base; // guest address of the command memory
} AmprCmdBuf;
_Static_assert(sizeof(AmprCmdBuf) == 0x18, "guest reserves 0x18 bytes for sce::Ampr::CommandBuffer");

static void ampr_once(const char *name, const char *what) {
  static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
  static char seen[64][64];
  static int n;
  pthread_mutex_lock(&lk);
  for (int i = 0; i < n; i++) {
    if (!strcmp(seen[i], name)) { pthread_mutex_unlock(&lk); return; }
  }
  if (n < 64) { snprintf(seen[n], sizeof(seen[n]), "%s", name); n++; }
  pthread_mutex_unlock(&lk);
  NPS5_LOG(NPS5_S_HLE, NPS5_L_WARN, "ampr: %s: %s", name, what);
}

#define CB(ctx, g) ((AmprCmdBuf *)NPS5_G2H((ctx), (g)))

static int32_t cb_append_n(GuestContext *ctx, AmprCmdBuf *cb, const void *rec, uint32_t n) {
  if (!cb->base) return AMPR_EPERM;
  if ((uint64_t)cb->offset + n > cb->size) return AMPR_EBUSY;
  if (!RECOMP_MEM_OK(ctx) || cb->base >= ctx->mem_size || cb->size > ctx->mem_size - cb->base) return AMPR_EFAULT;
  memcpy((uint8_t *)NPS5_G2H(ctx, cb->base) + cb->offset, rec, n);
  cb->offset += n;
  cb->num_commands++;
  return 0;
}

static int32_t cb_append(GuestContext *ctx, AmprCmdBuf *cb, const uint32_t w[6]) {
  return cb_append_n(ctx, cb, w, REC_BYTES);
}

#define RET32(ctx, v) do { (ctx)->rax = (uint64_t)(int64_t)(int32_t)(v); SHIM_RETURN(); } while (0)

void shim_sceAmprCommandBufferConstructor(GuestContext *ctx) {
  AmprCmdBuf *cb = CB(ctx, ctx->rdi);
  if (!cb) RET32(ctx, AMPR_EINVAL);
  memset(cb, 0, sizeof(*cb));
  RET32(ctx, 0);
}

void shim_sceAmprCommandBufferDestructor(GuestContext *ctx) {
  RET32(ctx, ctx->rdi ? 0 : AMPR_EINVAL);
}

void shim_sceAmprAprCommandBufferConstructor(GuestContext *ctx) {
  AmprCmdBuf *cb = CB(ctx, ctx->rdi);
  uint64_t *gather = (uint64_t *)NPS5_G2H(ctx, ctx->rsi);
  uint64_t *scatter = (uint64_t *)NPS5_G2H(ctx, ctx->rdx);
  if (!cb || !gather || !scatter) RET32(ctx, AMPR_EINVAL);
  cb->type = BUF_APR;
  *gather = 0;
  *scatter = 0;
  RET32(ctx, 0);
}

void shim_sceAmprAprCommandBufferDestructor(GuestContext *ctx) {
  RET32(ctx, ctx->rdi ? 0 : AMPR_EINVAL);
}

void shim_sceAmprCommandBufferReset(GuestContext *ctx) {
  AmprCmdBuf *cb = CB(ctx, ctx->rdi);
  if (!cb) RET32(ctx, AMPR_EINVAL);
  cb->offset = 0;
  cb->num_commands = 0;
  cb->recording &= (uint16_t)~REC_HAS_WAIT;
  RET32(ctx, 0);
}

void shim_sceAmprCommandBufferSetBuffer(GuestContext *ctx) {
  AmprCmdBuf *cb = CB(ctx, ctx->rdi);
  if (!cb) RET32(ctx, AMPR_EINVAL);
  cb->base = ctx->rsi;
  cb->size = (uint32_t)ctx->rdx;
  cb->recording &= (uint16_t)~REC_HAS_WAIT;
  cb->offset = 0;
  cb->num_commands = 0;
  RET32(ctx, 0);
}

void shim_sceAmprCommandBufferClearBuffer(GuestContext *ctx) {
  AmprCmdBuf *cb = CB(ctx, ctx->rdi);
  uint64_t old = 0;
  if (cb) {
    old = cb->base;
    cb->base = 0;
    cb->size = 0;
    cb->offset = 0;
    cb->num_commands = 0;
  }
  ctx->rax = old; // the guest address of the memory that was bound
  SHIM_RETURN();
}

void shim_sceAmprCommandBufferGetSize(GuestContext *ctx) {
  AmprCmdBuf *cb = CB(ctx, ctx->rdi);
  ctx->rax = cb ? cb->size : 0;
  SHIM_RETURN();
}

void shim_sceAmprCommandBufferGetCurrentOffset(GuestContext *ctx) {
  AmprCmdBuf *cb = CB(ctx, ctx->rdi);
  ctx->rax = cb ? cb->offset : 0;
  SHIM_RETURN();
}

void shim_sceAmprMeasureCommandSizeReadFile(GuestContext *ctx) {
  ctx->rax = REC_BYTES;
  SHIM_RETURN();
}

void shim_sceAmprMeasureCommandSizeWriteKernelEventQueue_04_00(GuestContext *ctx) {
  ctx->rax = REC_BYTES;
  SHIM_RETURN();
}

void shim_sceAmprCommandBufferWriteKernelEventQueue_04_00(GuestContext *ctx) {
  AmprCmdBuf *cb = CB(ctx, ctx->rdi);
  if (!cb) RET32(ctx, AMPR_EINVAL);
  if ((int32_t)ctx->rsi == 0) RET32(ctx, AMPR_EINVAL);
  uint32_t w[6] = {REC_EQUEUE, (uint32_t)ctx->rsi, (uint32_t)ctx->rdx, 0, (uint32_t)ctx->rcx,
                   (uint32_t)(ctx->rcx >> 32)};
  RET32(ctx, cb_append(ctx, cb, w));
}

void shim_sceAmprAprCommandBufferReadFile(GuestContext *ctx) {
  AmprCmdBuf *cb = CB(ctx, ctx->rdi);
  uint32_t file_id = (uint32_t)ctx->rcx;
  uint64_t dst = ctx->r8, size = ctx->r9, off = NPS5_GUEST_STACK_ARG(ctx, 0);
  if (!cb) RET32(ctx, AMPR_EINVAL);
  if (size == 0 || size > 0x100000000ull || off >= 0x10000000000ull || dst >= (1ull << 48)) RET32(ctx, AMPR_EINVAL);
  uint32_t w[6] = {REC_READFILE | (uint32_t)(off >> 32) << 8 | (uint32_t)(dst >> 32) << 16, file_id,
                   (uint32_t)dst, (uint32_t)(size - 1), (uint32_t)off, 0};
  int32_t rc = cb_append(ctx, cb, w);
  if (rc == 0) cb->recording |= REC_SCATTER_GATHER_VALID;
  RET32(ctx, rc);
}

// ---------------------------------------------------------------------------------------------
// APR: file ids, submission queue, worker.

typedef struct { char *path; uint64_t size; } AprFile;
static pthread_mutex_t g_files_lock = PTHREAD_MUTEX_INITIALIZER;
static AprFile *g_files;
static uint32_t g_nfiles, g_capfiles;
static uint32_t *g_htab; // open addressing, stores id+1; 0 = empty
static uint32_t g_htab_cap;

static uint64_t fnv(const char *s) {
  uint64_t h = 1469598103934665603ull;
  while (*s) { h ^= (uint8_t)*s++; h *= 1099511628211ull; }
  return h;
}

static void htab_insert(uint32_t id) {
  uint32_t i = (uint32_t)(fnv(g_files[id].path) & (g_htab_cap - 1));
  while (g_htab[i]) i = (i + 1) & (g_htab_cap - 1);
  g_htab[i] = id + 1;
}

// Finds or registers a host path. Lock held by caller.
static uint32_t file_intern(const char *host, uint64_t size) {
  if (g_htab_cap) {
    uint32_t i = (uint32_t)(fnv(host) & (g_htab_cap - 1));
    while (g_htab[i]) {
      if (!strcmp(g_files[g_htab[i] - 1].path, host)) return g_htab[i] - 1;
      i = (i + 1) & (g_htab_cap - 1);
    }
  }
  if (g_nfiles == g_capfiles) {
    g_capfiles = g_capfiles ? g_capfiles * 2 : 1024;
    g_files = (AprFile *)realloc(g_files, g_capfiles * sizeof(AprFile));
  }
  if ((g_nfiles + 1) * 2 > g_htab_cap) {
    g_htab_cap = g_htab_cap ? g_htab_cap * 2 : 4096;
    free(g_htab);
    g_htab = (uint32_t *)calloc(g_htab_cap, sizeof(uint32_t));
    for (uint32_t k = 0; k < g_nfiles; k++) htab_insert(k);
  }
  g_files[g_nfiles].path = strdup(host);
  g_files[g_nfiles].size = size;
  htab_insert(g_nfiles);
  return g_nfiles++;
}

static int apr_resolve(const char *guest, uint32_t *id, uint64_t *size) {
  char host[1024];
  struct stat st;
  if (!guest || ps4_vfs_resolve(guest, host, sizeof(host)) != 0) return 0;
  if (stat(host, &st) != 0) { // gamepack segmented file (<host>.nps5seg/): read through nps5_segfile in apr_read_file
      nps5_segfile *sf = nps5_segfile_lookup(host);
      if (!sf) return 0;
      st.st_mode = S_IFREG;
      st.st_size = (off_t)nps5_segfile_size(sf);
  }
  if (!S_ISREG(st.st_mode)) return 0;
  pthread_mutex_lock(&g_files_lock);
  uint32_t k = file_intern(host, (uint64_t)st.st_size);
  pthread_mutex_unlock(&g_files_lock);
  if (id) *id = k;
  if (size) *size = (uint64_t)st.st_size;
  return 1;
}

typedef struct AprJob {
  uint32_t id;
  int async;
  uint8_t *rec;
  uint32_t bytes;
  struct AprJob *next;
} AprJob;

static pthread_mutex_t g_q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_q_cv = PTHREAD_COND_INITIALIZER;   // work available
static pthread_cond_t g_done_cv = PTHREAD_COND_INITIALIZER; // a job completed
static AprJob *g_q_head, *g_q_tail;
static uint32_t g_issued;
static pthread_t g_worker;
static int g_worker_started;
// Outstanding (submitted, not completed) ids: small unsorted array.
static uint32_t *g_out_ids;
static uint32_t g_out_n, g_out_cap;
static GuestContext g_xlat; // mem_base/mem_size copy for the worker's pointer translation
static uint64_t g_reads_done, g_bytes_read, g_read_errors;
// Registered AMPR events: (equeue, ident) -> udata.
typedef struct { int32_t eq; uint64_t ident; uint64_t udata; int64_t eq64; } AmprEv;
// -gpu anyps5: the title's equeues belong to AnyPS5's libkernel (64-bit handles); it installs this post
// hook and registers events through nps5_ampr_note_external_event. NULL = the runtime's own equeues.
int (*nps5_ampr_ext_post)(int64_t eq, uint64_t ident, int64_t data);
#define AMPR_MAX_EVENTS 256
static AmprEv g_evs[AMPR_MAX_EVENTS];
static int g_nev;

static int out_contains(uint32_t id) {
  for (uint32_t i = 0; i < g_out_n; i++) if (g_out_ids[i] == id) return 1;
  return 0;
}

static void out_remove(uint32_t id) {
  for (uint32_t i = 0; i < g_out_n; i++) {
    if (g_out_ids[i] == id) { g_out_ids[i] = g_out_ids[--g_out_n]; return; }
  }
}

static ssize_t apr_kpread(int fd, void *buf, size_t n, off_t off) { return nps5_kpread(fd, buf, n, off); }

static void apr_read_file(uint32_t id, uint64_t dst, uint64_t size, uint64_t off) {
  char path[1024];
  pthread_mutex_lock(&g_files_lock);
  if (id >= g_nfiles) {
    pthread_mutex_unlock(&g_files_lock);
    g_read_errors++;
    NPS5_LOG(NPS5_S_HLE, NPS5_L_ERROR, "apr: read of unknown file id %u", id);
    return;
  }
  snprintf(path, sizeof(path), "%s", g_files[id].path);
  pthread_mutex_unlock(&g_files_lock);
  if (!RECOMP_MEM_OK(&g_xlat) || dst == 0 || dst >= g_xlat.mem_size || size > g_xlat.mem_size - dst) {
    g_read_errors++;
    NPS5_LOG(NPS5_S_HLE, NPS5_L_ERROR, "apr: read destination 0x%llx+0x%llx of %s is outside guest memory",
             (unsigned long long)dst, (unsigned long long)size, path);
    return;
  }
  int fd = open(path, O_RDONLY);
  nps5_segfile *sf = (fd < 0 && errno == ENOENT) ? nps5_segfile_lookup(path) : NULL;
  if (sf) {
      uint8_t *sout = (uint8_t *)NPS5_G2H(&g_xlat, dst);
      const ssize_t n = nps5_segfile_pread(sf, sout, (size_t)size, off, apr_kpread);
      const uint64_t got = n > 0 ? (uint64_t)n : 0;
      if (n < 0 || (got < size && off + got < nps5_segfile_size(sf))) {
          g_read_errors++;
          NPS5_LOG(NPS5_S_HLE, NPS5_L_ERROR, "apr: read failed on segmented %s at 0x%llx (errno %d)", path,
                   (unsigned long long)(off + got), errno);
      }
      if (got < size) memset(sout + got, 0, size - got); // past EOF reads as zeros
      g_reads_done++;
      g_bytes_read += got;
      return;
  }
  if (fd < 0) {
    g_read_errors++;
    NPS5_LOG(NPS5_S_HLE, NPS5_L_ERROR, "apr: cannot open %s (errno %d)", path, errno);
    return;
  }
  uint8_t *out = (uint8_t *)NPS5_G2H(&g_xlat, dst);
  uint64_t got = 0;
  while (got < size) {
    ssize_t n = nps5_kpread(fd, out + got, size - got, (off_t)(off + got));
    if (n < 0) {
      if (errno == EINTR) continue;
      g_read_errors++;
      NPS5_LOG(NPS5_S_HLE, NPS5_L_ERROR, "apr: read failed on %s at 0x%llx (errno %d)", path,
               (unsigned long long)(off + got), errno);
      break;
    }
    if (n == 0) break;
    got += (uint64_t)n;
  }
  ps4_vfs_readlog_note_path(path, fd, (long long)off, (long long)got); // NPS5_VFS_RANGELOG / NPS5_VFS_HOLE_CHECK
  if (got < size) memset(out + got, 0, size - got); // past EOF reads as zeros
  close(fd);
  { // NPS5_VFS_TRACE_INI=1: show which *.ini files the title reads (the uecvar overlay, docs/m3/uecvar.md)
    const char *e = getenv("NPS5_VFS_TRACE_INI"); size_t n = strlen(path);
    if (e && *e == '1' && n > 4 && !strcasecmp(path + n - 4, ".ini"))
      fprintf(stderr, "[apr] ini read: %s off=%llu got=%llu\n", path, (unsigned long long)off, (unsigned long long)got);
  }
  g_reads_done++;
  g_bytes_read += got;
}

// Record encoding (ours). Legacy fixed 24-byte records: REC_READFILE, REC_EQUEUE (packed in w[0..5]).
// Everything else follows the AnyPS5 layout: u32 opcode, u32 bytes, payload; Nop = 4-byte
// header 0x5452000f | (dwords-1) << 8 | type << 12 followed by its payload; compact gather = 12 bytes.
enum {
  OP_WRITEADDR = 2, OP_WRITECTR = 3, OP_WAITADDR = 4, OP_WAITCTR = 5, OP_ADDR_TIME = 7, OP_ADDR_CTR = 8,
  OP_ADDR_CTRPAIR = 9, OP_PUSHMARK = 10, OP_POPMARK = 11, OP_SETMARK = 12, OP_GATHER = 13, OP_SCATTER = 14,
  OP_GATHERSCATTER = 15, OP_RESETGS = 16, OP_MAP = 17, OP_MAPDIRECT = 18, OP_MAPEND = 20
};
#define NOP_TAG 0x5452000fu
#define NOP_MASK 0xffff00ffu

typedef struct { int valid; uint32_t fid; uint64_t next_dst, next_off; } ReadCur;

static _Atomic uint32_t g_ctr[256];
static pthread_mutex_t g_ctr_lock = PTHREAD_MUTEX_INITIALIZER;
uint64_t sceKernelGetProcessTimeCounter(void);

static uint32_t ctr_get(uint32_t i) { return atomic_load_explicit(&g_ctr[i & 255], memory_order_acquire); }
static void ctr_field(uint32_t access, uint32_t *bits, uint32_t *shift) {
  if (access == 0) { *bits = 64; *shift = 0; }
  else if (access == 1) { *bits = 32; *shift = 0; }
  else if (access < 4) { *bits = 16; *shift = (access - 2) * 16; }
  else { *bits = 8; *shift = (access - 4) * 8; }
}
static uint64_t bmask(uint32_t bits) { return bits == 64 ? ~0ull : (1ull << bits) - 1; }
static uint64_t ctr_read(uint32_t idx, uint32_t access) {
  uint32_t bits, sh;
  ctr_field(access, &bits, &sh);
  if (bits == 64) return (uint64_t)ctr_get(idx) | (uint64_t)ctr_get(idx + 1) << 32;
  return (ctr_get(idx) >> sh) & bmask(bits);
}
static void ctr_write(uint32_t idx, uint32_t access, uint32_t op, uint64_t v) {
  uint32_t bits, sh;
  ctr_field(access, &bits, &sh);
  pthread_mutex_lock(&g_ctr_lock);
  uint64_t cur = ctr_read(idx, access), nx;
  switch (op) {
  case 1: nx = cur | v; break;
  case 2: nx = cur & ~v; break;
  case 3: nx = cur ^ v; break;
  case 4: nx = cur + v; break;
  default: nx = v; break;
  }
  nx &= bmask(bits);
  if (bits == 64) {
    atomic_store_explicit(&g_ctr[(idx + 1) & 255], (uint32_t)(nx >> 32), memory_order_release);
    atomic_store_explicit(&g_ctr[idx & 255], (uint32_t)nx, memory_order_release);
  } else {
    uint32_t m = (uint32_t)(bmask(bits) << sh);
    uint32_t cur32 = atomic_load_explicit(&g_ctr[idx & 255], memory_order_relaxed);
    atomic_store_explicit(&g_ctr[idx & 255], (cur32 & ~m) | (uint32_t)(nx << sh), memory_order_release);
  }
  pthread_mutex_unlock(&g_ctr_lock);
}

static int wait_ok(uint32_t cmp, uint64_t v, uint64_t ref) {
  const uint64_t sg = 1ull << 63;
  switch (cmp) {
  case 0: return v == ref;
  case 1: return v > ref;
  case 2: return v < ref;
  case 3: return v != ref;
  case 4: return v - ref < sg;
  case 5: return (v ^ sg) > (ref ^ sg);
  case 6: return (v ^ sg) < (ref ^ sg);
  default: return 1;
  }
}

static uint64_t *g_ptr(uint64_t a) {
  if (!RECOMP_MEM_OK(&g_xlat) || a == 0 || a >= g_xlat.mem_size || g_xlat.mem_size - a < 8) return NULL;
  return (uint64_t *)NPS5_G2H(&g_xlat, a);
}

static void store_addr(uint64_t a, uint64_t v) {
  uint64_t *p = g_ptr(a);
  if (!p) { g_read_errors++; NPS5_LOG(NPS5_S_HLE, NPS5_L_ERROR, "ampr: write address 0x%llx outside guest memory", (unsigned long long)a); return; }
  __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static void do_read(ReadCur *rc, int op, uint32_t fid, uint64_t dst, uint64_t size, uint64_t off) {
  if (op != REC_READFILE) {
    if (!rc->valid) { ampr_once("gather", "gather/scatter read without a preceding ReadFile"); return; }
    fid = rc->fid;
    if (op == OP_GATHER) dst = rc->next_dst;
    if (op == OP_SCATTER) off = rc->next_off;
  }
  apr_read_file(fid, dst, size, off);
  rc->valid = 1; rc->fid = fid; rc->next_dst = dst + size; rc->next_off = off + size;
}

// Bytes a handler reads from a record of this opcode (the largest field offset + width it touches).
static uint32_t rec_min_bytes(uint32_t op) {
  switch (op) {
  case REC_READFILE: case REC_EQUEUE: return REC_BYTES;
  case 29: return 12;
  case OP_GATHER: case OP_SCATTER: case OP_GATHERSCATTER: return 40;
  case OP_WRITEADDR: case OP_WRITECTR: return 24;
  case OP_WAITADDR: case OP_WAITCTR: return 38;
  case OP_ADDR_TIME: return 16;
  case OP_ADDR_CTR: return 20;
  case OP_ADDR_CTRPAIR: return 24;
  default: return 8; // header only (RESETGS, markers, maps, unknown)
  }
}

// Reads the record header at pos. Returns 1 = record (op, size set), 2 = a NOP to skip (size set), 0 = end of the
// buffer (also a NOP or a header that runs past it: nothing of it is read), -1 = malformed: size outside the buffer
// or smaller than its opcode's handler reads. op and size are always written.
static int rec_next(const uint8_t *rec, uint32_t bytes, uint32_t pos, uint32_t *op_out, uint32_t *sz_out) {
  *op_out = 0;
  *sz_out = 0;
  if (pos > bytes || bytes - pos < 4) return 0;
  uint32_t first;
  memcpy(&first, rec + pos, 4);
  if ((first & NOP_MASK) == NOP_TAG) {
    const uint32_t nop = (((first >> 8) & 0xf) + 1) * 4;
    if (nop > bytes - pos) return 0;
    *op_out = ~0u;
    *sz_out = nop;
    return 2;
  }
  uint32_t op = first & 0xff, sz;
  if (op == REC_READFILE || op == REC_EQUEUE) sz = REC_BYTES;
  else if (op == 29) sz = 12;
  else { if (bytes - pos < 8) return 0; memcpy(&sz, rec + pos + 4, 4); }
  *op_out = op; *sz_out = sz;
  if (sz < 4 || sz < rec_min_bytes(op) || sz > bytes - pos) return -1;
  return 1;
}

// Validates a whole record buffer before anything runs. Returns 0 if malformed; *has_wait tells whether
// it holds a wait opcode (such a buffer must not run on the shared FIFO worker).
static int apr_scan(const uint8_t *rec, uint32_t bytes, int *has_wait) {
  uint32_t pos = 0, op, sz;
  *has_wait = 0;
  for (;;) {
    int k = rec_next(rec, bytes, pos, &op, &sz);
    if (k < 0) return 0;
    if (k == 0) return 1;
    if (k == 2) { pos += sz; continue; }
    if (op == OP_WAITADDR || op == OP_WAITCTR) *has_wait = 1;
    pos += sz;
  }
}

static void apr_execute(const uint8_t *rec, uint32_t bytes) {
  ReadCur rc = {0};
  uint32_t pos = 0;
  for (;;) {
    uint32_t op, sz;
    int k = rec_next(rec, bytes, pos, &op, &sz);
    if (k < 0) { NPS5_LOG(NPS5_S_HLE, NPS5_L_ERROR, "apr: malformed record op %u size %u", op, sz); break; }
    if (k == 0) break;
    if (k == 2) { pos += sz; continue; }
    uint32_t first;
    memcpy(&first, rec + pos, 4);
    uint32_t w[6] = {0};
    memcpy(w, rec + pos, sz < 24 ? sz : 24);
    const uint8_t *r = rec + pos;
#define Q(o) ({ uint64_t q_; memcpy(&q_, r + (o), 8); q_; })
#define D(o) ({ uint32_t d_; memcpy(&d_, r + (o), 4); d_; })
    switch (op) {
    case REC_READFILE: {
      uint64_t off = (uint64_t)((w[0] >> 8) & 0xff) << 32 | w[4];
      uint64_t dst = (uint64_t)(w[0] >> 16) << 32 | w[2];
      do_read(&rc, REC_READFILE, w[1], dst, (uint64_t)w[3] + 1, off);
      break;
    }
    case 29: { // compact gather
      uint64_t off = (uint64_t)(first >> 8) << 32 | w[2];
      do_read(&rc, OP_GATHER, 0, 0, (uint64_t)w[1] + 1, off);
      break;
    }
    case OP_GATHER: case OP_SCATTER: case OP_GATHERSCATTER:
      do_read(&rc, (int)op, D(8), Q(16), Q(24), Q(32));
      break;
    case OP_RESETGS: rc.valid = 0; break;
    case OP_WRITEADDR: store_addr(Q(8), Q(16)); break;
    case OP_WRITECTR: ctr_write(D(8), r[12], r[13], Q(16)); break;
    case OP_WAITADDR: case OP_WAITCTR: {
      int isctr = op == OP_WAITCTR;
      uint64_t addr = Q(8), ref = Q(16), mask = Q(24);
      uint32_t ctr = D(32), cmp = r[36], acc = r[37];
      uint32_t unused = 0;
      if (isctr) { uint32_t b, s; ctr_field(acc, &b, &s); unused = 64 - b; }
      ref = (ref & mask) << unused;
      for (;;) {
        uint64_t cur;
        if (isctr) cur = ctr_read(ctr, acc);
        else { uint64_t *p = g_ptr(addr); if (!p) break; cur = __atomic_load_n(p, __ATOMIC_ACQUIRE); }
        if (wait_ok(cmp, (cur & mask) << unused, ref)) break;
        usleep(50);
      }
      break;
    }
    case OP_ADDR_TIME: store_addr(Q(8), sceKernelGetProcessTimeCounter()); break;
    case OP_ADDR_CTR: store_addr(Q(8), ctr_get(D(16))); break;
    case OP_ADDR_CTRPAIR: store_addr(Q(8), (uint64_t)ctr_get(D(16)) | (uint64_t)ctr_get(D(20)) << 32); break;
    case OP_PUSHMARK: case OP_POPMARK: case OP_SETMARK: case OP_MAP: case OP_MAPDIRECT: case OP_MAPEND:
      // markers are debugger annotations. AMM maps (APR MapBegin/MapEnd) have nothing to do here: guest
      // memory is already addressable, file reads that follow write straight into it.
      break;
    case REC_EQUEUE: {
      int32_t eq = (int32_t)w[1];
      uint64_t ident = (uint32_t)w[2];
      int64_t data = (int64_t)((uint64_t)w[5] << 32 | w[4]);
      uint64_t udata = 0;
      int64_t eq64 = eq;
      int found = 0;
      pthread_mutex_lock(&g_q_lock);
      for (int i = 0; i < g_nev; i++) {
        if (g_evs[i].eq == eq && g_evs[i].ident == ident) { udata = g_evs[i].udata; eq64 = g_evs[i].eq64; found = 1; break; }
      }
      pthread_mutex_unlock(&g_q_lock);
      if (!found) {
        ampr_once("WriteKernelEventQueue", "event not registered with sceKernelAddAmprEvent; not delivered");
        break;
      }
      if (nps5_ampr_ext_post) nps5_ampr_ext_post(eq64, ident, data);
      else ps4_equeue_post_event(eq, ident, AMPR_FILTER, data, (void *)(uintptr_t)udata);
      break;
    }
    default:
      NPS5_LOG(NPS5_S_HLE, NPS5_L_ERROR, "apr: unknown command record 0x%x", op);
      break;
    }
    pos += sz;
  }
}

static void *apr_worker(void *arg) {
  (void)arg;
  for (;;) {
    pthread_mutex_lock(&g_q_lock);
    while (!g_q_head) pthread_cond_wait(&g_q_cv, &g_q_lock);
    AprJob *j = g_q_head;
    g_q_head = j->next;
    if (!g_q_head) g_q_tail = NULL;
    pthread_mutex_unlock(&g_q_lock);
    apr_execute(j->rec, j->bytes);
    pthread_mutex_lock(&g_q_lock);
    out_remove(j->id);
    pthread_cond_broadcast(&g_done_cv);
    pthread_mutex_unlock(&g_q_lock);
    free(j->rec);
    free(j);
  }
  return NULL;
}

static void *apr_async_job(void *arg) {
  AprJob *j = (AprJob *)arg;
  apr_execute(j->rec, j->bytes);
  pthread_mutex_lock(&g_q_lock);
  out_remove(j->id);
  pthread_cond_broadcast(&g_done_cv);
  pthread_mutex_unlock(&g_q_lock);
  free(j->rec);
  free(j);
  return NULL;
}

// Returns 0 or an errno value mapped by the caller.
static int apr_submit(GuestContext *ctx, uint64_t cb_g, uint32_t *id_out) {
  AmprCmdBuf *cb = CB(ctx, cb_g);
  if (!cb) return EINVAL;
  // the command buffer lives in guest memory: read each field once and use only the copies
  const uint64_t base = *(volatile uint64_t *)&cb->base;
  const uint32_t offset = *(volatile uint32_t *)&cb->offset, size = *(volatile uint32_t *)&cb->size;
  if (offset > size || (offset && (!base || !RECOMP_MEM_OK(ctx) || base >= ctx->mem_size || offset > ctx->mem_size - base)))
    return EFAULT;
  AprJob *j = (AprJob *)calloc(1, sizeof(*j));
  if (!j) return ENOMEM;
  j->bytes = offset;
  j->rec = (uint8_t *)malloc(offset ? offset : 1);
  if (!j->rec) { free(j); return ENOMEM; }
  if (offset) memcpy(j->rec, NPS5_G2H(ctx, base), offset);
  int has_wait = 0;
  if (!apr_scan(j->rec, offset, &has_wait)) {
    NPS5_LOG(NPS5_S_HLE, NPS5_L_ERROR, "apr: command buffer with a malformed record refused");
    free(j->rec); free(j);
    return EINVAL;
  }
  // a buffer that waits on a counter/address may depend on a later submission: own thread
  j->async = has_wait;
  pthread_mutex_lock(&g_q_lock);
  if (!g_worker_started) {
    g_xlat.mem_base = ctx->mem_base;
    g_xlat.mem_size = ctx->mem_size;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int prc = pthread_create(&g_worker, &at, apr_worker, NULL);
    pthread_attr_destroy(&at);
    if (prc) { pthread_mutex_unlock(&g_q_lock); free(j->rec); free(j); return EAGAIN; }
    g_worker_started = 1;
  }
  if (g_out_n == g_out_cap) {
    uint32_t ncap = g_out_cap ? g_out_cap * 2 : 64;
    uint32_t *ni = (uint32_t *)realloc(g_out_ids, ncap * sizeof(uint32_t));
    if (!ni) { pthread_mutex_unlock(&g_q_lock); free(j->rec); free(j); return ENOMEM; }
    g_out_ids = ni;
    g_out_cap = ncap;
  }
  const uint32_t id = g_issued + 1;
  j->id = id;
  g_out_ids[g_out_n++] = id;
  if (j->async) {
    pthread_t t; pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int prc = pthread_create(&t, &at, apr_async_job, j);
    pthread_attr_destroy(&at);
    if (prc) { out_remove(id); pthread_mutex_unlock(&g_q_lock); free(j->rec); free(j); return EAGAIN; }
  } else {
    if (g_q_tail) g_q_tail->next = j; else g_q_head = j;
    g_q_tail = j;
    pthread_cond_signal(&g_q_cv);
  }
  g_issued = id;
  pthread_mutex_unlock(&g_q_lock);
  if (id_out) *id_out = id; // j may already be finished and freed: never touch it after the unlock
  return 0;
}

// -1 + errno, the convention of the sceKernelApr* calls.
static void apr_fail(GuestContext *ctx, int e) {
  set_guest_errno(ctx, e);
  ctx->rax = (uint64_t)-1;
}

void shim_sceKernelAprSubmitCommandBuffer(GuestContext *ctx) {
  int e = apr_submit(ctx, ctx->rdi, NULL);
  if (e) apr_fail(ctx, e); else ctx->rax = 0;
  SHIM_RETURN();
}

void shim_sceKernelAprSubmitCommandBufferAndGetId(GuestContext *ctx) {
  uint32_t *idp = (uint32_t *)NPS5_G2H(ctx, ctx->rdx);
  if (!idp) { apr_fail(ctx, EINVAL); SHIM_RETURN(); }
  uint32_t id = 0;
  int e = apr_submit(ctx, ctx->rdi, &id);
  if (e) apr_fail(ctx, e); else { *idp = id; ctx->rax = 0; }
  SHIM_RETURN();
}

void shim_sceKernelAprSubmitCommandBufferAndGetResult(GuestContext *ctx) {
  uint32_t *resp = (uint32_t *)NPS5_G2H(ctx, ctx->rdx);
  uint32_t *idp = (uint32_t *)NPS5_G2H(ctx, ctx->rcx);
  if (!resp || !idp) { apr_fail(ctx, EINVAL); SHIM_RETURN(); }
  uint32_t id = 0;
  int e = apr_submit(ctx, ctx->rdi, &id);
  if (e) apr_fail(ctx, e); else { *resp = 0; *idp = id; ctx->rax = 0; }
  SHIM_RETURN();
}

void shim_sceKernelAprWaitCommandBuffer(GuestContext *ctx) {
  uint32_t id = (uint32_t)ctx->rdi;
  pthread_mutex_lock(&g_q_lock);
  while (id <= g_issued && out_contains(id)) pthread_cond_wait(&g_done_cv, &g_q_lock);
  pthread_mutex_unlock(&g_q_lock);
  ctx->rax = 0;
  SHIM_RETURN();
}

// Most names one resolve call takes, and whether [a, a+len) lies inside guest memory.
#define APR_MAX_RESOLVE (1u << 20)
static int garr_ok(const GuestContext *ctx, uint64_t a, uint64_t len) {
  return RECOMP_MEM_OK(ctx) && a != 0 && a < ctx->mem_size && len <= ctx->mem_size - a;
}

static void resolve_common(GuestContext *ctx, int sizes) {
  uint64_t *paths = (uint64_t *)NPS5_G2H(ctx, ctx->rdi);
  uint32_t n = (uint32_t)ctx->rsi;
  uint32_t *ids = (uint32_t *)NPS5_G2H(ctx, ctx->rdx);
  uint64_t *szs = sizes ? (uint64_t *)NPS5_G2H(ctx, ctx->rcx) : NULL;
  uint32_t *errp = (uint32_t *)NPS5_G2H(ctx, sizes ? ctx->r8 : ctx->rcx);
  if (!paths || !ids || (sizes && !szs)) { apr_fail(ctx, EINVAL); return; }
  if (n > APR_MAX_RESOLVE || !garr_ok(ctx, ctx->rdi, (uint64_t)n * 8) || !garr_ok(ctx, ctx->rdx, (uint64_t)n * 4) ||
      (sizes && !garr_ok(ctx, ctx->rcx, (uint64_t)n * 8))) {
    apr_fail(ctx, EINVAL);
    return;
  }
  for (uint32_t i = 0; i < n; i++) {
    const char *p = (const char *)NPS5_G2H(ctx, paths[i]);
    uint64_t sz = 0;
    if (!apr_resolve(p, &ids[i], &sz)) {
      ids[i] = APR_INVALID_FILE_ID;
      if (szs) szs[i] = 0;
      if (errp) *errp = i;
      apr_fail(ctx, ENOENT);
      return;
    }
    if (szs) szs[i] = sz;
  }
  ctx->rax = 0;
}

void shim_sceKernelAprResolveFilepathsToIds(GuestContext *ctx) {
  resolve_common(ctx, 0);
  SHIM_RETURN();
}

void shim_sceKernelAprResolveFilepathsToIdsAndFileSizes(GuestContext *ctx) {
  resolve_common(ctx, 1);
  SHIM_RETURN();
}

void shim_sceKernelAprGetFileSize(GuestContext *ctx) {
  uint64_t *szp = (uint64_t *)NPS5_G2H(ctx, ctx->rsi);
  uint32_t id = (uint32_t)ctx->rdi;
  if (!szp) { apr_fail(ctx, EINVAL); SHIM_RETURN(); }
  pthread_mutex_lock(&g_files_lock);
  if (id >= g_nfiles) {
    pthread_mutex_unlock(&g_files_lock);
    apr_fail(ctx, EINVAL);
    SHIM_RETURN();
  }
  *szp = g_files[id].size;
  pthread_mutex_unlock(&g_files_lock);
  ctx->rax = 0;
  SHIM_RETURN();
}

// int sceKernelAddAmprEvent(SceKernelEqueue eq, int id, void *udata)
void shim_sceKernelAddAmprEvent(GuestContext *ctx) {
  int32_t eq = (int32_t)ctx->rdi;
  uint64_t ident = (uint32_t)ctx->rsi;
  if (!ps4_equeue_exists(eq)) RET32(ctx, AMPR_EBADF);
  pthread_mutex_lock(&g_q_lock);
  int i;
  for (i = 0; i < g_nev; i++) if (g_evs[i].eq == eq && g_evs[i].ident == ident) break;
  if (i == g_nev) {
    if (g_nev == AMPR_MAX_EVENTS) {
      pthread_mutex_unlock(&g_q_lock);
      fprintf(stderr, "[ampr] ERROR: sceKernelAddAmprEvent: more than %d registered events; refused (ENOMEM)\n", AMPR_MAX_EVENTS);
      RET32(ctx, (int32_t)0x8002000C); // ENOMEM: table full
    }
    g_nev++;
  }
  g_evs[i].eq = eq;
  g_evs[i].ident = ident;
  g_evs[i].udata = ctx->rdx;
  g_evs[i].eq64 = eq;
  pthread_mutex_unlock(&g_q_lock);
  RET32(ctx, 0);
}

// Records an event the title registered on an external (AnyPS5) equeue, so a WriteKernelEventQueue
// command naming the low 32 bits of the handle finds it.
// Returns 0, or -1 when the table is full (reported loudly; the caller must undo the registration and fail with ENOMEM,
// otherwise a later WriteKernelEventQueue is dropped and the title waits forever for the completion).
int nps5_ampr_note_external_event(int64_t eq64, uint64_t ident, uint64_t udata) {
  int32_t eq = (int32_t)eq64;
  pthread_mutex_lock(&g_q_lock);
  int i;
  for (i = 0; i < g_nev; i++) if (g_evs[i].eq == eq && g_evs[i].ident == ident) break;
  if (i == g_nev) {
    if (g_nev == AMPR_MAX_EVENTS) {
      pthread_mutex_unlock(&g_q_lock);
      fprintf(stderr, "[ampr] ERROR: more than %d registered AMPR events on external equeues; event (eq=0x%llx ident=%llu) NOT registered\n",
              AMPR_MAX_EVENTS, (unsigned long long)eq64, (unsigned long long)ident);
      return -1;
    }
    g_nev++;
  }
  g_evs[i].eq = eq; g_evs[i].ident = ident; g_evs[i].udata = udata; g_evs[i].eq64 = eq64;
  pthread_mutex_unlock(&g_q_lock);
  return 0;
}

// sceKernelDeleteAmprEvent on an external equeue: forget the event so its slot is reused.
void nps5_ampr_remove_external_event(int64_t eq64, uint64_t ident) {
  int32_t eq = (int32_t)eq64;
  pthread_mutex_lock(&g_q_lock);
  for (int i = 0; i < g_nev; i++) {
    if (g_evs[i].eq == eq && g_evs[i].ident == ident) {
      g_evs[i] = g_evs[--g_nev];
      break;
    }
  }
  pthread_mutex_unlock(&g_q_lock);
}

// ---------------------------------------------------------------------------------------------
// Remaining libSceAmpr recording entry points (markers, nops, maps, gather/scatter, counters,
// completion writes) and their MeasureCommandSize companions. Record sizes below are what both
// sides use, so a title that sizes its buffer with Measure* fits exactly.

#define MEAS_INVALID ((uint64_t)(uint32_t)AMPR_EINVAL)
#define SZ_WRITEADDR 32u
#define SZ_WRITECTR 24u
#define SZ_WAIT 48u
#define SZ_ADDRCTR 24u
#define SZ_MAP 40u
#define SZ_HDR 8u
#define SZ_READ 40u
#define SZ_COMPACT 12u

#define RET_SZ(ctx, v) do { (ctx)->rax = (uint64_t)(v); SHIM_RETURN(); } while (0)

static int32_t rec_hdr(GuestContext *ctx, AmprCmdBuf *cb, uint32_t op, uint32_t sz, const void *payload, uint32_t plen) {
  uint8_t buf[64];
  if (sz > sizeof(buf) || SZ_HDR + plen > sz) return AMPR_EINVAL;
  memset(buf, 0, sz);
  memcpy(buf, &op, 4);
  memcpy(buf + 4, &sz, 4);
  if (plen) memcpy(buf + SZ_HDR, payload, plen);
  return cb_append_n(ctx, cb, buf, sz);
}

#define NEEDCB(ctx) AmprCmdBuf *cb = CB(ctx, (ctx)->rdi); if (!cb) RET32(ctx, AMPR_EINVAL)

static uint32_t marker_bytes(const char *t) { return SZ_HDR + (uint32_t)((strlen(t) + 8) & ~(size_t)7); }

static int32_t rec_marker(GuestContext *ctx, AmprCmdBuf *cb, uint32_t op, const char *t) {
  if (!t) return AMPR_EINVAL;
  if (!cb->base) return AMPR_EPERM;
  uint32_t n = marker_bytes(t);
  uint8_t *buf = (uint8_t *)calloc(1, n);
  memcpy(buf, &op, 4);
  memcpy(buf + 4, &n, 4);
  memcpy(buf + SZ_HDR, t, strlen(t));
  int32_t rc = cb_append_n(ctx, cb, buf, n);
  free(buf);
  return rc;
}

static int32_t rec_nop(GuestContext *ctx, AmprCmdBuf *cb, uint32_t header, const uint8_t *data, uint32_t dlen, uint32_t tot_dwords) {
  uint8_t buf[68] = {0};
  uint32_t n = tot_dwords * 4;
  if (n > sizeof(buf)) return AMPR_EINVAL;
  if (!cb->base) return AMPR_EPERM;
  memcpy(buf, &header, 4);
  if (data && dlen && dlen + 4 <= n) memcpy(buf + 4, data, dlen);
  return cb_append_n(ctx, cb, buf, n);
}

static uint32_t nop_header(uint32_t payload_dwords, uint32_t type) {
  return NOP_TAG | (payload_dwords & 0xf) << 8 | (type & 0xf) << 12;
}

void shim_sceAmprCommandBufferGetBufferBaseAddress(GuestContext *ctx) {
  AmprCmdBuf *cb = CB(ctx, ctx->rdi);
  ctx->rax = cb ? cb->base : 0;
  SHIM_RETURN();
}
void shim_sceAmprCommandBufferGetNumCommands(GuestContext *ctx) {
  AmprCmdBuf *cb = CB(ctx, ctx->rdi);
  ctx->rax = cb ? cb->num_commands : 0;
  SHIM_RETURN();
}
void shim_sceAmprCommandBufferGetType(GuestContext *ctx) {
  AmprCmdBuf *cb = CB(ctx, ctx->rdi);
  ctx->rax = cb ? cb->type : 0;
  SHIM_RETURN();
}

void shim_sceAmprCommandBufferNop(GuestContext *ctx) {
  NEEDCB(ctx);
  uint32_t d = (uint32_t)ctx->rsi;
  if (d == 0 || d > 16) RET32(ctx, AMPR_EINVAL);
  RET32(ctx, rec_nop(ctx, cb, nop_header(d - 1, 0), NULL, 0, d));
}
void shim_sceAmprCommandBufferNopWithData(GuestContext *ctx) {
  NEEDCB(ctx);
  uint32_t d = (uint32_t)ctx->rsi;
  if (d > 15) RET32(ctx, AMPR_EINVAL);
  const uint8_t *data = d && ctx->rdx ? (const uint8_t *)NPS5_G2H(ctx, ctx->rdx) : NULL;
  RET32(ctx, rec_nop(ctx, cb, nop_header(d, 0), data, d * 4, d + 1));
}
// (cb, i16 type, const void *payload, u32 payloadBytes, const u32 *word)
void shim_sceAmprCommandBufferConstructNop(GuestContext *ctx) {
  NEEDCB(ctx);
  uint32_t pb = (uint32_t)ctx->rcx;
  const uint8_t *payload = ctx->rdx ? (const uint8_t *)NPS5_G2H(ctx, ctx->rdx) : NULL;
  const uint8_t *word = ctx->r8 ? (const uint8_t *)NPS5_G2H(ctx, ctx->r8) : NULL;
  if (!cb->base) RET32(ctx, AMPR_EPERM);
  if (pb > (word ? 56u : 60u)) RET32(ctx, AMPR_EINVAL);
  if (pb && !payload) RET32(ctx, AMPR_EFAULT);
  uint32_t d = (pb + 3) / 4 + (word ? 1 : 0);
  uint8_t data[64] = {0};
  uint32_t o = 0;
  if (word) { memcpy(data, word, 4); o = 4; }
  if (pb) memcpy(data + o, payload, pb);
  RET32(ctx, rec_nop(ctx, cb, nop_header(d, (uint32_t)(uint16_t)ctx->rsi), data, d * 4, d + 1));
}

void shim_sceAmprCommandBufferPopMarker(GuestContext *ctx) {
  NEEDCB(ctx);
  RET32(ctx, rec_hdr(ctx, cb, OP_POPMARK, SZ_HDR, NULL, 0));
}
void shim_sceAmprCommandBufferPushMarker(GuestContext *ctx) {
  NEEDCB(ctx);
  RET32(ctx, rec_marker(ctx, cb, OP_PUSHMARK, (const char *)NPS5_G2H(ctx, ctx->rsi)));
}
void shim_sceAmprCommandBufferPushMarkerWithColor(GuestContext *ctx) {
  NEEDCB(ctx);
  RET32(ctx, rec_marker(ctx, cb, OP_PUSHMARK, (const char *)NPS5_G2H(ctx, ctx->rsi)));
}
void shim_sceAmprCommandBufferSetMarker(GuestContext *ctx) {
  NEEDCB(ctx);
  RET32(ctx, rec_marker(ctx, cb, OP_SETMARK, (const char *)NPS5_G2H(ctx, ctx->rsi)));
}
void shim_sceAmprCommandBufferSetMarkerWithColor(GuestContext *ctx) {
  NEEDCB(ctx);
  RET32(ctx, rec_marker(ctx, cb, OP_SETMARK, (const char *)NPS5_G2H(ctx, ctx->rsi)));
}
// (cb, u32 type, const char *text, const u32 *color)
void shim_sceAmprCommandBufferConstructMarker(GuestContext *ctx) {
  NEEDCB(ctx);
  const char *t = ctx->rdx ? (const char *)NPS5_G2H(ctx, ctx->rdx) : NULL;
  switch ((uint32_t)ctx->rsi) {
  case 1: RET32(ctx, rec_marker(ctx, cb, OP_SETMARK, t));
  case 2: RET32(ctx, rec_marker(ctx, cb, OP_PUSHMARK, t));
  case 3: RET32(ctx, rec_hdr(ctx, cb, OP_POPMARK, SZ_HDR, NULL, 0));
  case 5: RET32(ctx, ctx->rcx ? rec_marker(ctx, cb, OP_SETMARK, t) : AMPR_EINVAL);
  case 6: RET32(ctx, ctx->rcx ? rec_marker(ctx, cb, OP_PUSHMARK, t) : AMPR_EINVAL);
  default: RET32(ctx, AMPR_EINVAL);
  }
}

#define MAP_VALID(a, sz) ((a) != 0 && (sz) != 0 && (((a) | (sz)) & 0x3fff) == 0 && (a) + (sz) > (a))
#define PROT_VALID(p) (((uint32_t)(p) & 0xFFFFFC0Cu) == 0)

static int32_t map_begin(GuestContext *ctx, AmprCmdBuf *cb, uint32_t op, uint64_t addr, uint64_t doff, uint64_t size, int32_t type, int32_t prot) {
  if (!MAP_VALID(addr, size) || (doff & 0x3fff) || !PROT_VALID(prot)) return AMPR_EINVAL;
  if (cb->recording & 2) return AMPR_EPERM;
  uint8_t p[32];
  memcpy(p, &addr, 8); memcpy(p + 8, &doff, 8); memcpy(p + 16, &size, 8);
  memcpy(p + 24, &type, 4); memcpy(p + 28, &prot, 4);
  int32_t rc = rec_hdr(ctx, cb, op, SZ_MAP, p, 32);
  if (rc == 0) cb->recording |= 2;
  return rc;
}
void shim_sceAmprAprCommandBufferMapBegin(GuestContext *ctx) {
  NEEDCB(ctx);
  RET32(ctx, map_begin(ctx, cb, OP_MAP, ctx->rsi, 0, ctx->rdx, (int32_t)ctx->rcx, (int32_t)ctx->r8));
}
void shim_sceAmprAprCommandBufferMapDirectBegin(GuestContext *ctx) {
  NEEDCB(ctx);
  RET32(ctx, map_begin(ctx, cb, OP_MAPDIRECT, ctx->rsi, ctx->rdx, ctx->rcx, (int32_t)ctx->r8, (int32_t)ctx->r9));
}
void shim_sceAmprAprCommandBufferMapEnd(GuestContext *ctx) {
  NEEDCB(ctx);
  if (!(cb->recording & 2)) RET32(ctx, AMPR_EPERM);
  int32_t rc = rec_hdr(ctx, cb, OP_MAPEND, SZ_HDR, NULL, 0);
  if (rc == 0) cb->recording &= (uint16_t)~2u;
  RET32(ctx, rc);
}
void shim_sceAmprMeasureCommandSizeMapBegin(GuestContext *ctx) {
  RET_SZ(ctx, MAP_VALID(ctx->rdi, ctx->rsi) && PROT_VALID(ctx->r8) ? SZ_MAP : MEAS_INVALID);
}
void shim_sceAmprMeasureCommandSizeMapDirectBegin(GuestContext *ctx) {
  RET_SZ(ctx, MAP_VALID(ctx->rdi, ctx->rdx) && !(ctx->rsi & 0x3fff) && PROT_VALID(ctx->r9) ? SZ_MAP : MEAS_INVALID);
}
void shim_sceAmprMeasureCommandSizeMapEnd(GuestContext *ctx) { RET_SZ(ctx, SZ_HDR); }

// Gather/scatter reads.
static int read_ok(uint64_t dst, uint64_t size, uint64_t off, int need_dst) {
  if (size == 0 || size > 0x100000000ull || off >= 0x10000000000ull) return 0;
  if (need_dst && (dst == 0 || dst >= (1ull << 48))) return 0;
  return 1;
}
static int32_t rec_read(GuestContext *ctx, AmprCmdBuf *cb, uint32_t op, uint64_t dst, uint64_t size, uint64_t off) {
  if (!(cb->recording & REC_SCATTER_GATHER_VALID)) return AMPR_EINVAL;
  uint8_t p[32] = {0};
  memcpy(p + 8, &dst, 8); memcpy(p + 16, &size, 8); memcpy(p + 24, &off, 8);
  return rec_hdr(ctx, cb, op, SZ_READ, p, 32);
}
// (cb, mapState, sgState, size, offset)
void shim_sceAmprAprCommandBufferReadFileGather(GuestContext *ctx) {
  NEEDCB(ctx);
  if (!read_ok(0, ctx->rcx, ctx->r8, 0)) RET32(ctx, AMPR_EINVAL);
  if (!(cb->recording & REC_SCATTER_GATHER_VALID)) RET32(ctx, AMPR_EINVAL);
  uint32_t w[3] = {29u | (uint32_t)(ctx->r8 >> 32) << 8, (uint32_t)(ctx->rcx - 1), (uint32_t)ctx->r8};
  RET32(ctx, cb_append_n(ctx, cb, w, SZ_COMPACT));
}
// (cb, mapState, sgState, dst, size, offset)
void shim_sceAmprAprCommandBufferReadFileGatherScatter(GuestContext *ctx) {
  NEEDCB(ctx);
  if (!read_ok(ctx->rcx, ctx->r8, ctx->r9, 1)) RET32(ctx, AMPR_EINVAL);
  int32_t rc = rec_read(ctx, cb, OP_GATHERSCATTER, ctx->rcx, ctx->r8, ctx->r9);
  if (rc == 0) cb->recording |= REC_SCATTER_GATHER_VALID;
  RET32(ctx, rc);
}
// (cb, mapState, sgState, dst, size)
void shim_sceAmprAprCommandBufferReadFileScatter(GuestContext *ctx) {
  NEEDCB(ctx);
  if (!read_ok(ctx->rcx, ctx->r8, 0, 1)) RET32(ctx, AMPR_EINVAL);
  int32_t rc = rec_read(ctx, cb, OP_SCATTER, ctx->rcx, ctx->r8, 0);
  if (rc == 0) cb->recording |= REC_SCATTER_GATHER_VALID;
  RET32(ctx, rc);
}
void shim_sceAmprAprCommandBufferResetGatherScatterState(GuestContext *ctx) {
  NEEDCB(ctx);
  int32_t rc = rec_hdr(ctx, cb, OP_RESETGS, SZ_HDR, NULL, 0);
  if (rc == 0) cb->recording &= (uint16_t)~REC_SCATTER_GATHER_VALID;
  RET32(ctx, rc);
}
void shim_sceAmprMeasureCommandSizeReadFileGather(GuestContext *ctx) {
  RET_SZ(ctx, read_ok(0, ctx->rdi, ctx->rsi, 0) ? SZ_COMPACT : MEAS_INVALID);
}
void shim_sceAmprMeasureCommandSizeReadFileGatherScatter(GuestContext *ctx) {
  RET_SZ(ctx, read_ok(ctx->rdi, ctx->rsi, ctx->rdx, 1) ? SZ_READ : MEAS_INVALID);
}
void shim_sceAmprMeasureCommandSizeReadFileScatter(GuestContext *ctx) {
  RET_SZ(ctx, read_ok(ctx->rdi, ctx->rsi, 0, 1) ? SZ_READ : MEAS_INVALID);
}
void shim_sceAmprMeasureCommandSizeResetGatherScatterState(GuestContext *ctx) { RET_SZ(ctx, SZ_HDR); }

// Markers / nops measure.
void shim_sceAmprMeasureCommandSizePopMarker(GuestContext *ctx) { RET_SZ(ctx, SZ_HDR); }
static void meas_marker(GuestContext *ctx) {
  const char *t = ctx->rdi ? (const char *)NPS5_G2H(ctx, ctx->rdi) : NULL;
  RET_SZ(ctx, t ? marker_bytes(t) : MEAS_INVALID);
}
void shim_sceAmprMeasureCommandSizePushMarker(GuestContext *ctx) { meas_marker(ctx); }
void shim_sceAmprMeasureCommandSizePushMarkerWithColor(GuestContext *ctx) { meas_marker(ctx); }
void shim_sceAmprMeasureCommandSizeSetMarker(GuestContext *ctx) { meas_marker(ctx); }
void shim_sceAmprMeasureCommandSizeSetMarkerWithColor(GuestContext *ctx) { meas_marker(ctx); }
void shim_sceAmprMeasureCommandSizeNop(GuestContext *ctx) {
  uint32_t d = (uint32_t)ctx->rdi;
  RET_SZ(ctx, d == 0 || d > 16 ? MEAS_INVALID : (uint64_t)d * 4);
}
void shim_sceAmprMeasureCommandSizeNopWithData(GuestContext *ctx) {
  uint32_t d = (uint32_t)ctx->rdi;
  RET_SZ(ctx, d == 0 || d > 16 ? MEAS_INVALID : (uint64_t)d * 4);
}

// Waits, counters, completion writes. atStart==0 means "on completion" (illegal inside a map).
static int addr_ok(uint64_t a) { return a != 0 && (a & 7) == 0; }

void shim_sceAmprCommandBufferWaitOnAddress_04_00(GuestContext *ctx) {
  NEEDCB(ctx);
  uint64_t addr = ctx->rsi, ref = ctx->rdx;
  uint32_t cmp = (uint8_t)ctx->rcx, fl = (uint8_t)ctx->r8;
  if (!addr_ok(addr) || cmp > 6 || fl > 1) RET32(ctx, AMPR_EINVAL);
  uint64_t mask = ~0ull;
  uint8_t p[40] = {0};
  memcpy(p, &addr, 8); memcpy(p + 8, &ref, 8); memcpy(p + 16, &mask, 8);
  p[28] = (uint8_t)cmp;
  cb->recording |= REC_HAS_WAIT;
  RET32(ctx, rec_hdr(ctx, cb, OP_WAITADDR, SZ_WAIT, p, 40));
}
// (cb, counter, access, ref, compare, maskOp, mask, flush)
void shim_sceAmprCommandBufferWaitOnCounter_04_00(GuestContext *ctx) {
  NEEDCB(ctx);
  uint32_t counter = (uint8_t)ctx->rsi, acc = (uint8_t)ctx->rdx, cmp = (uint8_t)ctx->r8, mop = (uint8_t)ctx->r9;
  uint64_t ref = ctx->rcx, mask = NPS5_GUEST_STACK_ARG(ctx, 0);
  uint32_t fl = (uint8_t)NPS5_GUEST_STACK_ARG(ctx, 1);
  if (acc >= 8 || cmp > 6 || mop >= 2 || fl >= 2) RET32(ctx, AMPR_EINVAL);
  uint64_t applied = mop ? mask : ~0ull;
  uint8_t p[40] = {0};
  memcpy(p + 8, &ref, 8); memcpy(p + 16, &applied, 8); memcpy(p + 24, &counter, 4);
  p[28] = (uint8_t)cmp; p[29] = (uint8_t)acc;
  cb->recording |= REC_HAS_WAIT;
  RET32(ctx, rec_hdr(ctx, cb, OP_WAITCTR, SZ_WAIT, p, 40));
}
void shim_sceAmprMeasureCommandSizeWaitOnAddress_04_00(GuestContext *ctx) {
  RET_SZ(ctx, addr_ok(ctx->rdi) && (uint8_t)ctx->rdx <= 6 && (uint8_t)ctx->rcx <= 1 ? SZ_WAIT : MEAS_INVALID);
}
void shim_sceAmprMeasureCommandSizeWaitOnCounter_04_00(GuestContext *ctx) {
  // (counter, access, ref, compare, maskOp, mask, flush)
  uint32_t fl = (uint8_t)NPS5_GUEST_STACK_ARG(ctx, 0);
  RET_SZ(ctx, (uint8_t)ctx->rsi < 8 && (uint8_t)ctx->rcx <= 6 && (uint8_t)ctx->r8 < 2 && fl < 2 ? SZ_WAIT : MEAS_INVALID);
}

static int32_t rec_addr_from(GuestContext *ctx, AmprCmdBuf *cb, uint32_t op, uint64_t addr, uint32_t c0, uint32_t c1, uint64_t at_start) {
  if (!addr_ok(addr)) return AMPR_EINVAL;
  if (at_start == 0 && (cb->recording & 2)) return AMPR_EPERM;
  uint8_t p[16] = {0};
  memcpy(p, &addr, 8); memcpy(p + 8, &c0, 4); memcpy(p + 12, &c1, 4);
  return rec_hdr(ctx, cb, op, SZ_ADDRCTR, p, 16);
}
void shim_sceAmprCommandBufferWriteAddressFromCounter_04_00(GuestContext *ctx) {
  NEEDCB(ctx);
  uint32_t c = (uint8_t)ctx->rdx;
  if (c >= 128) RET32(ctx, AMPR_EINVAL);
  RET32(ctx, rec_addr_from(ctx, cb, OP_ADDR_CTR, ctx->rsi, c, 0, ctx->rcx));
}
void shim_sceAmprCommandBufferWriteAddressFromCounterPair_04_00(GuestContext *ctx) {
  NEEDCB(ctx);
  uint32_t c = (uint8_t)ctx->rdx;
  if (c & 0x81) RET32(ctx, AMPR_EINVAL);
  RET32(ctx, rec_addr_from(ctx, cb, OP_ADDR_CTRPAIR, ctx->rsi, c, c + 1, ctx->rcx));
}
void shim_sceAmprCommandBufferWriteAddressFromTimeCounter_04_00(GuestContext *ctx) {
  NEEDCB(ctx);
  RET32(ctx, rec_addr_from(ctx, cb, OP_ADDR_TIME, ctx->rsi, 0, 0, ctx->rdx));
}
void shim_sceAmprCommandBufferWriteAddress_04_00(GuestContext *ctx) {
  NEEDCB(ctx);
  uint64_t addr = ctx->rsi, val = ctx->rdx;
  uint32_t flags = (uint32_t)ctx->rcx;
  if (addr & 7) RET32(ctx, AMPR_EINVAL);
  uint8_t p[24] = {0};
  memcpy(p, &addr, 8); memcpy(p + 8, &val, 8); memcpy(p + 16, &flags, 4);
  RET32(ctx, rec_hdr(ctx, cb, OP_WRITEADDR, SZ_WRITEADDR, p, 24));
}
// (cb, counter, access, value, operation, atStart)
void shim_sceAmprCommandBufferWriteCounter_04_00(GuestContext *ctx) {
  NEEDCB(ctx);
  uint32_t c = (uint8_t)ctx->rsi, acc = (uint8_t)ctx->rdx, op = (uint8_t)ctx->r8;
  uint64_t val = ctx->rcx;
  if ((c & 0x80) || acc >= 8 || op >= 5) RET32(ctx, AMPR_EINVAL);
  if ((uint8_t)ctx->r9 == 0 && (cb->recording & 2)) RET32(ctx, AMPR_EPERM);
  uint8_t p[16] = {0};
  memcpy(p, &c, 4); p[4] = (uint8_t)acc; p[5] = (uint8_t)op; memcpy(p + 8, &val, 8);
  RET32(ctx, rec_hdr(ctx, cb, OP_WRITECTR, SZ_WRITECTR, p, 16));
}
void shim_sceAmprMeasureCommandSizeWriteAddress_04_00(GuestContext *ctx) { RET_SZ(ctx, SZ_WRITEADDR); }
void shim_sceAmprMeasureCommandSizeWriteAddressFromCounter_04_00(GuestContext *ctx) {
  RET_SZ(ctx, addr_ok(ctx->rdi) ? SZ_ADDRCTR : MEAS_INVALID);
}
void shim_sceAmprMeasureCommandSizeWriteAddressFromCounterPair_04_00(GuestContext *ctx) {
  RET_SZ(ctx, addr_ok(ctx->rdi) && !(ctx->rsi & 1) ? SZ_ADDRCTR : MEAS_INVALID);
}
void shim_sceAmprMeasureCommandSizeWriteAddressFromTimeCounter_04_00(GuestContext *ctx) {
  RET_SZ(ctx, addr_ok(ctx->rdi) ? SZ_ADDRCTR : MEAS_INVALID);
}
void shim_sceAmprMeasureCommandSizeWriteCounter_04_00(GuestContext *ctx) {
  RET_SZ(ctx, !((uint8_t)ctx->rdi & 0x80) && (uint8_t)ctx->rsi < 8 && (uint8_t)ctx->rcx < 5 ? SZ_WRITECTR : MEAS_INVALID);
}

// ---------------------------------------------------------------------------------------------
// libkernel APR file-id resolution, prefixed and "ForEach" variants, file stat; small libkernel
// calls Astro Bot imports (CondSignalto, ClearVirtualRangeName, GetOpenPsId).

void nps5_fill_guest_stat(void *gst, const struct stat *st);
void shim_scePthreadCondBroadcast(GuestContext *ctx);

#define SCE_ENOENT ((int32_t)0x80020002)

// Common worker. paths: array of guest char* ; prefix may be NULL. mode: 0 = stop at first failure
// (-1/errno, *errp = index), 1 = ForEach (results[i] per path, return = number of failures).
static int resolve_many(GuestContext *ctx, const char *prefix, uint64_t paths_g, uint32_t n, uint64_t ids_g,
                        uint64_t sizes_g, uint64_t out_g, int foreach_mode) {
  uint64_t *paths = (uint64_t *)NPS5_G2H(ctx, paths_g);
  uint32_t *ids = (uint32_t *)NPS5_G2H(ctx, ids_g);
  uint64_t *szs = sizes_g ? (uint64_t *)NPS5_G2H(ctx, sizes_g) : NULL;
  uint32_t *errp = foreach_mode ? NULL : (out_g ? (uint32_t *)NPS5_G2H(ctx, out_g) : NULL);
  int32_t *res = foreach_mode ? (int32_t *)NPS5_G2H(ctx, out_g) : NULL;
  if (!paths || !ids || (foreach_mode && !res)) { apr_fail(ctx, EINVAL); return -1; }
  if (n > APR_MAX_RESOLVE || !garr_ok(ctx, paths_g, (uint64_t)n * 8) || !garr_ok(ctx, ids_g, (uint64_t)n * 4) ||
      (szs && !garr_ok(ctx, sizes_g, (uint64_t)n * 8)) || (res && !garr_ok(ctx, out_g, (uint64_t)n * 4))) {
    apr_fail(ctx, EINVAL);
    return -1;
  }
  int failures = 0;
  size_t plen = prefix ? strlen(prefix) : 0;
  for (uint32_t i = 0; i < n; i++) {
    const char *p = (const char *)NPS5_G2H(ctx, paths[i]);
    char full[1024];
    uint64_t sz = 0;
    int ok = 0;
    if (p && !strstr(p, "/../")) {
      if (plen) { snprintf(full, sizeof(full), "%s%s", prefix, p); p = full; }
      ok = apr_resolve(p, &ids[i], &sz);
    }
    if (ok) {
      if (szs) szs[i] = sz;
      if (res) res[i] = 0;
      continue;
    }
    ids[i] = APR_INVALID_FILE_ID;
    if (szs) szs[i] = 0;
    if (foreach_mode) { res[i] = SCE_ENOENT; failures++; continue; }
    if (errp) *errp = i;
    apr_fail(ctx, ENOENT);
    return -1;
  }
  return foreach_mode ? failures : 0;
}

#define RET_RESOLVE(ctx, v) do { int r_ = (v); if (r_ >= 0) (ctx)->rax = (uint64_t)(int64_t)r_; SHIM_RETURN(); } while (0)

// (paths, n, ids, ForEach results)
void shim_sceKernelAprResolveFilepathsToIdsForEach(GuestContext *ctx) {
  RET_RESOLVE(ctx, resolve_many(ctx, NULL, ctx->rdi, (uint32_t)ctx->rsi, ctx->rdx, 0, ctx->rcx, 1));
}
// (paths, n, ids, sizes, results)
void shim_sceKernelAprResolveFilepathsToIdsAndFileSizesForEach(GuestContext *ctx) {
  RET_RESOLVE(ctx, resolve_many(ctx, NULL, ctx->rdi, (uint32_t)ctx->rsi, ctx->rdx, ctx->rcx, ctx->r8, 1));
}
// (prefix, names, n, ids, errorIndex)
void shim_sceKernelAprResolveFilepathsWithPrefixToIds(GuestContext *ctx) {
  RET_RESOLVE(ctx, resolve_many(ctx, (const char *)NPS5_G2H(ctx, ctx->rdi), ctx->rsi, (uint32_t)ctx->rdx, ctx->rcx, 0, ctx->r8, 0));
}
// (prefix, names, n, ids, sizes, errorIndex)
void shim_sceKernelAprResolveFilepathsWithPrefixToIdsAndFileSizes(GuestContext *ctx) {
  RET_RESOLVE(ctx, resolve_many(ctx, (const char *)NPS5_G2H(ctx, ctx->rdi), ctx->rsi, (uint32_t)ctx->rdx, ctx->rcx, ctx->r8, ctx->r9, 0));
}
// (prefix, names, n, ids, results)
void shim_sceKernelAprResolveFilepathsWithPrefixToIdsForEach(GuestContext *ctx) {
  RET_RESOLVE(ctx, resolve_many(ctx, (const char *)NPS5_G2H(ctx, ctx->rdi), ctx->rsi, (uint32_t)ctx->rdx, ctx->rcx, 0, ctx->r8, 1));
}
// (prefix, names, n, ids, sizes, results)
void shim_sceKernelAprResolveFilepathsWithPrefixToIdsAndFileSizesForEach(GuestContext *ctx) {
  RET_RESOLVE(ctx, resolve_many(ctx, (const char *)NPS5_G2H(ctx, ctx->rdi), ctx->rsi, (uint32_t)ctx->rdx, ctx->rcx, ctx->r8, ctx->r9, 1));
}

// int sceKernelAprGetFileStat(u32 id, SceKernelStat *st)
void shim_sceKernelAprGetFileStat(GuestContext *ctx) {
  uint32_t id = (uint32_t)ctx->rdi;
  void *gst = ctx->rsi ? NPS5_G2H(ctx, ctx->rsi) : NULL;
  if (!gst) { apr_fail(ctx, EINVAL); SHIM_RETURN(); }
  char path[1024];
  pthread_mutex_lock(&g_files_lock);
  if (id >= g_nfiles) { pthread_mutex_unlock(&g_files_lock); apr_fail(ctx, EINVAL); SHIM_RETURN(); }
  snprintf(path, sizeof(path), "%s", g_files[id].path);
  pthread_mutex_unlock(&g_files_lock);
  struct stat st;
  if (stat(path, &st) != 0) { apr_fail(ctx, ENOENT); SHIM_RETURN(); }
  nps5_fill_guest_stat(gst, &st);
  ctx->rax = 0;
  SHIM_RETURN();
}

// int scePthreadCondSignalto(ScePthreadCond *cond, ScePthread thread): wake the named waiter.
// Waking every waiter is a valid (spurious-wakeup) superset for predicate-loop users.
void shim_scePthreadCondSignalto(GuestContext *ctx) { shim_scePthreadCondBroadcast(ctx); }

// int sceKernelClearVirtualRangeName(void *addr, size_t len): debugger name for a range; nothing to clear.
void shim_sceKernelClearVirtualRangeName(GuestContext *ctx) { RET32(ctx, 0); }

// int sceKernelGetOpenPsId(SceOpenPsId *id): 16-byte console id; stable fake, not the user's.
void shim_sceKernelGetOpenPsId(GuestContext *ctx) {
  uint8_t *p = ctx->rdi ? (uint8_t *)NPS5_G2H(ctx, ctx->rdi) : NULL;
  if (!p) RET32(ctx, AMPR_EFAULT);
  for (int i = 0; i < 16; i++) p[i] = (uint8_t)(0xA5 ^ (i * 17));
  RET32(ctx, 0);
}
