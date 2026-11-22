// SPDX-License-Identifier: GPL-2.0-or-later
//
// Segmented files: see nps5_segfile.h. All state is under one mutex; reads hold it only to pick and pin a segment fd,
// never across the pread itself, so concurrent readers of different (or the same) segments do not serialize.
#include "nps5_segfile.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define SEG_SUFFIX ".nps5seg"
#define SEG_SUFFIX_LEN 8
#define SEG_MANIFEST_MAX (4u << 20)
#define SEG_MAX_COUNT 100000u

struct nps5_segfile {
  char *segdir;   // <dir>/<name>.nps5seg (host spelling)
  uint64_t size;  // logical size
  uint64_t seg_size;
  uint32_t nseg;
  int *fd;        // per segment, -1 = closed
  uint32_t *pin;  // readers using fd[i] right now
  uint64_t *used; // LRU tick of the last acquire
  dev_t dev;      // identity of the segment directory: one handle whatever case the guest spells the path in
  ino_t ino;
  nps5_segfile *next;
};

typedef struct SegDir { // one scanned host directory: its *.nps5seg entries (none = negative cache)
  char *dir;
  char **names;
  int n;
  struct SegDir *next;
} SegDir;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static nps5_segfile *g_files;
static SegDir *g_dirs;
static int g_open_fds;
static uint64_t g_tick;

int nps5_segfile_enabled(void) {
  static int on = -1;
  if (on < 0) {
    const char *e = getenv("NPS5_VFS_SEGFILES");
    on = (e && *e && *e != '0') ? 1 : 0;
  }
  return on;
}

static int fd_cap(void) {
  static int cap = 0;
  if (!cap) {
    const char *e = getenv("NPS5_VFS_SEGFILES_FDS");
    int v = e ? atoi(e) : 0;
    cap = v >= 2 ? v : 64;
  }
  return cap;
}

static int has_suffix_ci(const char *s, size_t n) { return n > SEG_SUFFIX_LEN && !strcasecmp(s + n - SEG_SUFFIX_LEN, SEG_SUFFIX); }

static SegDir *dir_scan_locked(const char *dir) {
  for (SegDir *d = g_dirs; d; d = d->next)
    if (!strcmp(d->dir, dir)) return d;
  SegDir *d = (SegDir *)calloc(1, sizeof *d);
  if (!d) return NULL;
  d->dir = strdup(dir);
  DIR *h = opendir(dir);
  if (h) {
    int cap = 0;
    struct dirent *e;
    while ((e = readdir(h)) != NULL) {
      const size_t n = strlen(e->d_name);
      if (!has_suffix_ci(e->d_name, n)) continue;
      if (d->n == cap) {
        cap = cap ? cap * 2 : 8;
        char **nn = (char **)realloc(d->names, (size_t)cap * sizeof(char *));
        if (!nn) break;
        d->names = nn;
      }
      d->names[d->n++] = strdup(e->d_name);
    }
    closedir(h);
  }
  d->next = g_dirs;
  g_dirs = d;
  return d;
}

// "key": <unsigned integer> anywhere in the text (the manifest's top-level keys are unique names).
static int json_u64(const char *txt, const char *key, uint64_t *out) {
  char pat[64];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char *p = strstr(txt, pat);
  if (!p) return 0;
  p += strlen(pat);
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  if (*p++ != ':') return 0;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  if (!isdigit((unsigned char)*p)) return 0;
  char *end;
  errno = 0;
  unsigned long long v = strtoull(p, &end, 10);
  if (errno || end == p) return 0;
  *out = (uint64_t)v;
  return 1;
}

static nps5_segfile *load_locked(const char *segdir, const char *shown) {
  char path[PATH_MAX];
  snprintf(path, sizeof path, "%s/manifest.json", segdir);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return NULL;
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size <= 0 || (uint64_t)st.st_size > SEG_MANIFEST_MAX) {
    close(fd);
    fprintf(stderr, "[vfs] segfile %s: manifest.json unreadable or too large: not used\n", shown);
    return NULL;
  }
  char *txt = (char *)malloc((size_t)st.st_size + 1);
  ssize_t got = txt ? pread(fd, txt, (size_t)st.st_size, 0) : -1;
  close(fd);
  if (got != (ssize_t)st.st_size) {
    free(txt);
    return NULL;
  }
  txt[got] = 0;
  uint64_t size = 0, seg = 0, cnt = 0;
  const int ok = strstr(txt, "\"nps5seg/1\"") && json_u64(txt, "total_size", &size) && json_u64(txt, "seg_size", &seg) &&
                 json_u64(txt, "seg_count", &cnt);
  free(txt);
  if (!ok || seg == 0 || cnt > SEG_MAX_COUNT || cnt != (size + seg - 1) / seg) {
    fprintf(stderr, "[vfs] segfile %s: manifest.json is not a valid nps5seg/1 manifest: not used\n", shown);
    return NULL;
  }
  for (uint64_t i = 0; i < cnt; i++) { // every segment must be there with the size the manifest implies
    const uint64_t want = (i + 1 < cnt) ? seg : size - seg * (cnt - 1);
    snprintf(path, sizeof path, "%s/%04llu", segdir, (unsigned long long)i);
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || (uint64_t)st.st_size != want) {
      fprintf(stderr, "[vfs] segfile %s: segment %04llu missing or not %llu bytes: not used\n", shown,
              (unsigned long long)i, (unsigned long long)want);
      return NULL;
    }
  }
  nps5_segfile *f = (nps5_segfile *)calloc(1, sizeof *f);
  if (!f) return NULL;
  f->segdir = strdup(segdir);
  f->size = size;
  f->seg_size = seg;
  f->nseg = (uint32_t)cnt;
  const size_t k = cnt ? (size_t)cnt : 1;
  f->fd = (int *)malloc(k * sizeof(int));
  f->pin = (uint32_t *)calloc(k, sizeof(uint32_t));
  f->used = (uint64_t *)calloc(k, sizeof(uint64_t));
  if (!f->segdir || !f->fd || !f->pin || !f->used) {
    free(f->segdir); free(f->fd); free(f->pin); free(f->used); free(f);
    return NULL;
  }
  for (size_t i = 0; i < k; i++) f->fd[i] = -1;
  fprintf(stderr, "[vfs] segfile %s: %u segments\n", shown, f->nseg);
  return f;
}

nps5_segfile *nps5_segfile_lookup(const char *host) {
  if (!host || !nps5_segfile_enabled()) return NULL;
  const char *slash = strrchr(host, '/');
  const char *base = slash ? slash + 1 : host;
  const size_t bl = strlen(base);
  if (!bl || has_suffix_ci(base, bl)) return NULL;
  char dir[PATH_MAX];
  if (!slash) snprintf(dir, sizeof dir, ".");
  else if (slash == host) snprintf(dir, sizeof dir, "/");
  else if ((size_t)(slash - host) < sizeof dir) snprintf(dir, sizeof dir, "%.*s", (int)(slash - host), host);
  else return NULL;
  nps5_segfile *res = NULL;
  pthread_mutex_lock(&g_mu);
  SegDir *d = dir_scan_locked(dir);
  const char *match = NULL;
  for (int i = 0; d && i < d->n; i++) {
    const char *nm = d->names[i];
    if (strlen(nm) == bl + SEG_SUFFIX_LEN && !strncasecmp(nm, base, bl)) {
      match = nm;
      if (!strncmp(nm, base, bl)) break; // exact spelling wins over a case-insensitive one
    }
  }
  if (match) {
    char segdir[PATH_MAX];
    snprintf(segdir, sizeof segdir, "%s%s%s", slash == host ? "" : dir, "/", match);
    if (!slash) snprintf(segdir, sizeof segdir, "%s", match);
    struct stat ds;
    if (stat(segdir, &ds) == 0 && S_ISDIR(ds.st_mode)) {
      for (nps5_segfile *f = g_files; f; f = f->next)
        if (f->dev == ds.st_dev && f->ino == ds.st_ino) { res = f; break; }
      if (!res && (res = load_locked(segdir, host)) != NULL) {
        res->dev = ds.st_dev;
        res->ino = ds.st_ino;
        res->next = g_files;
        g_files = res;
      }
    }
  }
  pthread_mutex_unlock(&g_mu);
  return res;
}

uint64_t nps5_segfile_size(const nps5_segfile *f) { return f ? f->size : 0; }
uint32_t nps5_segfile_count(const nps5_segfile *f) { return f ? f->nseg : 0; }

// Closes the least recently used unpinned segment fd of any segmented file. Lock held.
static void evict_one_locked(void) {
  nps5_segfile *bf = NULL;
  uint32_t bi = 0;
  uint64_t best = UINT64_MAX;
  for (nps5_segfile *f = g_files; f; f = f->next)
    for (uint32_t i = 0; i < f->nseg; i++)
      if (f->fd[i] >= 0 && f->pin[i] == 0 && f->used[i] < best) { best = f->used[i]; bf = f; bi = i; }
  if (bf) {
    close(bf->fd[bi]);
    bf->fd[bi] = -1;
    g_open_fds--;
  }
}

// The pinned fd of segment <i> (opened if needed), or -1 with errno.
static int acquire(nps5_segfile *f, uint32_t i) {
  pthread_mutex_lock(&g_mu);
  if (f->fd[i] < 0) {
    while (g_open_fds >= fd_cap()) {
      const int before = g_open_fds;
      evict_one_locked();
      if (g_open_fds == before) break; // everything pinned: go over the cap rather than wait
    }
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%04u", f->segdir, i);
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      const int e = errno;
      pthread_mutex_unlock(&g_mu);
      errno = e;
      return -1;
    }
    f->fd[i] = fd;
    g_open_fds++;
  }
  f->pin[i]++;
  f->used[i] = ++g_tick;
  const int fd = f->fd[i];
  pthread_mutex_unlock(&g_mu);
  return fd;
}

static void release(nps5_segfile *f, uint32_t i) {
  pthread_mutex_lock(&g_mu);
  f->pin[i]--;
  pthread_mutex_unlock(&g_mu);
}

static ssize_t piece(nps5_segfile *f, void *buf, size_t n, uint64_t off, nps5_segfile_reader rd) {
  if (!f) { errno = EBADF; return -1; }
  if (off >= f->size || n == 0) return 0;
  const uint32_t i = (uint32_t)(off / f->seg_size);
  const uint64_t in = off - (uint64_t)i * f->seg_size;
  const uint64_t seg_len = (i + 1 < f->nseg) ? f->seg_size : f->size - f->seg_size * (uint64_t)(f->nseg - 1);
  size_t want = n;
  if ((uint64_t)want > seg_len - in) want = (size_t)(seg_len - in);
  const int fd = acquire(f, i);
  if (fd < 0) return -1;
  const ssize_t r = rd ? rd(fd, buf, want, (off_t)in) : pread(fd, buf, want, (off_t)in);
  const int e = errno;
  release(f, i);
  if (r == 0) { // the segment is shorter than the manifest says (changed on disk since it was checked)
    fprintf(stderr, "[vfs] segfile %s: segment %04u ends early at 0x%llx\n", f->segdir, i, (unsigned long long)in);
    errno = EIO;
    return -1;
  }
  errno = e;
  return r;
}

ssize_t nps5_segfile_pread_piece(nps5_segfile *f, void *buf, size_t n, uint64_t off) { return piece(f, buf, n, off, NULL); }

ssize_t nps5_segfile_pread(nps5_segfile *f, void *buf, size_t n, uint64_t off, nps5_segfile_reader rd) {
  if (!f) { errno = EBADF; return -1; }
  if (off >= f->size) return 0;
  if ((uint64_t)n > f->size - off) n = (size_t)(f->size - off);
  size_t total = 0;
  while (total < n) {
    const ssize_t r = piece(f, (char *)buf + total, n - total, off + total, rd);
    if (r < 0) {
      if (errno == EINTR) continue;
      if (total) break;
      return -1;
    }
    if (r == 0) break;
    total += (size_t)r;
  }
  nps5_segfile_trace(f, off, buf, total);
  return (ssize_t)total;
}

void nps5_segfile_trace(const nps5_segfile *f, uint64_t off, const void *buf, size_t n) {
  static int limit = -1;
  static _Atomic int done;
  if (limit < 0) {
    const char *e = getenv("NPS5_VFS_SEGFILES_TRACE");
    limit = e ? atoi(e) : 0;
  }
  if (!f || !n || limit <= 0 || atomic_fetch_add(&done, 1) >= limit) return;
  uint64_t h = 1469598103934665603ull;
  const unsigned char *p = (const unsigned char *)buf;
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
  fprintf(stderr, "[vfs] segread %s off=%llu len=%zu fnv=%016llx\n", f->segdir, (unsigned long long)off, n,
          (unsigned long long)h);
}

int nps5_segfile_dirent(int dirfd, const char *name, unsigned char dtype, char *out, size_t out_sz) {
  if (!name || !nps5_segfile_enabled()) return 0;
  const size_t n = strlen(name);
  if (!has_suffix_ci(name, n) || dtype != 4 /* DT_DIR */) return 0;
  const size_t bl = n - SEG_SUFFIX_LEN;
  if (bl + 1 > out_sz) return 0;
  char m[NAME_MAX + 16];
  snprintf(m, sizeof m, "%s/manifest.json", name);
  if (faccessat(dirfd, m, F_OK, 0) != 0) return 0;
  memcpy(out, name, bl);
  out[bl] = 0;
  struct stat st;
  if (fstatat(dirfd, out, &st, 0) == 0) return -1; // the original is still there: list only it
  return 1;
}

void nps5_segfile_reset(void) {
  pthread_mutex_lock(&g_mu);
  for (nps5_segfile *f = g_files, *nx; f; f = nx) {
    nx = f->next;
    for (uint32_t i = 0; i < f->nseg; i++) if (f->fd[i] >= 0) close(f->fd[i]);
    free(f->segdir); free(f->fd); free(f->pin); free(f->used); free(f);
  }
  for (SegDir *d = g_dirs, *nx; d; d = nx) {
    nx = d->next;
    for (int i = 0; i < d->n; i++) free(d->names[i]);
    free(d->names); free(d->dir); free(d);
  }
  g_files = NULL;
  g_dirs = NULL;
  g_open_fds = 0;
  pthread_mutex_unlock(&g_mu);
}
