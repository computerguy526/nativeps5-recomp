// SPDX-License-Identifier: GPL-2.0-or-later
//
// Host test of gamepack segmented files (kernel/vfs/nps5_segfile.h): a file stored as <name>.nps5seg/{0000..,
// manifest.json} must read exactly like the original through every guest read path: ps4_vfs open / read (+ position,
// EOF) / pread (random offsets and lengths, across many segment boundaries) / lseek / fstat / stat / access / mmap
// copy-in / readv / preadv, sceKernelAioSubmitReadCommands, the AMPR (Apr) resolve + ReadFile worker, and directory
// listings (sceKernelGetdents), with case-insensitive names, a segment-fd cap far below the segment count, and
// concurrent readers. Every byte is compared with the original. Odd sizes: segment 65,537 bytes, file 1,000,003 bytes
// (16 segments, the last partial) and a file of exactly 3 segments.
// Modes: (none) = segments written here; "dir <app root>" = an app root already packed by tools-host/gamepack.py (the
// file Paks/Big-ps5.ucas is compared with <app root>.orig/Big-ps5.ucas); "off" = NPS5_VFS_SEGFILES=0: not visible.
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include "recomp_context.h"
#include "ps4_aio.h"
#include "ps4_vfs.h"
#include "nps5_segfile.h"
#include "nps5_log.h"

uint8_t g_nps5_level[NPS5_S_COUNT];
void nps5_log(int s, int l, const char *fmt, ...) {
    (void)s;
    (void)l;
    (void)fmt;
}
void nps5_log_compat(int s, int l, const char *tag, const char *fmt, ...) {
    (void)s;
    (void)l;
    (void)tag;
    (void)fmt;
}

ssize_t ps4_vfs_read(int fd, void *buf, size_t count);
ssize_t ps4_vfs_pread(int fd, void *buf, size_t count, off_t offset);
off_t ps4_vfs_lseek(int fd, off_t offset, int whence);
int ps4_vfs_fstat(int fd, struct stat *st);
int ps4_vfs_stat(const char *guest_path, struct stat *st);
int ps4_vfs_access(const char *guest_path, int mode);
int ps4_vfs_open(const char *guest_path, int host_flags, mode_t mode);
int ps4_vfs_close(int fd);
void *ps4_vfs_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset);

#define SHIM(n) void shim_##n(GuestContext *)
SHIM(sceKernelOpen);
SHIM(sceKernelClose);
SHIM(sceKernelGetdents);
SHIM(sceKernelStat);
SHIM(sceKernelFstat);
SHIM(sceKernelPreadv);
SHIM(readv);
SHIM(sceKernelAprResolveFilepathsToIdsAndFileSizes);
SHIM(sceAmprAprCommandBufferConstructor);
SHIM(sceAmprCommandBufferSetBuffer);
SHIM(sceAmprAprCommandBufferReadFile);
SHIM(sceKernelAprSubmitCommandBufferAndGetResult);
SHIM(sceKernelAprWaitCommandBuffer);

#define ARENA (64ULL << 20)
static GuestContext *C;
static int fails;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                         \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)
#define H(a) ((uint8_t *)C->mem_base + (a))
#define CALL(fn)                                                                                                       \
    do {                                                                                                               \
        uint64_t rsp0 = C->rsp;                                                                                        \
        fn(C);                                                                                                         \
        assert(C->rsp == rsp0 + 8);                                                                                    \
        C->rsp = rsp0;                                                                                                 \
    } while (0)
#define RAX32 ((uint32_t)C->rax)
static uint64_t bump = 0x100000;
static uint64_t galloc(size_t n) {
    uint64_t r = bump;
    bump += (n + 63) & ~63ULL;
    assert(bump < ARENA);
    return r;
}
static uint64_t gstr(const char *s) {
    uint64_t r = galloc(strlen(s) + 1);
    strcpy((char *)H(r), s);
    return r;
}
static void reset(void) {
    C->rdi = C->rsi = C->rdx = C->rcx = C->r8 = C->r9 = 0;
    C->rax = 0;
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

#define SEG 65537u
#define SZ 1000003u

static uint8_t *orig; // the original bytes of Big-ps5.ucas
static size_t orig_sz;
static uint8_t *orig3; // exact3.bin (3 whole segments)

static void write_file(const char *p, const void *b, size_t n) {
    FILE *f = fopen(p, "wb");
    assert(f);
    if (n) assert(fwrite(b, 1, n, f) == n);
    fclose(f);
}

// <dir>/<name>.nps5seg with segments of seg bytes and a manifest gamepack would write (format, sizes, counts).
static void make_segfile(const char *dir, const char *name, const uint8_t *b, size_t n, size_t seg) {
    char sd[1024], p[1100];
    snprintf(sd, sizeof sd, "%s/%s.nps5seg", dir, name);
    mkdir(sd, 0755);
    const size_t cnt = (n + seg - 1) / seg;
    for (size_t i = 0; i < cnt; i++) {
        snprintf(p, sizeof p, "%s/%04zu", sd, i);
        write_file(p, b + i * seg, (i + 1 < cnt) ? seg : n - seg * (cnt - 1));
    }
    snprintf(p, sizeof p, "%s/manifest.json", sd);
    FILE *f = fopen(p, "w");
    fprintf(f,
            "{\n  \"format\": \"nps5seg/1\",\n  \"name\": \"%s\",\n  \"total_size\": %zu,\n  \"seg_size\": %zu,\n"
            "  \"seg_count\": %zu,\n  \"sha256\": \"0\",\n  \"segments\": [{\"name\": \"0000\", \"size\": 1}]\n}\n",
            name, n, seg, cnt);
    fclose(f);
}

static int open_ro(const char *g) { return ps4_vfs_open(g, O_RDONLY, 0); }

static void test_stat_access(void) {
    struct stat st;
    CHECK(ps4_vfs_stat("/app0/Paks/Big-ps5.ucas", &st) == 0 && st.st_size == (off_t)orig_sz && S_ISREG(st.st_mode));
    CHECK(ps4_vfs_stat("/app0/paks/BIG-PS5.UCAS", &st) == 0 && st.st_size == (off_t)orig_sz); // case-insensitive
    CHECK(ps4_vfs_access("/app0/Paks/Big-ps5.ucas", R_OK) == 0);
    CHECK(ps4_vfs_access("/app0/Paks/Big-ps5.ucas", W_OK) == -1 && errno == EACCES);
    CHECK(ps4_vfs_stat("/app0/Paks/Big-ps5.ucas.nps5seg", &st) == 0 &&
          S_ISDIR(st.st_mode)); // the dir itself is still a dir
    CHECK(ps4_vfs_stat("/app0/Paks/nothere.ucas", &st) == -1 && errno == ENOENT);
    CHECK(ps4_vfs_open("/app0/Paks/Big-ps5.ucas", O_RDWR, 0) < 0); // the write gate: no writable open of game files
    if (orig3) CHECK(ps4_vfs_stat("/app0/Paks/exact3.bin", &st) == 0 && st.st_size == 3 * (off_t)SEG);
    // guest stat through the shim (FreeBSD struct: st_size at 0x48)
    reset();
    C->rdi = gstr("/app0/Paks/Big-ps5.ucas");
    C->rsi = galloc(256);
    CALL(shim_sceKernelStat);
    CHECK(RAX32 == 0 && *(int64_t *)H(C->rsi + 0x48) == (int64_t)orig_sz);
    printf("segfile: stat/access ok\n");
}

static void test_sequential(const char *g) {
    int fd = open_ro(g);
    CHECK(fd >= PS4_VFS_VIRTUAL_FD_BASE);
    struct stat st;
    CHECK(ps4_vfs_fstat(fd, &st) == 0 && st.st_size == (off_t)orig_sz);
    uint8_t *b = malloc(orig_sz + 100000);
    size_t got = 0;
    for (;;) { // odd chunk sizes, some larger than a segment
        size_t want = 1 + rnd() % (3 * SEG);
        ssize_t r = ps4_vfs_read(fd, b + got, want);
        CHECK(r >= 0);
        if (r <= 0) break;
        CHECK(got + (size_t)r <= orig_sz);
        CHECK((size_t)r == want || got + (size_t)r == orig_sz); // never short before EOF
        got += (size_t)r;
    }
    CHECK(got == orig_sz && memcmp(b, orig, orig_sz) == 0);
    CHECK(ps4_vfs_read(fd, b, 10) == 0); // at EOF
    CHECK(ps4_vfs_lseek(fd, 0, SEEK_CUR) == (off_t)orig_sz);
    // seeks: SET / CUR / END, past EOF, negative
    CHECK(ps4_vfs_lseek(fd, SEG - 3, SEEK_SET) == SEG - 3);
    CHECK(ps4_vfs_read(fd, b, 10) == 10 && memcmp(b, orig + SEG - 3, 10) == 0); // across the first boundary
    CHECK(ps4_vfs_lseek(fd, 5 * SEG, SEEK_CUR) == 5 * SEG + SEG + 7);
    CHECK(ps4_vfs_read(fd, b, 1) == 1 && b[0] == orig[6 * SEG + 7]);
    CHECK(ps4_vfs_lseek(fd, -5, SEEK_END) == (off_t)orig_sz - 5);
    CHECK(ps4_vfs_read(fd, b, 100) == 5 && memcmp(b, orig + orig_sz - 5, 5) == 0); // clamped at EOF
    CHECK(ps4_vfs_lseek(fd, 10, SEEK_END) == (off_t)orig_sz + 10);
    CHECK(ps4_vfs_read(fd, b, 100) == 0);
    CHECK(ps4_vfs_lseek(fd, -1, SEEK_SET) == -1 && errno == EINVAL);
    CHECK(ps4_vfs_close(fd) == 0);
    CHECK(ps4_vfs_read(fd, b, 1) == -1 && errno == EBADF);
    free(b);
}

static void test_random_pread(const char *g, int n) {
    int fd = open_ro(g);
    CHECK(fd >= 0);
    uint8_t *b = malloc(4 * SEG);
    int bad = 0;
    for (int i = 0; i < n; i++) {
        uint64_t off, len;
        switch (rnd() % 4) {
            case 0:
                off = rnd() % orig_sz;
                len = 1 + rnd() % (4 * SEG - 1);
                break; // anywhere, big
            case 1:
                off = (rnd() % (orig_sz / SEG + 1)) * SEG;
                off = off ? off - rnd() % 9 : 0;
                len = 1 + rnd() % 20;
                break; // at a boundary
            case 2:
                off = orig_sz - rnd() % 50;
                len = 1 + rnd() % 200;
                break; // at EOF
            default:
                off = orig_sz + rnd() % 1000;
                len = 1 + rnd() % 100;
                break; // past EOF
        }
        memset(b, 0xA5, len);
        const ssize_t r = ps4_vfs_pread(fd, b, len, (off_t)off);
        const size_t want = off >= orig_sz ? 0 : (off + len > orig_sz ? orig_sz - off : len);
        if (r != (ssize_t)want || (want && memcmp(b, orig + off, want) != 0)) bad++;
    }
    CHECK(bad == 0);
    if (bad) printf("  %d of %d random preads wrong\n", bad, n);
    CHECK(ps4_vfs_lseek(fd, 0, SEEK_CUR) == 0); // pread does not move the position
    ps4_vfs_close(fd);
    free(b);
}

static void test_readv_mmap(const char *g) {
    int fd = open_ro(g);
    uint8_t a[100], b[2 * SEG], c[777];
    struct iovec iov[3] = {{a, sizeof a}, {b, sizeof b}, {c, sizeof c}};
    const size_t tot = sizeof a + sizeof b + sizeof c;
    const off_t off = SEG - 50;
    CHECK(ps4_vfs_readv_virtual(fd, iov, 3, off) == (ssize_t)tot); // preadv across 3 boundaries
    CHECK(!memcmp(a, orig + off, sizeof a) && !memcmp(b, orig + off + sizeof a, sizeof b) &&
          !memcmp(c, orig + off + sizeof a + sizeof b, sizeof c));
    CHECK(ps4_vfs_lseek(fd, (off_t)orig_sz - 150, SEEK_SET) == (off_t)orig_sz - 150);
    CHECK(ps4_vfs_readv_virtual(fd, iov, 3, -1) == 150); // readv to EOF, stops at the short buffer
    CHECK(!memcmp(a, orig + orig_sz - 150, 100) && !memcmp(b, orig + orig_sz - 50, 50));
    // guest shims: preadv and readv go through the VFS for a virtual fd
    uint64_t gv = galloc(32), gb = galloc(3 * SEG);
    *(uint64_t *)H(gv) = gb;
    *(uint64_t *)H(gv + 8) = 3 * SEG - 11;
    reset();
    C->rdi = fd;
    C->rsi = gv;
    C->rdx = 1;
    C->rcx = 2 * SEG + 3;
    CALL(shim_sceKernelPreadv);
    CHECK((int64_t)C->rax == 3 * SEG - 11 && !memcmp(H(gb), orig + 2 * SEG + 3, 3 * SEG - 11));
    ps4_vfs_lseek(fd, 7 * SEG - 1, SEEK_SET);
    reset();
    C->rdi = fd;
    C->rsi = gv;
    C->rdx = 1;
    CALL(shim_readv);
    CHECK((int64_t)C->rax == 3 * SEG - 11 && !memcmp(H(gb), orig + 7 * SEG - 1, 3 * SEG - 11));
    CHECK(ps4_vfs_lseek(fd, 0, SEEK_CUR) == 10 * SEG - 12);
    // mmap copy-in: an unaligned range across boundaries, and one running past EOF (zero tail)
    const size_t ml = 5 * SEG;
    uint8_t *m = ps4_vfs_mmap(NULL, ml, PROT_READ, MAP_PRIVATE, fd, 4096);
    CHECK(m != MAP_FAILED && !memcmp(m, orig + 4096, ml));
    if (m != MAP_FAILED) munmap(m, ml);
    m = ps4_vfs_mmap(NULL, 3 * SEG, PROT_READ, MAP_PRIVATE, fd, (off_t)(orig_sz - SEG));
    CHECK(m != MAP_FAILED && !memcmp(m, orig + orig_sz - SEG, SEG) && m[SEG] == 0 && m[3 * SEG - 1] == 0);
    if (m != MAP_FAILED) munmap(m, 3 * SEG);
    ps4_vfs_close(fd);
    printf("segfile: readv/preadv/mmap ok\n");
}

static void test_aio(const char *g) {
    int fd = open_ro(g);
    enum { N = 6 };
    uint64_t req = galloc(N * sizeof(OrbisKernelAioRWRequest)), res = galloc(N * 16), idp = galloc(8);
    uint64_t offs[N] = {0, SEG - 1, 3 * SEG + 5, SZ - 1000, SZ - 10, SZ + 5};
    uint64_t lens[N] = {SEG + 2, 2, 2 * SEG, 1000, 100, 10};
    uint64_t bufs[N];
    OrbisKernelAioRWRequest *r = (OrbisKernelAioRWRequest *)H(req);
    for (int i = 0; i < N; i++) {
        bufs[i] = galloc(lens[i]);
        r[i] = (OrbisKernelAioRWRequest){(int64_t)offs[i], (int64_t)lens[i], bufs[i], res + 16 * i, fd, 0};
    }
    reset();
    C->rdi = req;
    C->rsi = N;
    C->rcx = idp;
    CALL(shim_sceKernelAioSubmitReadCommands);
    CHECK(RAX32 == 0);
    for (int i = 0; i < N; i++) {
        const OrbisKernelAioResult *o = (const OrbisKernelAioResult *)H(res + 16 * i);
        const uint64_t want = offs[i] >= orig_sz ? 0 : (offs[i] + lens[i] > orig_sz ? orig_sz - offs[i] : lens[i]);
        CHECK(o->returnValue == (int64_t)want && (!want || !memcmp(H(bufs[i]), orig + offs[i], want)));
    }
    ps4_vfs_close(fd);
    printf("segfile: aio ok\n");
}

static void test_apr(const char *g) {
    uint64_t paths = galloc(16), ids = galloc(16), sizes = galloc(16), erridx = galloc(8);
    *(uint64_t *)H(paths) = gstr(g);
    reset();
    C->rdi = paths;
    C->rsi = 1;
    C->rdx = ids;
    C->rcx = sizes;
    C->r8 = erridx;
    CALL(shim_sceKernelAprResolveFilepathsToIdsAndFileSizes);
    CHECK(RAX32 == 0 && *(uint64_t *)H(sizes) == orig_sz);
    const uint32_t fid = *(uint32_t *)H(ids);
    uint64_t cb = galloc(0x18), gs = galloc(8), ss = galloc(8), mem = galloc(512);
    reset();
    C->rdi = cb;
    C->rsi = gs;
    C->rdx = ss;
    CALL(shim_sceAmprAprCommandBufferConstructor);
    CHECK(RAX32 == 0);
    reset();
    C->rdi = cb;
    C->rsi = mem;
    C->rdx = 512;
    CALL(shim_sceAmprCommandBufferSetBuffer);
    CHECK(RAX32 == 0);
    enum { N = 4 };
    struct {
        uint64_t off, size, dst;
    } rd[N] = {{SEG - 7, 2 * SEG + 20}, {0, 100}, {9 * SEG + 1, 3 * SEG}, {SZ - 500, 4096}};
    for (int i = 0; i < N; i++) {
        rd[i].dst = galloc(rd[i].size + 64);
        memset(H(rd[i].dst), 0x55, rd[i].size + 64);
    }
    uint64_t stack = galloc(256);
    C->rsp = stack + 64;
    for (int i = 0; i < N; i++) {
        *(uint64_t *)H(C->rsp + 8) = rd[i].off;
        C->rdi = cb;
        C->rsi = gs;
        C->rdx = ss;
        C->rcx = fid;
        C->r8 = rd[i].dst;
        C->r9 = rd[i].size;
        CALL(shim_sceAmprAprCommandBufferReadFile);
        CHECK(RAX32 == 0);
    }
    uint64_t resp = galloc(8), idp = galloc(8);
    reset();
    C->rdi = cb;
    C->rsi = 0;
    C->rdx = resp;
    C->rcx = idp;
    CALL(shim_sceKernelAprSubmitCommandBufferAndGetResult);
    CHECK(RAX32 == 0);
    reset();
    C->rdi = *(uint32_t *)H(idp);
    CALL(shim_sceKernelAprWaitCommandBuffer);
    CHECK(RAX32 == 0);
    for (int i = 0; i < N; i++) {
        const uint64_t in = rd[i].off + rd[i].size > orig_sz ? orig_sz - rd[i].off : rd[i].size;
        CHECK(!memcmp(H(rd[i].dst), orig + rd[i].off, in));
        int z = 1;
        for (uint64_t k = in; k < rd[i].size; k++)
            if (H(rd[i].dst)[k]) z = 0;
        CHECK(z); // past EOF reads as zeros
        CHECK(H(rd[i].dst)[rd[i].size] == 0x55); // nothing written past the request
    }
    printf("segfile: apr ok\n");
}

// The guest's listing of <gdir>: name -> type, or -1 when missing.
static int list_type(const char *gdir, const char *name, int *count_of_name) {
    reset();
    C->rdi = gstr(gdir);
    C->rsi = O_RDONLY | 0x20000 /* O_DIRECTORY */;
    CALL(shim_sceKernelOpen);
    const int fd = (int)C->rax;
    CHECK(fd >= 0);
    const uint64_t buf = galloc(65536);
    int type = -1, cnt = 0;
    for (;;) {
        reset();
        C->rdi = fd;
        C->rsi = buf;
        C->rdx = 65536;
        CALL(shim_sceKernelGetdents);
        const int n = (int)C->rax;
        if (n <= 0) break;
        for (int o = 0; o < n;) {
            const uint8_t *e = H(buf + o);
            const uint16_t reclen = *(const uint16_t *)(e + 4);
            char nm[256];
            memcpy(nm, e + 8, e[7]);
            nm[e[7]] = 0;
            if (!strcmp(nm, name)) {
                type = e[6];
                cnt++;
            }
            if (strstr(nm, ".nps5seg")) cnt += 100; // a segment directory must never be listed
            o += reclen;
        }
    }
    reset();
    C->rdi = fd;
    CALL(shim_sceKernelClose);
    if (count_of_name) *count_of_name = cnt;
    return type;
}

static void test_listing(const char *root) {
    int cnt = 0;
    CHECK(list_type("/app0/Paks", "Big-ps5.ucas", &cnt) == 8 /* DT_REG */ && cnt == 1);
    // a pack in progress: the original next to its segment directory is listed once, the directory not at all
    char p[1100];
    snprintf(p, sizeof p, "%s/Paks/half.bin", root);
    write_file(p, "x", 1);
    snprintf(p, sizeof p, "%s/Paks", root);
    make_segfile(p, "half.bin", (const uint8_t *)"x", 1, SEG);
    CHECK(list_type("/app0/Paks", "half.bin", &cnt) == 8 && cnt == 1);
    printf("segfile: listing ok\n");
}

static void *reader(void *arg) {
    const char *g = (const char *)arg;
    int fd = open_ro(g);
    uint8_t *b = malloc(2 * SEG);
    long bad = 0;
    for (int i = 0; i < 3000; i++) {
        const uint64_t off = rnd() % orig_sz, len = 1 + rnd() % (2 * SEG);
        const ssize_t r = ps4_vfs_pread(fd, b, len, (off_t)off);
        const size_t want = off + len > orig_sz ? orig_sz - off : len;
        if (r != (ssize_t)want || memcmp(b, orig + off, want)) bad++;
    }
    ps4_vfs_close(fd);
    free(b);
    return (void *)bad;
}

static int count_seg_fds(void) {
    int n = 0;
    for (int fd = 0; fd < 4096; fd++) {
        char path[1024];
        if (fcntl(fd, F_GETPATH, path) == 0 && strstr(path, ".nps5seg/")) n++;
    }
    return n;
}

static void test_threads_and_cap(const char *g) {
    pthread_t th[6];
    for (int i = 0; i < 6; i++)
        pthread_create(&th[i], NULL, reader, (void *)g);
    long bad = 0;
    for (int i = 0; i < 6; i++) {
        void *r;
        pthread_join(th[i], &r);
        bad += (long)r;
    }
    CHECK(bad == 0);
    const int open_fds = count_seg_fds();
    CHECK(open_fds >= 1 && open_fds <= 3); // NPS5_VFS_SEGFILES_FDS=3, 16 segments read
    printf("segfile: 6 threads x 3000 preads ok, %d segment fds open (cap 3)\n", open_fds);
}

static void test_bad_manifest(const char *root) {
    char d[1100], p[1200];
    snprintf(d, sizeof d, "%s/Paks", root);
    make_segfile(d, "broken.bin", orig, 3 * SEG + 10, SEG);
    snprintf(p, sizeof p, "%s/broken.bin.nps5seg/0002", d);
    truncate(p, SEG - 1); // a segment shorter than the manifest says
    struct stat st;
    CHECK(ps4_vfs_stat("/app0/Paks/broken.bin", &st) == -1 && errno == ENOENT);
    CHECK(open_ro("/app0/Paks/broken.bin") == -1);
    CHECK(open_ro("/app0/Paks/broken.bin") == -1);
    // an invalid manifest is logged once, however often the game asks (negative cache)
    fflush(stderr);
    char lp[1200];
    snprintf(lp, sizeof lp, "%s.stderr", root);
    const int saved = dup(2);
    FILE *lf = freopen(lp, "w", stderr);
    (void)lf;
    for (int i = 0; i < 5; i++)
        CHECK(ps4_vfs_stat("/app0/Paks/badjson.bin", &st) == -1 && errno == ENOENT);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    clearerr(stderr);
    FILE *rf = fopen(lp, "r");
    char line[512];
    int logged = 0;
    while (rf && fgets(line, sizeof line, rf))
        if (strstr(line, "badjson.bin") && strstr(line, "not used")) logged++;
    if (rf) fclose(rf);
    unlink(lp);
    CHECK(logged == 1);
    printf("segfile: bad manifest refused, logged once ok\n");
}

// the plain file always wins over a segment directory next to it
static void test_plain_wins(const char *root) {
    char d[1100], p[1200];
    snprintf(d, sizeof d, "%s/Paks", root);
    snprintf(p, sizeof p, "%s/plain.bin", d);
    write_file(p, "plainfile", 9);
    make_segfile(d, "plain.bin", orig, SEG + 5, SEG);
    struct stat st;
    CHECK(ps4_vfs_stat("/app0/Paks/plain.bin", &st) == 0 && st.st_size == 9);
    int fd = open_ro("/app0/Paks/plain.bin");
    uint8_t b[16];
    CHECK(fd >= 0 && ps4_vfs_pread(fd, b, 16, 0) == 9 && !memcmp(b, "plainfile", 9));
    ps4_vfs_close(fd);
    printf("segfile: plain file wins ok\n");
}

int main(int argc, char **argv) {
    const int off_mode = argc > 1 && !strcmp(argv[1], "off");
    const char *packed = (argc > 2 && !strcmp(argv[1], "dir")) ? argv[2] : NULL;
    const int auto_mode = argc > 1 && !strcmp(argv[1], "auto"); // no setting at all: the feature is automatic
    if (auto_mode)
        unsetenv("NPS5_VFS_SEGFILES");
    else
        setenv("NPS5_VFS_SEGFILES", off_mode ? "0" : "1", 1);
    setenv("NPS5_VFS_SEGFILES_FDS", "3", 1);
    char root[600];
    C = calloc(1, sizeof(GuestContext));
    C->mem_size = ARENA;
    C->mem_base = calloc(1, ARENA);
    C->fs_base = 0x2000;
    C->rsp = 0x6000;
    if (packed) {
        snprintf(root, sizeof root, "%s", packed);
        char p[700];
        snprintf(p, sizeof p, "%s.orig/Big-ps5.ucas", packed);
        FILE *f = fopen(p, "rb");
        if (!f) {
            printf("segfile_unit: no %s\n", p);
            return 2;
        }
        fseeko(f, 0, SEEK_END);
        orig_sz = (size_t)ftello(f);
        fseeko(f, 0, SEEK_SET);
        orig = malloc(orig_sz);
        CHECK(fread(orig, 1, orig_sz, f) == orig_sz);
        fclose(f);
    } else {
        snprintf(root, sizeof root, "%s/segfile_unit.XXXXXX", getenv("SEGFILE_TMP") ? getenv("SEGFILE_TMP") : "/tmp");
        if (!mkdtemp(root)) return 2;
        char d[700];
        snprintf(d, sizeof d, "%s/Paks", root);
        mkdir(d, 0755);
        orig_sz = SZ;
        orig = malloc(SZ);
        for (size_t i = 0; i < SZ; i++)
            orig[i] = (uint8_t)(rnd() >> 24);
        make_segfile(d, "Big-ps5.ucas", orig, SZ, SEG);
        orig3 = malloc(3 * SEG);
        for (size_t i = 0; i < 3 * SEG; i++)
            orig3[i] = (uint8_t)(rnd() >> 16);
        make_segfile(d, "exact3.bin", orig3, 3 * SEG, SEG);
        { // a segment directory whose manifest is not JSON at all (the directory scan is cached, so it exists up front)
            char bd[800], bm[900];
            snprintf(bd, sizeof bd, "%s/badjson.bin.nps5seg", d);
            mkdir(bd, 0755);
            snprintf(bm, sizeof bm, "%s/manifest.json", bd);
            write_file(bm, "{ not a manifest", 16);
        }
    }
    {
        char ud[700];
        snprintf(ud, sizeof ud, "%s.ud0", root);
        setenv("NPS5_USERDATA_DIR", ud, 1);
        setenv("NPS5_TITLE_ID", "TESTSEG00", 1);
    }
    ps4_vfs_init(root);
    const char *g = "/app0/Paks/Big-ps5.ucas";
    if (off_mode) {
        struct stat st;
        CHECK(ps4_vfs_stat(g, &st) == -1 && open_ro(g) == -1);
        int cnt = 0;
        CHECK(list_type("/app0/Paks", "Big-ps5.ucas", &cnt) == -1 && cnt >= 100); // the directory shows as itself
        printf("segfile: NPS5_VFS_SEGFILES=0: not visible ok\n");
    } else {
        test_stat_access();
        test_sequential(g);
        test_sequential("/app0/PAKS/big-ps5.UCAS");
        test_random_pread(g, 20000);
        printf("segfile: sequential/seek/EOF/20000 random preads ok\n");
        if (orig3) { // a file of exactly 3 segments: the last byte, EOF at the boundary
            int fd = open_ro("/app0/Paks/exact3.bin");
            uint8_t b[8];
            CHECK(ps4_vfs_pread(fd, b, 8, 3 * SEG - 1) == 1 && b[0] == orig3[3 * SEG - 1]);
            CHECK(ps4_vfs_pread(fd, b, 8, 3 * SEG) == 0);
            ps4_vfs_close(fd);
        }
        test_readv_mmap(g);
        test_aio(g);
        test_apr(g);
        test_threads_and_cap(g);
        if (!packed) {
            test_listing(root);
            test_bad_manifest(root);
            test_plain_wins(root);
        }
    }
    if (!packed) {
        char cmd[1400];
        snprintf(cmd, sizeof cmd, "rm -rf '%s' '%s.ud0'", root, root);
        system(cmd);
    }
    printf(fails ? "segfile_unit: %d FAILURES\n" : "segfile_unit: all passed\n", fails);
    return fails ? 1 : 0;
}
