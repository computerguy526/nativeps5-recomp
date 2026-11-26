// SPDX-License-Identifier: GPL-2.0-or-later
// Host unit test of the UE5 shader library reader (nps5_ue5core.c, nps5_ue5vfs.c): synthetic IoStore containers and
// libraries built here (no game bytes), a stub "codec" as the decoder. Plain files through a test file layer, then the real
// VFS with a gamepack .nps5seg pack and a split container (partitions).
#include "nps5_ue5core.h"
#include "nps5_segfile.h"
#include "ps4_vfs.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                         \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

#define BS 4096u

// ---- builder ----
typedef struct Buf {
    uint8_t *p;
    size_t n, cap;
} Buf;
static void put(Buf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void put32(Buf *b, uint32_t v) { put(b, &v, 4); }
static void pad_to(Buf *b, size_t a) {
    static const uint8_t z[BS * 2] = {0};
    while (b->n % a) {
        size_t k = a - b->n % a;
        put(b, z, k > sizeof z ? sizeof z : k);
    }
}

typedef struct Chunk {
    uint8_t id[12];
    uint64_t off, len;
} Chunk;
typedef struct Cont {
    Buf ucas, blocks;
    Chunk ch[512];
    unsigned nch;
    unsigned parts;
    uint64_t part_size;
    uint8_t flags, version;
    unsigned block_method; // method byte of every block
} Cont;

static void cont_init(Cont *c) {
    memset(c, 0, sizeof *c);
    c->parts = 1;
    c->version = 8;
}
static void mkid(uint8_t *id, uint64_t n, int type) {
    memset(id, 0, 12);
    memcpy(id, &n, 8);
    id[11] = (uint8_t)type;
}

static void add_chunk(Cont *c, uint64_t idn, int type, const void *d, size_t len) {
    pad_to(&c->ucas, BS);
    Chunk *k = &c->ch[c->nch++];
    mkid(k->id, idn, type);
    k->off = c->ucas.n;
    k->len = len;
    for (size_t o = 0; o < len; o += BS) {
        uint32_t u = (uint32_t)(len - o < BS ? len - o : BS);
        uint64_t fo = c->ucas.n + o;
        uint8_t e[12] = {0};
        for (int i = 0; i < 5; i++)
            e[i] = (uint8_t)(fo >> (8 * i));
        for (int i = 0; i < 3; i++)
            e[5 + i] = (uint8_t)(u >> (8 * i));
        for (int i = 0; i < 3; i++)
            e[8 + i] = (uint8_t)(u >> (8 * i));
        e[11] = (uint8_t)c->block_method;
        put(&c->blocks, e, 12);
    }
    put(&c->ucas, d, len);
}

static void be5(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 5; i++)
        p[i] = (uint8_t)(v >> (8 * (4 - i)));
}

static void wfile(const char *path, const void *d, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(2);
    }
    fwrite(d, 1, n, f);
    fclose(f);
}

// Writes <dir>/<name>.utoc and the .ucas (partitions); returns the toc size. trunc > 0: only that many bytes of the toc.
static void cont_write(Cont *c, const char *dir, const char *name, size_t trunc) {
    Buf t = {0};
    uint8_t h[144] = {0};
    memcpy(h, "-==--==--==--==-", 16);
    h[16] = c->version;
    uint32_t v;
    v = 144;
    memcpy(h + 20, &v, 4);
    v = c->nch;
    memcpy(h + 24, &v, 4);
    v = (uint32_t)(c->blocks.n / 12);
    memcpy(h + 28, &v, 4);
    v = 12;
    memcpy(h + 32, &v, 4);
    v = 1;
    memcpy(h + 36, &v, 4);
    v = 32;
    memcpy(h + 40, &v, 4);
    v = BS;
    memcpy(h + 44, &v, 4);
    v = c->parts;
    memcpy(h + 52, &v, 4);
    h[80] = c->flags;
    v = 2;
    memcpy(h + 84, &v, 4); // seeds
    memcpy(h + 88, &c->part_size, 8);
    v = 1;
    memcpy(h + 96, &v, 4); // without perfect hash
    put(&t, h, sizeof h);
    for (unsigned i = 0; i < c->nch; i++)
        put(&t, c->ch[i].id, 12);
    for (unsigned i = 0; i < c->nch; i++) {
        uint8_t ol[10];
        be5(ol, c->ch[i].off);
        be5(ol + 5, c->ch[i].len);
        put(&t, ol, 10);
    }
    put32(&t, 0x1111);
    put32(&t, 0x2222); // seeds
    put32(&t, 0); // overflow
    put(&t, c->blocks.p, c->blocks.n);
    uint8_t nm[32] = {0};
    put(&t, nm, 32);
    char p[1024];
    snprintf(p, sizeof p, "%s/%s.utoc", dir, name);
    wfile(p, t.p, trunc ? trunc : t.n);
    free(t.p);
    if (c->parts <= 1) {
        snprintf(p, sizeof p, "%s/%s.ucas", dir, name);
        wfile(p, c->ucas.p, c->ucas.n);
    } else {
        for (unsigned i = 0; i < c->parts; i++) {
            uint64_t lo = i * c->part_size, hi = lo + c->part_size;
            if (hi > c->ucas.n) hi = c->ucas.n;
            if (lo >= hi) break;
            if (i == 0)
                snprintf(p, sizeof p, "%s/%s.ucas", dir, name);
            else
                snprintf(p, sizeof p, "%s/%s_s%u.ucas", dir, name, i);
            wfile(p, c->ucas.p + lo, hi - lo);
        }
    }
}

// ---- synthetic libraries ----
typedef struct GroupSpec {
    uint32_t id; // unique group number (first 4 bytes of the compressed data)
    uint32_t half; // compressed bytes (raw = 2 * half); stored == 1: raw stored, raw bytes = half
    int stored;
    uint32_t nshaders;
} GroupSpec;

#define MAXG 64
typedef struct World {
    GroupSpec g[MAXG];
    unsigned ng;
    uint8_t *raw[MAXG]; // expected decoded bytes
    uint32_t rawlen[MAXG];
} World;

static uint8_t *make_raw(uint32_t id, uint32_t half, int stored, uint8_t **comp, uint32_t *complen, uint32_t *rawlen) {
    uint8_t *c = malloc(half);
    uint32_t x = id * 2654435761u + 7;
    for (uint32_t i = 0; i < half; i++) {
        x = x * 1664525u + 1013904223u;
        c[i] = (uint8_t)(x >> 24);
    }
    memcpy(c, &id, 4);
    *comp = c;
    *complen = half;
    if (stored) {
        *rawlen = half;
        return memcpy(malloc(half), c, half);
    }
    uint8_t *r = malloc(half * 2);
    memcpy(r, c, half);
    for (uint32_t i = 0; i < half; i++)
        r[half + i] = c[i] ^ 0x11;
    *rawlen = half * 2;
    return r;
}

// The variant the fixtures are built for (version table row), and what to break.
static const Nps5Ue5Variant *g_var;
static uint32_t g_lib_ver;
enum {
    T_NONE = 0,
    T_UNC_PLUS1 /* group 1: uncompressed size off by one */,
    T_COMP_FIELD /* group 1: compressed size != chunk */,
    T_COMP_GT_UNC /* group 1 */,
    T_HUGE_COUNT /* a library array count beyond the chunk */,
    T_SHORT_LIB /* chunk ends inside an array */
};
static int g_tweak;
static void putn(Buf *b, const void *d, size_t n, size_t elem) {
    static const uint8_t z[64] = {0};
    put(b, d, n);
    if (elem > n) put(b, z, elem - n);
}

// Adds the groups of w[first..first+n) as chunks (type 9) to c, and the library chunk (type 8).
static void add_library(Cont *c, World *w, unsigned first, unsigned n, uint64_t id_base, int skip_chunk_of) {
    const Nps5Ue5Variant *v = g_var;
    Buf lib = {0};
    put32(&lib, g_lib_ver);
    put32(&lib, 1);
    {
        uint8_t z[20] = {0};
        putn(&lib, z, 20, v->elem[0]);
    } // map hashes
    unsigned nsh = 0;
    for (unsigned i = 0; i < n; i++)
        nsh += w->g[first + i].nshaders;
    put32(&lib, g_tweak == T_HUGE_COUNT ? 0x7ffffff0u : nsh);
    for (unsigned i = 0; i < nsh; i++) {
        uint8_t z[20] = {0};
        z[0] = (uint8_t)i;
        putn(&lib, z, 20, v->elem[1]);
    }
    put32(&lib, n);
    for (unsigned i = 0; i < n; i++) {
        uint8_t id[12];
        mkid(id, id_base + i, v->chunk_group);
        putn(&lib, id, 12, v->elem[2]);
    }
    put32(&lib, 1);
    {
        uint8_t z[8] = {0};
        putn(&lib, z, 8, v->elem[3]);
    } // map entries
    put32(&lib, nsh);
    for (unsigned i = 0; i < nsh; i++) {
        uint32_t ab[2] = {(i * 7) % 10 | (0xabcu << 4), i};
        putn(&lib, ab, 8, v->elem[4]);
    }
    put32(&lib, n);
    unsigned go = 0;
    for (unsigned i = 0; i < n; i++) {
        GroupSpec *g = &w->g[first + i];
        uint32_t comp = g->half, unc = g->stored ? g->half : g->half * 2;
        if (i == 1 && g_tweak == T_UNC_PLUS1) unc++;
        if (i == 1 && g_tweak == T_COMP_FIELD) comp += 5;
        if (i == 1 && g_tweak == T_COMP_GT_UNC) {
            comp = unc + 1;
        }
        uint32_t ge[4] = {go, g->nshaders, unc, comp};
        putn(&lib, ge, 16, v->elem[5]);
        go += g->nshaders;
    }
    put32(&lib, nsh);
    for (unsigned i = 0; i < nsh; i++) {
        uint32_t x = nsh - 1 - i;
        putn(&lib, &x, 4, v->elem[6]);
    } // reversed: tests the indirection
    add_chunk(c, id_base + 1000, v->chunk_library, lib.p, g_tweak == T_SHORT_LIB ? lib.n / 2 : lib.n);
    free(lib.p);
    for (unsigned i = 0; i < n; i++) {
        if ((int)(first + i) == skip_chunk_of) continue;
        GroupSpec *g = &w->g[first + i];
        uint8_t *comp;
        uint32_t cl, rl;
        w->raw[first + i] = make_raw(g->id, g->half, g->stored, &comp, &cl, &rl);
        w->rawlen[first + i] = rl;
        add_chunk(c, id_base + i, v->chunk_group, comp, cl);
        free(comp);
    }
}

static unsigned expected_freq(unsigned shaders_before, unsigned nsh_total, unsigned k) {
    unsigned si = nsh_total - 1 - (shaders_before + k);
    return (si * 7) % 10;
}

// ---- file layer on a plain directory ----
typedef struct FileIo {
    char root[512];
    const char *names[8];
    unsigned n;
} FileIo;
static int f_list(void *u, int (*cb)(void *, const char *), void *arg) {
    FileIo *f = u;
    for (unsigned i = 0; i < f->n; i++)
        if (cb(arg, f->names[i])) return 1;
    return 0;
}
static void *f_open(void *u, const char *path) {
    FileIo *f = u;
    char p[1100];
    snprintf(p, sizeof p, "%s/%s", f->root, path);
    int fd = open(p, O_RDONLY);
    return fd < 0 ? NULL : (void *)(intptr_t)(fd + 1);
}
static ssize_t f_pread(void *u, void *file, void *buf, size_t n, uint64_t off) {
    (void)u;
    return pread((int)((intptr_t)file - 1), buf, n, (off_t)off);
}
static void f_close(void *u, void *file) {
    (void)u;
    close((int)((intptr_t)file - 1));
}

// ---- stub decoder ----
static _Atomic int g_open_count, g_close_count, g_decodes;
static _Atomic int g_fail_id = -1;
static _Atomic int g_open_fail;
typedef struct DT {
    uint8_t *out;
    size_t cap;
} DT;
static void *d_open(void *u, uint32_t i) {
    (void)u;
    (void)i;
    if (g_open_fail) return NULL;
    g_open_count++;
    return calloc(1, sizeof(DT));
}
static const uint8_t *d_decode(void *t, const uint8_t *comp, uint32_t cl, uint32_t rl) {
    DT *d = t;
    g_decodes++;
    uint32_t id;
    memcpy(&id, comp, 4);
    if ((int)id == g_fail_id) return NULL;
    if (rl != 2 * cl) return NULL;
    if (d->cap < rl) {
        d->out = realloc(d->out, rl);
        d->cap = rl;
    }
    for (uint32_t i = 0; i < rl; i++)
        d->out[i] = comp[i % cl] ^ (i >= cl ? 0x11 : 0);
    return d->out;
}
static void d_close(void *t) {
    DT *d = t;
    free(d->out);
    free(d);
    g_close_count++;
}
static const Nps5Ue5Decoder STUB = {NULL, d_open, d_decode, d_close};

// ---- receiver ----
typedef struct Rx {
    pthread_mutex_t mu;
    unsigned seen;
    int bad; // wrong content
    struct {
        uint32_t lib, grp;
    } order[MAXG * 4];
    unsigned stop_after; // cancel after this many groups (0 = never)
    uint32_t last_done, total_seen;
    int progress_bad;
    World *w;
    unsigned lib_first[4]; // world index of group 0 of each library
    unsigned lib_n[4];
} Rx;
static int rx_group(void *arg, const KytyGpuUe5Group *g) {
    Rx *r = arg;
    pthread_mutex_lock(&r->mu);
    if (r->seen < MAXG * 4) {
        r->order[r->seen].lib = g->library;
        r->order[r->seen].grp = g->group;
    }
    r->seen++;
    unsigned wi = r->lib_first[g->library] + g->group;
    World *w = r->w;
    if (g->raw_bytes != w->rawlen[wi] || memcmp(g->bytes, w->raw[wi], g->raw_bytes) != 0) r->bad++;
    if (g->shader_count != w->g[wi].nshaders) r->bad++;
    unsigned before = 0;
    for (unsigned i = r->lib_first[g->library]; i < wi; i++)
        before += w->g[i].nshaders;
    unsigned nsh = 0;
    for (unsigned i = 0; i < r->lib_n[g->library]; i++)
        nsh += w->g[r->lib_first[g->library] + i].nshaders;
    for (uint32_t k = 0; k < g->shader_count; k++)
        if (g->frequencies[k] != expected_freq(before, nsh, k)) r->bad++;
    int stop = r->stop_after && r->seen >= r->stop_after;
    pthread_mutex_unlock(&r->mu);
    return stop;
}
static void rx_progress(void *arg, uint32_t done, uint32_t total) {
    Rx *r = arg; // called under the reader's own lock: monotonic
    if (done < r->last_done || done > total) r->progress_bad = 1;
    r->last_done = done;
    r->total_seen = total;
}

static void rx_init(Rx *r, World *w) {
    memset(r, 0, sizeof *r);
    pthread_mutex_init(&r->mu, NULL);
    r->w = w;
}

static void world_init(World *w, unsigned n, int stored_idx) {
    memset(w, 0, sizeof *w);
    w->ng = n;
    for (unsigned i = 0; i < n; i++) {
        w->g[i] = (GroupSpec){.id = 100 + i,
                              .half = 300 + 777 * (i % 5) + (i == 3 ? 9000 : 0),
                              .stored = (int)i == stored_idx,
                              .nshaders = 1 + i % 3};
    }
}

static char *mktmp(void) {
    static char t[256];
    snprintf(t, sizeof t, "%s/ue5code_unit.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    return mkdtemp(t);
}
static void rmrf(const char *d) {
    char c[600];
    snprintf(c, sizeof c, "rm -rf '%s'", d);
    if (system(c)) {
    }
}

static int run(const Nps5Ue5Io *io, KytyGpuUe5Query *q, Rx *rx, KytyGpuUe5Result *res) {
    q->struct_size = sizeof *q;
    q->abi = 1;
    q->group = rx_group;
    q->progress = rx_progress;
    q->arg = rx;
    memset(res, 0, sizeof *res);
    return nps5_ue5_extract(io, &STUB, q, res);
}

static void test_basic(const char *tmp) {
    char dir[600];
    snprintf(dir, sizeof dir, "%s/basic", tmp);
    mkdir(dir, 0755);
    World w;
    world_init(&w, 14, 5);
    Cont a, b;
    cont_init(&a);
    cont_init(&b);
    add_library(&a, &w, 0, 8, 0, -1);
    add_library(&b, &w, 8, 6, 5000, -1);
    cont_write(&a, dir, "pakchunk0-ps5", 0);
    cont_write(&b, dir, "pakchunk1-ps5", 0);
    FileIo fio = {.n = 2};
    snprintf(fio.root, sizeof fio.root, "%s", dir);
    fio.names[0] = "pakchunk0-ps5.utoc";
    fio.names[1] = "pakchunk1-ps5.utoc";
    Nps5Ue5Io io = {&fio, f_list, f_open, f_pread, f_close};
    for (unsigned threads = 0; threads <= 8; threads += (threads ? 3 : 1)) { // 0 (default 2), 1, 4, 7
        Rx rx;
        KytyGpuUe5Result res;
        KytyGpuUe5Query q = {0};
        rx_init(&rx, &w);
        rx.lib_first[0] = 0;
        rx.lib_n[0] = 8;
        rx.lib_first[1] = 8;
        rx.lib_n[1] = 6;
        q.threads = threads;
        g_open_count = g_close_count = 0;
        int rc = run(&io, &q, &rx, &res);
        CHECK(rc == 0);
        CHECK(res.containers == 2 && res.libraries == 2 && res.groups_total == 14);
        CHECK(res.groups_decoded == 14 && res.groups_failed == 0 && res.groups_skipped == 0);
        CHECK(rx.seen == 14 && rx.bad == 0 && !rx.progress_bad && rx.last_done == 14 && rx.total_seen == 14);
        CHECK(g_open_count == g_close_count && g_open_count >= 1);
        uint64_t rawsum = 0, compsum = 0;
        for (unsigned i = 0; i < 14; i++) {
            rawsum += w.rawlen[i];
            compsum += w.g[i].half;
        }
        CHECK(res.raw_bytes == rawsum && res.compressed_bytes == compsum);
        if (threads == 1) // one thread: delivered in container/library order
            for (unsigned i = 0; i < 14; i++)
                CHECK(rx.order[i].lib == (i < 8 ? 0u : 1u) && rx.order[i].grp == (i < 8 ? i : i - 8));
    }
    // the stored group (index 5) was not decoded: decodes < groups
    // max_raw_bytes: the big group (index 3, > 15000 raw) is skipped and counted
    {
        Rx rx;
        KytyGpuUe5Result res;
        KytyGpuUe5Query q = {0};
        rx_init(&rx, &w);
        rx.lib_first[0] = 0;
        rx.lib_n[0] = 8;
        rx.lib_first[1] = 8;
        rx.lib_n[1] = 6;
        q.max_raw_bytes = 4000;
        q.threads = 2;
        int rc = run(&io, &q, &rx, &res);
        CHECK(rc == 0 && res.groups_skipped >= 1 && res.groups_skipped + res.groups_decoded + res.groups_failed == 14);
        CHECK(rx.bad == 0 && rx.last_done == 14);
    }
    // a decoder failure in the middle: that group fails, the rest are delivered
    {
        Rx rx;
        KytyGpuUe5Result res;
        KytyGpuUe5Query q = {0};
        rx_init(&rx, &w);
        rx.lib_first[0] = 0;
        rx.lib_n[0] = 8;
        rx.lib_first[1] = 8;
        rx.lib_n[1] = 6;
        g_fail_id = 100 + 9;
        q.threads = 3;
        int rc = run(&io, &q, &rx, &res);
        g_fail_id = -1;
        CHECK(rc == 0 && res.groups_failed == 1 && res.groups_decoded == 13 && rx.seen == 13 && rx.bad == 0);
    }
    // the first group failing is -2
    {
        Rx rx;
        KytyGpuUe5Result res;
        KytyGpuUe5Query q = {0};
        rx_init(&rx, &w);
        rx.lib_first[0] = 0;
        rx.lib_n[0] = 8;
        rx.lib_first[1] = 8;
        rx.lib_n[1] = 6;
        g_fail_id = 100;
        int rc = run(&io, &q, &rx, &res);
        g_fail_id = -1;
        CHECK(rc == -2 && rx.seen == 0 && res.groups_failed == 1);
        g_open_fail = 1; // no decode context at all
        rc = run(&io, &q, &rx, &res);
        g_open_fail = 0;
        CHECK(rc == -2);
    }
    // cancel
    {
        Rx rx;
        KytyGpuUe5Result res;
        KytyGpuUe5Query q = {0};
        rx_init(&rx, &w);
        rx.lib_first[0] = 0;
        rx.lib_n[0] = 8;
        rx.lib_first[1] = 8;
        rx.lib_n[1] = 6;
        rx.stop_after = 4;
        q.threads = 1;
        int rc = run(&io, &q, &rx, &res);
        CHECK(rc == -3 && rx.seen == 4 && res.groups_decoded == 4);
    }
    rmrf(dir);
}

static void test_edges(const char *tmp) {
    char dir[600];
    snprintf(dir, sizeof dir, "%s/edges", tmp);
    mkdir(dir, 0755);
    World w;
    world_init(&w, 8, -1);
    FileIo fio = {.n = 0};
    snprintf(fio.root, sizeof fio.root, "%s", dir);
    Nps5Ue5Io io = {&fio, f_list, f_open, f_pread, f_close};
    Rx rx;
    KytyGpuUe5Result res;
    KytyGpuUe5Query q = {0};

    // no containers at all: unsupported
    rx_init(&rx, &w);
    CHECK(run(&io, &q, &rx, &res) == -1);

    // truncated toc, encrypted container, wrong version, wrong magic: all skipped, no libraries -> -1
    Cont c;
    cont_init(&c);
    add_library(&c, &w, 0, 4, 0, -1);
    cont_write(&c, dir, "trunc", 200);
    Cont e;
    cont_init(&e);
    e.flags = 2;
    add_library(&e, &w, 0, 4, 0, -1);
    cont_write(&e, dir, "enc", 0);
    Cont v;
    cont_init(&v);
    v.version = 3;
    add_library(&v, &w, 0, 4, 0, -1);
    cont_write(&v, dir, "ver", 0);
    char p[700];
    snprintf(p, sizeof p, "%s/magic.utoc", dir);
    wfile(p, "not a toc", 9);
    fio.n = 4;
    fio.names[0] = "trunc.utoc";
    fio.names[1] = "enc.utoc";
    fio.names[2] = "ver.utoc";
    fio.names[3] = "magic.utoc";
    rx_init(&rx, &w);
    CHECK(run(&io, &q, &rx, &res) == -1 && res.containers == 0 && rx.seen == 0);

    // compressed blocks (method 1): groups fail (their library chunk too -> no library)
    Cont m;
    cont_init(&m);
    m.block_method = 1;
    add_library(&m, &w, 0, 4, 0, -1);
    cont_write(&m, dir, "meth", 0);
    fio.n = 1;
    fio.names[0] = "meth.utoc";
    rx_init(&rx, &w);
    CHECK(run(&io, &q, &rx, &res) == -1);

    // a missing group chunk is not a job; a missing ucas partition makes the library unreadable
    Cont s;
    cont_init(&s);
    add_library(&s, &w, 0, 6, 0, 2);
    cont_write(&s, dir, "skipchunk", 0);
    fio.n = 1;
    fio.names[0] = "skipchunk.utoc";
    rx_init(&rx, &w);
    rx.lib_first[0] = 0;
    rx.lib_n[0] = 6;
    int rc = run(&io, &q, &rx, &res);
    CHECK(rc == 0 && res.groups_total == 5 && rx.seen == 5);

    // a ucas shorter than the blocks say: those groups fail (io), the run goes on, a library failing to read is dropped
    snprintf(p, sizeof p, "%s/skipchunk.ucas", dir);
    truncate(p, 5 * BS);
    rx_init(&rx, &w);
    rx.lib_first[0] = 0;
    rx.lib_n[0] = 6;
    rc = run(&io, &q, &rx, &res);
    CHECK(rc == -1 || res.groups_failed >= 1);

    // split container: chunks crossing partition boundaries
    Cont sp;
    cont_init(&sp);
    World w2;
    world_init(&w2, 10, 4);
    add_library(&sp, &w2, 0, 10, 0, -1);
    sp.parts = (unsigned)(sp.ucas.n / (3 * BS + 123)) + 1;
    sp.part_size = 3 * BS + 123;
    cont_write(&sp, dir, "split", 0);
    fio.n = 1;
    fio.names[0] = "split.utoc";
    rx_init(&rx, &w2);
    rx.lib_first[0] = 0;
    rx.lib_n[0] = 10;
    rc = run(&io, &q, &rx, &res);
    CHECK(rc == 0 && res.groups_decoded == 10 && rx.bad == 0 && rx.seen == 10);

    // null group callback / bad arguments
    q.group = NULL;
    CHECK(nps5_ue5_extract(&io, &STUB, &q, &res) == -1);
    rmrf(dir);
}

// ---- the real VFS: a plain container and a gamepack pack (.nps5seg) of the same ucas ----
static void write_pack(const char *dir, const char *name, const uint8_t *d, size_t n, size_t seg) {
    char sd[800], p[900];
    snprintf(sd, sizeof sd, "%s/%s.nps5seg", dir, name);
    mkdir(sd, 0755);
    size_t cnt = (n + seg - 1) / seg;
    for (size_t i = 0; i < cnt; i++) {
        snprintf(p, sizeof p, "%s/%04zu", sd, i);
        wfile(p, d + i * seg, (i + 1 < cnt) ? seg : n - seg * (cnt - 1));
    }
    snprintf(p, sizeof p, "%s/manifest.json", sd);
    char m[300];
    int l =
        snprintf(m, sizeof m, "{\"format\": \"nps5seg/1\", \"total_size\": %zu, \"seg_size\": %zu, \"seg_count\": %zu}",
                 n, seg, cnt);
    wfile(p, m, (size_t)l);
}

static void test_vfs(const char *tmp) {
    char app[600], paks[700], p[900];
    snprintf(app, sizeof app, "%s/app0", tmp);
    mkdir(app, 0755);
    snprintf(paks, sizeof paks, "%s/Game", app);
    mkdir(paks, 0755);
    snprintf(paks, sizeof paks, "%s/Game/Paks", app);
    mkdir(paks, 0755);
    World w;
    world_init(&w, 12, 7);
    Cont a, b, c;
    cont_init(&a);
    cont_init(&b);
    cont_init(&c);
    add_library(&a, &w, 0, 4, 0, -1);
    add_library(&b, &w, 4, 4, 5000, -1);
    add_library(&c, &w, 8, 4, 9000, -1);
    // a: plain; b: ucas packed as segments (segment size not a multiple of the block size); c: split in partitions, plain
    cont_write(&a, paks, "a-ps5", 0);
    cont_write(&b, paks, "b-ps5", 0);
    snprintf(p, sizeof p, "%s/b-ps5.ucas", paks);
    unlink(p);
    write_pack(paks, "b-ps5.ucas", b.ucas.p, b.ucas.n, 5000);
    c.parts = (unsigned)(c.ucas.n / 7000) + 1;
    c.part_size = 7000;
    cont_write(&c, paks, "c-ps5", 0);
    // a stray non-container file and a hidden dir are ignored
    snprintf(p, sizeof p, "%s/readme.txt", paks);
    wfile(p, "x", 1);
    ps4_vfs_init(app);
    const Nps5Ue5Io *io = nps5_ue5_vfs_io();
    Rx rx;
    KytyGpuUe5Result res;
    KytyGpuUe5Query q = {0};
    rx_init(&rx, &w);
    rx.lib_first[0] = 0;
    rx.lib_first[1] = 4;
    rx.lib_first[2] = 8;
    rx.lib_n[0] = rx.lib_n[1] = rx.lib_n[2] = 4;
    q.threads = 1;
    int rc = run(io, &q, &rx, &res);
    CHECK(rc == 0 && res.containers == 3 && res.libraries == 3 && res.groups_total == 12);
    CHECK(res.groups_decoded == 12 && res.groups_failed == 0 && rx.bad == 0 && rx.seen == 12);
    for (unsigned i = 0; i < 12; i++)
        CHECK(rx.order[i].lib == i / 4 && rx.order[i].grp == i % 4);
    q.threads = 4;
    rx_init(&rx, &w);
    rx.lib_first[0] = 0;
    rx.lib_first[1] = 4;
    rx.lib_first[2] = 8;
    rx.lib_n[0] = rx.lib_n[1] = rx.lib_n[2] = 4;
    rc = run(io, &q, &rx, &res);
    CHECK(rc == 0 && res.groups_decoded == 12 && rx.bad == 0);
    // a segmented utoc is listed under its own name
    {
        char tocp[900];
        snprintf(tocp, sizeof tocp, "%s/a-ps5.utoc", paks);
        FILE *f = fopen(tocp, "rb");
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *d = malloc((size_t)n);
        if (fread(d, 1, (size_t)n, f) != (size_t)n) fails++;
        fclose(f);
        unlink(tocp);
        write_pack(paks, "a-ps5.utoc", d, (size_t)n, 777);
        free(d);
        nps5_segfile_reset(); // the directory scan of the segment layer is cached
        rx_init(&rx, &w);
        rx.lib_first[0] = 0;
        rx.lib_first[1] = 4;
        rx.lib_first[2] = 8;
        rx.lib_n[0] = rx.lib_n[1] = rx.lib_n[2] = 4;
        rc = run(io, &q, &rx, &res);
        CHECK(rc == 0 && res.containers == 3 && res.groups_decoded == 12 && rx.bad == 0);
    }
    rmrf(app);
}

// ---- version table and failure cases ----
static void rx_for(Rx *rx, World *w, unsigned n) {
    rx_init(rx, w);
    rx->lib_first[0] = 0;
    rx->lib_n[0] = n;
}

static void test_variants(const char *tmp) {
    char dir[600];
    snprintf(dir, sizeof dir, "%s/variants", tmp);
    mkdir(dir, 0755);
    unsigned nv;
    const Nps5Ue5Variant *vs = nps5_ue5_variants(&nv);
    CHECK(nv >= 1);
    for (unsigned vi = 0; vi < nv; vi++) {
        g_var = &vs[vi];
        // every TOC version the row covers, with the library version at both ends of its range
        for (uint32_t tv = vs[vi].toc_min; tv <= vs[vi].toc_max; tv++) {
            for (int end = 0; end < 2; end++) {
                g_lib_ver = end ? (vs[vi].lib_max == 0xffffffffu ? 7 : vs[vi].lib_max) : vs[vi].lib_min;
                World w;
                world_init(&w, 5, 2);
                Cont c;
                cont_init(&c);
                c.version = (uint8_t)tv;
                add_library(&c, &w, 0, 5, 0, -1);
                cont_write(&c, dir, "v", 0);
                FileIo fio = {.n = 1};
                snprintf(fio.root, sizeof fio.root, "%s", dir);
                fio.names[0] = "v.utoc";
                Nps5Ue5Io io = {&fio, f_list, f_open, f_pread, f_close};
                Rx rx;
                KytyGpuUe5Result res;
                KytyGpuUe5Query q = {0};
                rx_for(&rx, &w, 5);
                CHECK(run(&io, &q, &rx, &res) == 0 && res.groups_decoded == 5 && rx.bad == 0);
            }
        }
        // a TOC version past the row (nothing else covers it) and a library version past the row: clean off, nothing decoded
        World w;
        world_init(&w, 5, -1);
        uint32_t unk_toc = vs[vi].toc_max + 1;
        int covered = 0;
        for (unsigned k = 0; k < nv; k++)
            if (unk_toc >= vs[k].toc_min && unk_toc <= vs[k].toc_max) covered = 1;
        if (!covered) {
            g_lib_ver = vs[vi].lib_min;
            Cont c;
            cont_init(&c);
            c.version = (uint8_t)unk_toc;
            add_library(&c, &w, 0, 5, 0, -1);
            cont_write(&c, dir, "unk", 0);
            FileIo fio = {.n = 1};
            snprintf(fio.root, sizeof fio.root, "%s", dir);
            fio.names[0] = "unk.utoc";
            Nps5Ue5Io io = {&fio, f_list, f_open, f_pread, f_close};
            Rx rx;
            KytyGpuUe5Result res;
            KytyGpuUe5Query q = {0};
            rx_for(&rx, &w, 5);
            g_decodes = 0;
            CHECK(run(&io, &q, &rx, &res) == -1 && rx.seen == 0 && g_decodes == 0 && res.containers == 0);
        }
        if (vs[vi].lib_max != 0xffffffffu) {
            g_lib_ver = vs[vi].lib_max + 1;
            Cont c;
            cont_init(&c);
            c.version = (uint8_t)vs[vi].toc_max;
            add_library(&c, &w, 0, 5, 0, -1);
            cont_write(&c, dir, "unkl", 0);
            FileIo fio = {.n = 1};
            snprintf(fio.root, sizeof fio.root, "%s", dir);
            fio.names[0] = "unkl.utoc";
            Nps5Ue5Io io = {&fio, f_list, f_open, f_pread, f_close};
            Rx rx;
            KytyGpuUe5Result res;
            KytyGpuUe5Query q = {0};
            rx_for(&rx, &w, 5);
            CHECK(run(&io, &q, &rx, &res) == -1 && rx.seen == 0);
        }
        g_lib_ver = vs[vi].lib_min;
    }
    rmrf(dir);
}

// One failure each; the host returns cleanly and the damaged group is not delivered (the intact ones may be).
static void test_failures(const char *tmp) {
    char dir[600];
    snprintf(dir, sizeof dir, "%s/fail", tmp);
    mkdir(dir, 0755);
    g_var = nps5_ue5_variants(NULL);
    g_lib_ver = g_var->lib_min;
    struct {
        int tweak;
        const char *what;
        int rc;
        unsigned failed;
        unsigned delivered;
    } T[] = {
        {T_UNC_PLUS1, "uncompressed size off by one: the decoder refuses (size mismatch)", 0, 1, 5},
        {T_COMP_FIELD, "library compressed size != chunk length", 0, 1, 5},
        {T_COMP_GT_UNC, "compressed larger than uncompressed", 0, 1, 5},
        {T_HUGE_COUNT, "array count beyond the library chunk: library dropped", -1, 0, 0},
        {T_SHORT_LIB, "library chunk ends inside an array: library dropped", -1, 0, 0},
    };
    for (unsigned i = 0; i < sizeof T / sizeof *T; i++) {
        g_tweak = T[i].tweak;
        World w;
        world_init(&w, 6, -1);
        Cont c;
        cont_init(&c);
        add_library(&c, &w, 0, 6, 0, -1);
        g_tweak = T_NONE;
        cont_write(&c, dir, "f", 0);
        FileIo fio = {.n = 1};
        snprintf(fio.root, sizeof fio.root, "%s", dir);
        fio.names[0] = "f.utoc";
        Nps5Ue5Io io = {&fio, f_list, f_open, f_pread, f_close};
        Rx rx;
        KytyGpuUe5Result res;
        KytyGpuUe5Query q = {0};
        rx_for(&rx, &w, 6);
        q.threads = 2;
        int rc = run(&io, &q, &rx, &res);
        if (rc != T[i].rc || res.groups_failed != T[i].failed || rx.seen != T[i].delivered || rx.bad)
            printf("  case %u (%s): rc %d failed %u seen %u bad %d\n", i, T[i].what, rc, res.groups_failed, rx.seen,
                   rx.bad);
        CHECK(rc == T[i].rc && res.groups_failed == T[i].failed && rx.seen == T[i].delivered && rx.bad == 0);
    }
    // a decoder that returns short output / an error for everything: -2, nothing delivered; one that fails mid-run keeps going
    {
        World w;
        world_init(&w, 6, -1);
        Cont c;
        cont_init(&c);
        add_library(&c, &w, 0, 6, 0, -1);
        cont_write(&c, dir, "f", 0);
        FileIo fio = {.n = 1};
        snprintf(fio.root, sizeof fio.root, "%s", dir);
        fio.names[0] = "f.utoc";
        Nps5Ue5Io io = {&fio, f_list, f_open, f_pread, f_close};
        Rx rx;
        KytyGpuUe5Result res;
        KytyGpuUe5Query q = {0};
        rx_for(&rx, &w, 6);
        g_fail_id = 100;
        CHECK(run(&io, &q, &rx, &res) == -2 && rx.seen == 0);
        g_fail_id = -1;
    }
    rmrf(dir);
}

// Random damage to the toc, the ucas and the library: never a crash or hang, only a clean return code.
static void test_fuzz(const char *tmp) {
    char dir[600];
    snprintf(dir, sizeof dir, "%s/fuzz", tmp);
    mkdir(dir, 0755);
    g_var = nps5_ue5_variants(NULL);
    g_lib_ver = g_var->lib_min;
    World w;
    world_init(&w, 9, 3);
    Cont c;
    cont_init(&c);
    add_library(&c, &w, 0, 9, 0, -1);
    cont_write(&c, dir, "z", 0);
    char tp[700], up[700];
    snprintf(tp, sizeof tp, "%s/z.utoc", dir);
    snprintf(up, sizeof up, "%s/z.ucas", dir);
    FILE *f = fopen(tp, "rb");
    fseek(f, 0, SEEK_END);
    long tn = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *toc = malloc((size_t)tn);
    if (fread(toc, 1, (size_t)tn, f) != (size_t)tn) fails++;
    fclose(f);
    f = fopen(up, "rb");
    fseek(f, 0, SEEK_END);
    long un = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *ucas = malloc((size_t)un);
    if (fread(ucas, 1, (size_t)un, f) != (size_t)un) fails++;
    fclose(f);
    FileIo fio = {.n = 1};
    snprintf(fio.root, sizeof fio.root, "%s", dir);
    fio.names[0] = "z.utoc";
    Nps5Ue5Io io = {&fio, f_list, f_open, f_pread, f_close};
    unsigned ok = 0, off = 0;
    uint32_t x = 12345;
    for (int it = 0; it < 400; it++) {
        uint8_t *t2 = memcpy(malloc((size_t)tn), toc, (size_t)tn), *u2 = memcpy(malloc((size_t)un), ucas, (size_t)un);
        long tl = tn, ul = un;
        x = x * 1664525u + 1013904223u;
        int kind = (int)(x >> 28) % 4;
        for (int k = 0, nflip = 1 + (int)((x >> 8) % 6); k < nflip; k++) {
            x = x * 1664525u + 1013904223u;
            if (kind == 0)
                t2[(x >> 4) % (uint32_t)tn] = (uint8_t)(x >> 24);
            else if (kind == 1)
                u2[(x >> 4) % (uint32_t)un] = (uint8_t)(x >> 24);
            else if (kind == 2) {
                t2[(x >> 4) % 144] = (uint8_t)(x >> 24);
            } // the header
            else {
                tl = (long)((x >> 4) % (uint32_t)tn);
                ul = (long)((x >> 9) % (uint32_t)un);
            }
        }
        wfile(tp, t2, (size_t)tl);
        wfile(up, u2, (size_t)ul);
        free(t2);
        free(u2);
        Rx rx;
        KytyGpuUe5Result res;
        KytyGpuUe5Query q = {0};
        rx_for(&rx, &w, 9);
        q.threads = 1 + it % 3;
        int rc = run(&io, &q, &rx, &res);
        CHECK(rc == 0 || rc == -1 || rc == -2);
        CHECK(res.groups_decoded + res.groups_failed + res.groups_skipped <= res.groups_total || rc != 0);
        if (rc == 0)
            ok++;
        else
            off++;
    }
    CHECK(ok > 0 && off > 0); // both outcomes happened: the damage is neither always harmless nor always fatal
    free(toc);
    free(ucas);
    rmrf(dir);
}

int main(void) {
    char *tmp = mktmp();
    if (!tmp) {
        perror("mkdtemp");
        return 2;
    }
    g_var = nps5_ue5_variants(NULL);
    g_lib_ver = g_var->lib_min;
    test_basic(tmp);
    test_edges(tmp);
    test_vfs(tmp);
    test_variants(tmp);
    test_failures(tmp);
    test_fuzz(tmp);
    rmrf(tmp);
    printf(fails ? "ue5code: %d FAILED\n" : "ue5code: all ok\n", fails);
    return fails ? 1 : 0;
}
