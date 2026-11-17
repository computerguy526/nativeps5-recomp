// C driver for pkg/thunkgen/memtests_adv_test.go: includes the thunks the test generated, supplies
// HLE functions that record what they received, and checks it. Guest memory is an ordinary
// buffer; in base mode the bias is the buffer's address, in identity mode guest == host.
#include "mt.h"
#include "nps5_thunk.h"
#include "hle.h"
#include "thunks.c"

#include <stdlib.h>

static uint8_t *buf;
static GuestContext *ctx;
#if NPS5_ADDR_MODE_IDENTITY
#define GA(p) ((uint64_t)(uintptr_t)(p))
#else
#define GA(p) ((uint64_t)((uint8_t *)(p) - buf))
#endif
#define BUFSZ 0x100000
// Test double of nps5_guest_safe_read (core/nps5_mem.c): the guest memory here is `buf`; anything
// outside it is unreadable.
bool nps5_guest_safe_read(uint64_t g, void *dst, size_t len) {
#if NPS5_ADDR_MODE_IDENTITY
  uint64_t off = g - (uint64_t)(uintptr_t)buf;
#else
  uint64_t off = g;
#endif
  if (off > BUFSZ || len > BUFSZ - off) return false;
  memcpy(dst, buf + off, len);
  return true;
}
static uint64_t stk(int n, uint64_t v) { uint64_t *s = (uint64_t *)(buf + 0x80000); s[1 + n] = v; return 0; }
static void callstart(void) { ctx->rsp = GA(buf + 0x80000); stk(-1, 0); }

// ---- recorded arguments ----
static long r_l[16]; static void *r_p[16]; static double r_d[16];
long hle_ints(GuestContext *c, long a1, int a2, unsigned a3, size_t a4, uint16_t a5, long a6, int a7, long a8, unsigned char a9) {
  (void)c; r_l[0] = a1; r_l[1] = a2; r_l[2] = a3; r_l[3] = (long)a4; r_l[4] = a5; r_l[5] = a6; r_l[6] = a7; r_l[7] = a8; r_l[8] = a9;
  return -5;
}
int hle_ptrs(GuestContext *c, void *p0, const char *p1, void *p2, void *p3, void *p4, void *p5, void *p6, void *p7) {
  (void)c; r_p[0] = p0; r_p[1] = (void *)p1; r_p[2] = p2; r_p[3] = p3; r_p[4] = p4; r_p[5] = p5; r_p[6] = p6; r_p[7] = p7; return 0;
}
static void *ret_ptr_val; static int slot_mode;
void *hle_retptr(GuestContext *c) { (void)c; return ret_ptr_val; }
void *hle_retfail(GuestContext *c) { (void)c; return ret_ptr_val; }
int hle_slot(GuestContext *c, void **slot) { (void)c; r_p[0] = slot ? *slot : (void *)0x1; if (slot_mode == 1) *slot = (void *)(buf + 0x1234); if (slot_mode == 2) *slot = NULL; return 0; }
static int iov_calls, iov_null; static void *iov_base0; static size_t iov_len0; static void *iov_base1;
static int iov_bad;
ssize_t hle_iov(GuestContext *c, const struct iovec *iov, int n) {
  (void)c; iov_calls++; if (iov == NPS5_BAD_GUEST_ARRAY) { iov_bad++; return -1; } if (!iov) { iov_null++; return -1; }
  iov_base0 = n > 0 ? iov[0].iov_base : NULL; iov_len0 = n > 0 ? iov[0].iov_len : 0; iov_base1 = n > 1 ? iov[1].iov_base : NULL; return n;
}
#ifdef WITH_DOUBLE
int hle_dbl(GuestContext *c, double d, int i) { (void)c; r_d[0] = d; r_l[0] = i; return 0; }
double hle_dret(GuestContext *c) { (void)c; return 2.5; }
int hle_dbl9(GuestContext *c, double a, double b, double c2, double d, double e, double f, double g, double h, double i, int x) { (void)c; r_d[0]=a; r_d[7]=h; r_d[8]=i; r_l[0]=x; (void)c2;(void)d;(void)e;(void)f;(void)g; return 0; }
int hle_float(GuestContext *c, float f) { (void)c; r_d[0] = f; return 0; }
#endif

int main(void) {
  mt_init();
  buf = aligned_alloc(0x4000, BUFSZ); memset(buf, 0, BUFSZ);
  ctx = calloc(1, sizeof *ctx);
#if NPS5_ADDR_MODE_IDENTITY
  ctx->mem_base = NULL;
#else
  ctx->mem_base = buf;
  ctx->mem_size = BUFSZ; // the end of the guest window: a pointer at or above it translates to NULL
#endif
  // ---- integers in registers and on the stack ----
  mt_begin("thunk." MODE ".int_args_regs_and_stack", "thunk");
  callstart();
  ctx->rdi = 0x1122334455667788ULL; ctx->rsi = 0xFFFFFFFF80000000ULL; ctx->rdx = 0x1FFFFFFFFULL; ctx->rcx = 0xFFFFFFFFFFFFFFFFULL;
  ctx->r8 = 0xABCDEF; ctx->r9 = 0x7FFFFFFFFFFFFFFFULL;
  stk(0, 0x100000001ULL); stk(1, 0xDEADBEEFCAFEF00DULL); stk(2, 0x1234);   // 7th..9th
  shim_ints(ctx);
  MT_EXPECT(r_l[0] == 0x1122334455667788L, "arg1 %lx", r_l[0]);
  MT_EXPECT(r_l[1] == (int)0x80000000, "arg2 (int from 0xFFFFFFFF80000000) = %ld", r_l[1]);
  MT_EXPECT(r_l[2] == 0xFFFFFFFFUL, "arg3 (unsigned from 0x1FFFFFFFF) = %lx", r_l[2]);
  MT_EXPECT(r_l[3] == -1L, "arg4 size_t %lx", r_l[3]);
  MT_EXPECT(r_l[4] == 0xCDEF, "arg5 uint16 %lx", r_l[4]);
  MT_EXPECT(r_l[5] == 0x7FFFFFFFFFFFFFFFL, "arg6 %lx", r_l[5]);
  MT_EXPECT(r_l[6] == 1, "arg7 (stack 0, int from 0x1_0000_0001) = %ld", r_l[6]);
  MT_EXPECT(r_l[7] == (long)0xDEADBEEFCAFEF00DULL, "arg8 (stack 1) = %lx", r_l[7]);
  MT_EXPECT(r_l[8] == 0x34, "arg9 (stack 2, uchar from 0x1234) = %lx", r_l[8]);
  MT_EXPECT(ctx->rax == (uint64_t)-5, "return value %llx", (unsigned long long)ctx->rax);
  MT_EXPECT(ctx->rsp == GA(buf + 0x80000) + 8, "rsp not advanced by the return address (%llx)", (unsigned long long)ctx->rsp);
  mt_end();

  // ---- pointers: NULL, translation, register and stack positions ----
  mt_begin("thunk." MODE ".pointer_args_null_and_stack", "thunk");
  callstart();
  ctx->rdi = 0; ctx->rsi = GA(buf + 0x100); ctx->rdx = 1; ctx->rcx = GA(buf + BUFSZ - 1); ctx->r8 = 0; ctx->r9 = GA(buf + 0x4000);
  stk(0, 0); stk(1, GA(buf + 0x2000));
  shim_ptrs(ctx);
  MT_EXPECT(r_p[0] == NULL, "NULL biased to %p", r_p[0]);
  MT_EXPECT(r_p[1] == (void *)(buf + 0x100), "p1 %p want %p", r_p[1], (void *)(buf + 0x100));
#if NPS5_ADDR_MODE_IDENTITY
  MT_EXPECT(r_p[2] == (void *)1, "guest address 1 -> %p", r_p[2]);
#else
  MT_EXPECT(r_p[2] != NULL && r_p[2] == (void *)(buf + 1), "guest address 1 -> %p (must be bias+1, not NULL)", r_p[2]);
#endif
  MT_EXPECT(r_p[3] == (void *)(buf + BUFSZ - 1), "pointer at the end of the area %p", r_p[3]);
  MT_EXPECT(r_p[4] == NULL, "NULL in r8 %p", r_p[4]);
  MT_EXPECT(r_p[5] == (void *)(buf + 0x4000), "p5 %p", r_p[5]);
  MT_EXPECT(r_p[6] == NULL, "NULL as a stack argument became %p", r_p[6]);
  MT_EXPECT(r_p[7] == (void *)(buf + 0x2000), "stack pointer arg translated to %p want %p", r_p[7], (void *)(buf + 0x2000));
  mt_end();

  // ---- pointer results ----
  mt_begin("thunk." MODE ".pointer_results", "thunk");
  callstart(); ret_ptr_val = NULL; shim_retptr(ctx);
  MT_EXPECT(ctx->rax == 0, "NULL result became 0x%llx", (unsigned long long)ctx->rax);
  callstart(); ret_ptr_val = buf + 0x5000; shim_retptr(ctx);
  MT_EXPECT(ctx->rax == GA(buf + 0x5000), "pointer result 0x%llx", (unsigned long long)ctx->rax);
  callstart(); ret_ptr_val = (void *)-1; shim_retfail(ctx);
  MT_EXPECT(ctx->rax == ~0ULL, "MAP_FAILED result translated to 0x%llx", (unsigned long long)ctx->rax);
  callstart(); ret_ptr_val = NULL; shim_retfail(ctx);
  MT_EXPECT(ctx->rax == 0, "NULL result of the fail-variant 0x%llx", (unsigned long long)ctx->rax);
  mt_end();

  // ---- pointer slots ----
  mt_begin("thunk." MODE ".pointer_slot_roundtrip", "thunk");
  uint64_t vals[] = {0, 1, 0x200000000ULL, (1ULL << 40) + 5, ~0ULL, 1ULL << 63};
  for (int i = 0; i < 6; i++) {
    uint64_t *slot = (uint64_t *)(buf + 0x300); callstart();
    uint64_t in = vals[i];
#if NPS5_ADDR_MODE_IDENTITY
    if (i == 1) in = GA(buf + 0x40);
#endif
    *slot = in; ctx->rdi = GA(slot); slot_mode = 0; shim_slot(ctx);
    MT_EXPECT(*slot == in, "untouched slot 0x%llx came back as 0x%llx", (unsigned long long)in, (unsigned long long)*slot);
  }
  { uint64_t *slot = (uint64_t *)(buf + 0x300); callstart(); *slot = 0; ctx->rdi = GA(slot); slot_mode = 1; shim_slot(ctx);
    MT_EXPECT(*slot == GA(buf + 0x1234), "callee-written slot holds 0x%llx want 0x%llx", (unsigned long long)*slot, (unsigned long long)GA(buf + 0x1234));
    callstart(); *slot = GA(buf + 8); slot_mode = 2; ctx->rdi = GA(slot); shim_slot(ctx);
    MT_EXPECT(*slot == 0, "callee-cleared slot is 0x%llx", (unsigned long long)*slot);
    callstart(); ctx->rdi = 0; slot_mode = 0; r_p[0] = (void *)0x77; shim_slot(ctx);
    MT_EXPECT(r_p[0] == (void *)0x1, "NULL slot pointer: callee saw %p", r_p[0]);
  }
  mt_end();

  // ---- struct arrays ----
  mt_begin("thunk." MODE ".struct_array_bounds_and_nested_pointers", "thunk");
  { uint64_t *iov = (uint64_t *)(buf + 0x1000);
    iov[0] = GA(buf + 0x2000); iov[1] = 77; iov[2] = 0; iov[3] = 5;
    callstart(); ctx->rdi = GA(iov); ctx->rsi = 2; shim_iov(ctx);
    MT_EXPECT(iov_base0 == (void *)(buf + 0x2000) && iov_len0 == 77, "iov[0] = {%p, %zu}", iov_base0, iov_len0);
    MT_EXPECT(iov_base1 == NULL, "iov[1].iov_base NULL became %p", iov_base1);
    iov_calls = iov_null = 0;
    int64_t counts[] = {0, -1, 1025, 0x7FFFFFFF, (int64_t)0xFFFFFFFF00000000LL, 1024};
    for (size_t i = 0; i < sizeof counts / sizeof *counts; i++) {
      iov_null = 0; callstart(); ctx->rdi = GA(iov); ctx->rsi = (uint64_t)counts[i]; shim_iov(ctx);
      int n = (int)counts[i];
#if !NPS5_ADDR_MODE_IDENTITY
      int ok = (n > 0 && n <= 1024) ? 1 : 0;
      MT_EXPECT(ok ? iov_null == 0 : iov_null == 1, "count %lld: %s", (long long)counts[i], ok ? "valid count rejected" : "out-of-range count reached the callee with an array");
      (void)n;
#endif
    }
    callstart(); ctx->rdi = 0; ctx->rsi = 2; iov_null = 0; shim_iov(ctx);
    MT_EXPECT(iov_null == 1, "NULL array pointer reached the callee as non-NULL");
#if !NPS5_ADDR_MODE_IDENTITY
    // an array that is not readable is marked for the callee (EFAULT), not dereferenced by the thunk
    iov_calls = 0; iov_null = 0; callstart(); ctx->rdi = BUFSZ - 8; ctx->rsi = 2; shim_iov(ctx);
    MT_EXPECT(iov_calls == 1 && iov_bad == 1, "unreadable iovec array: calls=%d bad=%d", iov_calls, iov_bad);
#endif
  }
  mt_end();

  // ---- guest state the thunk must not disturb ----
  mt_begin("thunk." MODE ".guest_callee_saved_state_preserved", "thunk");
  ctx->rbx = 0xB1; ctx->rbp = 0xB2; ctx->r12 = 0xC1; ctx->r13 = 0xC2; ctx->r14 = 0xC3; ctx->r15 = 0xC4; ctx->xmm[8].u64[0] = 0xF8; ctx->fs_base = 0xF5; ctx->mxcsr = 0x1f80; ctx->df = 0;
  callstart(); ctx->rdi = 1; shim_ints(ctx);
  MT_EXPECT(ctx->rbx == 0xB1 && ctx->rbp == 0xB2 && ctx->r12 == 0xC1 && ctx->r13 == 0xC2 && ctx->r14 == 0xC3 && ctx->r15 == 0xC4, "callee-saved GPRs changed");
  MT_EXPECT(ctx->xmm[8].u64[0] == 0xF8 && ctx->fs_base == 0xF5 && ctx->mxcsr == 0x1f80 && ctx->df == 0, "xmm/fs/mxcsr/df changed");
  mt_end();

#ifdef WITH_DOUBLE
  // The descriptors below cannot be generated by an implementation that only knows integer
  // classes; thunkgen is required to reject them (SPEC 4.2: "fails the build on any export it
  // cannot classify") or to marshal them from xmm registers.
  mt_begin("thunk." MODE ".double_arg_in_xmm0", "thunk");
  callstart(); ctx->xmm[0].f64[0] = 3.5; ctx->rdi = 42; ctx->rsi = 7; r_d[0] = 0; shim_dbl(ctx);
  MT_EXPECT(r_d[0] == 3.5 && r_l[0] == 7, "thunkgen accepts a 'double' parameter and marshals it as (double)rdi" " [" "got d=%g i=%ld" "]", r_d[0], r_l[0]);
  mt_end();
  mt_begin("thunk." MODE ".float_arg_in_xmm0", "thunk");
  callstart(); ctx->xmm[0].f32[0] = 1.5f; ctx->rdi = 9; r_d[0] = 0; shim_float(ctx);
  MT_EXPECT(r_d[0] == 1.5, "thunkgen accepts a 'float' parameter and marshals it as (float)rdi" " [" "got %g" "]", r_d[0]);
  mt_end();
  mt_begin("thunk." MODE ".nine_double_args_stack_overflow", "thunk");
  callstart(); for (int i = 0; i < 8; i++) ctx->xmm[i].f64[0] = 10 + i; ctx->rdi = 99; { double nine = 99.25; memcpy(buf + 0x80000 + 8, &nine, 8); }
  r_d[0] = r_d[7] = r_d[8] = 0; shim_dbl9(ctx);
  MT_EXPECT(r_d[0] == 10 && r_d[7] == 17 && r_d[8] == 99.25 && r_l[0] == 99, "9 double arguments (8 in xmm0-7, 1 on the stack) are not marshalled" " [" "got %g %g %g int=%ld" "]", r_d[0], r_d[7], r_d[8], r_l[0]);
  mt_end();
  mt_begin("thunk." MODE ".double_return_in_xmm0", "thunk");
  callstart(); ctx->xmm[0].f64[0] = 0; ctx->rax = 0; shim_dret(ctx);
  MT_EXPECT(ctx->xmm[0].f64[0] == 2.5, "a double result is put in rax as an integer instead of xmm0" " [" "xmm0=%g rax=0x%llx" "]", ctx->xmm[0].f64[0], (unsigned long long)ctx->rax);
  mt_end();
#endif
  return mt_status();
}
