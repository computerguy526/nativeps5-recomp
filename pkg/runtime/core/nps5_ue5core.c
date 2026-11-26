// SPDX-License-Identifier: GPL-2.0-or-later
//
// UE5 shader library reader: IoStore table of contents (v5..v8 layout), the shader code library chunks (type 8) and their
// shader groups (type 9 chunks), decoded on N threads through a pluggable decoder. See nps5_ue5core.h.
//
// Container layout read here (little endian unless noted):
//   .utoc header 144 B: magic "-==--==--==--==-", u8 version, 3 reserved, u32 header size, u32 entry count, u32 compressed block
//     count, u32 block entry size (12), u32 method name count, u32 method name length (32), u32 block size, u32 directory index
//     size, u32 partition count, u64 container id, 16 B key guid, u8 container flags (1 compressed, 2 encrypted, 4 signed), 3
//     reserved, u32 perfect hash seed count (offset 84), u64 partition size, u32 chunks without perfect hash, ...
//   then chunk ids (12 B each: u64 id, u16 index (big endian), u8 pad, u8 type), chunk offset/length (5 B big endian + 5 B big
//   endian, in uncompressed container space), the perfect hash seeds (u32) and overflow indices (u32), the compression blocks
//   (12 B: 40 bit offset, 24 bit compressed size, 24 bit uncompressed size, u8 method; 0 = none), the method names.
//   .ucas holds the blocks (partition p > 0: <name>_s<p>.ucas, partition_size bytes each).
// Shader library chunk (type 8): u32 version, then arrays (each u32 count + elements): shader map hashes (20 B), shader hashes
//   (20 B), group chunk ids (12 B), shader map entries (8 B), shader entries (8 B: low 4 bits of the first word = stage),
//   group entries (16 B: shader indices offset, shader count, uncompressed size, compressed size), shader indices (u32).
// A group's chunk holds the group compressed (the library's compressed size) or, when compressed == uncompressed, raw.

#include "nps5_ue5core.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TOC_HEADER_MIN 144u
#define MAX_PARTITIONS 64u
#define MAX_TOC_BYTES (1ull << 31)
#define MAX_LIBRARY_BYTES (256ull << 20)
#define DEFAULT_MAX_RAW (64u << 20)

// The version table. The library header version (u32 at offset 0 of the library chunk) is 1 in the shipped PS5 builds of the two
// UE5 titles this was written for; the array layout was checked on both (every shader and group accounted for).
static const Nps5Ue5Variant g_variants[] = {
    {"ue5-ps5-iostore", 5, 8, 1, 1, 8, 9, {20, 20, 12, 8, 8, 16, 4}, 1},
};

const Nps5Ue5Variant *nps5_ue5_variants(unsigned *count) {
    if (count) *count = (unsigned)(sizeof g_variants / sizeof *g_variants);
    return g_variants;
}

static const Nps5Ue5Variant *variant_for_toc(uint32_t v) {
    for (size_t i = 0; i < sizeof g_variants / sizeof *g_variants; i++)
        if (v >= g_variants[i].toc_min && v <= g_variants[i].toc_max) return &g_variants[i];
    return NULL;
}

static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static uint64_t rd64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}
static uint64_t be40(const uint8_t *p) {
    return ((uint64_t)p[0] << 32) | ((uint64_t)p[1] << 24) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 8) | p[4];
}

typedef struct Container {
    const Nps5Ue5Variant *var;
    char base[1024]; // path without ".utoc"
    uint32_t block_size, nblocks, partitions;
    uint64_t partition_size;
    uint8_t *blocks; // nblocks * 12
    // chunk lookup for type 9: open addressing over {id, offset, length}
    uint32_t tab_mask;
    struct Slot {
        uint8_t id[12];
        uint8_t used;
        uint64_t off;
        uint32_t len;
    } *tab;
    void *files[MAX_PARTITIONS];
    pthread_mutex_t mu;
} Container;

typedef struct Lib {
    Container *c;
    uint32_t index;
    uint8_t *data;
    uint32_t nse, nge, ni;
    const uint8_t *gids, *se, *ge, *idx;
    uint32_t ngroups;
} Lib;

typedef struct Job {
    const Lib *lib;
    uint32_t group;
    uint64_t off;
    uint32_t len;
} Job;

typedef struct Env {
    const Nps5Ue5Io *io;
    const Nps5Ue5Decoder *dec;
    unsigned logged;
    const KytyGpuUe5Query *q;
    uint32_t max_raw;
    Container **cont;
    size_t ncont, capcont;
    Lib *libs;
    size_t nlibs, caplibs;
    Job *jobs;
    size_t njobs, capjobs;
    _Atomic size_t next;
    _Atomic uint32_t done, decoded, failed, skipped;
    _Atomic uint64_t comp_bytes, raw_bytes;
    _Atomic int cancel;
    pthread_mutex_t prog_mu;
    uint32_t total;
} Env;

#define LOG_LIMIT 12
static void ulog(Env *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void ulog(Env *e, const char *fmt, ...) {
    if (e->logged++ >= LOG_LIMIT) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("[ue5code] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

// ---- container file access ----

static int read_full(const Nps5Ue5Io *io, void *file, void *buf, size_t n, uint64_t off) {
    uint8_t *b = buf;
    while (n) {
        ssize_t r = io->pread(io->user, file, b, n, off);
        if (r <= 0) return -1;
        b += r;
        off += (uint64_t)r;
        n -= (size_t)r;
    }
    return 0;
}

static void *partition_file(Env *e, Container *c, uint32_t p) {
    if (p >= MAX_PARTITIONS || p >= c->partitions) return NULL;
    pthread_mutex_lock(&c->mu);
    if (!c->files[p]) {
        char path[1100];
        if (p == 0)
            snprintf(path, sizeof path, "%s.ucas", c->base);
        else
            snprintf(path, sizeof path, "%s_s%u.ucas", c->base, p);
        c->files[p] = e->io->open(e->io->user, path);
    }
    void *f = c->files[p];
    pthread_mutex_unlock(&c->mu);
    return f;
}

// n bytes of container (compressed block space) offset off, across partition files.
static int container_read(Env *e, Container *c, uint64_t off, uint8_t *buf, size_t n) {
    while (n) {
        uint32_t p = 0;
        uint64_t in = off;
        size_t take = n;
        if (c->partitions > 1 && c->partition_size) {
            p = (uint32_t)(off / c->partition_size);
            in = off % c->partition_size;
            if (take > c->partition_size - in) take = (size_t)(c->partition_size - in);
        }
        void *f = partition_file(e, c, p);
        if (!f || read_full(e->io, f, buf, take, in) != 0) return -1;
        buf += take;
        off += take;
        n -= take;
    }
    return 0;
}

// A chunk (offset/length in uncompressed container space) into buf. Only blocks without compression.
static int read_chunk(Env *e, Container *c, uint64_t off, uint64_t len, uint8_t *buf) {
    if (!len) return 0;
    const uint64_t bs = c->block_size;
    const uint64_t first = off / bs, last = (off + len - 1) / bs;
    if (last >= c->nblocks) return -1;
    uint64_t skip = off - first * bs, done = 0;
    for (uint64_t b = first; b <= last && done < len; b++) {
        const uint8_t *e12 = c->blocks + 12 * b;
        const uint64_t boff = rd64(e12) & 0xffffffffffull;
        const uint32_t usz = rd32(e12 + 8) & 0xffffff;
        const uint32_t csz = rd32(e12 + 5) & 0xffffff;
        const uint32_t method = e12[11];
        if (method != 0 || usz != csz || skip >= usz) return -1;
        size_t take = usz - (size_t)skip;
        if (take > len - done) take = (size_t)(len - done);
        if (container_read(e, c, boff + skip, buf + done, take) != 0) return -1;
        done += take;
        skip = 0;
    }
    return done == len ? 0 : -1;
}

// ---- table of contents ----

static uint32_t id_hash(const uint8_t *id) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 12; i++)
        h = (h ^ id[i]) * 0x100000001b3ull;
    return (uint32_t)(h ^ (h >> 32));
}

static void tab_put(Container *c, const uint8_t *id, uint64_t off, uint32_t len) {
    uint32_t i = id_hash(id) & c->tab_mask;
    while (c->tab[i].used) {
        if (memcmp(c->tab[i].id, id, 12) == 0) break;
        i = (i + 1) & c->tab_mask;
    }
    memcpy(c->tab[i].id, id, 12);
    c->tab[i].used = 1;
    c->tab[i].off = off;
    c->tab[i].len = len;
}

static const struct Slot *tab_get(const Container *c, const uint8_t *id) {
    if (!c->tab) return NULL;
    uint32_t i = id_hash(id) & c->tab_mask;
    while (c->tab[i].used) {
        if (memcmp(c->tab[i].id, id, 12) == 0) return &c->tab[i];
        i = (i + 1) & c->tab_mask;
    }
    return NULL;
}

static void container_free(Env *e, Container *c) {
    for (uint32_t i = 0; i < MAX_PARTITIONS; i++)
        if (c->files[i]) e->io->close(e->io->user, c->files[i]);
    free(c->blocks);
    free(c->tab);
    pthread_mutex_destroy(&c->mu);
    free(c);
}

static void lib_free(Lib *l) { free(l->data); }

static int vec_grow(void **p, size_t *cap, size_t need, size_t esz) {
    if (need <= *cap) return 0;
    size_t nc = *cap ? *cap * 2 : 16;
    while (nc < need)
        nc *= 2;
    void *np = realloc(*p, nc * esz);
    if (!np) return -1;
    *p = np;
    *cap = nc;
    return 0;
}

// Parses a library chunk into l (takes ownership of data). 0 = ok.
static int lib_parse(Lib *l, const Nps5Ue5Variant *var, uint8_t *data, uint32_t len, int *bad_version) {
    uint64_t o = 4;
    // arrays in order: map hashes (20), shader hashes (20), group ids (12), map entries (8), shader entries (8),
    // group entries (16), shader indices (4)
    const uint32_t *esz = var->elem;
    uint32_t cnt[7];
    const uint8_t *ptr[7];
    if (len < 8) return -1;
    const uint32_t lver = rd32(data);
    if (lver < var->lib_min || lver > var->lib_max) {
        *bad_version = (int)lver;
        return -1;
    }
    for (int i = 0; i < 7; i++) {
        if (o + 4 > len) return -1;
        cnt[i] = rd32(data + o);
        o += 4;
        if (o + (uint64_t)cnt[i] * esz[i] > len) return -1;
        ptr[i] = data + o;
        o += (uint64_t)cnt[i] * esz[i];
    }
    l->data = data;
    l->gids = ptr[2];
    l->se = ptr[4];
    l->ge = ptr[5];
    l->idx = ptr[6];
    l->ngroups = cnt[5] < cnt[2] ? cnt[5] : cnt[2];
    l->nse = cnt[4];
    l->nge = cnt[5];
    l->ni = cnt[6];
    return 0;
}

// Reads one container's TOC and its libraries; adds libs and jobs. Returns 1 container accepted, 0 skipped.
static int load_container(Env *e, const char *toc_path) {
    const Nps5Ue5Io *io = e->io;
    void *tf = io->open(io->user, toc_path);
    if (!tf) return 0;
    uint8_t h[TOC_HEADER_MIN];
    int ok = read_full(io, tf, h, sizeof h, 0) == 0 && memcmp(h, "-==--==--==--==-", 16) == 0;
    const uint32_t version = h[16], hsize = rd32(h + 20), nent = rd32(h + 24), nblk = rd32(h + 28),
                   blksz_entry = rd32(h + 32), namelen = rd32(h + 40), block_size = rd32(h + 44), parts = rd32(h + 52),
                   nseeds = rd32(h + 84), nover = rd32(h + 96);
    const uint8_t flags = h[80];
    const Nps5Ue5Variant *var = ok ? variant_for_toc(version) : NULL;
    if (ok && !var)
        ulog(e, "%s: IoStore TOC version %u is not in the version table (new UE version?): not read", toc_path,
             version);
    if (!ok || !var || hsize < TOC_HEADER_MIN || blksz_entry != 12 || (flags & 2) || !block_size ||
        block_size > (1u << 28) || !parts || parts > MAX_PARTITIONS || namelen > 256 || nent > (1u << 26) ||
        nblk > (1u << 28)) {
        io->close(io->user, tf);
        return 0;
    }
    const uint64_t part_size = rd64(h + 88);
    uint64_t need = (uint64_t)hsize + 12ull * nent + 10ull * nent + 4ull * nseeds + 4ull * nover + 12ull * nblk;
    if (need > MAX_TOC_BYTES) {
        io->close(io->user, tf);
        return 0;
    }
    uint8_t *toc = malloc((size_t)need);
    if (!toc || read_full(io, tf, toc, (size_t)need, 0) != 0) {
        free(toc);
        io->close(io->user, tf);
        return 0;
    }
    io->close(io->user, tf);

    Container *c = calloc(1, sizeof *c);
    if (!c) {
        free(toc);
        return 0;
    }
    c->var = var;
    pthread_mutex_init(&c->mu, NULL);
    snprintf(c->base, sizeof c->base, "%s", toc_path);
    size_t bl = strlen(c->base);
    if (bl > 5) c->base[bl - 5] = 0; // ".utoc"
    c->block_size = block_size;
    c->nblocks = nblk;
    c->partitions = parts;
    c->partition_size = part_size;
    const uint8_t *ids = toc + hsize;
    const uint8_t *offl = ids + 12ull * nent;
    c->blocks = malloc((size_t)(12ull * nblk) + 1);
    if (!c->blocks) {
        container_free(e, c);
        free(toc);
        return 0;
    }
    memcpy(c->blocks, offl + 10ull * nent + 4ull * nseeds + 4ull * nover, 12ull * nblk);
    uint32_t ngroup_chunks = 0;
    for (uint32_t i = 0; i < nent; i++)
        if (ids[12ull * i + 11] == var->chunk_group) ngroup_chunks++;
    uint32_t cap = 16;
    while (cap < ngroup_chunks * 2u)
        cap <<= 1;
    c->tab = calloc(cap, sizeof *c->tab);
    c->tab_mask = cap - 1;
    if (!c->tab) {
        container_free(e, c);
        free(toc);
        return 0;
    }
    for (uint32_t i = 0; i < nent; i++) {
        if (ids[12ull * i + 11] != var->chunk_group) continue;
        const uint8_t *ol = offl + 10ull * i;
        tab_put(c, ids + 12ull * i, be40(ol), (uint32_t)be40(ol + 5));
    }
    if (vec_grow((void **)&e->cont, &e->capcont, e->ncont + 1, sizeof *e->cont) != 0) {
        container_free(e, c);
        free(toc);
        return 0;
    }
    e->cont[e->ncont++] = c;
    // libraries
    for (uint32_t i = 0; i < nent; i++) {
        if (ids[12ull * i + 11] != var->chunk_library) continue;
        const uint8_t *ol = offl + 10ull * i;
        const uint64_t off = be40(ol), len = be40(ol + 5);
        if (!len || len > MAX_LIBRARY_BYTES) continue;
        uint8_t *buf = malloc((size_t)len);
        Lib l = {0};
        int bad_version = -1;
        if (!buf || read_chunk(e, c, off, len, buf) != 0 || lib_parse(&l, var, buf, (uint32_t)len, &bad_version) != 0) {
            if (buf && bad_version >= 0)
                ulog(e, "%s: shader library header version %d is not in the version table (new UE version?): not read",
                     toc_path, bad_version);
            else if (buf)
                ulog(e, "%s: shader library chunk unreadable or malformed: not read", toc_path);
            free(buf);
            continue;
        }
        l.c = c;
        l.index = (uint32_t)e->nlibs;
        if (e->nlibs == 0)
            ulog(e,
                 "%s: IoStore TOC version %u, shader library version %u, %u groups, %u shaders, %u indices (table row "
                 "%s)",
                 toc_path, version, rd32(buf), l.nge, l.nse, l.ni, var->name);
        if (vec_grow((void **)&e->libs, &e->caplibs, e->nlibs + 1, sizeof *e->libs) != 0) {
            free(buf);
            continue;
        }
        e->libs[e->nlibs++] = l;
    }
    free(toc);
    return 1;
}

static int collect_cb(void *arg, const char *path) {
    Env *e = arg;
    load_container(e, path);
    return 0;
}

// ---- decoding ----

enum { R_OK = 0, R_SKIP, R_FAIL_IO, R_FAIL_DEC };

typedef struct Thread {
    void *dec;
    uint8_t *comp;
    size_t comp_cap;
    uint32_t *freq;
    size_t freq_cap;
} Thread;

static int run_job(Env *e, Thread *t, const Job *j) {
    const Lib *l = j->lib;
    const uint8_t *g = l->ge + 16ull * j->group;
    const uint32_t go = rd32(g), n = rd32(g + 4), unc = rd32(g + 8), comp = rd32(g + 12);
    if (unc > e->max_raw) return R_SKIP;
    if (!comp || !unc || j->len != comp || comp > unc || (uint64_t)go + n > l->ni) return R_FAIL_IO;
    if (t->comp_cap < comp) {
        uint8_t *nb = realloc(t->comp, comp);
        if (!nb) return R_FAIL_IO;
        t->comp = nb;
        t->comp_cap = comp;
    }
    if (read_chunk(e, l->c, j->off, comp, t->comp) != 0) return R_FAIL_IO;
    const uint8_t *raw = t->comp;
    if (comp != unc) {
        raw = e->dec->decode(t->dec, t->comp, comp, unc);
        if (!raw) return R_FAIL_DEC;
    }
    if (t->freq_cap < n) {
        uint32_t *nf = realloc(t->freq, (size_t)n * 4);
        if (!nf) return R_FAIL_IO;
        t->freq = nf;
        t->freq_cap = n;
    }
    for (uint32_t k = 0; k < n; k++) {
        const uint32_t si = rd32(l->idx + 4ull * (go + k));
        t->freq[k] = si < l->nse ? rd32(l->se + 8ull * si) & 15 : 0;
    }
    atomic_fetch_add(&e->comp_bytes, comp);
    atomic_fetch_add(&e->raw_bytes, unc);
    KytyGpuUe5Group out = {.library = l->index,
                           .group = j->group,
                           .shader_count = n,
                           .raw_bytes = unc,
                           .frequencies = t->freq,
                           .bytes = raw};
    if (e->q->group(e->q->arg, &out)) atomic_store(&e->cancel, 1);
    return R_OK;
}

static void account(Env *e, int r) {
    if (r == R_OK)
        atomic_fetch_add(&e->decoded, 1);
    else if (r == R_SKIP)
        atomic_fetch_add(&e->skipped, 1);
    else
        atomic_fetch_add(&e->failed, 1);
    const uint32_t d = atomic_fetch_add(&e->done, 1) + 1;
    if (e->q->progress) {
        pthread_mutex_lock(&e->prog_mu);
        e->q->progress(e->q->arg, d, e->total);
        pthread_mutex_unlock(&e->prog_mu);
    }
}

static void thread_free(Env *e, Thread *t) {
    if (t->dec) e->dec->thread_close(t->dec);
    free(t->comp);
    free(t->freq);
    memset(t, 0, sizeof *t);
}

static void work_loop(Env *e, Thread *t) {
    for (;;) {
        if (atomic_load(&e->cancel)) return;
        const size_t i = atomic_fetch_add(&e->next, 1);
        if (i >= e->njobs) return;
        account(e, run_job(e, t, &e->jobs[i]));
    }
}

typedef struct Worker {
    Env *e;
    uint32_t index;
    pthread_t th;
    int started;
} Worker;

static void *worker_main(void *arg) {
    Worker *w = arg;
    Thread t = {0};
    t.dec = w->e->dec->thread_open(w->e->dec->user, w->index);
    if (t.dec) work_loop(w->e, &t);
    thread_free(w->e, &t);
    return NULL;
}

int nps5_ue5_extract(const Nps5Ue5Io *io, const Nps5Ue5Decoder *dec, const KytyGpuUe5Query *q, KytyGpuUe5Result *out) {
    if (!io || !dec || !q || !out || !q->group || !io->list || !io->open || !io->pread || !io->close) return -1;
    memset(out, 0, sizeof *out);
    const double t0 = now_ms();
    Env *e = calloc(1, sizeof *e);
    if (!e) return -1;
    e->io = io;
    e->dec = dec;
    e->q = q;
    e->max_raw = q->max_raw_bytes ? q->max_raw_bytes : DEFAULT_MAX_RAW;
    pthread_mutex_init(&e->prog_mu, NULL);
    int rc = 0;
    io->list(io->user, collect_cb, e);
    out->containers = (uint32_t)e->ncont;
    out->libraries = (uint32_t)e->nlibs;
    for (size_t li = 0; li < e->nlibs; li++) {
        const Lib *l = &e->libs[li];
        for (uint32_t gi = 0; gi < l->ngroups; gi++) {
            const struct Slot *s = tab_get(l->c, l->gids + 12ull * gi);
            if (!s) continue;
            if (vec_grow((void **)&e->jobs, &e->capjobs, e->njobs + 1, sizeof *e->jobs) != 0) {
                rc = -1;
                goto done;
            }
            e->jobs[e->njobs++] = (Job){.lib = l, .group = gi, .off = s->off, .len = s->len};
        }
    }
    e->total = (uint32_t)e->njobs;
    out->groups_total = e->total;
    if (!e->nlibs) {
        rc = -1;
        goto done;
    }
    if (e->q->progress) e->q->progress(e->q->arg, 0, e->total);
    uint32_t nthreads = q->threads ? q->threads : 2;
    if (nthreads > 8) nthreads = 8;
    if (nthreads > e->njobs) nthreads = e->njobs ? (uint32_t)e->njobs : 1;

    Thread t0s = {0};
    t0s.dec = dec->thread_open(dec->user, 0);
    if (!t0s.dec) {
        rc = e->njobs ? -2 : 0;
        goto done;
    }
    // Probe: the first group that is not skipped, on this thread. A decoder failure there ends the run.
    while (e->next < e->njobs) {
        const size_t i = e->next++;
        const int r = run_job(e, &t0s, &e->jobs[i]);
        account(e, r);
        if (r == R_FAIL_DEC) {
            rc = -2;
            thread_free(e, &t0s);
            goto done;
        }
        if (r != R_SKIP || atomic_load(&e->cancel)) break;
    }
    Worker *ws = calloc(nthreads, sizeof *ws);
    for (uint32_t k = 1; ws && k < nthreads; k++) {
        ws[k].e = e;
        ws[k].index = k;
        ws[k].started = pthread_create(&ws[k].th, NULL, worker_main, &ws[k]) == 0;
    }
    work_loop(e, &t0s);
    for (uint32_t k = 1; ws && k < nthreads; k++)
        if (ws[k].started) pthread_join(ws[k].th, NULL);
    free(ws);
    thread_free(e, &t0s);
    if (atomic_load(&e->cancel)) rc = -3;
done:
    out->groups_decoded = atomic_load(&e->decoded);
    out->groups_failed = atomic_load(&e->failed);
    out->groups_skipped = atomic_load(&e->skipped);
    out->compressed_bytes = atomic_load(&e->comp_bytes);
    out->raw_bytes = atomic_load(&e->raw_bytes);
    out->ms = (uint64_t)(now_ms() - t0);
    for (size_t i = 0; i < e->nlibs; i++)
        lib_free(&e->libs[i]);
    for (size_t i = 0; i < e->ncont; i++)
        container_free(e, e->cont[i]);
    free(e->libs);
    free(e->cont);
    free(e->jobs);
    pthread_mutex_destroy(&e->prog_mu);
    free(e);
    return rc;
}
