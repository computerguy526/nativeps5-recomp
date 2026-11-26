// SPDX-License-Identifier: GPL-2.0-or-later
//
// File access of the UE5 shader library reader through the runtime's VFS: the containers are read as the guest would read
// them (/app0/<path>), so plain files and gamepack .nps5seg packs both work. The list walks the application directory on the
// host (a segmented <name>.utoc.nps5seg is listed as <name>.utoc).

#include "nps5_ue5core.h"
#include "ps4_vfs.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WALK_DEPTH 8

typedef struct Names {
    char **v;
    size_t n, cap;
} Names;

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void walk(const char *root, const char *rel, int depth, Names *out) {
    char dir[2048];
    snprintf(dir, sizeof dir, "%s%s%s", root, *rel ? "/" : "", rel);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *nm = de->d_name;
        if (nm[0] == '.') continue;
        char r[2048];
        snprintf(r, sizeof r, "%s%s%s", rel, *rel ? "/" : "", nm);
        const size_t l = strlen(nm);
        if (de->d_type == DT_DIR && l > 8 && strcmp(nm + l - 8, ".nps5seg") == 0) {
            if (l > 13 && strcmp(nm + l - 13, ".utoc.nps5seg") == 0)
                r[strlen(r) - 8] = 0;
            else
                continue;
        } else if (de->d_type == DT_DIR) {
            if (depth < WALK_DEPTH) walk(root, r, depth + 1, out);
            continue;
        } else if (!(l > 5 && strcmp(nm + l - 5, ".utoc") == 0)) {
            continue;
        }
        if (out->n == out->cap) {
            out->cap = out->cap ? out->cap * 2 : 16;
            out->v = realloc(out->v, out->cap * sizeof *out->v);
        }
        out->v[out->n++] = strdup(r);
    }
    closedir(d);
}

static int vfs_list(void *user, int (*cb)(void *arg, const char *path), void *arg) {
    (void)user;
    const char *root = ps4_vfs_get_app_root();
    if (!root || !*root) return 0;
    Names nm = {0};
    walk(root, "", 0, &nm);
    qsort(nm.v, nm.n, sizeof *nm.v, cmp_str);
    int rc = 0;
    for (size_t i = 0; i < nm.n; i++) {
        if (!rc) rc = cb(arg, nm.v[i]);
        free(nm.v[i]);
    }
    free(nm.v);
    return rc;
}

static void *vfs_open(void *user, const char *path) {
    (void)user;
    char g[2200];
    snprintf(g, sizeof g, "/app0/%s", path);
    const int fd = ps4_vfs_open(g, O_RDONLY, 0);
    return fd < 0 ? NULL : (void *)(intptr_t)(fd + 1);
}

static ssize_t vfs_pread(void *user, void *file, void *buf, size_t n, uint64_t off) {
    (void)user;
    return ps4_vfs_pread((int)((intptr_t)file - 1), buf, n, (off_t)off);
}

static void vfs_close(void *user, void *file) {
    (void)user;
    ps4_vfs_close((int)((intptr_t)file - 1));
}

const Nps5Ue5Io *nps5_ue5_vfs_io(void) {
    static const Nps5Ue5Io io = {NULL, vfs_list, vfs_open, vfs_pread, vfs_close};
    return &io;
}
