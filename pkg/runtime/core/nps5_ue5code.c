// SPDX-License-Identifier: GPL-2.0-or-later
//
// UE5 shader library code source (uewarmlist, generic for UE5 titles).
//
// A UE5 title keeps its shader code in the IoStore shader library: one Oodle block per shader group. We ship no
// shader code and no decoder: the title's own (translated) OodleLZ_Decompress decodes its own groups on a guest
// context the host owns, so the pipeline list can be compiled behind the loading card at start. The decoder's
// guest vaddr and a fingerprint of its first 64 bytes are per-title data (games/<id>/game.tsv `ue5` row).
//
// This file holds the host-owned contexts, the guest decoder binding of the reader (nps5_ue5core.c) and the WP0 self-test
// (NPS5_UE5_SELFTEST). The ABI 1.8 entry in nps5_kyty.c calls nps5_ue5_extract_groups.

#include "nps5_ue5code.h"
#include "nps5_hostcall.h"
#include "nps5_thunk.h"
#include "nps5_ue5core.h"
#include "recomp_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

GuestContext *nps5_hostctx_create(GuestContext *process, uint64_t stack_bytes) {
    if (!process) return NULL;
    GuestContext *proc = process->process_ctx ? process->process_ctx : process;
    return recomp_create_thread_context(proc, stack_bytes ? stack_bytes : (1ULL << 20));
}

void nps5_hostctx_destroy(GuestContext *ctx) { recomp_free_thread_context(ctx); }

static uint64_t ue5_fnv64(const uint8_t *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001b3ULL;
    return h;
}

static double ue5_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

// ---- the title's decoder as the reader's decoder ----

static GuestContext *g_process;

void nps5_ue5_set_process(GuestContext *process) {
    if (process && !g_process) g_process = process->process_ctx ? process->process_ctx : process;
}

#define UE5_DEC_MEM (8ULL << 20)
#define UE5_DEC_STACK (1ULL << 20)

typedef struct GuestDec {
    GuestContext *proc, *ctx;
    uint64_t decoder;
    uint64_t g_comp, comp_cap, g_raw, raw_cap, g_mem;
} GuestDec;

typedef struct GuestDecShared {
    uint64_t decoder;
} GuestDecShared;

static void *gdec_open(void *user, uint32_t index) {
    (void)index;
    const GuestDecShared *sh = user;
    GuestContext *proc = g_process;
    if (!proc) return NULL;
    GuestDec *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->proc = proc;
    d->decoder = sh->decoder;
    d->ctx = nps5_hostctx_create(proc, UE5_DEC_STACK);
    d->g_mem = recomp_vm_alloc(proc, UE5_DEC_MEM);
    if (!d->ctx || !d->g_mem) {
        if (d->g_mem) recomp_vm_free(proc, d->g_mem, UE5_DEC_MEM);
        if (d->ctx) nps5_hostctx_destroy(d->ctx);
        free(d);
        return NULL;
    }
    return d;
}

static int gdec_grow(GuestDec *d, uint64_t *addr, uint64_t *cap, uint64_t need) {
    if (*cap >= need) return 0;
    const uint64_t nc = (need + 0xffff) & ~0xffffULL;
    const uint64_t na = recomp_vm_alloc(d->proc, (size_t)nc + 4096);
    if (!na) return -1;
    if (*addr) recomp_vm_free(d->proc, *addr, (size_t)*cap + 4096);
    *addr = na;
    *cap = nc;
    return 0;
}

static const uint8_t *gdec_decode(void *thread, const uint8_t *comp, uint32_t comp_len, uint32_t raw_len) {
    GuestDec *d = thread;
    if (gdec_grow(d, &d->g_comp, &d->comp_cap, comp_len) || gdec_grow(d, &d->g_raw, &d->raw_cap, raw_len)) return NULL;
    memcpy(NPS5_G2H(d->proc, d->g_comp), comp, comp_len);
    // OodleLZ_Decompress(comp, compLen, raw, rawLen, fuzzSafe=1, checkCRC=1, verbosity=0, decBufBase=0, decBufSize=0,
    //                    fpCallback=0, callbackUserData=0, decoderMemory, decoderMemorySize, threadPhase=3)
    const uint64_t args[14] = {d->g_comp, comp_len, d->g_raw, raw_len, 1, 1, 0, 0, 0, 0, 0, d->g_mem, UE5_DEC_MEM, 3};
    const uint64_t got = nps5_hostcall_run_n(d->ctx, d->decoder, args, 14);
    if (got != raw_len) return NULL;
    return NPS5_G2H(d->proc, d->g_raw);
}

static void gdec_close(void *thread) {
    GuestDec *d = thread;
    if (d->g_comp) recomp_vm_free(d->proc, d->g_comp, (size_t)d->comp_cap + 4096);
    if (d->g_raw) recomp_vm_free(d->proc, d->g_raw, (size_t)d->raw_cap + 4096);
    recomp_vm_free(d->proc, d->g_mem, UE5_DEC_MEM);
    nps5_hostctx_destroy(d->ctx);
    free(d);
}

int nps5_ue5_extract_groups(const KytyGpuUe5Query *q, KytyGpuUe5Result *out) {
    if (!q || !out || !g_process || q->abi != 1 || !q->decoder || q->struct_size < sizeof *q) return -1;
    GuestDecShared sh = {q->decoder};
    const Nps5Ue5Decoder dec = {&sh, gdec_open, gdec_decode, gdec_close};
    return nps5_ue5_extract(nps5_ue5_vfs_io(), &dec, q, out);
}

void nps5_ue5_selftest_maybe(GuestContext *process) {
    nps5_ue5_set_process(process);
    const char *spec = getenv("NPS5_UE5_SELFTEST");
    if (!spec || !*spec || !process) return;
    char buf[2048];
    snprintf(buf, sizeof buf, "%s", spec);
    char *f[4] = {0};
    char *p = buf;
    for (int i = 0; i < 4 && p; i++) {
        f[i] = p;
        p = strchr(p, ':');
        if (p) *p++ = '\0';
    }
    if (!f[3]) {
        fprintf(stderr, "[ue5code] selftest: bad spec (decoder:in:raw:out)\n");
        exit(2);
    }
    const uint64_t decoder = strtoull(f[0], NULL, 16);
    const uint64_t raw_len = strtoull(f[2], NULL, 10);
    GuestContext *proc = process->process_ctx ? process->process_ctx : process;
    FILE *in = fopen(f[1], "rb");
    if (!in) {
        fprintf(stderr, "[ue5code] selftest: cannot open input\n");
        exit(2);
    }
    fseek(in, 0, SEEK_END);
    const long comp_len = ftell(in);
    fseek(in, 0, SEEK_SET);
    const uint64_t dec_mem_len = 8ULL << 20;
    const uint64_t g_comp = recomp_vm_alloc(proc, (size_t)comp_len + 4096);
    const uint64_t g_raw = recomp_vm_alloc(proc, (size_t)raw_len + (1 << 16));
    const uint64_t g_mem = recomp_vm_alloc(proc, (size_t)dec_mem_len);
    if (!g_comp || !g_raw || !g_mem) {
        fprintf(stderr, "[ue5code] selftest: guest alloc failed\n");
        exit(2);
    }
    if (fread(NPS5_G2H(proc, g_comp), 1, (size_t)comp_len, in) != (size_t)comp_len) {
        fprintf(stderr, "[ue5code] selftest: short read\n");
        exit(2);
    }
    fclose(in);
    const uint64_t fp = ue5_fnv64((const uint8_t *)NPS5_G2H(proc, decoder), 64);
    GuestContext *hc = nps5_hostctx_create(proc, 1ULL << 20);
    if (!hc) {
        fprintf(stderr, "[ue5code] selftest: context create failed\n");
        exit(2);
    }
    // OodleLZ_Decompress(comp, compLen, raw, rawLen, fuzzSafe=1, checkCRC=1, verbosity=0, decBufBase=0, decBufSize=0,
    //                    fpCallback=0, callbackUserData=0, decoderMemory, decoderMemorySize, threadPhase=3)
    const uint64_t args[14] = {g_comp, (uint64_t)comp_len, g_raw, raw_len, 1, 1, 0, 0, 0, 0, 0, g_mem, dec_mem_len, 3};
    const double t0 = ue5_now_ms();
    const uint64_t got = nps5_hostcall_run_n(hc, decoder, args, 14);
    const double t1 = ue5_now_ms();
    const uint64_t out_fnv = ue5_fnv64((const uint8_t *)NPS5_G2H(proc, g_raw), (size_t)raw_len);
    FILE *out = fopen(f[3], "wb");
    if (out) {
        fwrite(NPS5_G2H(proc, g_raw), 1, (size_t)raw_len, out);
        fclose(out);
    }
    fprintf(
        stderr,
        "[ue5code] selftest: decoder 0x%llx fp %016llx: %ld -> %lld bytes (want %llu) in %.2f ms, out fnv %016llx\n",
        (unsigned long long)decoder, (unsigned long long)fp, comp_len, (long long)got, (unsigned long long)raw_len,
        t1 - t0, (unsigned long long)out_fnv);
    fflush(stderr);
    nps5_hostctx_destroy(hc);
    _exit(got == raw_len ? 0 : 3);
}
