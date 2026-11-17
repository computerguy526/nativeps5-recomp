// SPDX-License-Identifier: GPL-2.0-or-later
// libc HLE core: guest callbacks, logged stubs, guest heap, data imports.
#include "nps5_libc.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

// ------------------------------------------------------------------ guest callback
uint64_t nps5_libc_call_guest(GuestContext *ctx, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
  uint64_t s_rax = ctx->rax, s_rcx = ctx->rcx, s_rdx = ctx->rdx, s_rsi = ctx->rsi, s_rdi = ctx->rdi;
  uint64_t s_r8 = ctx->r8, s_r9 = ctx->r9, s_r10 = ctx->r10, s_r11 = ctx->r11, s_rsp = ctx->rsp, s_rip = ctx->rip;
  ctx->rdi = a0; ctx->rsi = a1; ctx->rdx = a2; ctx->rcx = a3;
  // At shim entry rsp points at the return address (rsp % 16 == 8). Drop below it, align to 16,
  // then push a null return address so the callee sees rsp % 16 == 8 as after a CALL.
  uint64_t sp = (s_rsp - 128) & ~15ULL;
  sp -= 8;
  *(uint64_t *)NPS5_G2H(ctx, sp) = 0;
  ctx->rsp = sp;
  recomp_dispatch(ctx, fn);
  uint64_t ret = ctx->rax;
  ctx->rsp = s_rsp; ctx->rip = s_rip;
  ctx->rdi = s_rdi; ctx->rsi = s_rsi; ctx->rdx = s_rdx; ctx->rcx = s_rcx;
  ctx->r8 = s_r8; ctx->r9 = s_r9; ctx->r10 = s_r10; ctx->r11 = s_r11; ctx->rax = s_rax;
  return ret;
}

// ------------------------------------------------------------------ logged stubs
void nps5_libc_stub(GuestContext *ctx, const char *name, uint64_t retval) {
  static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
  static char seen[64][40];
  static int nseen;
  int first = 1;
  pthread_mutex_lock(&mu);
  for (int i = 0; i < nseen; i++)
    if (strcmp(seen[i], name) == 0) { first = 0; break; }
  if (first && nseen < 64) strlcpy(seen[nseen++], name, sizeof seen[0]);
  pthread_mutex_unlock(&mu);
  if (first)
    nps5_log(NPS5_S_HLE, NPS5_L_WARN, "libc stub %s called (not implemented): returning 0x%llx rip=0x%llx", name,
             (unsigned long long)retval, (unsigned long long)(ctx ? ctx->rip : 0));
}

// ------------------------------------------------------------------ guest heap
// Small blocks: power-of-two size classes (32 B .. 64 KiB) carved from 1 MiB chunks obtained with
// recomp_vm_alloc_named_aligned, with per-class free lists. Large blocks: one VM allocation each,
// released with recomp_vm_free. Every block has a 16-byte header {u64 usable_size, u64 tag|align_off}
// immediately before the user pointer; memalign puts the header just before the aligned pointer.
#define HDR 16ULL
#define MIN_SHIFT 5
#define MAX_SHIFT 16
#define NCLASS (MAX_SHIFT - MIN_SHIFT + 1)
#define CHUNK (1ULL << 20)
#define TAG_SMALL 0x534d414cULL  // "SMAL"
#define TAG_LARGE 0x4c415247ULL  // "LARG"
#define TAG_ALIGN 0x414c4947ULL  // "ALIG": header of an aligned block, `size` = distance to the real block

static pthread_mutex_t g_heap_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_free[NCLASS];
static uint64_t g_cur, g_cur_end;

typedef struct { uint64_t size, tag; } BlockHdr;

static BlockHdr *hdr_of(GuestContext *ctx, uint64_t p) { return (BlockHdr *)NPS5_G2H(ctx, p - HDR); }

static int class_of(uint64_t n) {
  int s = MIN_SHIFT;
  while (s <= MAX_SHIFT && (1ULL << s) < n + HDR) s++;
  return s > MAX_SHIFT ? -1 : s - MIN_SHIFT;
}

uint64_t nps5_libc_malloc(GuestContext *ctx, uint64_t size) {
  if (size == 0) size = 1;
  if (size > (1ULL << 40)) { lc_errno(ctx, ENOMEM); return 0; }
  int c = class_of(size);
  if (c < 0) {
    uint64_t total = (size + HDR + 4095) & ~4095ULL;
    uint64_t base = recomp_vm_alloc_named_aligned(ctx, total, 4096, PROT_READ | PROT_WRITE, 0, "libc.heap.large");
    if (base == (uint64_t)-1 || base == 0) { lc_errno(ctx, ENOMEM); return 0; }
    BlockHdr *h = (BlockHdr *)NPS5_G2H(ctx, base);
    h->size = total - HDR; h->tag = TAG_LARGE;
    return base + HDR;
  }
  uint64_t bsz = 1ULL << (c + MIN_SHIFT), p;
  pthread_mutex_lock(&g_heap_mu);
  if (g_free[c]) {
    p = g_free[c];
    g_free[c] = *(uint64_t *)NPS5_G2H(ctx, p);
  } else {
    if (g_cur + bsz > g_cur_end) {
      uint64_t chunk = recomp_vm_alloc_named_aligned(ctx, CHUNK, 4096, PROT_READ | PROT_WRITE, 0, "libc.heap");
      if (chunk == (uint64_t)-1 || chunk == 0) { pthread_mutex_unlock(&g_heap_mu); lc_errno(ctx, ENOMEM); return 0; }
      g_cur = chunk; g_cur_end = chunk + CHUNK;
    }
    p = g_cur + HDR;
    g_cur += bsz;
  }
  pthread_mutex_unlock(&g_heap_mu);
  BlockHdr *h = hdr_of(ctx, p);
  h->size = bsz - HDR; h->tag = TAG_SMALL;
  return p;
}

static BlockHdr *real_hdr(GuestContext *ctx, uint64_t *p) {
  BlockHdr *h = hdr_of(ctx, *p);
  if (h->tag == TAG_ALIGN) { *p -= h->size; h = hdr_of(ctx, *p); }
  return h;
}

void nps5_libc_free(GuestContext *ctx, uint64_t p) {
  if (!p) return;
  BlockHdr *h = real_hdr(ctx, &p);
  if (h->tag == TAG_SMALL) {
    int c = class_of(h->size);  // size + HDR == class size
    h->tag = 0;
    pthread_mutex_lock(&g_heap_mu);
    *(uint64_t *)NPS5_G2H(ctx, p) = g_free[c];
    g_free[c] = p;
    pthread_mutex_unlock(&g_heap_mu);
  } else if (h->tag == TAG_LARGE) {
    uint64_t total = h->size + HDR;
    h->tag = 0;
    recomp_vm_free(ctx, p - HDR, total);
  } else {
    nps5_log(NPS5_S_HLE, NPS5_L_ERROR, "libc free(0x%llx): invalid or double free (tag 0x%llx)", (unsigned long long)p,
             (unsigned long long)h->tag);
  }
}

uint64_t nps5_libc_usable_size(GuestContext *ctx, uint64_t p) {
  if (!p) return 0;
  uint64_t base = p;
  BlockHdr *h = real_hdr(ctx, &base);
  if (h->tag != TAG_SMALL && h->tag != TAG_LARGE) return 0;
  return h->size - (p - base);
}

uint64_t nps5_libc_calloc(GuestContext *ctx, uint64_t n, uint64_t size) {
  uint64_t total;
  if (__builtin_mul_overflow(n, size, &total)) { lc_errno(ctx, ENOMEM); return 0; }
  uint64_t p = nps5_libc_malloc(ctx, total);
  if (p) memset(NPS5_G2H(ctx, p), 0, total);
  return p;
}

uint64_t nps5_libc_realloc(GuestContext *ctx, uint64_t p, uint64_t size) {
  if (!p) return nps5_libc_malloc(ctx, size);
  if (size == 0) { nps5_libc_free(ctx, p); return 0; }
  uint64_t old = nps5_libc_usable_size(ctx, p);
  if (size <= old) return p;
  uint64_t q = nps5_libc_malloc(ctx, size);
  if (!q) return 0;
  memcpy(NPS5_G2H(ctx, q), NPS5_G2H(ctx, p), old);
  nps5_libc_free(ctx, p);
  return q;
}

uint64_t nps5_libc_memalign(GuestContext *ctx, uint64_t align, uint64_t size) {
  if (align <= 16) return nps5_libc_malloc(ctx, size);
  if (align & (align - 1)) { lc_errno(ctx, EINVAL); return 0; }
  uint64_t need;
  if (__builtin_add_overflow(size, align + HDR, &need) || align > (1ULL << 40)) { lc_errno(ctx, ENOMEM); return 0; }
  uint64_t raw = nps5_libc_malloc(ctx, need);
  if (!raw) return 0;
  uint64_t p = (raw + HDR + align - 1) & ~(align - 1);
  BlockHdr *h = hdr_of(ctx, p);
  h->size = p - raw; h->tag = TAG_ALIGN;
  return p;
}

#define SH(name) void shim_##name(GuestContext *ctx)
SH(malloc) { LC_RET(nps5_libc_malloc(ctx, ctx->rdi)); }
SH(calloc) { LC_RET(nps5_libc_calloc(ctx, ctx->rdi, ctx->rsi)); }
SH(realloc) { LC_RET(nps5_libc_realloc(ctx, ctx->rdi, ctx->rsi)); }
SH(reallocf) {
  uint64_t r = nps5_libc_realloc(ctx, ctx->rdi, ctx->rsi);
  if (!r && ctx->rsi) nps5_libc_free(ctx, ctx->rdi);
  LC_RET(r);
}
SH(free) { nps5_libc_free(ctx, ctx->rdi); SHIM_RETURN(); }
SH(aligned_alloc) { LC_RET(nps5_libc_memalign(ctx, ctx->rdi, ctx->rsi)); }
SH(memalign) { LC_RET(nps5_libc_memalign(ctx, ctx->rdi, ctx->rsi)); }
SH(malloc_usable_size) { LC_RET(nps5_libc_usable_size(ctx, ctx->rdi)); }
SH(posix_memalign) {
  uint64_t align = ctx->rsi;
  if (align < 8 || (align & (align - 1))) LC_RET_INT(EINVAL);
  uint64_t p = nps5_libc_memalign(ctx, align, ctx->rdx);
  if (!p) LC_RET_INT(ENOMEM);
  *(uint64_t *)NPS5_G2H(ctx, ctx->rdi) = p;
  LC_RET_INT(0);
}

// ------------------------------------------------------------------ data imports
// __stdinp/__stdoutp/__stderrp (FILE *), __isthreaded, __mb_cur_max, ... A data import's GOT slot
// must hold the ADDRESS of the variable, so each one gets an 8-byte guest cell.
extern uint64_t nps5_libc_std_file(GuestContext *ctx, int which);  // nps5_libc_stdio.c

bool nps5_libc_bind_data_import(GuestContext *ctx, uint64_t slot, const char *name) {
  if (!name || !ctx || !RECOMP_MEM_OK(ctx) || slot + 8 > ctx->mem_size) return false;
  uint64_t val;
  if (!strcmp(name, "__stdinp")) val = nps5_libc_std_file(ctx, 0);
  else if (!strcmp(name, "__stdoutp")) val = nps5_libc_std_file(ctx, 1);
  else if (!strcmp(name, "__stderrp")) val = nps5_libc_std_file(ctx, 2);
  else if (!strcmp(name, "__isthreaded")) val = 1;
  else if (!strcmp(name, "__mb_cur_max")) val = 1;  // C locale
  else return false;
  uint64_t cell = nps5_libc_malloc(ctx, 8);
  if (!cell) return false;
  *(uint64_t *)NPS5_G2H(ctx, cell) = val;
  *(uint64_t *)NPS5_G2H(ctx, slot) = cell;
  nps5_log(NPS5_S_HLE, NPS5_L_DEBUG, "libc data import %s: slot 0x%llx -> cell 0x%llx (value 0x%llx)", name,
           (unsigned long long)slot, (unsigned long long)cell, (unsigned long long)val);
  return true;
}

// ------------------------------------------------------------------ lean variants (docs/m3/leanshim.md)
// Same bodies as the shims above, in the lean ABI: arguments in a0.., result returned (a void
// function returns the RAX it was called with, as SHIM_RETURN leaves it).
#include "nps5_lean.h"
NPS5_LEAN_FN(malloc) { return nps5_libc_malloc(ctx, a0); }
NPS5_LEAN_FN(calloc) { return nps5_libc_calloc(ctx, a0, a1); }
NPS5_LEAN_FN(realloc) { return nps5_libc_realloc(ctx, a0, a1); }
NPS5_LEAN_FN(free) { nps5_libc_free(ctx, a0); return ctx->rax; }
NPS5_LEAN_FN(aligned_alloc) { return nps5_libc_memalign(ctx, a0, a1); }
NPS5_LEAN_FN(memalign) { return nps5_libc_memalign(ctx, a0, a1); }
NPS5_LEAN_FN(malloc_usable_size) { return nps5_libc_usable_size(ctx, a0); }
