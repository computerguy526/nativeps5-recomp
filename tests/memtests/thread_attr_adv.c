// SPDX-License-Identifier: GPL-2.0-or-later
//
// scePthreadAttrGet / Getstack / Getstackaddr / Getstacksize through the real shims (vmh.h fake register file): the
// attributes of a running thread report its usable stack, the bounds a conservative garbage collector scans (IL2CPP's
// Boehm GC in the Unity PS5 player). Built in base and identity mode; needs kernel/threading/ps4_threading.c.
#include <stdlib.h>
#include "vmh.h"
#include "ps4_threading.h"

#define AREA "thread"
#define N(s) "thread." VMH_MODE "." s

void shim_scePthreadAttrInit(GuestContext *);
void shim_scePthreadAttrDestroy(GuestContext *);
void shim_scePthreadAttrGet(GuestContext *);
void shim_scePthreadAttrGetstack(GuestContext *);
void shim_scePthreadAttrGetstackaddr(GuestContext *);
void shim_scePthreadAttrGetstacksize(GuestContext *);

static uint32_t call2(void (*fn)(GuestContext *), uint64_t a, uint64_t b) {
    setrsp();
    ctx->rdi = a;
    ctx->rsi = b;
    fn(ctx);
    return (uint32_t)ctx->rax;
}
static uint32_t call3(void (*fn)(GuestContext *), uint64_t a, uint64_t b, uint64_t c) {
    setrsp();
    ctx->rdi = a;
    ctx->rsi = b;
    ctx->rdx = c;
    fn(ctx);
    return (uint32_t)ctx->rax;
}

static void t_self_stack(void) {
    mt_begin(N("attr_get_reports_the_running_stack"), AREA);
    const uint64_t attr = arg_slot + 0x100, addr = arg_slot + 0x200, size = arg_slot + 0x208, addr2 = arg_slot + 0x210;
    *(uint64_t *)P(addr) = 0x1111;
    *(uint64_t *)P(size) = 0x2222;
    *(uint64_t *)P(addr2) = 0x3333;
    MT_EXPECT(call2(shim_scePthreadAttrInit, attr, 0) == 0, "AttrInit");
    RecompThread *self = recomp_get_current_thread();
    MT_EXPECT(self != NULL, "no current thread");
    MT_EXPECT(call2(shim_scePthreadAttrGet, self ? (uint64_t)(uintptr_t)self : 0, attr) == 0, "AttrGet(self)");
    MT_EXPECT(call3(shim_scePthreadAttrGetstack, attr, addr, size) == 0, "AttrGetstack");
    MT_EXPECT(call2(shim_scePthreadAttrGetstackaddr, attr, addr2) == 0, "AttrGetstackaddr");
    const uint64_t lo = *(uint64_t *)P(addr), len = *(uint64_t *)P(size);
    MT_EXPECT(lo == stk_lo && lo + len == stk_top, "main stack reported as [0x%llx, 0x%llx), want [0x%llx, 0x%llx)",
              (unsigned long long)lo, (unsigned long long)(lo + len), (unsigned long long)stk_lo,
              (unsigned long long)stk_top);
    MT_EXPECT(*(uint64_t *)P(addr2) == lo, "Getstackaddr 0x%llx differs from Getstack",
              (unsigned long long)*(uint64_t *)P(addr2));
    MT_EXPECT(ctx->rsp > lo && ctx->rsp < lo + len, "rsp 0x%llx outside the reported stack",
              (unsigned long long)ctx->rsp);
    *(uint64_t *)P(size) = 0;
    MT_EXPECT(call2(shim_scePthreadAttrGetstacksize, attr, size) == 0 && *(uint64_t *)P(size) == len, "Getstacksize");
    MT_EXPECT(call2(shim_scePthreadAttrGet, 0x7fffffff0ULL, attr) == 0x80020003u, "unknown thread: ESRCH");
    MT_EXPECT(call2(shim_scePthreadAttrGet, 0, 0) == 0x80020016u, "NULL attr: EINVAL");
    MT_EXPECT(call3(shim_scePthreadAttrGetstack, attr, 0, size) == 0x80020016u, "Getstack NULL out: EINVAL");
    call2(shim_scePthreadAttrDestroy, attr, 0);
    mt_end();
}

int main(void) {
    setenv("NPS5_PTHREAD_ATTR_REAL", "1", 1); // the real shims are opt-in (Unity profile)
    vmh_init();
    t_self_stack();
    return mt_status();
}
