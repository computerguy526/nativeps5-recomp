#include "ps4_sfo.h"
#include "ps4_vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#pragma pack(push, 1)
typedef struct {
    uint8_t magic[4];
    uint32_t version;
    uint32_t key_table_offset;
    uint32_t data_table_offset;
    uint32_t entries_count;
} PsfHeader;

typedef struct {
    uint16_t key_offset;
    uint16_t param_fmt;
    uint32_t param_len;
    uint32_t param_max_len;
    uint32_t data_offset;
} PsfRawEntry;
#pragma pack(pop)

static uint16_t read_le16(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint16_t)(b[0] | (b[1] << 8));
}

static uint32_t read_le32(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint32_t)(b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24));
}

static bool ps4_sfo_find_and_load_sfo(Ps4SfoContext *sfo);

bool ps4_sfo_load_memory(Ps4SfoContext *sfo, const uint8_t *buf, size_t size) {
    if (!sfo || !buf || size < sizeof(PsfHeader)) {
        return false;
    }

    // Verify magic '\0PSF'
    if (buf[0] != 0x00 || buf[1] != 'P' || buf[2] != 'S' || buf[3] != 'F') {
        return false;
    }

    uint32_t key_table_offset = read_le32(buf + 8);
    uint32_t data_table_offset = read_le32(buf + 12);
    uint32_t entries_count = read_le32(buf + 16);

    if (key_table_offset > size || data_table_offset > size) {
        return false;
    }

    uint64_t entries_size = (uint64_t)entries_count * sizeof(PsfRawEntry);
    if (sizeof(PsfHeader) + entries_size > key_table_offset) {
        return false;
    }

    sfo->data = (uint8_t *)buf;
    sfo->size = size;
    sfo->key_table_offset = key_table_offset;
    sfo->data_table_offset = data_table_offset;
    sfo->entries_count = entries_count;
    sfo->is_allocated = false;
    return true;
}

bool ps4_sfo_load_file(Ps4SfoContext *sfo, const char *path) {
    if (!sfo || !path || *path == '\0') {
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }

    long file_size = ftell(f);
    if (file_size < (long)sizeof(PsfHeader) || file_size > 16 * 1024 * 1024) {
        fclose(f);
        return false;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }

    uint8_t *buffer = (uint8_t *)malloc((size_t)file_size);
    if (!buffer) {
        fclose(f);
        return false;
    }

    if (fread(buffer, 1, (size_t)file_size, f) != (size_t)file_size) {
        free(buffer);
        fclose(f);
        return false;
    }
    fclose(f);

    if (!ps4_sfo_load_memory(sfo, buffer, (size_t)file_size)) {
        free(buffer);
        return false;
    }

    sfo->is_allocated = true;
    return true;
}

void ps4_sfo_free(Ps4SfoContext *sfo) {
    if (!sfo) return;
    if (sfo->is_allocated && sfo->data) {
        free(sfo->data);
    }
    memset(sfo, 0, sizeof(*sfo));
}

static const PsfRawEntry *find_entry(const Ps4SfoContext *sfo, const char *key) {
    if (!sfo || !sfo->data || !key) return NULL;

    const uint8_t *entries_base = sfo->data + sizeof(PsfHeader);
    const char *key_table = (const char *)(sfo->data + sfo->key_table_offset);

    for (uint32_t i = 0; i < sfo->entries_count; i++) {
        const PsfRawEntry *e = (const PsfRawEntry *)(entries_base + i * sizeof(PsfRawEntry));
        uint16_t key_off = read_le16(&e->key_offset);
        if (sfo->key_table_offset + key_off >= sfo->size) {
            continue;
        }

        const char *cur_key = key_table + key_off;
        // Verify key string bounds
        size_t max_key_len = sfo->size - (sfo->key_table_offset + key_off);
        if (strnlen(cur_key, max_key_len) == max_key_len) {
            continue; // Not null-terminated
        }

        if (strcmp(cur_key, key) == 0) {
            return e;
        }
    }
    return NULL;
}

bool ps4_sfo_get_int(const Ps4SfoContext *sfo, const char *key, int32_t *out_val) {
    if (!out_val) return false;
    const PsfRawEntry *e = find_entry(sfo, key);
    if (!e) return false;

    uint32_t data_off = read_le32(&e->data_offset);
    uint32_t param_len = read_le32(&e->param_len);

    if (param_len != 4) return false;
    if (sfo->data_table_offset + data_off + 4 > sfo->size) return false;

    *out_val = (int32_t)read_le32(sfo->data + sfo->data_table_offset + data_off);
    return true;
}

bool ps4_sfo_get_string(const Ps4SfoContext *sfo, const char *key, char *out_buf, size_t out_buf_sz) {
    if (!out_buf || out_buf_sz == 0) return false;
    const PsfRawEntry *e = find_entry(sfo, key);
    if (!e) return false;

    uint32_t data_off = read_le32(&e->data_offset);
    uint32_t param_len = read_le32(&e->param_len);

    if (sfo->data_table_offset + data_off + param_len > sfo->size) return false;

    const char *src = (const char *)(sfo->data + sfo->data_table_offset + data_off);
    size_t copy_len = param_len < out_buf_sz ? param_len : out_buf_sz - 1;
    memcpy(out_buf, src, copy_len);
    out_buf[copy_len] = '\0';
    return true;
}

static bool ps4_sfo_find_and_load_sfo(Ps4SfoContext *sfo) {
    char test_path[1024];

    // 1. Through VFS
    const char *vfs_candidates[] = {
        "/app0/sce_sys/param.sfo",
        "/app0/sce_sys/sce_sys/param.sfo",
        "/app0/param.sfo",
    };
    for (size_t i = 0; i < sizeof(vfs_candidates) / sizeof(vfs_candidates[0]); i++) {
        if (ps4_vfs_resolve(vfs_candidates[i], test_path, sizeof(test_path)) == 0) {
            if (access(test_path, R_OK) == 0) {
                if (ps4_sfo_load_file(sfo, test_path)) {
                    return true;
                }
            }
        }
    }

    // 2. Relative to app_root
    const char *app_root = ps4_vfs_get_app_root();
    if (app_root && *app_root) {
        const char *rel_candidates[] = {
            "sce_sys/param.sfo",
            "sce_sys/sce_sys/param.sfo",
            "param.sfo",
        };
        for (size_t i = 0; i < sizeof(rel_candidates) / sizeof(rel_candidates[0]); i++) {
            snprintf(test_path, sizeof(test_path), "%s/%s", app_root, rel_candidates[i]);
            if (access(test_path, R_OK) == 0) {
                if (ps4_sfo_load_file(sfo, test_path)) {
                    return true;
                }
            }
        }
    }

    // 3. Fallback to current working directory
    const char *local_candidates[] = {
        "sce_sys/param.sfo",
        "sce_sys/sce_sys/param.sfo",
        "param.sfo",
        "assets/sce_sys/param.sfo",
    };
    for (size_t i = 0; i < sizeof(local_candidates) / sizeof(local_candidates[0]); i++) {
        if (access(local_candidates[i], R_OK) == 0) {
            if (ps4_sfo_load_file(sfo, local_candidates[i])) {
                return true;
            }
        }
    }

    return false;
}

// ---------------------------------------------------------------------------------------------
// PS5 sce_sys/param.json -> an in-memory PSF with the keys the HLE reads (TITLE_ID, CONTENT_ID,
// TITLE, APP_VER, VERSION, CATEGORY, APPLICATION_CATEGORY_TYPE, USER_DEFINED_PARAM_1..4). PS5
// packages carry no param.sfo; building the PSF here gives sceAppContent*, savedata, the game key
// and the GPU cache the same view of the title as on PS4. A real param.sfo always wins.

// Value of "key": "..." (string) at or after `from`; returns pointer past the closing quote or NULL.
static const char *pj_string(const char *from, const char *key, char *out, size_t out_sz) {
    char pat[96];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(from, pat);
    while (p) {
        const char *q = p + strlen(pat);
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q == ':') {
            q++;
            while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
            if (*q != '"') return NULL;
            q++;
            size_t k = 0;
            while (*q && *q != '"') {
                if (*q == '\\' && q[1]) q++;
                if (k + 1 < out_sz) out[k++] = *q;
                q++;
            }
            out[k] = '\0';
            return *q == '"' ? q + 1 : NULL;
        }
        p = strstr(p + 1, pat);
    }
    return NULL;
}

// Integer value of "key": N (or "key": "N"); true if present.
static bool pj_int(const char *from, const char *key, int64_t *out) {
    char pat[96];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(from, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == '"') p++;
    char *end = NULL;
    long long v = strtoll(p, &end, 0);
    if (end == p) return false;
    *out = v;
    return true;
}

typedef struct {
    char key[40];
    bool is_int;
    char str[256];
    int32_t num;
} PjEntry;

static void pj_add_str(PjEntry *e, int *n, const char *key, const char *val) {
    if (!val || !*val) return;
    snprintf(e[*n].key, sizeof(e[*n].key), "%s", key);
    e[*n].is_int = false;
    snprintf(e[*n].str, sizeof(e[*n].str), "%s", val);
    (*n)++;
}

static void pj_add_int(PjEntry *e, int *n, const char *key, int32_t v) {
    snprintf(e[*n].key, sizeof(e[*n].key), "%s", key);
    e[*n].is_int = true;
    e[*n].num = v;
    (*n)++;
}

bool ps4_sfo_from_param_json(Ps4SfoContext *sfo, const char *json, size_t json_len) {
    if (!sfo || !json) return false;
    char *text = (char *)malloc(json_len + 1);
    if (!text) return false;
    memcpy(text, json, json_len);
    text[json_len] = '\0';

    PjEntry ent[16];
    int n = 0;
    char v[256], dl[64];
    bool ok = pj_string(text, "titleId", v, sizeof(v)) != NULL && v[0];
    if (!ok) {
        free(text);
        return false;
    }
    pj_add_str(ent, &n, "TITLE_ID", v);
    if (pj_string(text, "contentId", v, sizeof(v))) pj_add_str(ent, &n, "CONTENT_ID", v);
    // title: localizedParameters[defaultLanguage].titleName, else the first titleName
    const char *lp = strstr(text, "\"localizedParameters\"");
    if (lp) {
        const char *scope = lp;
        if (pj_string(lp, "defaultLanguage", dl, sizeof(dl))) {
            char lk[80];
            snprintf(lk, sizeof(lk), "\"%s\"", dl);
            const char *lang = strstr(lp, lk);
            // the language key as an object key (followed by ':'), not the defaultLanguage value
            while (lang) {
                const char *q = lang + strlen(lk);
                while (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t') q++;
                if (*q == ':') { scope = lang; break; }
                lang = strstr(lang + 1, lk);
            }
        }
        if (pj_string(scope, "titleName", v, sizeof(v))) pj_add_str(ent, &n, "TITLE", v);
    }
    if (pj_string(text, "contentVersion", v, sizeof(v))) pj_add_str(ent, &n, "APP_VER", v);
    if (pj_string(text, "masterVersion", v, sizeof(v))) pj_add_str(ent, &n, "VERSION", v);
    int64_t cat = 0;
    if (pj_int(text, "applicationCategoryType", &cat)) {
        pj_add_int(ent, &n, "APPLICATION_CATEGORY_TYPE", (int32_t)cat);
        pj_add_str(ent, &n, "CATEGORY", "gd"); // 0 = full application; the PS5 types all load as an app
    }
    for (int i = 1; i <= 4; i++) {
        char k[48];
        int64_t iv;
        snprintf(k, sizeof(k), "userDefinedParam%d", i);
        if (pj_int(text, k, &iv)) {
            snprintf(k, sizeof(k), "USER_DEFINED_PARAM_%d", i);
            pj_add_int(ent, &n, k, (int32_t)iv);
        }
    }
    free(text);

    size_t keys_sz = 0, data_sz = 0;
    for (int i = 0; i < n; i++) {
        keys_sz += strlen(ent[i].key) + 1;
        data_sz += ent[i].is_int ? 4 : (strlen(ent[i].str) + 1 + 3) / 4 * 4;
    }
    size_t hdr = sizeof(PsfHeader) + (size_t)n * sizeof(PsfRawEntry);
    size_t key_off = hdr, data_off = (hdr + keys_sz + 3) / 4 * 4;
    size_t total = data_off + data_sz;
    uint8_t *buf = (uint8_t *)calloc(1, total);
    if (!buf) return false;
    PsfHeader *h = (PsfHeader *)buf;
    h->magic[1] = 'P'; h->magic[2] = 'S'; h->magic[3] = 'F';
    h->version = 0x101;
    h->key_table_offset = (uint32_t)key_off;
    h->data_table_offset = (uint32_t)data_off;
    h->entries_count = (uint32_t)n;
    size_t ko = 0, dof = 0;
    for (int i = 0; i < n; i++) {
        PsfRawEntry *e = (PsfRawEntry *)(buf + sizeof(PsfHeader)) + i;
        size_t kl = strlen(ent[i].key) + 1;
        memcpy(buf + key_off + ko, ent[i].key, kl);
        e->key_offset = (uint16_t)ko;
        ko += kl;
        e->data_offset = (uint32_t)dof;
        if (ent[i].is_int) {
            e->param_fmt = PSF_FMT_INT;
            e->param_len = e->param_max_len = 4;
            memcpy(buf + data_off + dof, &ent[i].num, 4);
            dof += 4;
        } else {
            size_t l = strlen(ent[i].str) + 1;
            e->param_fmt = PSF_FMT_STRING;
            e->param_len = (uint32_t)l;
            e->param_max_len = (uint32_t)((l + 3) / 4 * 4);
            memcpy(buf + data_off + dof, ent[i].str, l);
            dof += (l + 3) / 4 * 4;
        }
    }
    if (!ps4_sfo_load_memory(sfo, buf, total)) {
        free(buf);
        return false;
    }
    sfo->is_allocated = true;
    return true;
}

static bool sfo_load_param_json_file(Ps4SfoContext *sfo, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char *buf = (char *)malloc(1 << 20);
    if (!buf) { fclose(f); return false; }
    size_t n = fread(buf, 1, (1 << 20) - 1, f);
    fclose(f);
    bool ok = ps4_sfo_from_param_json(sfo, buf, n);
    free(buf);
    return ok;
}

bool ps4_sfo_find_and_load(Ps4SfoContext *sfo) {
    if (ps4_sfo_find_and_load_sfo(sfo)) return true;
    char p[1100];
    if (ps4_vfs_resolve("/app0/sce_sys/param.json", p, sizeof(p)) == 0 && access(p, R_OK) == 0 &&
        sfo_load_param_json_file(sfo, p))
        return true;
    const char *root = ps4_vfs_get_app_root();
    if (root && *root) {
        snprintf(p, sizeof(p), "%s/sce_sys/param.json", root);
        if (access(p, R_OK) == 0 && sfo_load_param_json_file(sfo, p)) return true;
    }
    return access("sce_sys/param.json", R_OK) == 0 && sfo_load_param_json_file(sfo, "sce_sys/param.json");
}
