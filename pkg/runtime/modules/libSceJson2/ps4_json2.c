// libSceJson / libSceJson2 (sce::Json C++ object API), HLE.
//
// Every object the guest can hold a pointer to lives in GUEST memory and has the console's size
// and field offsets (checked against AnyPS5 libSceJson2 and the ASTRO BOT eboot, which keeps
// Strings in 8-byte stack slots and reads Object pairs inline):
//   String / Array / Object / iterators : 8 bytes (one guest pointer)
//   Value                               : 32 bytes {parent, auxiliary, payload @16, reserved, type @28}
//   std::pair<String, Value> (Object)   : 48 bytes {key @0, reserved, value @16}
// Internal representation (ours, the guest only sees it through the functions):
//   String  = guest pointer to NUL-terminated UTF-8 (0 = empty)
//   Array   = guest pointer to JVec {count, cap, items}; items = guest pointers to Values
//   Object  = guest pointer to JVec; items = guest pointers to pairs (insertion order)
//   Value payload = scalar bits, or the guest address of a String / Array / Object handle
//   iterator = guest address of the current slot of the items array (0 for an empty container)
// Copies are deep. Memory comes from a bump arena in guest memory and is NOT returned (the
// destructors do nothing): a few bytes per parsed JSON token; fine for settings-sized files.
// Not modelled: the user allocator (MemAllocator), null-access / type-mismatch callbacks,
// special-float formats, the file/callback Parser entry points.
#include "ps4_json2.h"
#include "nps5_thunk.h"
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { J_NULL = 0, J_BOOL = 1, J_INT = 2, J_UINT = 3, J_REAL = 4, J_STR = 5, J_ARR = 6, J_OBJ = 7 };
#define J_ERR_PARSE 0x80848101u

typedef struct JValue {
    uint64_t parent, aux, payload;
    uint32_t reserved, type;
} JValue;
typedef struct JPair {
    uint64_t key, reserved;
    JValue value;
} JPair;
typedef struct JVec {
    uint64_t count, cap, items;
} JVec;
_Static_assert(sizeof(JValue) == 32 && offsetof(JValue, payload) == 16 && offsetof(JValue, type) == 28, "sce::Json::Value layout");
_Static_assert(sizeof(JPair) == 48 && offsetof(JPair, value) == 16, "sce::Json pair layout");

#define H(g) ((g) ? (void *)NPS5_G2H(ctx, (uint64_t)(g)) : NULL)
#define RET(v) do { ctx->rax = (uint64_t)(v); SHIM_RETURN(); } while (0)

// ---- guest arena
static pthread_mutex_t g_j_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_j_cur, g_j_end;
static uint64_t j_alloc(GuestContext *ctx, size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (n == 0) n = 16;
    pthread_mutex_lock(&g_j_mu);
    if (g_j_cur + n > g_j_end) {
        size_t chunk = n > (1u << 20) ? ((n + 0xffff) & ~(size_t)0xffff) : (1u << 20);
        uint64_t g = recomp_vm_alloc(ctx, chunk);
        if (!g) {
            pthread_mutex_unlock(&g_j_mu);
            return 0;
        }
        g_j_cur = g;
        g_j_end = g + chunk;
    }
    uint64_t r = g_j_cur;
    g_j_cur += n;
    pthread_mutex_unlock(&g_j_mu);
    memset(H(r), 0, n);
    return r;
}
static uint64_t j_strndup(GuestContext *ctx, const char *s, size_t n) {
    uint64_t g = j_alloc(ctx, n + 1);
    if (g && n) memcpy(H(g), s, n);
    return g;
}
static uint64_t j_strdup(GuestContext *ctx, const char *s) { return s && *s ? j_strndup(ctx, s, strlen(s)) : 0; }

// Shared "null" objects handed out when the guest asks a Value for the wrong type.
static uint64_t g_j_null; // 64 zero bytes: [0,32) a null Value, +32 an empty handle, +40 a zero scalar, +48 ""
static uint64_t j_null(GuestContext *ctx) {
    if (!g_j_null) {
        uint64_t g = j_alloc(ctx, 64);
        pthread_mutex_lock(&g_j_mu);
        if (!g_j_null) g_j_null = g;
        pthread_mutex_unlock(&g_j_mu);
    }
    return g_j_null;
}
#define J_NULL_VALUE(ctx) (j_null(ctx))
#define J_NULL_HANDLE(ctx) (j_null(ctx) + 32)
#define J_NULL_SCALAR(ctx) (j_null(ctx) + 40)
#define J_EMPTY_CHARS(ctx) (j_null(ctx) + 48)

// ---- handles
static const char *str_chars(GuestContext *ctx, uint64_t handle) { // handle = guest address of a String
    const uint64_t *h = (const uint64_t *)H(handle);
    const char *s = h && *h ? (const char *)H(*h) : NULL;
    return s ? s : "";
}
static uint64_t new_handle(GuestContext *ctx, uint64_t content) {
    uint64_t g = j_alloc(ctx, 8);
    if (g) *(uint64_t *)H(g) = content;
    return g;
}
static JVec *vec_get(GuestContext *ctx, uint64_t handle, int create) {
    uint64_t *h = (uint64_t *)H(handle);
    if (!h) return NULL;
    if (!*h && create) *h = j_alloc(ctx, sizeof(JVec));
    return (JVec *)H(*h);
}
static void vec_push(GuestContext *ctx, JVec *v, uint64_t item) {
    if (!v) return;
    if (v->count == v->cap) {
        uint64_t ncap = v->cap ? v->cap * 2 : 8;
        uint64_t n = j_alloc(ctx, ncap * 8);
        if (!n) return;
        if (v->count) memcpy(H(n), H(v->items), v->count * 8);
        v->items = n;
        v->cap = ncap;
    }
    ((uint64_t *)H(v->items))[v->count++] = item;
}
static uint64_t vec_at(GuestContext *ctx, const JVec *v, uint64_t i) {
    return v && i < v->count ? ((const uint64_t *)H(v->items))[i] : 0;
}

static void value_copy(GuestContext *ctx, JValue *dst, const JValue *src);
static void array_copy(GuestContext *ctx, uint64_t dst_handle, uint64_t src_handle) {
    uint64_t *d = (uint64_t *)H(dst_handle);
    if (!d) return;
    *d = 0;
    const JVec *sv = vec_get(ctx, src_handle, 0);
    if (!sv) return;
    JVec *dv = vec_get(ctx, dst_handle, 1);
    for (uint64_t i = 0; i < sv->count; i++) {
        uint64_t nv = j_alloc(ctx, sizeof(JValue));
        if (!nv) return;
        value_copy(ctx, (JValue *)H(nv), (const JValue *)H(vec_at(ctx, sv, i)));
        vec_push(ctx, dv, nv);
    }
}
static void object_copy(GuestContext *ctx, uint64_t dst_handle, uint64_t src_handle) {
    uint64_t *d = (uint64_t *)H(dst_handle);
    if (!d) return;
    *d = 0;
    const JVec *sv = vec_get(ctx, src_handle, 0);
    if (!sv) return;
    JVec *dv = vec_get(ctx, dst_handle, 1);
    for (uint64_t i = 0; i < sv->count; i++) {
        const JPair *sp = (const JPair *)H(vec_at(ctx, sv, i));
        uint64_t np = j_alloc(ctx, sizeof(JPair));
        if (!np || !sp) return;
        JPair *p = (JPair *)H(np);
        p->key = j_strdup(ctx, (const char *)H(sp->key));
        value_copy(ctx, &p->value, &sp->value);
        vec_push(ctx, dv, np);
    }
}
static void value_set_scalar(JValue *v, uint32_t type, uint64_t payload) {
    if (!v) return;
    v->type = type;
    v->payload = payload;
}
static void value_set_string(GuestContext *ctx, JValue *v, const char *s) {
    if (v) value_set_scalar(v, J_STR, new_handle(ctx, j_strdup(ctx, s)));
}
static void value_set_array(GuestContext *ctx, JValue *v, uint64_t src_handle) {
    if (!v) return;
    uint64_t h = new_handle(ctx, 0);
    if (src_handle) array_copy(ctx, h, src_handle);
    value_set_scalar(v, J_ARR, h);
}
static void value_set_object(GuestContext *ctx, JValue *v, uint64_t src_handle) {
    if (!v) return;
    uint64_t h = new_handle(ctx, 0);
    if (src_handle) object_copy(ctx, h, src_handle);
    value_set_scalar(v, J_OBJ, h);
}
static void value_copy(GuestContext *ctx, JValue *dst, const JValue *src) {
    if (!dst) return;
    if (!src) {
        value_set_scalar(dst, J_NULL, 0);
        return;
    }
    if (dst == src) return;
    switch (src->type) {
    case J_STR: value_set_string(ctx, dst, str_chars(ctx, src->payload)); break;
    case J_ARR: value_set_array(ctx, dst, src->payload); break;
    case J_OBJ: value_set_object(ctx, dst, src->payload); break;
    default: value_set_scalar(dst, src->type, src->payload); break;
    }
}
// The pair of `key` in the object behind `handle`; inserted when missing and `create`.
static JPair *object_find(GuestContext *ctx, uint64_t handle, const char *key, int create, uint64_t *out_addr) {
    if (!key) key = "";
    JVec *v = vec_get(ctx, handle, create);
    if (!v) return NULL;
    for (uint64_t i = 0; i < v->count; i++) {
        uint64_t pa = vec_at(ctx, v, i);
        JPair *p = (JPair *)H(pa);
        const char *k = p && p->key ? (const char *)H(p->key) : "";
        if (p && strcmp(k, key) == 0) {
            if (out_addr) *out_addr = pa;
            return p;
        }
    }
    if (!create) return NULL;
    uint64_t np = j_alloc(ctx, sizeof(JPair));
    if (!np) return NULL;
    JPair *p = (JPair *)H(np);
    p->key = j_strdup(ctx, key);
    vec_push(ctx, v, np);
    if (out_addr) *out_addr = np;
    return p;
}

// ---- text
typedef struct JBuf {
    char *p;
    size_t n, cap;
} JBuf;
static void jb_put(JBuf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t nc = (b->cap ? b->cap * 2 : 256) + n;
        char *np = (char *)realloc(b->p, nc);
        if (!np) return;
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void jb_str(JBuf *b, const char *s) { jb_put(b, s, strlen(s)); }
static void jb_quoted(JBuf *b, const char *s) {
    jb_put(b, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char esc[8];
        switch (c) {
        case '"': jb_put(b, "\\\"", 2); break;
        case '\\': jb_put(b, "\\\\", 2); break;
        case '\n': jb_put(b, "\\n", 2); break;
        case '\r': jb_put(b, "\\r", 2); break;
        case '\t': jb_put(b, "\\t", 2); break;
        case '\b': jb_put(b, "\\b", 2); break;
        case '\f': jb_put(b, "\\f", 2); break;
        default:
            if (c < 0x20) {
                snprintf(esc, sizeof(esc), "\\u%04x", c);
                jb_str(b, esc);
            } else {
                jb_put(b, (const char *)&c, 1);
            }
        }
    }
    jb_put(b, "\"", 1);
}
static void jb_real(JBuf *b, double d) {
    char t[40];
    if (!isfinite(d)) {
        jb_str(b, "null");
        return;
    }
    snprintf(t, sizeof(t), "%.17g", d);
    if (!strpbrk(t, ".eE")) strcat(t, ".0");
    jb_str(b, t);
}
static void serialize(GuestContext *ctx, JBuf *b, const JValue *v, int depth) {
    char t[32];
    if (!v || depth > 256) {
        jb_str(b, "null");
        return;
    }
    switch (v->type) {
    case J_BOOL: jb_str(b, (v->payload & 0xff) ? "true" : "false"); break;
    case J_INT: snprintf(t, sizeof(t), "%" PRId64, (int64_t)v->payload); jb_str(b, t); break;
    case J_UINT: snprintf(t, sizeof(t), "%" PRIu64, v->payload); jb_str(b, t); break;
    case J_REAL: {
        double d;
        memcpy(&d, &v->payload, 8);
        jb_real(b, d);
        break;
    }
    case J_STR: jb_quoted(b, str_chars(ctx, v->payload)); break;
    case J_ARR: {
        const JVec *a = vec_get(ctx, v->payload, 0);
        jb_str(b, "[");
        for (uint64_t i = 0; a && i < a->count; i++) {
            if (i) jb_str(b, ",");
            serialize(ctx, b, (const JValue *)H(vec_at(ctx, a, i)), depth + 1);
        }
        jb_str(b, "]");
        break;
    }
    case J_OBJ: {
        const JVec *o = vec_get(ctx, v->payload, 0);
        jb_str(b, "{");
        for (uint64_t i = 0; o && i < o->count; i++) {
            const JPair *p = (const JPair *)H(vec_at(ctx, o, i));
            if (!p) continue;
            if (i) jb_str(b, ",");
            jb_quoted(b, p->key ? (const char *)H(p->key) : "");
            jb_str(b, ":");
            serialize(ctx, b, &p->value, depth + 1);
        }
        jb_str(b, "}");
        break;
    }
    default: jb_str(b, "null"); break;
    }
}
// Appends text to the guest String at `handle`.
static void string_append(GuestContext *ctx, uint64_t handle, const char *text) {
    uint64_t *h = (uint64_t *)H(handle);
    if (!h || !text) return;
    const char *old = *h ? (const char *)H(*h) : "";
    size_t a = strlen(old), n = strlen(text);
    uint64_t g = j_alloc(ctx, a + n + 1);
    if (!g) return;
    memcpy(H(g), old, a);
    memcpy((char *)H(g) + a, text, n);
    *h = g;
}

// ---- parser
typedef struct JParse {
    GuestContext *ctx;
    const char *p, *end;
    int depth;
} JParse;
static void jp_ws(JParse *s) {
    while (s->p < s->end && (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r')) s->p++;
}
static int jp_hex4(JParse *s, unsigned *out) {
    if (s->end - s->p < 4) return 0;
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s->p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    s->p += 4;
    *out = v;
    return 1;
}
static void jb_utf8(JBuf *b, unsigned cp) {
    char t[4];
    if (cp < 0x80) {
        t[0] = (char)cp;
        jb_put(b, t, 1);
    } else if (cp < 0x800) {
        t[0] = (char)(0xc0 | (cp >> 6));
        t[1] = (char)(0x80 | (cp & 0x3f));
        jb_put(b, t, 2);
    } else if (cp < 0x10000) {
        t[0] = (char)(0xe0 | (cp >> 12));
        t[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        t[2] = (char)(0x80 | (cp & 0x3f));
        jb_put(b, t, 3);
    } else {
        t[0] = (char)(0xf0 | (cp >> 18));
        t[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
        t[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
        t[3] = (char)(0x80 | (cp & 0x3f));
        jb_put(b, t, 4);
    }
}
// Parses a quoted string into a host buffer (caller frees b->p).
static int jp_string(JParse *s, JBuf *b) {
    if (s->p >= s->end || *s->p != '"') return 0;
    s->p++;
    jb_put(b, "", 0);
    while (s->p < s->end) {
        char c = *s->p++;
        if (c == '"') return 1;
        if (c != '\\') {
            jb_put(b, &c, 1);
            continue;
        }
        if (s->p >= s->end) return 0;
        c = *s->p++;
        switch (c) {
        case 'n': jb_put(b, "\n", 1); break;
        case 't': jb_put(b, "\t", 1); break;
        case 'r': jb_put(b, "\r", 1); break;
        case 'b': jb_put(b, "\b", 1); break;
        case 'f': jb_put(b, "\f", 1); break;
        case 'u': {
            unsigned cp, lo;
            if (!jp_hex4(s, &cp)) return 0;
            if (cp >= 0xd800 && cp < 0xdc00 && s->end - s->p >= 6 && s->p[0] == '\\' && s->p[1] == 'u') {
                const char *save = s->p;
                s->p += 2;
                if (jp_hex4(s, &lo) && lo >= 0xdc00 && lo < 0xe000) cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                else s->p = save;
            }
            jb_utf8(b, cp);
            break;
        }
        default: jb_put(b, &c, 1); break; // \" \\ \/ and anything else: the character itself
        }
    }
    return 0;
}
static int jp_value(JParse *s, JValue *out);
static int jp_number(JParse *s, JValue *out) {
    const char *b = s->p;
    int integral = 1;
    if (s->p < s->end && *s->p == '-') s->p++;
    if (s->p >= s->end || *s->p < '0' || *s->p > '9') return 0;
    while (s->p < s->end && *s->p >= '0' && *s->p <= '9') s->p++;
    if (s->p < s->end && *s->p == '.') {
        integral = 0;
        s->p++;
        if (s->p >= s->end || *s->p < '0' || *s->p > '9') return 0;
        while (s->p < s->end && *s->p >= '0' && *s->p <= '9') s->p++;
    }
    if (s->p < s->end && (*s->p == 'e' || *s->p == 'E')) {
        integral = 0;
        s->p++;
        if (s->p < s->end && (*s->p == '+' || *s->p == '-')) s->p++;
        if (s->p >= s->end || *s->p < '0' || *s->p > '9') return 0;
        while (s->p < s->end && *s->p >= '0' && *s->p <= '9') s->p++;
    }
    char t[64];
    size_t n = (size_t)(s->p - b);
    if (n >= sizeof(t)) n = sizeof(t) - 1;
    memcpy(t, b, n);
    t[n] = 0;
    if (integral) {
        errno = 0;
        if (t[0] == '-') {
            long long v = strtoll(t, NULL, 10);
            if (errno == 0) {
                value_set_scalar(out, J_INT, (uint64_t)v);
                return 1;
            }
        } else {
            unsigned long long v = strtoull(t, NULL, 10);
            if (errno == 0) {
                value_set_scalar(out, J_UINT, (uint64_t)v);
                return 1;
            }
        }
    }
    double d = strtod(t, NULL);
    uint64_t bits;
    memcpy(&bits, &d, 8);
    value_set_scalar(out, J_REAL, bits);
    return 1;
}
static int jp_lit(JParse *s, const char *lit) {
    size_t n = strlen(lit);
    if ((size_t)(s->end - s->p) < n || memcmp(s->p, lit, n) != 0) return 0;
    s->p += n;
    return 1;
}
static int jp_value(JParse *s, JValue *out) {
    GuestContext *ctx = s->ctx;
    jp_ws(s);
    if (s->p >= s->end || s->depth > 512) return 0;
    char c = *s->p;
    if (c == '{') {
        s->p++;
        s->depth++;
        value_set_object(ctx, out, 0);
        jp_ws(s);
        if (s->p < s->end && *s->p == '}') {
            s->p++;
            s->depth--;
            return 1;
        }
        for (;;) {
            jp_ws(s);
            JBuf key = {0};
            if (!jp_string(s, &key)) {
                free(key.p);
                return 0;
            }
            jp_ws(s);
            if (s->p >= s->end || *s->p != ':') {
                free(key.p);
                return 0;
            }
            s->p++;
            JPair *p = object_find(ctx, out->payload, key.p ? key.p : "", 1, NULL);
            free(key.p);
            if (!p || !jp_value(s, &p->value)) return 0;
            jp_ws(s);
            if (s->p < s->end && *s->p == ',') {
                s->p++;
                continue;
            }
            if (s->p < s->end && *s->p == '}') {
                s->p++;
                s->depth--;
                return 1;
            }
            return 0;
        }
    }
    if (c == '[') {
        s->p++;
        s->depth++;
        value_set_array(ctx, out, 0);
        jp_ws(s);
        if (s->p < s->end && *s->p == ']') {
            s->p++;
            s->depth--;
            return 1;
        }
        JVec *a = vec_get(ctx, out->payload, 1);
        for (;;) {
            uint64_t nv = j_alloc(ctx, sizeof(JValue));
            if (!nv || !jp_value(s, (JValue *)H(nv))) return 0;
            vec_push(ctx, a, nv);
            jp_ws(s);
            if (s->p < s->end && *s->p == ',') {
                s->p++;
                continue;
            }
            if (s->p < s->end && *s->p == ']') {
                s->p++;
                s->depth--;
                return 1;
            }
            return 0;
        }
    }
    if (c == '"') {
        JBuf b = {0};
        int ok = jp_string(s, &b);
        if (ok) value_set_string(ctx, out, b.p ? b.p : "");
        free(b.p);
        return ok;
    }
    if (c == 't' && jp_lit(s, "true")) {
        value_set_scalar(out, J_BOOL, 1);
        return 1;
    }
    if (c == 'f' && jp_lit(s, "false")) {
        value_set_scalar(out, J_BOOL, 0);
        return 1;
    }
    if (c == 'n' && jp_lit(s, "null")) {
        value_set_scalar(out, J_NULL, 0);
        return 1;
    }
    return jp_number(s, out);
}

// ================================================================ shims
// Initializer / allocator plumbing: accepted, not used.
void shim__ZN3sce4Json12MemAllocatorC2Ev(GuestContext *ctx) { RET(ctx->rdi); }
void shim__ZN3sce4Json12MemAllocatorD2Ev(GuestContext *ctx) { RET(0); }
void shim__ZN3sce4Json14InitParameter2C1Ev(GuestContext *ctx) { RET(ctx->rdi); }
void shim__ZN3sce4Json14InitParameter212setAllocatorEPNS0_12MemAllocatorEPv(GuestContext *ctx) { RET(0); }
void shim__ZN3sce4Json14InitParameter217setFileBufferSizeEm(GuestContext *ctx) { RET(0); }
void shim__ZN3sce4Json11InitializerC1Ev(GuestContext *ctx) { RET(ctx->rdi); }
void shim__ZN3sce4Json11InitializerD1Ev(GuestContext *ctx) { RET(0); }
void shim__ZN3sce4Json11Initializer10initializeEPKNS0_14InitParameter2E(GuestContext *ctx) { RET(0); }
void shim__ZN3sce4Json11Initializer10initializeEPKNS0_13InitParameterE(GuestContext *ctx) { RET(0); }
void shim__ZN3sce4Json11Initializer9terminateEv(GuestContext *ctx) { RET(0); }

// ---- String
void shim__ZN3sce4Json6StringC1Ev(GuestContext *ctx) {
    uint64_t *s = (uint64_t *)H(ctx->rdi);
    if (s) *s = 0;
    RET(ctx->rdi);
}
void shim__ZN3sce4Json6StringC1EPKc(GuestContext *ctx) {
    uint64_t *s = (uint64_t *)H(ctx->rdi);
    if (s) *s = j_strdup(ctx, (const char *)H(ctx->rsi));
    RET(ctx->rdi);
}
void shim__ZN3sce4Json6StringC1ERKS1_(GuestContext *ctx) {
    uint64_t *s = (uint64_t *)H(ctx->rdi);
    if (s) *s = j_strdup(ctx, str_chars(ctx, ctx->rsi));
    RET(ctx->rdi);
}
void shim__ZN3sce4Json6StringD1Ev(GuestContext *ctx) { RET(0); }
void shim__ZN3sce4Json6StringaSERKS1_(GuestContext *ctx) {
    uint64_t *s = (uint64_t *)H(ctx->rdi);
    if (s && ctx->rdi != ctx->rsi) *s = j_strdup(ctx, str_chars(ctx, ctx->rsi));
    RET(ctx->rdi);
}
void shim__ZNK3sce4Json6String5c_strEv(GuestContext *ctx) {
    const uint64_t *s = (const uint64_t *)H(ctx->rdi);
    RET(s && *s ? *s : J_EMPTY_CHARS(ctx));
}
void shim__ZNK3sce4Json6String5emptyEv(GuestContext *ctx) { RET(str_chars(ctx, ctx->rdi)[0] == 0); }
void shim__ZNK3sce4Json6String6lengthEv(GuestContext *ctx) { RET(strlen(str_chars(ctx, ctx->rdi))); }
void shim__ZNK3sce4Json6StringeqEPKc(GuestContext *ctx) {
    const char *o = (const char *)H(ctx->rsi);
    RET(strcmp(str_chars(ctx, ctx->rdi), o ? o : "") == 0);
}

// ---- Value: constructors and set()
#define JV(g) ((JValue *)H(g))
static JValue *value_ctor(GuestContext *ctx) {
    JValue *v = JV(ctx->rdi);
    if (v) memset(v, 0, sizeof(*v));
    return v;
}
static uint64_t xmm0_bits(GuestContext *ctx) {
    uint64_t b;
    memcpy(&b, &ctx->xmm[0], 8);
    return b;
}
void shim__ZN3sce4Json5ValueC1Ev(GuestContext *ctx) { value_ctor(ctx); RET(ctx->rdi); }
void shim__ZN3sce4Json5ValueC1Eb(GuestContext *ctx) { value_set_scalar(value_ctor(ctx), J_BOOL, ctx->rsi & 1); RET(ctx->rdi); }
void shim__ZN3sce4Json5ValueC1El(GuestContext *ctx) { value_set_scalar(value_ctor(ctx), J_INT, ctx->rsi); RET(ctx->rdi); }
void shim__ZN3sce4Json5ValueC1Em(GuestContext *ctx) { value_set_scalar(value_ctor(ctx), J_UINT, ctx->rsi); RET(ctx->rdi); }
void shim__ZN3sce4Json5ValueC1Ed(GuestContext *ctx) { value_set_scalar(value_ctor(ctx), J_REAL, xmm0_bits(ctx)); RET(ctx->rdi); }
void shim__ZN3sce4Json5ValueC1EPKc(GuestContext *ctx) { value_set_string(ctx, value_ctor(ctx), (const char *)H(ctx->rsi)); RET(ctx->rdi); }
void shim__ZN3sce4Json5ValueC1ERKNS0_6StringE(GuestContext *ctx) { value_set_string(ctx, value_ctor(ctx), str_chars(ctx, ctx->rsi)); RET(ctx->rdi); }
void shim__ZN3sce4Json5ValueC1ERKNS0_5ArrayE(GuestContext *ctx) { value_set_array(ctx, value_ctor(ctx), ctx->rsi); RET(ctx->rdi); }
void shim__ZN3sce4Json5ValueC1ERKNS0_6ObjectE(GuestContext *ctx) { value_set_object(ctx, value_ctor(ctx), ctx->rsi); RET(ctx->rdi); }
void shim__ZN3sce4Json5ValueC1ERKS1_(GuestContext *ctx) {
    if (ctx->rdi != ctx->rsi) value_copy(ctx, value_ctor(ctx), JV(ctx->rsi));
    RET(ctx->rdi);
}
void shim__ZN3sce4Json5ValueD1Ev(GuestContext *ctx) { RET(0); }
void shim__ZN3sce4Json5ValueaSERKS1_(GuestContext *ctx) { value_copy(ctx, JV(ctx->rdi), JV(ctx->rsi)); RET(ctx->rdi); }
void shim__ZN3sce4Json5Value3setERKS1_(GuestContext *ctx) { value_copy(ctx, JV(ctx->rdi), JV(ctx->rsi)); RET(ctx->rdi); }
void shim__ZN3sce4Json5Value3setEb(GuestContext *ctx) { value_set_scalar(JV(ctx->rdi), J_BOOL, ctx->rsi & 1); RET(ctx->rdi); }
void shim__ZN3sce4Json5Value3setEl(GuestContext *ctx) { value_set_scalar(JV(ctx->rdi), J_INT, ctx->rsi); RET(ctx->rdi); }
void shim__ZN3sce4Json5Value3setEm(GuestContext *ctx) { value_set_scalar(JV(ctx->rdi), J_UINT, ctx->rsi); RET(ctx->rdi); }
void shim__ZN3sce4Json5Value3setEd(GuestContext *ctx) { value_set_scalar(JV(ctx->rdi), J_REAL, xmm0_bits(ctx)); RET(ctx->rdi); }
void shim__ZN3sce4Json5Value3setEPKc(GuestContext *ctx) { value_set_string(ctx, JV(ctx->rdi), (const char *)H(ctx->rsi)); RET(ctx->rdi); }
void shim__ZN3sce4Json5Value3setERKNS0_6StringE(GuestContext *ctx) { value_set_string(ctx, JV(ctx->rdi), str_chars(ctx, ctx->rsi)); RET(ctx->rdi); }
void shim__ZN3sce4Json5Value3setERKNS0_5ArrayE(GuestContext *ctx) { value_set_array(ctx, JV(ctx->rdi), ctx->rsi); RET(ctx->rdi); }
void shim__ZN3sce4Json5Value3setERKNS0_6ObjectE(GuestContext *ctx) { value_set_object(ctx, JV(ctx->rdi), ctx->rsi); RET(ctx->rdi); }
void shim__ZN3sce4Json5Value3setENS0_9ValueTypeE(GuestContext *ctx) {
    JValue *v = JV(ctx->rdi);
    switch ((uint32_t)ctx->rsi) {
    case J_STR: value_set_string(ctx, v, ""); break;
    case J_ARR: value_set_array(ctx, v, 0); break;
    case J_OBJ: value_set_object(ctx, v, 0); break;
    default: value_set_scalar(v, (uint32_t)ctx->rsi <= J_REAL ? (uint32_t)ctx->rsi : J_NULL, 0); break;
    }
    RET(ctx->rdi);
}

// ---- Value: accessors. The scalar getters return REFERENCES (a guest address).
void shim__ZNK3sce4Json5Value7getTypeEv(GuestContext *ctx) {
    const JValue *v = JV(ctx->rdi);
    RET(v ? v->type : J_NULL);
}
void shim__ZNK3sce4Json5Value10getBooleanEv(GuestContext *ctx) {
    const JValue *v = JV(ctx->rdi);
    RET(v && v->type == J_BOOL ? ctx->rdi + offsetof(JValue, payload) : J_NULL_SCALAR(ctx));
}
void shim__ZNK3sce4Json5Value10getIntegerEv(GuestContext *ctx) {
    const JValue *v = JV(ctx->rdi);
    RET(v && (v->type == J_INT || v->type == J_UINT) ? ctx->rdi + offsetof(JValue, payload) : J_NULL_SCALAR(ctx));
}
void shim__ZNK3sce4Json5Value11getUIntegerEv(GuestContext *ctx) {
    const JValue *v = JV(ctx->rdi);
    RET(v && (v->type == J_INT || v->type == J_UINT) ? ctx->rdi + offsetof(JValue, payload) : J_NULL_SCALAR(ctx));
}
void shim__ZNK3sce4Json5Value7getRealEv(GuestContext *ctx) {
    const JValue *v = JV(ctx->rdi);
    RET(v && v->type == J_REAL ? ctx->rdi + offsetof(JValue, payload) : J_NULL_SCALAR(ctx));
}
void shim__ZNK3sce4Json5Value9getStringEv(GuestContext *ctx) {
    const JValue *v = JV(ctx->rdi);
    RET(v && v->type == J_STR && v->payload ? v->payload : J_NULL_HANDLE(ctx));
}
void shim__ZNK3sce4Json5Value8getArrayEv(GuestContext *ctx) {
    const JValue *v = JV(ctx->rdi);
    RET(v && v->type == J_ARR && v->payload ? v->payload : J_NULL_HANDLE(ctx));
}
void shim__ZNK3sce4Json5Value9getObjectEv(GuestContext *ctx) {
    const JValue *v = JV(ctx->rdi);
    RET(v && v->type == J_OBJ && v->payload ? v->payload : J_NULL_HANDLE(ctx));
}
void shim__ZN3sce4Json5Value10referArrayEv(GuestContext *ctx) {
    JValue *v = JV(ctx->rdi);
    if (v && v->type != J_ARR) value_set_array(ctx, v, 0);
    RET(v ? v->payload : J_NULL_HANDLE(ctx));
}
void shim__ZN3sce4Json5Value11referObjectEv(GuestContext *ctx) {
    JValue *v = JV(ctx->rdi);
    if (v && v->type != J_OBJ) value_set_object(ctx, v, 0);
    RET(v ? v->payload : J_NULL_HANDLE(ctx));
}
void shim__ZNK3sce4Json5Value5countEv(GuestContext *ctx) {
    const JValue *v = JV(ctx->rdi);
    const JVec *c = v && (v->type == J_ARR || v->type == J_OBJ) ? vec_get(ctx, v->payload, 0) : NULL;
    RET(c ? c->count : 0);
}
void shim__ZNK3sce4Json5ValueixEPKc(GuestContext *ctx) {
    const JValue *v = JV(ctx->rdi);
    uint64_t pa = 0;
    if (v && v->type == J_OBJ && object_find(ctx, v->payload, (const char *)H(ctx->rsi), 0, &pa)) RET(pa + offsetof(JPair, value));
    RET(J_NULL_VALUE(ctx));
}
void shim__ZNK3sce4Json5ValueixEm(GuestContext *ctx) {
    const JValue *v = JV(ctx->rdi);
    if (v && v->type == J_ARR) {
        uint64_t e = vec_at(ctx, vec_get(ctx, v->payload, 0), ctx->rsi);
        if (e) RET(e);
    } else if (v && v->type == J_OBJ) {
        uint64_t e = vec_at(ctx, vec_get(ctx, v->payload, 0), ctx->rsi);
        if (e) RET(e + offsetof(JPair, value));
    }
    RET(J_NULL_VALUE(ctx));
}
// Value *Value::referValue(const String &key): a LOOKUP that returns a pointer, null when the value is not an
// object or has no such key. It never inserts and never changes the value's type. ASTRO BOT uses it as the
// "has key" test of optional fields (`if (v.referValue("replaceTo")) ... v["replaceTo"].getString()`): the
// earlier shim created the key with a null value and returned it, so every optional field read as present
// and empty (45 "v.getType() == kValueTypeBoolean" asserts per title load were the visible sign).
void shim__ZN3sce4Json5Value10referValueERKNS0_6StringE(GuestContext *ctx) {
    JValue *v = JV(ctx->rdi);
    uint64_t pa = 0;
    if (v && ctx->rsi && v->type == J_OBJ && object_find(ctx, v->payload, str_chars(ctx, ctx->rsi), 0, &pa)) RET(pa + offsetof(JPair, value));
    RET(0);
}
void shim__ZN3sce4Json5Value9serializeERNS0_6StringE(GuestContext *ctx) {
    JBuf b = {0};
    serialize(ctx, &b, JV(ctx->rdi), 0);
    string_append(ctx, ctx->rsi, b.p ? b.p : "");
    free(b.p);
    RET(0);
}
void shim__ZNK3sce4Json5Value8toStringERNS0_6StringE(GuestContext *ctx) {
    const JValue *v = JV(ctx->rdi);
    if (v && v->type == J_STR) {
        string_append(ctx, ctx->rsi, str_chars(ctx, v->payload));
    } else if (v && v->type == J_ARR) {
        string_append(ctx, ctx->rsi, "array");
    } else if (v && v->type == J_OBJ) {
        string_append(ctx, ctx->rsi, "object");
    } else {
        JBuf b = {0};
        serialize(ctx, &b, v, 0);
        string_append(ctx, ctx->rsi, b.p ? b.p : "");
        free(b.p);
    }
    RET(0);
}

// ---- Array
void shim__ZN3sce4Json5ArrayC1Ev(GuestContext *ctx) {
    uint64_t *a = (uint64_t *)H(ctx->rdi);
    if (a) *a = 0;
    RET(ctx->rdi);
}
void shim__ZN3sce4Json5ArrayD1Ev(GuestContext *ctx) { RET(0); }
void shim__ZN3sce4Json5Array9push_backERKNS0_5ValueE(GuestContext *ctx) {
    JVec *a = vec_get(ctx, ctx->rdi, 1);
    uint64_t nv = a ? j_alloc(ctx, sizeof(JValue)) : 0;
    if (nv) {
        value_copy(ctx, JV(nv), JV(ctx->rsi));
        vec_push(ctx, a, nv);
    }
    RET(ctx->rdi);
}
void shim__ZNK3sce4Json5Array4sizeEv(GuestContext *ctx) {
    const JVec *a = vec_get(ctx, ctx->rdi, 0);
    RET(a ? a->count : 0);
}
void shim__ZNK3sce4Json5Array5emptyEv(GuestContext *ctx) {
    const JVec *a = vec_get(ctx, ctx->rdi, 0);
    RET(!a || a->count == 0);
}
void shim__ZNK3sce4Json5Array4backEv(GuestContext *ctx) {
    const JVec *a = vec_get(ctx, ctx->rdi, 0);
    uint64_t e = a && a->count ? vec_at(ctx, a, a->count - 1) : 0;
    RET(e ? e : J_NULL_VALUE(ctx));
}
// begin()/end() return the iterator through the hidden result pointer: rdi = result, rsi = this.
static void iter_result(GuestContext *ctx, int end) {
    const JVec *c = vec_get(ctx, ctx->rsi, 0);
    uint64_t *out = (uint64_t *)H(ctx->rdi);
    if (out) *out = c && c->count ? c->items + (end ? c->count * 8 : 0) : 0;
    RET(ctx->rdi);
}
static void iter_next(GuestContext *ctx) {
    uint64_t *it = (uint64_t *)H(ctx->rdi);
    if (it && *it) *it += 8;
    RET(ctx->rdi);
}
static void iter_ne(GuestContext *ctx) {
    const uint64_t *a = (const uint64_t *)H(ctx->rdi), *b = (const uint64_t *)H(ctx->rsi);
    RET((a ? *a : 0) != (b ? *b : 0));
}
static uint64_t iter_item(GuestContext *ctx) {
    const uint64_t *it = (const uint64_t *)H(ctx->rdi);
    const uint64_t *slot = it && *it ? (const uint64_t *)H(*it) : NULL;
    return slot ? *slot : 0;
}
void shim__ZNK3sce4Json5Array5beginEv(GuestContext *ctx) { iter_result(ctx, 0); }
void shim__ZNK3sce4Json5Array3endEv(GuestContext *ctx) { iter_result(ctx, 1); }
void shim__ZN3sce4Json5Array8iteratorD1Ev(GuestContext *ctx) { RET(0); }
void shim__ZN3sce4Json5Array8iteratorppEv(GuestContext *ctx) { iter_next(ctx); }
void shim__ZNK3sce4Json5Array8iteratorneERKS2_(GuestContext *ctx) { iter_ne(ctx); }
void shim__ZNK3sce4Json5Array8iteratordeEv(GuestContext *ctx) {
    uint64_t e = iter_item(ctx);
    RET(e ? e : J_NULL_VALUE(ctx));
}

// ---- Object
void shim__ZN3sce4Json6ObjectC1Ev(GuestContext *ctx) {
    uint64_t *o = (uint64_t *)H(ctx->rdi);
    if (o) *o = 0;
    RET(ctx->rdi);
}
void shim__ZN3sce4Json6ObjectC1ERKS1_(GuestContext *ctx) {
    if (ctx->rdi != ctx->rsi) object_copy(ctx, ctx->rdi, ctx->rsi);
    RET(ctx->rdi);
}
void shim__ZN3sce4Json6ObjectD1Ev(GuestContext *ctx) { RET(0); }
void shim__ZN3sce4Json6ObjectaSERKS1_(GuestContext *ctx) {
    if (ctx->rdi != ctx->rsi) object_copy(ctx, ctx->rdi, ctx->rsi);
    RET(ctx->rdi);
}
void shim__ZN3sce4Json6Object5clearEv(GuestContext *ctx) {
    JVec *o = vec_get(ctx, ctx->rdi, 0);
    if (o) o->count = 0;
    RET(0);
}
void shim__ZN3sce4Json6ObjectixERKNS0_6StringE(GuestContext *ctx) {
    uint64_t pa = 0;
    if (object_find(ctx, ctx->rdi, str_chars(ctx, ctx->rsi), 1, &pa)) RET(pa + offsetof(JPair, value));
    RET(J_NULL_VALUE(ctx));
}
void shim__ZNK3sce4Json6Object5beginEv(GuestContext *ctx) { iter_result(ctx, 0); }
void shim__ZNK3sce4Json6Object3endEv(GuestContext *ctx) { iter_result(ctx, 1); }
void shim__ZN3sce4Json6Object8iteratorD1Ev(GuestContext *ctx) { RET(0); }
void shim__ZN3sce4Json6Object8iteratorppEv(GuestContext *ctx) { iter_next(ctx); }
void shim__ZNK3sce4Json6Object8iteratorneERKS2_(GuestContext *ctx) { iter_ne(ctx); }
void shim__ZNK3sce4Json6Object8iteratordeEv(GuestContext *ctx) {
    uint64_t e = iter_item(ctx);
    if (!e) { // dereferencing end(): an empty pair, so the caller reads a null Value, not address 0
        static uint64_t g_empty_pair;
        if (!g_empty_pair) g_empty_pair = j_alloc(ctx, sizeof(JPair));
        e = g_empty_pair;
    }
    RET(e);
}

// ---- Parser::parse(Value &out, const char *text, size_t size) (static)
void shim__ZN3sce4Json6Parser5parseERNS0_5ValueEPKcm(GuestContext *ctx) {
    JValue *out = JV(ctx->rdi);
    const char *text = (const char *)H(ctx->rsi);
    if (!out || !text) RET((uint32_t)J_ERR_PARSE);
    JParse s = {ctx, text, text + ctx->rdx, 0};
    if (ctx->rdx >= 3 && (unsigned char)text[0] == 0xef && (unsigned char)text[1] == 0xbb && (unsigned char)text[2] == 0xbf) s.p += 3; // UTF-8 BOM
    // Empty text is a parse error that leaves the destination as it was (the console library returns before it
    // touches the value); the title parses optional, usually empty, property strings this way.
    if (ctx->rdx == 0) RET((uint32_t)J_ERR_PARSE);
    value_set_scalar(out, J_NULL, 0);
    if (!jp_value(&s, out)) {
        char head[49];
        size_t hn = ctx->rdx < sizeof(head) - 1 ? (size_t)ctx->rdx : sizeof(head) - 1;
        memcpy(head, text, hn);
        head[hn] = 0;
        for (size_t i = 0; i < hn; i++) if ((unsigned char)head[i] < 0x20) head[i] = '.';
        NPS5_LOG(NPS5_S_HLE, NPS5_L_WARN, "sce::Json::Parser::parse: syntax error at offset %lld of %llu: \"%s\"", (long long)(s.p - text), (unsigned long long)ctx->rdx, head);
        ctx->rax = (uint64_t)(int64_t)(int32_t)J_ERR_PARSE;
        SHIM_RETURN();
    }
    RET(0);
}
