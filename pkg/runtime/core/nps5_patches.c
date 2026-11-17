// SPDX-License-Identifier: GPL-2.0-or-later
#include "nps5_patches.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const Nps5PatchDesc *g_descs;
static int g_n;
static uint8_t *g_flags;
static const char *g_profile = "";
static uint8_t g_val[NPS5_PATCH_MAX]; // pending values
static int g_resolved;

void nps5_patches_register(int abi, const char *profile, const Nps5PatchDesc *descs, int n, uint8_t *flags, int nflags) {
    if (abi != NPS5_PATCH_ABI || !descs || !flags || n <= 0 || n > NPS5_PATCH_MAX || g_resolved) return;
    for (int i = 0; i < n; i++)
        if ((int)descs[i].slot >= nflags || !descs[i].key) return;
    g_descs = descs;
    g_n = n;
    g_flags = flags;
    g_profile = profile ? profile : "";
    for (int i = 0; i < n; i++) g_val[i] = descs[i].def != 0;
}

int nps5_patches_count(void) { return g_n; }
const char *nps5_patches_profile(void) { return g_profile; }
const Nps5PatchDesc *nps5_patches_desc(int i) { return i >= 0 && i < g_n ? &g_descs[i] : NULL; }
int nps5_patches_get(int i) { return i >= 0 && i < g_n ? g_val[i] : 0; }

void nps5_patches_set(int i, int on) {
    if (i >= 0 && i < g_n && !g_resolved) g_val[i] = on != 0;
}

int nps5_patches_cfg_line(const char *key, int value) {
    if (!key || strncmp(key, "patch.", 6) != 0) return 0;
    for (int i = 0; i < g_n; i++) {
        if (!strcmp(key + 6, g_descs[i].key)) {
            nps5_patches_set(i, value);
            return 1;
        }
    }
    return 0;
}

void nps5_patches_cfg_write(FILE *f) {
    for (int i = 0; i < g_n; i++) fprintf(f, "patch.%s=%d\n", g_descs[i].key, g_val[i]);
}

void nps5_patches_resolve(void) {
    if (g_resolved) return;
    for (int i = 0; i < g_n; i++) {
        const Nps5PatchDesc *d = &g_descs[i];
        const char *src = g_val[i] == (d->def != 0) ? "default" : "settings";
        char generic[80] = "NPS5_PATCH_";
        size_t k = strlen(generic);
        for (const char *p = d->key; *p && k + 1 < sizeof(generic); p++) generic[k++] = (char)toupper((unsigned char)*p);
        generic[k] = '\0';
        const char *names[2] = {generic, d->env};
        for (int e = 0; e < 2; e++) { // the profile's own variable wins over the generic one
            const char *v = names[e] && *names[e] ? getenv(names[e]) : NULL;
            if (v && *v) {
                g_val[i] = strcmp(v, "0") != 0;
                src = names[e];
            }
        }
        g_flags[d->slot] = g_val[i];
        fprintf(stderr, "[ps4-settings] patch %s: %s = %s (%s; %u site%s, first guest 0x%llx)\n", d->key, d->title,
                g_val[i] ? d->on : d->off, src, d->nsites, d->nsites == 1 ? "" : "s",
                d->nsites ? (unsigned long long)d->sites[0] : 0ull);
    }
    g_resolved = 1;
}
