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
#include <sys/stat.h>

#define WALK_DEPTH 8

typedef struct Names {
    char **v;
    size_t n, cap;
    char **real; // realpaths of the containers listed (one container is listed once, however many links lead to it)
    size_t nreal, capreal;
    struct stat *
        dirs; // directories walked: (st_dev, st_ino), so a link back to an ancestor or a second link to one is not walked again
    size_t ndirs, capdirs;
    const char *root; // the application root the relative paths are under
    int oom;
} Names;

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int names_push(char ***v, size_t *n, size_t *cap, char *s) {
    if (*n == *cap) {
        const size_t nc = *cap ? *cap * 2 : 16;
        char **nv = realloc(*v, nc * sizeof **v);
        if (!nv) return -1;
        *v = nv;
        *cap = nc;
    }
    (*v)[(*n)++] = s;
    return 0;
}

// 1 when the directory was already walked (or cannot be identified), else records it.
static int dir_seen(Names *out, const char *dir) {
    struct stat st;
    if (stat(dir, &st) != 0) return 1;
    for (size_t i = 0; i < out->ndirs; i++)
        if (out->dirs[i].st_dev == st.st_dev && out->dirs[i].st_ino == st.st_ino) return 1;
    if (out->ndirs == out->capdirs) {
        const size_t nc = out->capdirs ? out->capdirs * 2 : 16;
        struct stat *nd = realloc(out->dirs, nc * sizeof *nd);
        if (!nd) {
            out->oom = 1;
            return 1;
        }
        out->dirs = nd;
        out->capdirs = nc;
    }
    out->dirs[out->ndirs++] = st;
    return 0;
}

// 1 when the container at rel path r has its data beside it (<base>.ucas, plain or packed): a link to a table of contents does
// not bring the partitions along, so such a path cannot be read even though its realpath is a good container.
static int has_data(const Names *out, const char *r) {
    const size_t l = strlen(r);
    if (l < 5 || strcmp(r + l - 5, ".utoc") != 0) return 0;
    char p[4400];
    struct stat st;
    snprintf(p, sizeof p, "%s/%.*s.ucas", out->root, (int)(l - 5), r);
    if (stat(p, &st) == 0) return 1;
    snprintf(p, sizeof p, "%s/%.*s.ucas.nps5seg", out->root, (int)(l - 5), r);
    return stat(p, &st) == 0;
}

// Adds a container (rel path r; fs path of the toc or its .nps5seg folder) unless the same file was listed already. Of two paths
// to one file the first is kept, unless it has no partitions beside it and the new one has (then the new path replaces it).
static void add_container(Names *out, const char *r, const char *fs) {
    char rp[4200];
    if (!realpath(fs, rp)) snprintf(rp, sizeof rp, "%s", fs);
    for (size_t i = 0; i < out->nreal; i++) {
        if (strcmp(out->real[i], rp) != 0) continue;
        if (!has_data(out, out->v[i]) && has_data(out, r)) {
            char *b = strdup(r);
            if (!b) {
                out->oom = 1;
                return;
            }
            free(out->v[i]);
            out->v[i] = b;
        }
        return;
    }
    char *a = strdup(rp), *b = strdup(r);
    if (!a || !b || names_push(&out->real, &out->nreal, &out->capreal, a) != 0) {
        free(a);
        free(b);
        out->oom = 1;
        return;
    }
    if (names_push(&out->v, &out->n, &out->cap, b) != 0) {
        free(b);
        out->nreal--;
        free(a);
        out->oom = 1;
    }
}

static void walk(const char *root, const char *rel, int depth, Names *out) {
    char dir[2048];
    snprintf(dir, sizeof dir, "%s%s%s", root, *rel ? "/" : "", rel);
    if (dir_seen(out, dir)) return;
    DIR *d = opendir(dir);
    if (!d) return;
    // entries in name order, so that of two paths to one thing the same one is listed on every run
    char **ents = NULL;
    size_t nent = 0, capent = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char *c = strdup(de->d_name);
        if (!c || names_push(&ents, &nent, &capent, c) != 0) {
            free(c);
            out->oom = 1;
            break;
        }
    }
    closedir(d);
    qsort(ents, nent, sizeof *ents, cmp_str);
    for (size_t ei = 0; ei < nent; ei++) {
        const char *nm = ents[ei];
        char r[2048], full[4200];
        snprintf(r, sizeof r, "%s%s%s", rel, *rel ? "/" : "", nm);
        snprintf(full, sizeof full, "%s/%s", dir, nm);
        const size_t l = strlen(nm);
        struct stat st;
        // stat follows links: linked folders and files count (the translated builds link app0 content)
        const int type = stat(full, &st) != 0 ? 0 : S_ISDIR(st.st_mode) ? 1 : S_ISREG(st.st_mode) ? 2 : 0;
        if (type == 1 && l > 8 && strcmp(nm + l - 8, ".nps5seg") == 0) {
            if (l > 13 && strcmp(nm + l - 13, ".utoc.nps5seg") == 0) {
                r[strlen(r) - 8] = 0;
                add_container(out, r, full);
            }
        } else if (type == 1) {
            if (depth < WALK_DEPTH) walk(root, r, depth + 1, out);
        } else if (type == 2 && l > 5 && strcmp(nm + l - 5, ".utoc") == 0) {
            add_container(out, r, full);
        }
    }
    for (size_t i = 0; i < nent; i++)
        free(ents[i]);
    free(ents);
}

static int vfs_list(void *user, int (*cb)(void *arg, const char *path), void *arg) {
    (void)user;
    const char *root = ps4_vfs_get_app_root();
    if (!root || !*root) return 0;
    Names nm = {0};
    nm.root = root;
    walk(root, "", 0, &nm);
    qsort(nm.v, nm.n, sizeof *nm.v, cmp_str);
    for (size_t i = 0; i < nm.nreal; i++)
        free(nm.real[i]);
    free(nm.real);
    free(nm.dirs);
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
