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

#ifdef NPS5_UE5_DEV_PROBES
#include <CommonCrypto/CommonDigest.h>
#endif
#include <pthread.h>
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

// ---- the title's decoder as the reader's decoder ----

static GuestContext *g_process;

void nps5_ue5_set_process(GuestContext *process) {
    if (process && !g_process) g_process = process->process_ctx ? process->process_ctx : process;
}

// Every touch of the guest (contexts, guest memory, the guest call, the decoder address check) goes through these, so that a
// unit test can stand in for the guest. recomp_vm_alloc reports a failure as (uint64_t)-1, never 0.
#define NPS5_VM_FAIL ((uint64_t)-1)

static uint64_t ops_vm_alloc(GuestContext *p, size_t n) { return recomp_vm_alloc(p, n); }
static void ops_vm_free(GuestContext *p, uint64_t a, size_t n) { recomp_vm_free(p, a, n); }
static uint64_t ops_call(GuestContext *c, uint64_t t, const uint64_t *a, unsigned n) {
    return nps5_hostcall_run_n(c, t, a, n);
}
static void *ops_g2h(GuestContext *p, uint64_t g) { return NPS5_G2H(p, g); }
// The decoder must be translated code (an entry in the dispatch table): a call to any other address would abort the process.
static int ops_decoder_ok(uint64_t v) {
    if (!v || v >= (1ULL << 48)) return 0;
    recomp_fn_t **l2 = g_dispatch_l1[(v >> DISPATCH_L1_SHIFT) & DISPATCH_LEVEL_MASK];
    if (!l2) return 0;
    recomp_fn_t *l3 = l2[(v >> DISPATCH_L2_SHIFT) & DISPATCH_LEVEL_MASK];
    return l3 && l3[v & DISPATCH_LEVEL_MASK];
}

static const Nps5Ue5GuestOps g_real_ops = {
    nps5_hostctx_create, nps5_hostctx_destroy, ops_vm_alloc, ops_vm_free, ops_call, ops_g2h, ops_decoder_ok};
static const Nps5Ue5GuestOps *g_ops = &g_real_ops;

void nps5_ue5_set_guest_ops_for_test(const Nps5Ue5GuestOps *ops, GuestContext *process) {
    g_ops = ops ? ops : &g_real_ops;
    g_process = process;
}

#define UE5_DEC_MEM (8ULL << 20)
#define UE5_DEC_STACK (1ULL << 20)
#define UE5_ABI_OODLELZ14 1u

// OodleLZ_Decompress(comp, compLen, raw, rawLen, fuzzSafe=1, checkCRC=1, verbosity=0, decBufBase=0, decBufSize=0,
//                    fpCallback=0, callbackUserData=0, decoderMemory, decoderMemorySize, threadPhase=3): abi 1
static void ue5_oodle_args(uint64_t a[14], uint64_t comp, uint64_t comp_len, uint64_t raw, uint64_t raw_len,
                           uint64_t mem, uint64_t mem_len) {
    const uint64_t t[14] = {comp, comp_len, raw, raw_len, 1, 1, 0, 0, 0, 0, 0, mem, mem_len, 3};
    memcpy(a, t, sizeof t);
}

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
    d->ctx = g_ops->ctx_create(proc, UE5_DEC_STACK);
    d->g_mem = d->ctx ? g_ops->vm_alloc(proc, UE5_DEC_MEM) : NPS5_VM_FAIL;
    if (!d->ctx || d->g_mem == NPS5_VM_FAIL) {
        if (d->ctx) g_ops->ctx_destroy(d->ctx);
        free(d);
        return NULL;
    }
    return d;
}

static int gdec_grow(GuestDec *d, uint64_t *addr, uint64_t *cap, uint64_t need) {
    if (*cap >= need) return 0;
    uint64_t nc = (need + 0xffff) & ~0xffffULL;
    // Geometric: a stream of slowly growing groups costs O(log) allocations, not one per group (bounded at 256 MiB).
    if (*cap && *cap * 2 > nc && *cap * 2 <= (256ull << 20)) nc = *cap * 2;
    const uint64_t na = g_ops->vm_alloc(d->proc, (size_t)nc + 4096);
    if (na == NPS5_VM_FAIL) return -1;
    if (*addr) g_ops->vm_free(d->proc, *addr, (size_t)*cap + 4096);
    *addr = na;
    *cap = nc;
    return 0;
}

static const uint8_t *gdec_decode(void *thread, const uint8_t *comp, uint32_t comp_len, uint32_t raw_len) {
    GuestDec *d = thread;
    if (gdec_grow(d, &d->g_comp, &d->comp_cap, comp_len) || gdec_grow(d, &d->g_raw, &d->raw_cap, raw_len)) return NULL;
    memcpy(g_ops->g2h(d->proc, d->g_comp), comp, comp_len);
    uint64_t args[14];
    ue5_oodle_args(args, d->g_comp, comp_len, d->g_raw, raw_len, d->g_mem, UE5_DEC_MEM);
    const uint64_t got = g_ops->call(d->ctx, d->decoder, args, 14);
    if (got != raw_len) return NULL;
    return g_ops->g2h(d->proc, d->g_raw);
}

static void gdec_close(void *thread) {
    GuestDec *d = thread;
    if (d->g_comp) g_ops->vm_free(d->proc, d->g_comp, (size_t)d->comp_cap + 4096);
    if (d->g_raw) g_ops->vm_free(d->proc, d->g_raw, (size_t)d->raw_cap + 4096);
    g_ops->vm_free(d->proc, d->g_mem, UE5_DEC_MEM);
    g_ops->ctx_destroy(d->ctx);
    free(d);
}

int nps5_ue5_extract_groups(const KytyGpuUe5Query *q, KytyGpuUe5Result *out) {
    if (!q || !out || !g_process || q->abi != UE5_ABI_OODLELZ14 || !q->decoder ||
        q->struct_size < KYTY_GPU_UE5_QUERY_SIZE_BASE)
        return -1;
    if (!g_ops->decoder_ok(q->decoder)) {
        fprintf(stderr, "[ue5code] decoder 0x%llx is not translated guest code: shader code source off\n",
                (unsigned long long)q->decoder);
        return -1;
    }
    GuestDecShared sh = {q->decoder};
    const Nps5Ue5Decoder dec = {&sh, gdec_open, gdec_decode, gdec_close};
    return nps5_ue5_extract(nps5_ue5_vfs_io(), &dec, q, out);
}

// Developer probes (NPS5_UE5_EXTRACT_TEST, NPS5_UE5_EXTRACT_TEST_CODE, NPS5_UE5_SELFTEST) dump decoded shader code to files.
// They exist only in a build made with -DNPS5_UE5_DEV_PROBES (off by default): the shipped binary cannot dump a title's code.
#ifdef NPS5_UE5_DEV_PROBES
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

// The split constants below (marker "1234", u32 0x18, offsets -0x14 and -0x0c) belong to Kyty's ue5Group.h; this is a probe only.
// NPS5_UE5_EXTRACT_TEST=<decoder hex>:<out tsv>[:<threads>]: reads the whole shader library before the guest entry through
// the same path as the ABI 1.8 entry, splits every group at the AGC shader marker ("1234", u32 0x18) and writes one line per
// shader (library, group, index, stage, sha1 of the code, code bytes) to the file, then logs the totals and exits. For
// verification on a box; off when unset.
typedef struct ProbeCtx {
    FILE *out, *code;
    uint64_t code_off;
    pthread_mutex_t mu;
    uint64_t shaders, bad_split, nocode, code_bytes;
} ProbeCtx;

static int probe_group(void *arg, const KytyGpuUe5Group *g) {
    ProbeCtx *p = arg;
    static const char tag[8] = {'1', '2', '3', '4', 0x18, 0, 0, 0};
    const uint8_t *d = g->bytes;
    uint32_t m[4096], nm = 0;
    for (uint32_t i = 0; i + 8 <= g->raw_bytes && nm < 4095; i++)
        if (d[i] == '1' && memcmp(d + i, tag, 8) == 0) m[nm++] = i;
    m[nm] = g->raw_bytes;
    pthread_mutex_lock(&p->mu);
    if (nm != g->shader_count) p->bad_split++;
    for (uint32_t j = 0; j < nm && j < g->shader_count; j++) {
        uint32_t st = 0, sz = 0;
        if (m[j] >= 0x14) memcpy(&st, d + m[j] - 0x14, 4);
        if (m[j] >= 0x0c) memcpy(&sz, d + m[j] - 0x0c, 4);
        const uint64_t e = (uint64_t)st + sz;
        const uint32_t slen = m[j + 1] - m[j];
        if (e > slen || e <= st) {
            p->nocode++;
            continue;
        }
        unsigned char dg[CC_SHA1_DIGEST_LENGTH];
        CC_SHA1(d + m[j] + st, (CC_LONG)(e - st), dg);
        fprintf(p->out, "%u\t%u\t%u\t%u\t", g->library, g->group, j, g->frequencies[j]);
        for (int k = 0; k < CC_SHA1_DIGEST_LENGTH; k++)
            fprintf(p->out, "%02x", dg[k]);
        fprintf(p->out, "\t%llu\t%llu\n", (unsigned long long)(e - st), (unsigned long long)p->code_off);
        if (p->code) fwrite(d + m[j] + st, 1, (size_t)(e - st), p->code);
        p->code_off += e - st;
        p->shaders++;
        p->code_bytes += e - st;
    }
    pthread_mutex_unlock(&p->mu);
    return 0;
}

static void probe_maybe(GuestContext *process) {
    const char *spec = getenv("NPS5_UE5_EXTRACT_TEST");
    if (!spec || !*spec) return;
    char buf[2048];
    snprintf(buf, sizeof buf, "%s", spec);
    char *outp = strchr(buf, ':');
    if (!outp) exit(2);
    *outp++ = 0;
    char *thr = strchr(outp, ':');
    if (thr) *thr++ = 0;
    ProbeCtx pc = {.out = fopen(outp, "w")};
    if (getenv("NPS5_UE5_EXTRACT_TEST_CODE")) pc.code = fopen(getenv("NPS5_UE5_EXTRACT_TEST_CODE"), "wb");
    pthread_mutex_init(&pc.mu, NULL);
    if (!pc.out) exit(2);
    const uint64_t decoder = strtoull(buf, NULL, 16);
    if (!ops_decoder_ok(decoder)) exit(2);
    const uint64_t fp = ue5_fnv64((const uint8_t *)NPS5_G2H(process, decoder), 64);
    KytyGpuUe5Query q = {.struct_size = sizeof q,
                         .abi = 1,
                         .decoder = decoder,
                         .threads = thr ? (uint32_t)atoi(thr) : 2,
                         .group = probe_group,
                         .arg = &pc};
    KytyGpuUe5Result r;
    const double t0 = ue5_now_ms();
    const int rc = nps5_ue5_extract_groups(&q, &r);
    fclose(pc.out);
    if (pc.code) fclose(pc.code);
    char logp[2200];
    snprintf(logp, sizeof logp, "%s.summary", outp);
    FILE *lf = fopen(logp, "w");
    if (lf) {
        fprintf(lf,
                "rc %d containers %u libraries %u groups %u decoded %u failed %u skipped %u comp %llu raw %llu ms %llu "
                "shaders %llu code_bytes %llu split_mismatch %llu nocode %llu\n",
                rc, r.containers, r.libraries, r.groups_total, r.groups_decoded, r.groups_failed, r.groups_skipped,
                (unsigned long long)r.compressed_bytes, (unsigned long long)r.raw_bytes, (unsigned long long)r.ms,
                (unsigned long long)pc.shaders, (unsigned long long)pc.code_bytes, (unsigned long long)pc.bad_split,
                (unsigned long long)pc.nocode);
        fclose(lf);
    }
    fprintf(stderr,
            "[ue5code] extract-test: decoder 0x%llx fp %016llx rc %d, %u containers, %u libraries, %u groups (%u "
            "decoded, %u failed, %u "
            "skipped), %llu -> %llu bytes in %llu ms (wall %.0f ms), threads %u, shaders %llu (%llu code bytes), split "
            "mismatches %llu, "
            "no-code %llu\n",
            (unsigned long long)decoder, (unsigned long long)fp, rc, r.containers, r.libraries, r.groups_total,
            r.groups_decoded, r.groups_failed, r.groups_skipped, (unsigned long long)r.compressed_bytes,
            (unsigned long long)r.raw_bytes, (unsigned long long)r.ms, ue5_now_ms() - t0, q.threads,
            (unsigned long long)pc.shaders, (unsigned long long)pc.code_bytes, (unsigned long long)pc.bad_split,
            (unsigned long long)pc.nocode);
    fflush(stderr);
    _exit(rc == 0 ? 0 : 3);
}

static void nps5_ue5_dev_probes(GuestContext *process) {
    probe_maybe(process);
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
    if (g_comp == NPS5_VM_FAIL || g_raw == NPS5_VM_FAIL || g_mem == NPS5_VM_FAIL) {
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
    uint64_t args[14];
    ue5_oodle_args(args, g_comp, (uint64_t)comp_len, g_raw, raw_len, g_mem, dec_mem_len);
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
#endif // NPS5_UE5_DEV_PROBES

void nps5_ue5_selftest_maybe(GuestContext *process) {
    nps5_ue5_set_process(process);
#ifdef NPS5_UE5_DEV_PROBES
    nps5_ue5_dev_probes(process);
#endif
}
