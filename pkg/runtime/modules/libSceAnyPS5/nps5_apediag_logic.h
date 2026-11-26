// SPDX-License-Identifier: GPL-2.0-or-later
//
// Pure logic of the [apediag] object tracer (nps5_apediag_shims.h): no guest memory, no stdio of its own, so
// tests/apediag/apediag_unit.c can drive it with synthetic object tables.
//
// [apediag] is an off-by-default diagnostic for a level whose scripted chain (counter -> delay trigger -> sequence) does not
// advance. It watches objects of a few guest classes from wrapped update functions and logs state TRANSITIONS only:
//   - a per-object shadow of a window of the object (diff: "+0xOFF:old->new", an offset that changed more than a limit is muted),
//   - level-object identity: the title keeps a 32-bit FNV-1a hash of the level object's id string in a record the object points
//     to; the tracer scans an object (and one pointer level deep) for the hashes listed in a file the USER passes
//     (NPS5_APEDIAG_IDS=<file>, lines "<hex hash> <label>"; the file is local, nothing about a level is compiled in),
//   - a counter's "ready" count from the states of the title's bot objects (same rule as the title's own counter update).
#ifndef NPS5_APEDIAG_LOGIC_H
#define NPS5_APEDIAG_LOGIC_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NPS5_AD_MAX_IDS 256
#define NPS5_AD_WIN_MAX 0x400 // widest watched window (bytes)

typedef struct {
    uint32_t hash;
    char label[40];
} Nps5AdId;

static inline uint32_t nps5_ad_fnv(const void *p, size_t n) {
    const uint8_t *s = (const uint8_t *)p;
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < n; i++) h = (h ^ s[i]) * 0x01000193u;
    return h;
}

// Parses "<hex> <label>" lines ('#' starts a comment). Returns the number of ids stored.
static inline int nps5_ad_ids_parse(const char *text, Nps5AdId *out, int max) {
    int n = 0;
    const char *p = text;
    while (*p && n < max) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char line[128];
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len);
        line[len] = 0;
        p = e ? e + 1 : p + len;
        char *hash = line;
        while (*hash == ' ' || *hash == '\t') hash++;
        if (!*hash || *hash == '#') continue;
        char *end = NULL;
        unsigned long v = strtoul(hash, &end, 16);
        if (end == hash) continue;
        while (*end == ' ' || *end == '\t') end++;
        char *lab = end;
        size_t ll = strlen(lab);
        while (ll && (lab[ll - 1] == '\r' || lab[ll - 1] == ' ')) lab[--ll] = 0;
        out[n].hash = (uint32_t)v;
        snprintf(out[n].label, sizeof out[n].label, "%s", *lab ? lab : "id");
        n++;
    }
    return n;
}

static inline int nps5_ad_id_find(const Nps5AdId *ids, int n, uint32_t h) {
    for (int i = 0; i < n; i++)
        if (ids[i].hash == h) return i;
    return -1;
}

// First 4-byte aligned dword of buf[0..len) that is one of the ids: returns the id index (or -1), *off = byte offset.
static inline int nps5_ad_scan_ids(const uint8_t *buf, size_t len, const Nps5AdId *ids, int n, size_t *off) {
    for (size_t o = 0; o + 4 <= len; o += 4) {
        uint32_t w;
        memcpy(&w, buf + o, 4);
        if (!w) continue;
        const int k = nps5_ad_id_find(ids, n, w);
        if (k >= 0) {
            if (off) *off = o;
            return k;
        }
    }
    return -1;
}

// One traced object: window [lo, lo+len) of the object, its previous contents, per-dword change counts.
typedef struct {
    uint64_t addr; // guest address of the object (0 = free slot)
    uint32_t lo, len;
    uint8_t shadow[NPS5_AD_WIN_MAX];
    uint8_t nchg[NPS5_AD_WIN_MAX / 4];
    int id; // index into the id table, -1 = untagged
    uint32_t lines;
    uint8_t primed;
} Nps5AdObj;

static inline void nps5_ad_obj_init(Nps5AdObj *o, uint64_t addr, uint32_t lo, uint32_t len) {
    memset(o, 0, sizeof *o);
    o->addr = addr;
    o->lo = lo;
    o->len = len > NPS5_AD_WIN_MAX ? NPS5_AD_WIN_MAX : len & ~3u;
    o->id = -1;
}

// Compares cur (the window, o->len bytes) with the shadow and appends " +0xOFF:old->new" for each changed dword to out (at most
// cap bytes, always terminated). An offset that changed more than mute_after times is not listed again (a timer). The first call
// only primes the shadow and returns 0 (use nps5_ad_hexdump for the initial contents). Returns the number of dwords listed.
static inline int nps5_ad_diff(Nps5AdObj *o, const uint8_t *cur, char *out, size_t cap, unsigned mute_after) {
    int listed = 0;
    size_t used = 0;
    if (cap) out[0] = 0;
    if (!o->primed) {
        memcpy(o->shadow, cur, o->len);
        o->primed = 1;
        return 0;
    }
    for (uint32_t i = 0; i < o->len; i += 4) {
        if (!memcmp(o->shadow + i, cur + i, 4)) continue;
        uint32_t a, b;
        memcpy(&a, o->shadow + i, 4);
        memcpy(&b, cur + i, 4);
        memcpy(o->shadow + i, cur + i, 4);
        uint8_t *c = &o->nchg[i / 4];
        if (*c < 255) (*c)++;
        if (*c > mute_after) continue;
        if (cap - used > 40) {
            used += (size_t)snprintf(out + used, cap - used, " +0x%x:%x->%x%s", o->lo + i, a, b,
                                     *c == mute_after ? "(last)" : "");
            listed++;
        }
    }
    return listed;
}

// Counter rule of the title (counter, Type 0): a bot is "ready" when its state is `want` and it has been in it
// longer than `thr` seconds; the counter fires when ready >= count.
static inline int nps5_ad_ready_count(const uint32_t *state, const float *tin, const uint8_t *valid, int n, uint32_t want,
                                      float thr) {
    int r = 0;
    for (int i = 0; i < n; i++)
        if (valid[i] && state[i] == want && tin[i] > thr) r++;
    return r;
}

// DelayTrigger FSM state ids (the title's enum table order).
static inline const char *nps5_ad_dt_state_name(uint32_t s) {
    static const char *n[] = {"Wait", "EnterDelay", "Enter", "Triggering", "LeaveDelay", "Leave"};
    return s < 6 ? n[s] : "?";
}

// Log line budget: tagged objects get `tagged` lines each, untagged `untagged`, all objects together `total`.
typedef struct {
    uint32_t total, used, untagged, tagged;
} Nps5AdBudget;

static inline int nps5_ad_budget_take(Nps5AdBudget *b, Nps5AdObj *o) {
    const uint32_t cap = o->id >= 0 ? b->tagged : b->untagged;
    if (b->used >= b->total || o->lines >= cap) return 0;
    o->lines++;
    b->used++;
    return 1;
}

#endif
