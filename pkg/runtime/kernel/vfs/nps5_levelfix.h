// nps5_levelfix.h: read-time level attribute fix served through the VFS (header-only, pure helpers + one driver).
//
// Same rules as the launcher's `levelfix` rows (scripts/nps5lib/scenery.py apply_fix), so every launch path gets the fix:
// the level file of the player's own game folder is never written; a patched copy goes to a cache directory and is mounted
// read-only over the guest path (so read / pread / mmap / stat all see one consistent file, of the same size).
//   - the object is found by its <Name>: the LAST <len> characters hash (sha256, first 16 hex digits) to <hash>; no game name
//     is in the source. Exactly one object of the class must match and carry exactly one <attr> value;
//   - the source must hash to the table's sha256 when it comes from the game folder; a file that comes from NPS5_OVERLAY_DIR
//     (scenery overlay / launcher-fixed file) is already derived from it, so only the structure is checked there;
//   - value == old -> replaced by new ("applied"); value == new -> nothing to do ("already"); anything else -> "skipped: why".
// The patched copy lives in a CACHE directory (nps5_lf_pick_cache_dir: NPS5_LEVELFIX_CACHE_DIR, else next to the program cache
// KYTY_PROGRAM_CACHE_PATH, else $PS4RECOMP_CACHE_DIR, else <app support>/nativeps5/<title>/gpu-cache), never in the per-title
// userdata (the save area), and never inside the game folder.
// It runs ONCE per launch (nps5_levelfix_apply): a second ps4_vfs_init (which clears the mounts) only re-mounts the recorded copies.
// Default ON for the titles in the table; NPS5_LEVELFIX=0 opts out. One log line per level at init.
#ifndef NPS5_LEVELFIX_H
#define NPS5_LEVELFIX_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    const char *title; // title id
    const char *level; // level folder name (log + cache file name)
    const char *rel; // path below /app0
    const char *src; // sha256 (hex) of the original level file
    const char *cls; // <Lox> value of the object
    int obj_len; // the object's <Name> suffix length
    const char *obj_hash; // sha256 hex prefix (16) of that suffix
    const char *attr;
    const char *old_v, *new_v;
} nps5_lf_entry;

// Built-in table. No readable game names: the object is a length + hash key.
static const nps5_lf_entry nps5_lf_table[] = {
    {"PPSA21564", "hero_ape_escape", "data/prein/levels/hero_ape_escape/level.lvx",
     "717419584dc68f6231c860c93deb152cdce3f00185c3b06b7b294998061c7b3c", "delay_trigger", 59, "a1da091914d9c49b",
     "EnterDelay", "0.6", "8.0"},
};

// ---- sha256 -------------------------------------------------------------------------------------------------------------
static inline uint32_t nps5_lf_rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static void nps5_lf_sha256_hex(const uint8_t *d, size_t n, char hex[65]) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t total = ((n + 9 + 63) / 64) * 64;
    for (size_t off = 0; off < total; off += 64) {
        uint8_t blk[64];
        for (int i = 0; i < 64; i++) {
            size_t p = off + (size_t)i;
            if (p < n)
                blk[i] = d[p];
            else if (p == n)
                blk[i] = 0x80;
            else if (p >= total - 8)
                blk[i] = (uint8_t)(((uint64_t)n * 8) >> (8 * (7 - (p - (total - 8)))));
            else
                blk[i] = 0;
        }
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)blk[4 * i] << 24 | (uint32_t)blk[4 * i + 1] << 16 | (uint32_t)blk[4 * i + 2] << 8 |
                   blk[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = nps5_lf_rotr(w[i - 15], 7) ^ nps5_lf_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = nps5_lf_rotr(w[i - 2], 17) ^ nps5_lf_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], dd = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (nps5_lf_rotr(e, 6) ^ nps5_lf_rotr(e, 11) ^ nps5_lf_rotr(e, 25)) + ((e & f) ^ (~e & g)) +
                          K[i] + w[i];
            uint32_t t2 =
                (nps5_lf_rotr(a, 2) ^ nps5_lf_rotr(a, 13) ^ nps5_lf_rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g;
            g = f;
            f = e;
            e = dd + t1;
            dd = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += dd;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
    for (int i = 0; i < 8; i++)
        snprintf(hex + 8 * i, 9, "%08x", h[i]);
}

// ---- the edit -----------------------------------------------------------------------------------------------------------
enum { NPS5_LF_APPLIED = 0, NPS5_LF_ALREADY = 1, NPS5_LF_SKIPPED = 2 };

static const char *nps5_lf_memmem(const char *h, size_t hn, const char *nd, size_t nn) {
    if (nn == 0 || hn < nn) return NULL;
    for (size_t i = 0; i + nn <= hn; i++)
        if (h[i] == nd[0] && memcmp(h + i, nd, nn) == 0) return h + i;
    return NULL;
}

// Content of <tag>...</tag> inside [b, b+n): 1 and (*s,*len) when found.
static int nps5_lf_field(const char *b, size_t n, const char *tag, const char **s, size_t *len) {
    char o[64], c[64];
    snprintf(o, sizeof o, "<%s>", tag);
    snprintf(c, sizeof c, "</%s>", tag);
    const char *p = nps5_lf_memmem(b, n, o, strlen(o));
    if (!p) return 0;
    p += strlen(o);
    const char *q = nps5_lf_memmem(p, (size_t)(b + n - p), c, strlen(c));
    if (!q) return 0;
    *s = p;
    *len = (size_t)(q - p);
    return 1;
}

// Applies one entry to a buffer. *out (malloc, same length as in) is set on APPLIED. reason is set on SKIPPED.
// src_check: require the sha256 of `in` to equal the entry's src.
static int nps5_lf_apply(const char *in, size_t n, const nps5_lf_entry *e, int src_check, char **out, char *reason,
                         size_t rsz) {
    *out = NULL;
#define LF_SKIP(...)                                                                                                   \
    do {                                                                                                               \
        snprintf(reason, rsz, __VA_ARGS__);                                                                            \
        return NPS5_LF_SKIPPED;                                                                                        \
    } while (0)
    if (strlen(e->old_v) != strlen(e->new_v)) LF_SKIP("table row changes the length");
    if (src_check) {
        char hex[65];
        nps5_lf_sha256_hex((const uint8_t *)in, n, hex);
        if (strcmp(hex, e->src) != 0) LF_SKIP("source sha256 differs (another game version?)");
    }
    const char *hit = NULL;
    size_t hit_n = 0;
    int hits = 0;
    const char *p = in, *end = in + n;
    while (p < end) {
        const char *a = nps5_lf_memmem(p, (size_t)(end - p), "<LevelObject>", 13);
        if (!a) break;
        const char *z = nps5_lf_memmem(a, (size_t)(end - a), "</LevelObject>", 14);
        if (!z) break;
        size_t bn = (size_t)(z + 14 - a);
        const char *s;
        size_t l;
        if (nps5_lf_field(a, bn, "Lox", &s, &l) && l == strlen(e->cls) && memcmp(s, e->cls, l) == 0 &&
            nps5_lf_field(a, bn, "Name", &s, &l) && l >= (size_t)e->obj_len) {
            char hex[65];
            nps5_lf_sha256_hex((const uint8_t *)s + l - e->obj_len, (size_t)e->obj_len, hex);
            if (strncmp(hex, e->obj_hash, 16) == 0) {
                hit = a;
                hit_n = bn;
                hits++;
            }
        }
        p = z + 14;
    }
    if (hits != 1) LF_SKIP("%d objects match (exactly 1 needed)", hits);
    char open[96];
    snprintf(open, sizeof open, "<%s type=\"", e->attr);
    const char *v = NULL;
    int vals = 0;
    for (const char *q = hit;;) {
        q = nps5_lf_memmem(q, (size_t)(hit + hit_n - q), open, strlen(open));
        if (!q) break;
        vals++;
        v = q;
        q += strlen(open);
    }
    if (vals != 1) LF_SKIP("the object has %d <%s> values (exactly 1 needed)", vals, e->attr);
    const char *gt = memchr(v, '>', (size_t)(hit + hit_n - v));
    char close[96];
    snprintf(close, sizeof close, "</%s>", e->attr);
    const char *cl = gt ? nps5_lf_memmem(gt + 1, (size_t)(hit + hit_n - gt - 1), close, strlen(close)) : NULL;
    if (!gt || !cl) LF_SKIP("malformed <%s> element", e->attr);
    size_t vl = (size_t)(cl - gt - 1);
    if (vl == strlen(e->new_v) && memcmp(gt + 1, e->new_v, vl) == 0) return NPS5_LF_ALREADY;
    if (!(vl == strlen(e->old_v) && memcmp(gt + 1, e->old_v, vl) == 0)) {
        char got[32];
        snprintf(got, sizeof got, "%.*s", (int)(vl < 24 ? vl : 24), gt + 1);
        LF_SKIP("%s is '%s', expected '%s' (another game version?)", e->attr, got, e->old_v);
    }
    char *o = (char *)malloc(n ? n : 1);
    if (!o) LF_SKIP("out of memory");
    memcpy(o, in, n);
    memcpy(o + (size_t)(gt + 1 - in), e->new_v, vl);
    *out = o;
    return NPS5_LF_APPLIED;
#undef LF_SKIP
}

// ---- cache directory choice (pure) ---------------------------------------------------------------------------------------
// Picks the directory of the patched copies; 1 and out set, or 0 when no safe place exists. A directory inside the per-title
// userdata root (the save area) is refused, so a patched level is never written next to the player's saves.
static int nps5_lf_under(const char *path, const char *root) {
    if (!root || !*root) return 0;
    size_t n = strlen(root);
    while (n > 1 && root[n - 1] == '/')
        n--;
    return strncmp(path, root, n) == 0 && (path[n] == '/' || path[n] == 0);
}
static int nps5_lf_pick_cache_dir(const char *explicit_dir, const char *program_cache_path,
                                  const char *recomp_cache_dir, const char *home, const char *tid,
                                  const char *userdata_root, char *out, size_t sz) {
    int n = -1;
    if (explicit_dir && *explicit_dir)
        n = snprintf(out, sz, "%s", explicit_dir);
    else if (program_cache_path && strchr(program_cache_path, '/')) {
        const char *sl = strrchr(program_cache_path, '/');
        n = snprintf(out, sz, "%.*s/levelfix", (int)(sl - program_cache_path), program_cache_path);
        if (sl == program_cache_path) n = snprintf(out, sz, "/levelfix");
    } else if (recomp_cache_dir && *recomp_cache_dir)
        n = snprintf(out, sz, "%s/levelfix", recomp_cache_dir);
    else if (home && *home && tid && *tid)
        n = snprintf(out, sz, "%s/Library/Application Support/nativeps5/%s/gpu-cache/levelfix", home, tid);
    if (n < 0 || (size_t)n >= sz) return 0;
    if (nps5_lf_under(out, userdata_root)) return 0;
    return 1;
}

// ---- driver (file layer behind callbacks, so a test can fake it) ---------------------------------------------------------
typedef struct {
    const char *title; // title id ("" = unknown)
    const char *opt_out; // getenv("NPS5_LEVELFIX") value or NULL; "0" = off
    int (*resolve)(void *u, const char *guest, char *host, size_t sz); // 0 or -errno; sees overlay mounts
    int (*in_app_root)(void *u, const char *host); // 1 when the host path is inside the game folder
    int (*cache_dir)(void *u, char *dir, size_t sz); // 1 and a created directory, or 0
    int (*mount_ro)(void *u, const char *guest, const char *host);
    void *u;
    struct nps5_lf_state *st; // once-per-launch memory (NULL = every call runs)
} nps5_lf_env;

// Once per launch: the mounts made by the first run, kept so a later init (which clears the mounts) can re-mount without
// re-reading or re-hashing anything and without another log line.
#define NPS5_LF_MAX_MOUNTS 8
typedef struct nps5_lf_state {
    int ran;
    int n;
    char guest[NPS5_LF_MAX_MOUNTS][600], host[NPS5_LF_MAX_MOUNTS][1100];
} nps5_lf_state;

static int nps5_lf_read_file(const char *path, char **buf, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || st.st_size < 0 || st.st_size > (off_t)(512u << 20)) {
        fclose(f);
        return 0;
    }
    char *b = (char *)malloc((size_t)st.st_size + 1);
    if (!b) {
        fclose(f);
        return 0;
    }
    size_t r = fread(b, 1, (size_t)st.st_size, f);
    fclose(f);
    if (r != (size_t)st.st_size) {
        free(b);
        return 0;
    }
    *buf = b;
    *n = r;
    return 1;
}

static void nps5_lf_log(const nps5_lf_entry *e, const char *res) {
    fprintf(stderr, "[levelfix] %s: %s %s -> %s (%s)\n", e->level, e->attr, e->old_v, e->new_v, res);
}

// Runs every table entry of the title. Returns the number of levels now served patched (mounted).
static int nps5_levelfix_run(const nps5_lf_env *env, const nps5_lf_entry *tab, size_t ntab) {
    int mounted = 0;
    for (size_t i = 0; i < ntab; i++) {
        const nps5_lf_entry *e = &tab[i];
        if (!env->title || strcmp(env->title, e->title) != 0) continue;
        char why[256];
        if (env->opt_out && !strcmp(env->opt_out, "0")) {
            nps5_lf_log(e, "skipped: NPS5_LEVELFIX=0");
            continue;
        }
        char guest[600], host[1100];
        snprintf(guest, sizeof guest, "/app0/%s", e->rel);
        if (env->resolve(env->u, guest, host, sizeof host) != 0) {
            nps5_lf_log(e, "skipped: level file not found");
            continue;
        }
        char *src = NULL;
        size_t sn = 0;
        if (!nps5_lf_read_file(host, &src, &sn)) {
            nps5_lf_log(e, "skipped: level file not readable");
            continue;
        }
        int from_game = env->in_app_root(env->u, host);
        char *out = NULL;
        int r = nps5_lf_apply(src, sn, e, from_game, &out, why, sizeof why);
        free(src);
        if (r == NPS5_LF_ALREADY) {
            nps5_lf_log(e, "already");
            continue;
        }
        if (r == NPS5_LF_SKIPPED) {
            char m[300];
            snprintf(m, sizeof m, "skipped: %s", why);
            nps5_lf_log(e, m);
            continue;
        }
        char dir[1000], path[1100], tmp[1120];
        if (!env->cache_dir(env->u, dir, sizeof dir)) {
            nps5_lf_log(e, "skipped: no cache directory");
            free(out);
            continue;
        }
        char hx[65];
        nps5_lf_sha256_hex((const uint8_t *)out, sn, hx);
        snprintf(path, sizeof path, "%s/%s-%.16s.lvx", dir, e->level,
                 hx); // content-addressed: a stale file can never be served
        char *have = NULL;
        size_t hn = 0;
        int ok = nps5_lf_read_file(path, &have, &hn) && hn == sn && memcmp(have, out, sn) == 0;
        free(have);
        if (!ok) {
            snprintf(tmp, sizeof tmp, "%s.tmp%d", path, (int)getpid());
            FILE *f = fopen(tmp, "wb");
            ok = f && fwrite(out, 1, sn, f) == sn && fclose(f) == 0 && rename(tmp, path) == 0;
            if (!ok) unlink(tmp);
        }
        free(out);
        if (!ok) {
            nps5_lf_log(e, "skipped: cannot write the cache copy");
            continue;
        }
        if (env->mount_ro(env->u, guest, path) != 0) {
            nps5_lf_log(e, "skipped: mount failed");
            continue;
        }
        if (env->st && env->st->n < NPS5_LF_MAX_MOUNTS) {
            snprintf(env->st->guest[env->st->n], sizeof env->st->guest[0], "%s", guest);
            snprintf(env->st->host[env->st->n], sizeof env->st->host[0], "%s", path);
            env->st->n++;
        }
        nps5_lf_log(e, "applied");
        mounted++;
    }
    return mounted;
}

// Entry point of ps4_vfs_init: the first call runs the table; later calls only re-mount what the first one made.
static int nps5_levelfix_apply(const nps5_lf_env *env, const nps5_lf_entry *tab, size_t ntab) {
    nps5_lf_state *st = env->st;
    if (!st) return nps5_levelfix_run(env, tab, ntab);
    if (!st->ran) {
        st->ran = 1;
        return nps5_levelfix_run(env, tab, ntab);
    }
    int mounted = 0;
    for (int i = 0; i < st->n; i++)
        if (env->mount_ro(env->u, st->guest[i], st->host[i]) == 0) mounted++;
    return mounted;
}

#endif
