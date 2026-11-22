#include "nps5_scene_prepare.h"
#include "ps4_vfs.h"
#include "nps5_mem.h"
#include "ps4_pak.h"
#include "nps5_diag.h"
#include "ps4_sfo.h"
#include <ctype.h>
#include <limits.h>
#include <dirent.h>
#include <execinfo.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/xattr.h>
#include <sys/param.h>
#include <unistd.h>

#define PATH_MAX_VFS 1100

#if defined(__APPLE__)
#include <libgen.h>
#include <mach-o/dyld.h>
#endif

static char *g_app_root = NULL;
static char *g_app_root_real = NULL; // realpath of g_app_root: the host side of the write guard
static pthread_mutex_t g_vfs_mutex = PTHREAD_MUTEX_INITIALIZER;

#define MAX_VIRTUAL_FILES 256
typedef struct {
  int active;
  int pak_fd;
  uint64_t data_offset;
  uint64_t file_size;
  off_t cur_offset;
  char path[512];
} VirtualPakFile;

static VirtualPakFile g_vfiles[MAX_VIRTUAL_FILES];
static pthread_mutex_t g_vfile_mutex = PTHREAD_MUTEX_INITIALIZER;

static char *discover_app_root(const char *hint) {
  // 1. Environment variable override
  const char *env = getenv("NPS5_APP0_DIR"); // data folder on a separate read-only volume (wins over PS4_APP_DIR)
  if (!env || !*env) env = getenv("PS4_APP_DIR");
  if (env && *env && access(env, F_OK) == 0) {
    char resolved[1024];
    if (realpath(env, resolved) != NULL) {
      return strdup(resolved);
    }
    return strdup(env);
  }

#if defined(__APPLE__)
  // 2. Check if running inside macOS .app bundle: <exe_dir>/../SharedSupport or Resources
  char exec_path[1024] = {0};
  uint32_t size = sizeof(exec_path);
  char exe_dir_buf[1024] = {0};
  char *exe_dir = NULL;
  if (_NSGetExecutablePath(exec_path, &size) == 0) {
    char canonical_exec[1024];
    if (realpath(exec_path, canonical_exec) != NULL) {
      strncpy(exe_dir_buf, canonical_exec, sizeof(exe_dir_buf) - 1);
      exe_dir = dirname(exe_dir_buf);

      // Check inside macOS .app bundle: <exe_dir>/../SharedSupport
      char test_path[1024];
      snprintf(test_path, sizeof(test_path), "%s/../SharedSupport", exe_dir);
      char canonical_res[1024];
      if (realpath(test_path, canonical_res) != NULL &&
          access(canonical_res, F_OK) == 0) {
        return strdup(canonical_res);
      }

      // Check inside macOS .app bundle: <exe_dir>/../Resources
      snprintf(test_path, sizeof(test_path), "%s/../Resources", exe_dir);
      if (realpath(test_path, canonical_res) != NULL &&
          access(canonical_res, F_OK) == 0) {
        return strdup(canonical_res);
      }
    }
  }
#endif

  // 3. Check compile-time hint if provided and contains valid game assets/markers
  if (hint && *hint && access(hint, F_OK) == 0) {
    char check_path[1024];
    snprintf(check_path, sizeof(check_path), "%s/sce_sys", hint);
    int has_sce_sys = (access(check_path, F_OK) == 0);
    snprintf(check_path, sizeof(check_path), "%s/param.sfo", hint);
    int has_sfo = (access(check_path, F_OK) == 0);
    snprintf(check_path, sizeof(check_path), "%s/eboot.bin", hint);
    int has_eboot = (access(check_path, F_OK) == 0);
    snprintf(check_path, sizeof(check_path), "%s/Media", hint);
    int has_media = (access(check_path, F_OK) == 0);

    if (has_sce_sys || has_sfo || has_eboot || has_media) {
      char canonical_hint[1024];
      if (realpath(hint, canonical_hint) != NULL) {
        return strdup(canonical_hint);
      }
      return strdup(hint);
    }
  }

#if defined(__APPLE__)
  if (exe_dir) {
    char test_path[1024];
    char canonical_res[1024];

    // Check if <exe_dir> is itself a self-contained game folder with markers
    snprintf(test_path, sizeof(test_path), "%s/param.sfo", exe_dir);
    if (access(test_path, F_OK) == 0) {
      if (realpath(exe_dir, canonical_res) != NULL) return strdup(canonical_res);
      return strdup(exe_dir);
    }
    snprintf(test_path, sizeof(test_path), "%s/sce_sys", exe_dir);
    if (access(test_path, F_OK) == 0) {
      if (realpath(exe_dir, canonical_res) != NULL) return strdup(canonical_res);
      return strdup(exe_dir);
    }

    // Check <exe_dir>/assets
    snprintf(test_path, sizeof(test_path), "%s/assets", exe_dir);
    if (access(test_path, F_OK) == 0) {
      if (realpath(exe_dir, canonical_res) != NULL) {
        return strdup(canonical_res);
      }
      return strdup(exe_dir);
    }

    // Check parent directory hierarchy up to 4 levels for assets/
    char cur[1024];
    strncpy(cur, exe_dir, sizeof(cur) - 1);
    cur[sizeof(cur) - 1] = '\0';
    for (int i = 0; i < 4; i++) {
      char *parent = dirname(cur);
      snprintf(test_path, sizeof(test_path), "%s/assets", parent);
      if (access(test_path, F_OK) == 0) {
        if (realpath(parent, canonical_res) != NULL) {
          return strdup(canonical_res);
        }
        return strdup(parent);
      }
      strncpy(cur, parent, sizeof(cur) - 1);
      cur[sizeof(cur) - 1] = '\0';
    }
  }
#endif

  // 4. Check current working directory ./assets or ./Media
  if (access("assets", F_OK) == 0 || access("Media", F_OK) == 0) {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
      return strdup(cwd);
    }
    return strdup(".");
  }

  // 5. Fallback to hint if provided, otherwise "."
  if (hint && *hint && access(hint, F_OK) == 0) {
    char canonical_hint[1024];
    if (realpath(hint, canonical_hint) != NULL) {
      return strdup(canonical_hint);
    }
    return strdup(hint);
  }

  return strdup(".");
}

// Growable table (the overlay mounts one entry per file; a fixed table silently dropped the rest).
typedef struct {
  char *guest_prefix;
  char *host_path;
  int active;
  int ro; // read-only mount: the write gate refuses every mutation below it, whatever the host path is
} VfsMount;

static VfsMount *g_mounts = NULL;
static int g_mount_count = 0;
static int g_mount_cap = 0;

static void vfs_mounts_clear_locked(void) {
  for (int i = 0; i < g_mount_count; i++) { free(g_mounts[i].guest_prefix); free(g_mounts[i].host_path); }
  free(g_mounts);
  g_mounts = NULL;
  g_mount_count = g_mount_cap = 0;
}

// ---------------------------------------------------------------------------------------------
// Per-title writable roots. The application image is read-only; everything a title writes goes
// below one host directory per title (see ps4_vfs.h).

static char g_title_id[64];
static int g_title_id_known;
static pthread_mutex_t g_title_mutex = PTHREAD_MUTEX_INITIALIZER; // held while the id is computed (the SFO loader re-enters the VFS)

static void vfs_sanitize_id(char *s) {
  for (; *s; s++) {
    const char c = *s;
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) *s = '_';
  }
}

// "titleId": "PPSA01234" in sce_sys/param.json (PS5 packages carry no param.sfo).
static bool vfs_title_id_from_param_json(const char *root, char *out, size_t out_sz) {
  char path[1100];
  snprintf(path, sizeof(path), "%s/sce_sys/param.json", root);
  FILE *f = fopen(path, "r");
  if (!f) return false;
  char buf[16384];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';
  const char *p = strstr(buf, "\"titleId\"");
  if (!p) return false;
  p = strchr(p + 9, ':');
  if (!p) return false;
  p = strchr(p, '"');
  if (!p) return false;
  p++;
  size_t k = 0;
  while (p[k] && p[k] != '"' && k + 1 < out_sz) { out[k] = p[k]; k++; }
  out[k] = '\0';
  return k > 0 && p[k] == '"';
}

const char *ps4_vfs_title_id(void) {
  pthread_mutex_lock(&g_title_mutex);
  if (!g_title_id_known) {
    char id[sizeof(g_title_id)] = {0};
    const char *env = getenv("NPS5_TITLE_ID");
    pthread_mutex_lock(&g_vfs_mutex);
    char *root = strdup(g_app_root ? g_app_root : ".");
    pthread_mutex_unlock(&g_vfs_mutex);
    if (env && *env) {
      snprintf(id, sizeof(id), "%s", env);
    } else if (!vfs_title_id_from_param_json(root ? root : ".", id, sizeof(id))) {
      id[0] = '\0';
      Ps4SfoContext sfo = {0}; // resolves paths through the VFS (g_vfs_mutex is not held here)
      char tid[64] = {0};
      if (ps4_sfo_find_and_load(&sfo)) {
        ps4_sfo_get_string(&sfo, "TITLE_ID", tid, sizeof(tid));
        ps4_sfo_free(&sfo);
      }
      snprintf(id, sizeof(id), "%s", tid);
    }
    free(root);
    vfs_sanitize_id(id);
    memcpy(g_title_id, id, sizeof(g_title_id));
    g_title_id_known = 1;
  }
  pthread_mutex_unlock(&g_title_mutex);
  return g_title_id;
}

static int vfs_mkdir_p(const char *path) {
  char tmp[1100];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
      *p = '/';
    }
  }
  return (mkdir(tmp, 0755) != 0 && errno != EEXIST) ? -1 : 0;
}

static bool vfs_host_in_app_root(const char *host);
bool ps4_vfs_userdata_dir(const char *kind, char *out, size_t out_sz) {
  if (!kind || !out || out_sz == 0) return false;
  const char *root = getenv("NPS5_USERDATA_DIR");
  int n;
  if (root && *root) {
    n = snprintf(out, out_sz, "%s/%s", root, kind);
  } else {
    const char *legacy = getenv("PS4RECOMP_DATA_DIR");
    const char *tid = ps4_vfs_title_id();
    if (!*tid) tid = "UNKNOWN";
    if (legacy && *legacy) {
      if (strcmp(kind, "data") != 0) return false; // download0/temp0/savedata keep the PS4Recomp layout there
      n = snprintf(out, out_sz, "%s/data/%s", legacy, tid);
    } else {
      const char *home = getenv("HOME");
      if (!home || !*home) return false;
      n = snprintf(out, out_sz, "%s/Library/Application Support/nativeps5/%s/%s", home, tid, kind);
    }
  }
  if (n < 0 || (size_t)n >= out_sz) return false;
  // A userdata root inside the application directory would put guest-writable files (and the exit-time temp sweep) into the
  // dump: refuse it (checked before the directory is created).
  if (vfs_host_in_app_root(out)) {
    fprintf(stderr, "[ps4-vfs] ERROR: the per-title %s directory %s lies inside the application directory; refused (set NPS5_USERDATA_DIR elsewhere)\n", kind, out);
    return false;
  }
  // /temp0 is private to this process: two instances of a title (or two titles whose id is unknown) must not
  // empty each other's files. Directories of dead processes are swept at start (vfs_sweep_stale_temp).
  if (!strcmp(kind, "temp")) {
    n = snprintf(out + n, out_sz - (size_t)n, "/p%d", (int)getpid()) + n;
    if ((size_t)n >= out_sz) return false;
  }
  return vfs_mkdir_p(out) == 0;
}

static char g_temp_dir[1100]; // the /temp0 directory this process owns ("" = none)

static void vfs_remove_tree(const char *dir, int keep_top) {
  DIR *d = opendir(dir);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      char p[1400];
      snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
      struct stat st;
      if (lstat(p, &st) != 0) continue;
      if (S_ISDIR(st.st_mode)) vfs_remove_tree(p, 0);
      else unlink(p);
    }
    closedir(d);
  }
  if (!keep_top) rmdir(dir);
}

static void vfs_clear_temp(void) {
  if (g_temp_dir[0]) vfs_remove_tree(g_temp_dir, 0);
}

// Removes <temp>/p<pid> of processes that no longer exist (a run that did not exit cleanly).
static void vfs_sweep_stale_temp(const char *own_dir) {
  char parent[1100];
  snprintf(parent, sizeof(parent), "%s", own_dir);
  char *slash = strrchr(parent, '/');
  if (!slash) return;
  *slash = '\0';
  DIR *d = opendir(parent);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] != 'p' || e->d_name[1] < '0' || e->d_name[1] > '9') continue;
    char *end = NULL;
    long pid = strtol(e->d_name + 1, &end, 10);
    if (!end || *end || pid <= 0 || pid == (long)getpid()) continue;
    if (kill((pid_t)pid, 0) == 0 || errno != ESRCH) continue; // still running (or not ours to judge)
    char p[1400];
    snprintf(p, sizeof(p), "%s/%s", parent, e->d_name);
    vfs_remove_tree(p, 0);
  }
  closedir(d);
}

// /data, /temp0 and /download0 exist for every application. Mounted at init so that no path
// below them can ever resolve into the application image. When a directory cannot be created the
// root stays unmounted and ps4_vfs_resolve fails for it (ENODEV): there is deliberately no fallback
// into the dump.
static void vfs_mount_userdata(void) {
  static const struct { const char *guest, *kind; } roots[] = {{"/data", "data"}, {"/temp0", "temp"}, {"/download0", "download0"}};
  for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
    char dir[1100];
    if (!ps4_vfs_userdata_dir(roots[i].kind, dir, sizeof(dir))) {
      const char *legacy = getenv("PS4RECOMP_DATA_DIR");
      if (strcmp(roots[i].kind, "data") != 0 && legacy && *legacy && !(getenv("NPS5_USERDATA_DIR") && *getenv("NPS5_USERDATA_DIR"))) {
        fprintf(stderr, "[ps4-vfs] note: PS4RECOMP_DATA_DIR layout: %s is not provided (set NPS5_USERDATA_DIR)\n", roots[i].guest);
        continue;
      }
      fprintf(stderr, "[ps4-vfs] ERROR: cannot create the per-title %s directory (set NPS5_USERDATA_DIR to a writable place); %s is unavailable, "
                      "access fails with ENODEV\n", roots[i].kind, roots[i].guest);
      continue;
    }
    if (!strcmp(roots[i].kind, "temp")) {
      static int registered;
      snprintf(g_temp_dir, sizeof(g_temp_dir), "%s", dir);
      vfs_remove_tree(dir, 1); // this process's own directory (non-empty only when the VFS is initialised again)
      vfs_sweep_stale_temp(dir);
      if (!registered) { registered = 1; atexit(vfs_clear_temp); }
    }
    ps4_vfs_mount(roots[i].guest, dir);
  }
  // /devlog is the development-kit log volume (absent on a retail console, where opening it fails as it does here by
  // default). NPS5_VFS_DEVLOG=1 mounts it on <userdata>/devlog with an app/ directory, so a title that logs to
  // /devlog/app (the Unity PS5 player writes its player log to /devlog/app/debug.log) leaves that log on the host.
  const char *dl = getenv("NPS5_VFS_DEVLOG");
  if (dl && dl[0] == '1') {
    char dir[1100], app[1200];
    if (ps4_vfs_userdata_dir("devlog", dir, sizeof(dir))) {
      snprintf(app, sizeof(app), "%s/app", dir);
      if (vfs_mkdir_p(app) == 0) {
        ps4_vfs_mount("/devlog", dir);
        fprintf(stderr, "[ps4-vfs] /devlog -> %s (NPS5_VFS_DEVLOG)\n", dir);
      }
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Per-title overlay (docs/m3/uecvar.md). Every regular file below NPS5_OVERLAY_DIR (default
// <userdata>/overlay when it exists) is mounted read-only over the same relative path below /app0,
// so a title sees an Engine.ini / UECommandLine.txt of ours without the application image (which
// stays EROFS and untouched) ever being written. Only files: a directory under /app0 keeps its
// real listing. Guest paths are compared case-insensitively (the console file system is).
static int g_overlay_dropped;

static int vfs_overlay_walk(const char *host_dir, const char *rel, int depth) {
  DIR *d = opendir(host_dir);
  if (!d) return 0;
  int n = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.') continue;
    char host[1100], grel[1100];
    int a = snprintf(host, sizeof(host), "%s/%s", host_dir, e->d_name);
    int b = snprintf(grel, sizeof(grel), "%s%s%s", rel, *rel ? "/" : "", e->d_name);
    struct stat st;
    if (a < 0 || (size_t)a >= sizeof(host) || b < 0 || (size_t)b >= sizeof(grel)) {
      g_overlay_dropped++;
      fprintf(stderr, "[ps4-vfs] WARNING: overlay path too long, not mounted: %s/%s\n", host_dir, e->d_name);
      continue;
    }
    // Directories are walked only when they are real directories (a symlinked directory could loop or leave the
    // overlay tree); a symlink to a regular file is mounted like the file.
    if (lstat(host, &st) != 0) continue;
    if (S_ISLNK(st.st_mode) && (stat(host, &st) != 0 || S_ISDIR(st.st_mode))) continue;
    if (S_ISDIR(st.st_mode)) {
      if (depth < 12) n += vfs_overlay_walk(host, grel, depth + 1);
    } else if (S_ISREG(st.st_mode)) {
      char guest[1200];
      snprintf(guest, sizeof(guest), "/app0/%s", grel);
      int r = ps4_vfs_mount_ro(guest, host);
      if (r == 0) {
        n++;
        PS4_LOGF(NPS5_S_KERNEL, NPS5_L_DEBUG, "ps4-vfs", "Overlay: %s -> %s\n", guest, host);
      } else {
        g_overlay_dropped++;
        fprintf(stderr, "[ps4-vfs] WARNING: overlay file not mounted (%s): %s\n", strerror(-r), guest);
      }
    }
  }
  closedir(d);
  return n;
}

static void vfs_mount_overlay(void) {
  char dir[1100];
  const char *env = getenv("NPS5_OVERLAY_DIR");
  if (env && *env) {
    snprintf(dir, sizeof(dir), "%s", env);
  } else {
    const char *root = getenv("NPS5_USERDATA_DIR");
    if (!root || !*root) return; // the implicit location only with an explicit per-title data root
    snprintf(dir, sizeof(dir), "%s/overlay", root);
  }
  struct stat st;
  if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) return;
  g_overlay_dropped = 0;
  int n = vfs_overlay_walk(dir, "", 0);
  fprintf(stderr, "[ps4-vfs] overlay: %d file(s) mounted over /app0\n", n);
  PS4_LOGF(NPS5_S_KERNEL, NPS5_L_DEBUG, "ps4-vfs", "overlay directory: %s\n", dir);
  if (g_overlay_dropped)
    fprintf(stderr, "[ps4-vfs] ERROR: overlay: %d file(s) NOT mounted and invisible to the title (see warnings above)\n", g_overlay_dropped);
}

void ps4_vfs_init(const char *app_root) {
  pthread_mutex_lock(&g_vfs_mutex);
  if (g_app_root) {
    free(g_app_root);
    g_app_root = NULL;
  }

  g_app_root = discover_app_root(app_root);

  // Strip trailing slash if present
  if (g_app_root) {
    size_t len = strlen(g_app_root);
    while (len > 1 && g_app_root[len - 1] == '/') {
      g_app_root[len - 1] = '\0';
      len--;
    }
  }

  PS4_LOGF(NPS5_S_KERNEL, NPS5_L_DEBUG, "ps4-vfs", "Application root: %s\n",
         g_app_root ? g_app_root : "(null)");

  vfs_mounts_clear_locked();

  pthread_mutex_lock(&g_vfile_mutex);
  memset(g_vfiles, 0, sizeof(g_vfiles));
  pthread_mutex_unlock(&g_vfile_mutex);

  free(g_app_root_real);
  g_app_root_real = g_app_root ? realpath(g_app_root, NULL) : NULL;
  pthread_mutex_unlock(&g_vfs_mutex);

  pthread_mutex_lock(&g_title_mutex);
  g_title_id_known = 0;
  pthread_mutex_unlock(&g_title_mutex);
  vfs_mount_userdata();
  vfs_mount_overlay();

  // Initialize universal PAK provider
  ps4_pak_init(g_app_root);
}

const char *ps4_vfs_get_app_root(void) {
  pthread_mutex_lock(&g_vfs_mutex);
  const char *root = g_app_root ? g_app_root : ".";
  pthread_mutex_unlock(&g_vfs_mutex);
  return root;
}

static int vfs_mount_flags(const char *guest_prefix, const char *host_path, int ro) {
  if (!guest_prefix || !host_path || *guest_prefix == '\0' ||
      *host_path == '\0') {
    return -EINVAL;
  }
  if (strlen(host_path) >= 1024) return -ENAMETOOLONG; // longer than the resolver's buffers: would truncate and alias

  pthread_mutex_lock(&g_vfs_mutex);
  for (int i = 0; i < g_mount_count; i++) {
    if (g_mounts[i].active &&
        strcasecmp(g_mounts[i].guest_prefix, guest_prefix) == 0) {
      char *hp = strdup(host_path);
      if (!hp) { pthread_mutex_unlock(&g_vfs_mutex); return -ENOMEM; }
      free(g_mounts[i].host_path);
      g_mounts[i].host_path = hp;
      g_mounts[i].ro = ro;
      pthread_mutex_unlock(&g_vfs_mutex);
      return 0;
    }
  }

  if (g_mount_count >= g_mount_cap) {
    int cap = g_mount_cap ? g_mount_cap * 2 : 64;
    VfsMount *nm = (VfsMount *)realloc(g_mounts, (size_t)cap * sizeof(VfsMount));
    if (!nm) {
      pthread_mutex_unlock(&g_vfs_mutex);
      return -ENOMEM;
    }
    g_mounts = nm;
    g_mount_cap = cap;
  }

  char *gp = strdup(guest_prefix), *hp = strdup(host_path);
  if (!gp || !hp) {
    free(gp);
    free(hp);
    pthread_mutex_unlock(&g_vfs_mutex);
    return -ENOMEM;
  }
  VfsMount *m = &g_mounts[g_mount_count++];
  m->guest_prefix = gp;
  m->host_path = hp;
  m->active = 1;
  m->ro = ro;

  pthread_mutex_unlock(&g_vfs_mutex);
  return 0;
}

int ps4_vfs_mount(const char *guest_prefix, const char *host_path) { return vfs_mount_flags(guest_prefix, host_path, 0); }
int ps4_vfs_mount_ro(const char *guest_prefix, const char *host_path) { return vfs_mount_flags(guest_prefix, host_path, 1); }

int ps4_vfs_unmount(const char *guest_prefix) {
  if (!guest_prefix) {
    return -EINVAL;
  }

  pthread_mutex_lock(&g_vfs_mutex);
  for (int i = 0; i < g_mount_count; i++) {
    if (g_mounts[i].active &&
        strcasecmp(g_mounts[i].guest_prefix, guest_prefix) == 0) {
      g_mounts[i].active = 0;
      pthread_mutex_unlock(&g_vfs_mutex);
      return 0;
    }
  }
  pthread_mutex_unlock(&g_vfs_mutex);
  return -ENOENT;
}

static void normalize_chroot_subpath(const char *subpath, char *out,
                                     size_t out_sz) {
  if (!subpath || !out || out_sz == 0)
    return;
  out[0] = '\0';

  char buf[1024];
  size_t slen = strlen(subpath);
  if (slen >= sizeof(buf)) {
    slen = sizeof(buf) - 1;
  }
  memcpy(buf, subpath, slen);
  buf[slen] = '\0';

  const char *segs[64];
  int seg_count = 0;

  char *saveptr = NULL;
  char *token = strtok_r(buf, "/", &saveptr);
  while (token) {
    if (strcmp(token, ".") == 0 || *token == '\0') {
      // Ignore current directory
    } else if (strcmp(token, "..") == 0) {
      if (seg_count > 0) {
        seg_count--;
      }
    } else {
      if (seg_count < 64) {
        segs[seg_count++] = token;
      }
    }
    token = strtok_r(NULL, "/", &saveptr);
  }

  size_t written = 0;
  for (int i = 0; i < seg_count; i++) {
    size_t part_len = strlen(segs[i]);
    if (written + part_len + 2 >= out_sz)
      break;
    if (written > 0) {
      out[written++] = '/';
    }
    memcpy(out + written, segs[i], part_len);
    written += part_len;
    out[written] = '\0';
  }
  out[written] = '\0';
}

// ---- directory listing cache (NPS5_VFS_DIRCACHE=1; default off) -------------------------------------------------
// A failed access() makes case_insensitive_lookup read the whole parent directory for a case-insensitive match; a
// level load probes thousands of files that do not exist in directories of 23k entries. With the cache the listing
// is read once and a miss costs one hash lookup. A listing is trusted only while the directory's inode and mtime
// (ns) are unchanged AND the mtime is older than DIRCACHE_RACY_S at scan time (an entry created right after the scan
// within the timestamp granularity would otherwise be missed); every mutator also bumps a global generation
// (ps4_vfs_resolve_for_write), which invalidates all listings. Match semantics are the old loop's: the first entry
// in readdir order for which strcasecmp(name, part) == 0.
#define DIRCACHE_SLOTS 32
#define DIRCACHE_MAX_ENTRIES 1000000
#define DIRCACHE_RACY_S 2
typedef struct {
  char *path;              // directory (as resolved), NULL = free
  ino_t ino; dev_t dev;
  struct timespec mtime;
  uint64_t gen;
  uint32_t count, cap;     // entries, hash table capacity (power of two)
  char **names;            // [count] malloc'd
  int32_t *table;          // [cap] index into names, -1 empty
} dircache_entry_t;
static pthread_mutex_t g_dircache_mu = PTHREAD_MUTEX_INITIALIZER;
static dircache_entry_t g_dircache[DIRCACHE_SLOTS];
static unsigned g_dircache_next;
static _Atomic uint64_t g_dircache_gen;
static _Atomic uint64_t g_dircache_hits, g_dircache_scans;
static int g_dircache_on = -1;

static int dircache_enabled(void) {
  int on = g_dircache_on;
  if (on < 0) { const char *e = getenv("NPS5_VFS_DIRCACHE"); on = e && *e && strcmp(e, "0") != 0; g_dircache_on = on; }
  return on;
}
void ps4_vfs_dircache_set(int on) { g_dircache_on = on ? 1 : 0; }
void ps4_vfs_dircache_stats(uint64_t *hits, uint64_t *scans) { if (hits) *hits = g_dircache_hits; if (scans) *scans = g_dircache_scans; }
static void dircache_bump(void) { atomic_fetch_add(&g_dircache_gen, 1); }

static uint64_t dircache_hash(const char *s) {
  uint64_t h = 1469598103934665603ull;
  for (; *s; s++) { h ^= (unsigned char)tolower((unsigned char)*s); h *= 1099511628211ull; }
  return h;
}
static void dircache_free(dircache_entry_t *e) {
  if (!e->path) return;
  for (uint32_t i = 0; i < e->count; i++) free(e->names[i]);
  free(e->names); free(e->table); free(e->path);
  memset(e, 0, sizeof *e);
}
// Reads `dir`; returns 0 and fills *e (path stays NULL on failure).
static int dircache_scan(const char *dir, const struct stat *st, dircache_entry_t *e) {
  DIR *d = opendir(dir);
  if (!d) return -1;
  uint32_t cap_names = 256, n = 0;
  int bad = 0;                         // too big / out of memory: nothing is kept, the caller scans the old way
  char **names = malloc(cap_names * sizeof(char *));
  struct dirent *de;
  while (names && (de = readdir(d)) != NULL) {
    if (n >= DIRCACHE_MAX_ENTRIES) { bad = 1; break; }
    if (n == cap_names) { cap_names *= 2; char **nn = realloc(names, cap_names * sizeof(char *)); if (!nn) { bad = 1; break; } names = nn; }
    names[n] = strdup(de->d_name);
    if (!names[n]) { bad = 1; break; }
    n++;
  }
  closedir(d);
  if (!names) return -1;
  if (bad) { for (uint32_t i = 0; i < n; i++) free(names[i]); free(names); return -1; }
  uint32_t cap = 16; while (cap < n * 2u) cap <<= 1;
  int32_t *table = malloc(cap * sizeof(int32_t));
  if (!table) { for (uint32_t i = 0; i < n; i++) free(names[i]); free(names); return -1; }
  for (uint32_t i = 0; i < cap; i++) table[i] = -1;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t j = (uint32_t)dircache_hash(names[i]) & (cap - 1);
    int dup = 0;
    for (; table[j] >= 0; j = (j + 1) & (cap - 1))
      if (strcasecmp(names[table[j]], names[i]) == 0) { dup = 1; break; }  // keep the first in readdir order
    if (!dup) table[j] = (int32_t)i;
  }
  e->path = strdup(dir); e->ino = st->st_ino; e->dev = st->st_dev; e->mtime = st->st_mtimespec;
  e->count = n; e->cap = cap; e->names = names; e->table = table;
  if (!e->path) { dircache_free(e); return -1; }
  return 0;
}
// 1 and *match = the entry name when `part` matches case-insensitively; 0 no match; -1 cache not usable (scan the
// directory the old way).
static int dircache_lookup(const char *dir, const char *part, char *match, size_t match_sz) {
  struct stat st;
  if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
  struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
  int racy = now.tv_sec - st.st_mtimespec.tv_sec < DIRCACHE_RACY_S;
  uint64_t gen = atomic_load(&g_dircache_gen);
  int r = 0;
  pthread_mutex_lock(&g_dircache_mu);
  dircache_entry_t *e = NULL;
  for (int i = 0; i < DIRCACHE_SLOTS; i++)
    if (g_dircache[i].path && strcmp(g_dircache[i].path, dir) == 0) { e = &g_dircache[i]; break; }
  if (e && (e->gen != gen || e->ino != st.st_ino || e->dev != st.st_dev ||
            e->mtime.tv_sec != st.st_mtimespec.tv_sec || e->mtime.tv_nsec != st.st_mtimespec.tv_nsec)) {
    dircache_free(e); e = NULL;
  }
  if (!e) {
    if (racy) { pthread_mutex_unlock(&g_dircache_mu); return -1; }
    dircache_entry_t fresh; memset(&fresh, 0, sizeof fresh);
    if (dircache_scan(dir, &st, &fresh) != 0) { pthread_mutex_unlock(&g_dircache_mu); return -1; }
    fresh.gen = gen;
    atomic_fetch_add(&g_dircache_scans, 1);
    e = &g_dircache[g_dircache_next++ % DIRCACHE_SLOTS];
    dircache_free(e); *e = fresh;
  } else {
    atomic_fetch_add(&g_dircache_hits, 1);
  }
  for (uint32_t j = (uint32_t)dircache_hash(part) & (e->cap - 1); e->table[j] >= 0; j = (j + 1) & (e->cap - 1)) {
    const char *nm = e->names[e->table[j]];
    if (strcasecmp(nm, part) == 0) { snprintf(match, match_sz, "%s", nm); r = 1; break; }
  }
  pthread_mutex_unlock(&g_dircache_mu);
  return r;
}

static int case_insensitive_lookup(const char *host_path, char *out_path,
                                   size_t out_sz) {
  if (!host_path || !out_path || out_sz == 0)
    return -EINVAL;

  if (access(host_path, F_OK) == 0) {
    if (strlen(host_path) >= out_sz)
      return -ENAMETOOLONG;
    strncpy(out_path, host_path, out_sz - 1);
    out_path[out_sz - 1] = '\0';
    return 0;
  }

  char temp[1024];
  size_t hlen = strlen(host_path);
  if (hlen >= sizeof(temp))
    return -ENAMETOOLONG;
  memcpy(temp, host_path, hlen + 1);

  const char *parts[64];
  int num_parts = 0;

  char *curr = temp;
  while (*curr) {
    if (access(curr, F_OK) == 0) {
      break;
    }
    char *slash = strrchr(curr, '/');
    if (!slash || slash == curr) {
      if (slash == curr) {
        if (num_parts < 64)
          parts[num_parts++] = slash + 1;
        *slash = '\0';
      } else {
        if (num_parts < 64)
          parts[num_parts++] = curr;
        curr[0] = '\0';
      }
      break;
    }
    if (num_parts < 64)
      parts[num_parts++] = slash + 1;
    *slash = '\0';
  }

  char resolved[1024];
  if (*curr) {
    strncpy(resolved, curr, sizeof(resolved) - 1);
    resolved[sizeof(resolved) - 1] = '\0';
  } else {
    strncpy(resolved, "/", sizeof(resolved) - 1);
    resolved[sizeof(resolved) - 1] = '\0';
  }

  for (int i = num_parts - 1; i >= 0; i--) {
    const char *part = parts[i];
    if (!part || *part == '\0') {
      continue;
    }

    char test_path[1024];
    int is_root = (strcmp(resolved, "/") == 0);
    snprintf(test_path, sizeof(test_path), "%s%s%s", resolved,
             is_root ? "" : "/", part);
    if (access(test_path, F_OK) == 0) {
      strncpy(resolved, test_path, sizeof(resolved) - 1);
      resolved[sizeof(resolved) - 1] = '\0';
      continue;
    }

    int matched = 0;
    char cached_name[NAME_MAX + 1];
    int cr = dircache_enabled() ? dircache_lookup(resolved, part, cached_name, sizeof cached_name) : -1;
    if (cr >= 0) {
      if (cr == 1) {
        snprintf(test_path, sizeof(test_path), "%s%s%s", resolved, is_root ? "" : "/", cached_name);
        strncpy(resolved, test_path, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
        matched = 1;
      }
    }
    DIR *dir = cr < 0 ? opendir(resolved) : NULL;
    if (dir) {
      struct dirent *de;
      while ((de = readdir(dir)) != NULL) {
        if (strcasecmp(de->d_name, part) == 0) {
          snprintf(test_path, sizeof(test_path), "%s%s%s", resolved,
                   is_root ? "" : "/", de->d_name);
          strncpy(resolved, test_path, sizeof(resolved) - 1);
          resolved[sizeof(resolved) - 1] = '\0';
          matched = 1;
          break;
        }
      }
      closedir(dir);
    }

    if (!matched) {
      snprintf(test_path, sizeof(test_path), "%s%s%s", resolved,
               is_root ? "" : "/", part);
      strncpy(resolved, test_path, sizeof(resolved) - 1);
      resolved[sizeof(resolved) - 1] = '\0';
    }
  }

  if (strlen(resolved) >= out_sz)
    return -ENAMETOOLONG;
  strncpy(out_path, resolved, out_sz - 1);
  out_path[out_sz - 1] = '\0';
  return 0;
}

static bool vfs_prefix_ci(const char *g, const char *root) {
  size_t n = strlen(root);
  return strncasecmp(g, root, n) == 0 && (g[n] == '/' || g[n] == '\0');
}

static bool vfs_is_userdata_root(const char *g) {
  return vfs_prefix_ci(g, "/data") || vfs_prefix_ci(g, "/temp0") || vfs_prefix_ci(g, "/download0");
}

// How a guest path was resolved: through a mount (the per-title roots, overlay files), below the application
// root (/app0, /hostapp, relative paths), or passed through as a host path.
enum { VFS_VIA_MOUNT = 0, VFS_VIA_APPROOT = 1, VFS_VIA_RAW = 2 };

static void vfs_join_root(char *raw_host, size_t sz, const char *root, const char *subpath) {
  while (*subpath == '/') subpath++;
  char norm_subpath[1024];
  normalize_chroot_subpath(subpath, norm_subpath, sizeof(norm_subpath));
  if (norm_subpath[0]) snprintf(raw_host, sz, "%s/%s", root, norm_subpath);
  else snprintf(raw_host, sz, "%s", root);
}

static int vfs_resolve_ex(const char *guest_path, char *host_path, size_t host_path_sz, int *kind, int *mount_ro, char *mount_root, size_t mount_root_sz) {
  if (kind) *kind = VFS_VIA_MOUNT;
  if (mount_ro) *mount_ro = 0;
  if (mount_root && mount_root_sz) mount_root[0] = '\0';
  if (!guest_path || !host_path || host_path_sz == 0) {
    return -EINVAL;
  }
  { // NPS5_VFS_TRACE_INI=1: log every lookup of a *.ini path (which config files does the title probe?)
    static int trace = -1;
    if (trace < 0) { const char *e = getenv("NPS5_VFS_TRACE_INI"); trace = e && *e == '1'; }
    if (trace) {
      size_t n = strlen(guest_path);
      if (n > 4 && !strcasecmp(guest_path + n - 4, ".ini")) {
        int overlay = 0;
        pthread_mutex_lock(&g_vfs_mutex);
        for (int i = 0; i < g_mount_count; i++) if (g_mounts[i].active && !strcasecmp(guest_path, g_mounts[i].guest_prefix)) overlay = 1;
        pthread_mutex_unlock(&g_vfs_mutex);
        fprintf(stderr, "[ps4-vfs] ini lookup: %s%s\n", guest_path, overlay ? " (overlay)" : "");
        if (overlay) { void *bt[4]; int nb = backtrace(bt, 4); backtrace_symbols_fd(bt, nb, 2); }
      }
    }
  }

  pthread_mutex_lock(&g_vfs_mutex);
  const char *root = g_app_root ? g_app_root : ".";
  char raw_host[1024];
  raw_host[0] = '\0';
  int via = VFS_VIA_MOUNT;
  int err = 0;

  for (int i = 0; i < g_mount_count; i++) {
    if (!g_mounts[i].active)
      continue;
    size_t plen = strlen(g_mounts[i].guest_prefix);
    if (strncasecmp(guest_path, g_mounts[i].guest_prefix, plen) == 0 &&
        (guest_path[plen] == '/' || guest_path[plen] == '\0')) {
      vfs_join_root(raw_host, sizeof(raw_host), g_mounts[i].host_path, guest_path + plen);
      if (mount_ro) *mount_ro = g_mounts[i].ro;
      if (mount_root && mount_root_sz) snprintf(mount_root, mount_root_sz, "%s", g_mounts[i].host_path);
      break;
    }
  }

  if (raw_host[0] == '\0') {
    if (vfs_prefix_ci(guest_path, "/app0")) {
      via = VFS_VIA_APPROOT;
      vfs_join_root(raw_host, sizeof(raw_host), root, guest_path + 5);
    } else if (vfs_prefix_ci(guest_path, "/hostapp")) {
      via = VFS_VIA_APPROOT;
      vfs_join_root(raw_host, sizeof(raw_host), root, guest_path + 8);
    } else if (vfs_is_userdata_root(guest_path)) {
      // /data, /temp0 or /download0 whose per-title mount could not be created at init: fail closed. (/data used to
      // resolve to <app root>/data, i.e. into the dump, which made it writable.)
      err = -ENODEV;
    } else if (guest_path[0] != '/') {
      via = VFS_VIA_APPROOT;
      vfs_join_root(raw_host, sizeof(raw_host), root, guest_path);
    } else {
      via = VFS_VIA_RAW;
      strncpy(raw_host, guest_path, sizeof(raw_host) - 1);
      raw_host[sizeof(raw_host) - 1] = '\0';
    }
  }
  pthread_mutex_unlock(&g_vfs_mutex);

  if (err) return err;
  if (kind) *kind = via;
  return case_insensitive_lookup(raw_host, host_path, host_path_sz);
}

int ps4_vfs_resolve(const char *guest_path, char *host_path, size_t host_path_sz) {
  return vfs_resolve_ex(guest_path, host_path, host_path_sz, NULL, NULL, NULL, 0);
}

static const char *get_app0_subpath(const char *guest_path) {
  if (!guest_path)
    return NULL;
  if (strncmp(guest_path, "/app0/", 6) == 0) {
    return guest_path + 6;
  }
  if (strcmp(guest_path, "/app0") == 0) {
    return "";
  }
  if (strncmp(guest_path, "/hostapp/", 9) == 0) {
    return guest_path + 9;
  }
  if (guest_path[0] != '/') {
    return guest_path;
  }
  return NULL;
}

// /app0 is the title's read-only package image on the console: nothing under it can be created,
// written, renamed or removed (EROFS). Titles probe for that (Unreal Engine tries to write
// Engine/Saved under /app0 and falls back to /download0), and the dump on the host stays as it is.
// Case-insensitive, like the mount matching and the console file system.
bool ps4_vfs_is_readonly(const char *guest_path) {
  if (!guest_path) return false;
  if (guest_path[0] != '/') return true; // relative: the working directory is the application image
  return vfs_prefix_ci(guest_path, "/hostapp") || vfs_prefix_ci(guest_path, "/app0");
}

// Lexically normalizes an absolute host path ("." and ".." removed, repeated slashes folded).
static void vfs_lex_normalize(const char *in, char *out, size_t sz) {
  char abs[2200];
  if (in[0] == '/') {
    snprintf(abs, sizeof(abs), "%s", in);
  } else {
    char cwd[1100];
    if (!getcwd(cwd, sizeof(cwd))) cwd[0] = '\0';
    snprintf(abs, sizeof(abs), "%s/%s", cwd, in);
  }
  size_t w = 0;
  out[0] = '\0';
  char *save = NULL;
  char *segs[256];
  int n = 0;
  for (char *t = strtok_r(abs, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
    if (!strcmp(t, ".") || !*t) continue;
    if (!strcmp(t, "..")) { if (n) n--; continue; }
    if (n < 256) segs[n++] = t;
  }
  for (int i = 0; i < n; i++) {
    int k = snprintf(out + w, sz - w, "/%s", segs[i]);
    if (k < 0 || (size_t)k >= sz - w) break;
    w += (size_t)k;
  }
  if (!w) snprintf(out, sz, "/");
}

// True when the host path lies inside the application directory (the dump): the deepest existing ancestor is
// canonicalized with realpath, so symlinks and ".." cannot hide it. Compared case-insensitively. With
// include_ancestors a path that is the root itself or one of its ancestors also counts (renaming, removing or
// truncating an ancestor moves or destroys the dump). The gate FAILS CLOSED: when the root is set but could not be
// canonicalized (dump moved or unmounted) every path counts as inside.
static bool vfs_host_hits_root(const char *host, bool include_ancestors) {
  pthread_mutex_lock(&g_vfs_mutex);
  char *root_raw = g_app_root ? strdup(g_app_root) : NULL;
  pthread_mutex_unlock(&g_vfs_mutex);
  if (!root_raw) return false; // no application root configured yet: nothing to protect
  // Resolved on every check (not cached at init): a dump that is moved or unmounted later makes the gate fail closed.
  char *root = realpath(root_raw, NULL);
  free(root_raw);
  if (!root) return true;
  char norm[1100], probe[1100], rp[PATH_MAX_VFS];
  vfs_lex_normalize(host, norm, sizeof(norm));
  snprintf(probe, sizeof(probe), "%s", norm);
  bool in = false;
  for (;;) {
    if (realpath(probe, rp)) {
      char full[2200];
      snprintf(full, sizeof(full), "%s%s", rp, norm + strlen(probe));
      size_t rl = strlen(root), fl = strlen(full);
      in = strncasecmp(full, root, rl) == 0 && (full[rl] == '/' || full[rl] == '\0');
      if (!in && include_ancestors) {
        // "full" is an ancestor of root: root continues with '/' right after it (or full is "/")
        in = fl <= rl && strncasecmp(full, root, fl) == 0 && (fl == 1 || root[fl] == '/' || root[fl] == '\0');
      }
      break;
    }
    char *slash = strrchr(probe, '/');
    if (!slash || slash == probe) break;
    *slash = '\0';
  }
  free(root);
  return in;
}
static bool vfs_host_in_app_root(const char *host) { return vfs_host_hits_root(host, false); }

// An absolute guest path that is neither below /app0, /hostapp nor a mount resolves to the host path itself (read
// only use by the title's own tools and tests). Writing through it is refused (EACCES) except for the device files
// below; NPS5_VFS_RAW_WRITE=1 restores the unrestricted behaviour.
static bool vfs_raw_write_env(void) {
  static _Atomic int logged;
  const char *e = getenv("NPS5_VFS_RAW_WRITE");
  bool on = e && *e == '1';
  if (on && !atomic_exchange(&logged, 1)) fprintf(stderr, "[ps4-vfs] NPS5_VFS_RAW_WRITE=1: writes to unmounted host paths are NOT refused\n");
  return on;
}

static bool vfs_raw_write_allowed(const char *guest_path) {
  if (vfs_raw_write_env()) return true;
  return !strcmp(guest_path, "/dev/null");
}

// Logs the first few refusals only; always returns -EACCES.
static int vfs_raw_write_refuse(const char *what, const char *guest_path) {
  static _Atomic int n;
  int k = atomic_fetch_add(&n, 1);
  if (k < 8) fprintf(stderr, "[ps4-vfs] refused %s of unmounted host path (EACCES): %.200s%s\n", what, guest_path, k == 7 ? " (further refusals not logged)" : "");
  return -EACCES;
}

// True when the deepest existing ancestor of host, canonicalized, lies inside the (canonicalized) mount directory:
// a symlink created or found inside a writable mount cannot lead a write outside it. A mount directory that cannot
// be canonicalized is not judged here (the lexical clamp in the resolver still applies).
static bool vfs_host_inside_mount(const char *host, const char *mount_dir) {
  if (vfs_raw_write_env()) return true;
  char root[PATH_MAX_VFS];
  if (!realpath(mount_dir, root)) return true;
  char norm[1100], probe[1100], rp[PATH_MAX_VFS];
  vfs_lex_normalize(host, norm, sizeof(norm));
  snprintf(probe, sizeof(probe), "%s", norm);
  for (;;) {
    if (realpath(probe, rp)) {
      char full[2200];
      snprintf(full, sizeof(full), "%s%s", rp, norm + strlen(probe));
      size_t rl = strlen(root);
      return strncmp(full, root, rl) == 0 && (full[rl] == '/' || full[rl] == '\0' || rl == 1);
    }
    char *slash = strrchr(probe, '/');
    if (!slash || slash == probe) return false;
    *slash = '\0';
  }
}

// THE write gate. Every path-taking call that creates, changes or removes anything resolves through here (the
// ps4_vfs_* mutators below; shims must not call ps4_vfs_resolve for a mutation). Returns 0, or a negative errno:
// -EROFS for the read-only application image (case-insensitive, relative paths, and any path that resolves into
// the application directory by a host path, a "..", a symlink or a mount of any kind) and for read-only mounts,
// -ENODEV for a per-title root that has no mount.
int ps4_vfs_resolve_for_write(const char *guest_path, char *host_path, size_t host_path_sz) {
  if (!guest_path) return -EFAULT;
  if (ps4_vfs_is_readonly(guest_path)) return -EROFS;
  dircache_bump(); // a mutation follows: no directory listing may outlive it
  int kind = 0, ro = 0;
  char mroot[1100];
  int r = vfs_resolve_ex(guest_path, host_path, host_path_sz, &kind, &ro, mroot, sizeof(mroot));
  if (r < 0) return r;
  if (ro) return -EROFS;
  if (vfs_host_hits_root(host_path, true)) return -EROFS;
  if (kind == VFS_VIA_RAW && !vfs_raw_write_allowed(guest_path)) return vfs_raw_write_refuse("write", guest_path);
  if (kind == VFS_VIA_MOUNT && mroot[0] && !vfs_host_inside_mount(host_path, mroot)) return vfs_raw_write_refuse("write (symlink out of mount)", guest_path);
  return 0;
}

int ps4_vfs_check_writable(const char *guest_path) {
  char h[1024];
  return ps4_vfs_resolve_for_write(guest_path, h, sizeof(h));
}

// An fd that refers to a file in the application image can be changed through fchmod/fchown/futimes/ftruncate even
// when it was opened read-only. Virtual (pak) fds and fds whose path is inside the application directory are refused.
int ps4_vfs_fd_check_writable(int fd) {
  if (ps4_vfs_is_virtual_fd(fd)) return -EROFS;
#ifdef F_GETPATH
  char p[PATH_MAX_VFS];
  if (fcntl(fd, F_GETPATH, p) == 0) {
    if (vfs_host_in_app_root(p)) return -EROFS;
  }
#endif
  return 0;
}

#define VFS_W1(guest, hv)                                      \
  char hv[1024];                                               \
  do {                                                         \
    int r_ = ps4_vfs_resolve_for_write((guest), hv, sizeof(hv)); \
    if (r_ < 0) { errno = -r_; return -1; }                    \
  } while (0)
#define VFS_FD(fd)                                       \
  do {                                                   \
    int r_ = ps4_vfs_fd_check_writable(fd);              \
    if (r_ < 0) { errno = -r_; return -1; }              \
  } while (0)

int ps4_vfs_unlink(const char *g) { VFS_W1(g, h); return unlink(h); }
int ps4_vfs_mkdir(const char *g, mode_t mode) { VFS_W1(g, h); return mkdir(h, mode); }
int ps4_vfs_rmdir(const char *g) { VFS_W1(g, h); return rmdir(h); }
int ps4_vfs_remove(const char *g) { VFS_W1(g, h); return remove(h); }
int ps4_vfs_truncate(const char *g, off_t len) { VFS_W1(g, h); return truncate(h, len); }
int ps4_vfs_chmod(const char *g, mode_t mode) { VFS_W1(g, h); return chmod(h, mode); }
int ps4_vfs_chown(const char *g, uid_t u, gid_t gr) { VFS_W1(g, h); return chown(h, u, gr); }
int ps4_vfs_chflags(const char *g, unsigned int fl) { VFS_W1(g, h); return chflags(h, fl); }
int ps4_vfs_mkfifo(const char *g, mode_t mode) { VFS_W1(g, h); return mkfifo(h, mode); }
int ps4_vfs_utimes(const char *g, const struct timeval tv[2]) { VFS_W1(g, h); return utimes(h, tv); }
int ps4_vfs_setxattr(const char *g, const char *name, const void *v, size_t n, int flags) {
  VFS_W1(g, h);
  return setxattr(h, name, v, n, 0, flags);
}
int ps4_vfs_removexattr(const char *g, const char *name) { VFS_W1(g, h); return removexattr(h, name, 0); }
int ps4_vfs_rename(const char *a, const char *b) {
  VFS_W1(a, ha);
  VFS_W1(b, hb);
  return rename(ha, hb);
}
// A hard link shares the inode, so a writable name for an image file would write the image: both ends must be writable.
int ps4_vfs_link(const char *src, const char *dst) {
  VFS_W1(src, hs);
  VFS_W1(dst, hd);
  return link(hs, hd);
}
// The link text is stored as the resolved host path; a link to the application image is refused for the same reason.
int ps4_vfs_symlink(const char *target, const char *linkpath) {
  VFS_W1(linkpath, hl);
  char ht[1024];
  if (!target) { errno = EFAULT; return -1; }
  if (ps4_vfs_is_readonly(target)) { errno = EROFS; return -1; }
  int kind = 0;
  if (vfs_resolve_ex(target, ht, sizeof(ht), &kind, NULL, NULL, 0) < 0) snprintf(ht, sizeof(ht), "%s", target);
  // A relative host target is relative to the link's directory.
  char abs_t[2300];
  if (ht[0] != '/') snprintf(abs_t, sizeof(abs_t), "%s/../%s", hl, ht);
  else snprintf(abs_t, sizeof(abs_t), "%s", ht);
  // Refuse a target that is the application directory, inside it, or one of its ancestors (a link through which the
  // gate would otherwise be walked around), for every kind of path.
  if (vfs_host_hits_root(abs_t, true)) { errno = EROFS; return -1; }
  // The link text must not lead out of the writable mounts (a later write through the link would escape the gate).
  if (target[0] == '/' && kind == VFS_VIA_RAW && !vfs_raw_write_allowed(target)) { errno = -vfs_raw_write_refuse("symlink", target); return -1; }
  return symlink(ht, hl);
}
int ps4_vfs_fchmod(int fd, mode_t mode) { VFS_FD(fd); return fchmod(fd, mode); }
int ps4_vfs_fchown(int fd, uid_t u, gid_t gr) { VFS_FD(fd); return fchown(fd, u, gr); }
int ps4_vfs_futimes(int fd, const struct timeval tv[2]) { VFS_FD(fd); return futimes(fd, tv); }
int ps4_vfs_ftruncate(int fd, off_t len) { VFS_FD(fd); return ftruncate(fd, len); }
int ps4_vfs_fsetxattr(int fd, const char *name, const void *v, size_t n, int flags) {
  VFS_FD(fd);
  return fsetxattr(fd, name, v, n, 0, flags);
}

// Opens a gate-approved host path for writing. The descriptor is re-checked after the open (F_GETPATH): another guest
// thread may have swapped a path component for a symlink between the gate and the open. O_TRUNC is applied only after
// that check (ftruncate), so a refused open leaves the file's bytes alone. (A file that O_CREAT itself created inside
// the image by that race stays; the bytes of existing files are never touched.)
static int vfs_open_checked(const char *host, int flags, mode_t mode, bool trunc) {
  int fl = flags & ~O_TRUNC;
  int fd = open(host, fl, mode);
  if (fd < 0) return -1;
  int r = ps4_vfs_fd_check_writable(fd);
  if (r < 0) { close(fd); errno = -r; return -1; }
  if (trunc) {
    if ((flags & O_ACCMODE) == O_RDONLY) {
      // O_RDONLY|O_TRUNC is undefined by POSIX; there is no descriptor that may truncate: nothing is truncated
    } else if (ftruncate(fd, 0) != 0) {
      int e = errno; close(fd); errno = e; return -1;
    }
  }
  return fd;
}

FILE *ps4_vfs_fopen(const char *guest_path, const char *mode) {
  if (!guest_path || !mode) { errno = EFAULT; return NULL; }
  char host[1024];
  bool write = strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+') || strchr(mode, 'x');
  int r = write ? ps4_vfs_resolve_for_write(guest_path, host, sizeof(host)) : ps4_vfs_resolve(guest_path, host, sizeof(host));
  if (r < 0) {
    errno = (write || r == -ENODEV || r == -EROFS) ? -r : ENOENT;
    return NULL;
  }
  if (!write) return fopen(host, mode);
  // Write modes: open the descriptor ourselves (without truncating), re-check where it really landed, then truncate.
  int fl = strchr(mode, '+') ? O_RDWR : O_WRONLY;
  bool trunc = false;
  if (strchr(mode, 'w')) { fl |= O_CREAT; trunc = true; }
  if (strchr(mode, 'a')) fl |= O_CREAT | O_APPEND;
  if (strchr(mode, 'x')) fl |= O_EXCL;
  int fd = vfs_open_checked(host, fl, 0666, trunc);
  if (fd < 0) return NULL;
  FILE *f = fdopen(fd, mode);
  if (!f) { int e = errno; close(fd); errno = e; }
  return f;
}

bool ps4_vfs_is_virtual_fd(int fd) {
  return (fd >= PS4_VFS_VIRTUAL_FD_BASE &&
          fd < PS4_VFS_VIRTUAL_FD_BASE + MAX_VIRTUAL_FILES);
}

// [scene-open] <name> t=<s>: first open of any file under <key><name>/ (path relative to app0, case-insensitive, any depth of
// prefix such as data/prein/). <key> is NPS5_SCENE_OPEN_KEY (profile game.tsv key scene_open_key; default "levels/", a
// trailing "/" is added; "off" or empty disables). Printed once per name. The legacy "[level-open] <name> t=<s>" line is
// printed right after it (alias kept for one release so smartwatch.py and agent scripts keep working; NPS5_NO_LEVEL_ALIAS=1
// drops it). Lock-free lookup (acquire-loaded count over immutable entries), the mutex is taken only the first time a name
// is seen; no allocation. Cost per open: one scan of the path for the key. Disable all with NPS5_NO_LEVEL_LOG=1.
#define LVL_MAX 128
#define LVL_NAME 48
#define SCENE_KEY_MAX 64
static char g_lvl_names[LVL_MAX][LVL_NAME];
static _Atomic int g_lvl_count;
static pthread_mutex_t g_lvl_mu = PTHREAD_MUTEX_INITIALIZER;
static void lp_phase(const char *name, double t);   // load accounting below: a new scene starts a phase
static struct timespec g_lvl_t0;
static int g_lvl_off, g_lvl_alias = 1;
static char g_scene_key[SCENE_KEY_MAX] = "levels/";
static size_t g_scene_klen = 7;
// The scene the game is in: the scene directory of the last file opened under the key, menus and overlays excluded
// (NPS5_SCENE_TAG_SKIP: comma-separated name prefixes, default "ui_"; profile key scene_tag_skip). The renderer tags
// the pipelines it records with it and remembers the last one per cache (shader_prepare=scene). Entries of g_lvl_names
// are immutable once published, so the pointer stays valid.
static _Atomic(const char *) g_scene_current;
static char g_scene_skip[160] = "ui_";
static void scene_set_current(const char *entry) {
    if (atomic_load_explicit(&g_scene_current, memory_order_relaxed) == entry) return;
    if (nps5_scene_tag_skipped(entry, g_scene_skip)) return;
    atomic_store_explicit(&g_scene_current, entry, memory_order_release);
}
__attribute__((visibility("default"), used)) const char *nps5_vfs_current_scene(void) {
    const char *c = atomic_load_explicit(&g_scene_current, memory_order_acquire);
    return c ? c : "";
}
// Parse the key value (NULL = default) into key/klen; returns 0 when scene logging is off. Pure, for the unit test.
int ps4_vfs_scene_key_parse(const char *v, char *key, size_t cap, size_t *klen) {
  if (!v) v = "levels/";
  while (*v == ' ' || *v == '\t') v++;
  size_t n = strlen(v);
  while (n && (v[n - 1] == ' ' || v[n - 1] == '\t' || v[n - 1] == '\n')) n--;
  if (n == 0 || (n == 3 && !strncasecmp(v, "off", 3))) { key[0] = 0; *klen = 0; return 0; }
  if (n + 2 > cap) { snprintf(key, cap, "levels/"); *klen = 7; return 1; }
  memcpy(key, v, n);
  if (key[n - 1] != '/') key[n++] = '/';
  key[n] = 0;
  *klen = n;
  return 1;
}
// Re-read the environment and forget every logged name (constructor; also used by tests).
void ps4_vfs_scene_reset(void) {
  pthread_mutex_lock(&g_lvl_mu);
  atomic_store_explicit(&g_lvl_count, 0, memory_order_release);
  g_lvl_off = getenv("NPS5_NO_LEVEL_LOG") != NULL;
  g_lvl_alias = getenv("NPS5_NO_LEVEL_ALIAS") == NULL;
  if (!ps4_vfs_scene_key_parse(getenv("NPS5_SCENE_OPEN_KEY"), g_scene_key, sizeof g_scene_key, &g_scene_klen)) g_lvl_off = 1;
  {
      const char *sk = getenv("NPS5_SCENE_TAG_SKIP");
      if (sk) snprintf(g_scene_skip, sizeof g_scene_skip, "%s", sk);
  }
  pthread_mutex_unlock(&g_lvl_mu);
}
__attribute__((constructor)) static void lvl_init_clock(void) {
  clock_gettime(CLOCK_MONOTONIC, &g_lvl_t0);
  ps4_vfs_scene_reset();
}
int ps4_vfs_note_scene_path(const char *path) {
  if (!path || g_lvl_off) return 0;
  const char *p = path;
  const size_t kl = g_scene_klen;
  for (;; p++) {
    char c = *p;
    if (!c) return 0;
    if ((p == path || p[-1] == '/') && strncasecmp(p, g_scene_key, kl) == 0) break;
  }
  p += kl;
  const char *e = p;
  while (*e && *e != '/') e++;
  size_t n = (size_t)(e - p);
  if (*e != '/' || n == 0 || n >= LVL_NAME) return 0;  // a file directly under the key is not a scene directory
  char nm[LVL_NAME];
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)p[i];
    nm[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (c < 0x20 || c >= 0x7f) ? '?' : (char)c;  // printable ASCII only
  }
  nm[n] = 0;
  int cnt = atomic_load_explicit(&g_lvl_count, memory_order_acquire);
  for (int i = 0; i < cnt; i++)
      if (!strcmp(g_lvl_names[i], nm)) {
          scene_set_current(g_lvl_names[i]);
          return 0;
      }
  int added = 0;
  pthread_mutex_lock(&g_lvl_mu);
  cnt = atomic_load_explicit(&g_lvl_count, memory_order_relaxed);
  int found = 0;
  for (int i = 0; i < cnt; i++) if (!strcmp(g_lvl_names[i], nm)) { found = 1; break; }
  if (!found && cnt >= LVL_MAX) {
    static int warned;
    if (!warned) { warned = 1; fprintf(stderr, "[level-open] name table full (%d): further levels are not logged\n", LVL_MAX); }
  }
  if (!found && cnt < LVL_MAX) {
    memcpy(g_lvl_names[cnt], nm, n + 1);
    atomic_store_explicit(&g_lvl_count, cnt + 1, memory_order_release);
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    double s = (double)(t.tv_sec - g_lvl_t0.tv_sec) + (double)(t.tv_nsec - g_lvl_t0.tv_nsec) * 1e-9;
    fprintf(stderr, "[scene-open] %s t=%.1f\n", nm, s);
    if (g_lvl_alias) fprintf(stderr, "[level-open] %s t=%.1f\n", nm, s);
    lp_phase(nm, s);
    scene_set_current(g_lvl_names[cnt]);
    added = 1;
  }
  pthread_mutex_unlock(&g_lvl_mu);
  return added;
}

int ps4_vfs_note_level_path(const char *path) { return ps4_vfs_note_scene_path(path); }  // legacy name

// ---------------------------------------------------------------------------------------------
// Load accounting.
//   nps5_vfs_bytes_read / nps5_vfs_phase   exported counters a loading card can read by symbol: bytes read through the
//                                          VFS since start, and the phase (0 = boot, n = the n-th scene opened). Always
//                                          kept: two relaxed atomic adds per read.
//   NPS5_VFS_LOADPROFILE=<tsv>             phase, name, start second, bytes read, second of the first picture
//   NPS5_VFS_READLOG=<file>                per guest path: bytes read, file size, opens, reads, highest end offset, first
//                                          and last second. Diagnostic: one lock per open/read while it is on.
// Both files are rewritten every 2 s and at exit. Unset = nothing is opened, no thread, one flag test per call. The
// files hold guest paths (names of the title's files), never host paths.
#define LP_MAX 64
#define RL_MAX 131072
#define RL_FDS 16384
_Atomic unsigned long long nps5_vfs_bytes_read;
_Atomic int nps5_vfs_phase;
static _Atomic unsigned long long g_lp_bytes[LP_MAX];
static double g_lp_t0[LP_MAX];              // under g_rl_mu
static char g_lp_name[LP_MAX][LVL_NAME];    // under g_rl_mu
static double g_lp_first_pic = -1;          // under g_rl_mu
typedef struct { char path[400]; uint64_t size, opens, reads, bytes, maxend; double t_first, t_last; } RlEnt;
static RlEnt *g_rl;                         // under g_rl_mu
static int g_rl_n;                          // under g_rl_mu
static int g_rl_on;                         // set once in the constructor
static char g_rl_file[1024], g_lp_file[1024];
static _Atomic int g_rl_fd[RL_FDS];         // fd -> entry + 1
static pthread_mutex_t g_rl_mu = PTHREAD_MUTEX_INITIALIZER;

static double rl_now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)(t.tv_sec - g_lvl_t0.tv_sec) + (double)(t.tv_nsec - g_lvl_t0.tv_nsec) * 1e-9;
}
// A scene was opened for the first time: the next phase starts (called with the scene name).
static void lp_phase(const char *name, double t) {
  pthread_mutex_lock(&g_rl_mu);
  const int ph = atomic_load_explicit(&nps5_vfs_phase, memory_order_relaxed) + 1;
  if (ph < LP_MAX) {
    snprintf(g_lp_name[ph], LVL_NAME, "%s", name);
    g_lp_t0[ph] = t;
    atomic_store_explicit(&nps5_vfs_phase, ph, memory_order_release);
  }
  pthread_mutex_unlock(&g_rl_mu);
}
// The presenter calls this at the first picture.
void nps5_vfs_mark_first_picture(void) {
  pthread_mutex_lock(&g_rl_mu);
  if (g_lp_first_pic < 0) g_lp_first_pic = rl_now();
  pthread_mutex_unlock(&g_rl_mu);
}
// Writes through a temporary name: a reader never sees half a file.
static FILE *rl_tmp_open(const char *file, char *tmp, size_t cap) {
  if (!file[0] || snprintf(tmp, cap, "%s.tmp", file) >= (int)cap) return NULL;
  return fopen(tmp, "w");
}
static void lp_dump(void) {
  char tmp[1040];
  pthread_mutex_lock(&g_rl_mu);
  FILE *o = rl_tmp_open(g_lp_file, tmp, sizeof tmp);
  if (o) {
    fprintf(o, "# phase\tname\tstart_s\tbytes_read\tfirst_picture_s\n");
    const int n = atomic_load_explicit(&nps5_vfs_phase, memory_order_acquire);
    for (int i = 0; i <= n && i < LP_MAX; i++)
      fprintf(o, "%d\t%s\t%.1f\t%llu\t%.1f\n", i, i ? g_lp_name[i] : "boot", g_lp_t0[i],
              (unsigned long long)atomic_load_explicit(&g_lp_bytes[i], memory_order_relaxed), g_lp_first_pic);
    fclose(o);
    rename(tmp, g_lp_file);
  }
  pthread_mutex_unlock(&g_rl_mu);
}
static void rl_dump(void) {
  if (!g_rl_on) return;
  char tmp[1040];
  pthread_mutex_lock(&g_rl_mu);
  FILE *f = rl_tmp_open(g_rl_file, tmp, sizeof tmp);
  if (f) {
    for (int i = 0; i < g_rl_n; i++) {
      const RlEnt *e = &g_rl[i];
      fprintf(f, "%llu\t%llu\t%llu\t%llu\t%llu\t%.2f\t%.2f\t%s\n", (unsigned long long)e->bytes, (unsigned long long)e->size,
              (unsigned long long)e->opens, (unsigned long long)e->reads, (unsigned long long)e->maxend, e->t_first, e->t_last, e->path);
    }
    fclose(f);
    rename(tmp, g_rl_file);
  }
  pthread_mutex_unlock(&g_rl_mu);
}
static void rl_dump_all(void) { rl_dump(); lp_dump(); }
static void *rl_thread(void *a) { (void)a; for (;;) { sleep(2); rl_dump_all(); } return NULL; }
// A name that does not fit is refused (a cut-off name would be another file).
static int rl_take_name(char *dst, size_t cap, const char *env) {
  const char *v = getenv(env);
  if (!v || !*v) return 0;
  if (strlen(v) + 5 > cap) { fprintf(stderr, "[vfs] %s: name too long, ignored\n", env); return 0; }
  memcpy(dst, v, strlen(v) + 1);
  return 1;
}
__attribute__((constructor)) static void rl_init(void) {
  const int want_log = rl_take_name(g_rl_file, sizeof g_rl_file, "NPS5_VFS_READLOG");
  const int want_profile = rl_take_name(g_lp_file, sizeof g_lp_file, "NPS5_VFS_LOADPROFILE");
  if (want_log) {
    g_rl = calloc(RL_MAX, sizeof(RlEnt));
    if (g_rl) g_rl_on = 1;
    else g_rl_file[0] = 0;
  }
  if (!g_rl_on && !want_profile) return;
  atexit(rl_dump_all);
  pthread_t th;
  if (pthread_create(&th, NULL, rl_thread, NULL) == 0) pthread_detach(th);
}
static void rl_open(int fd, const char *guest) {
  if (!g_rl_on || fd < 0 || fd >= RL_FDS || !guest) return;
  pthread_mutex_lock(&g_rl_mu);
  int idx = -1;
  for (int i = 0; i < g_rl_n; i++) if (!strcmp(g_rl[i].path, guest)) { idx = i; break; }
  if (idx < 0 && g_rl_n < RL_MAX) {
    idx = g_rl_n++;
    snprintf(g_rl[idx].path, sizeof(g_rl[idx].path), "%s", guest);
    g_rl[idx].t_first = rl_now();
    struct stat st;
    if (fstat(fd, &st) == 0) g_rl[idx].size = (uint64_t)st.st_size;
  }
  if (idx >= 0) g_rl[idx].opens++;
  atomic_store_explicit(&g_rl_fd[fd], idx + 1, memory_order_release);
  pthread_mutex_unlock(&g_rl_mu);
}
// n bytes were read from fd at offset off (-1 = not known).
void ps4_vfs_readlog_note(int fd, long long off, long long n) {
  if (n <= 0) return;
  atomic_fetch_add_explicit(&nps5_vfs_bytes_read, (unsigned long long)n, memory_order_relaxed);
  const int ph = atomic_load_explicit(&nps5_vfs_phase, memory_order_relaxed);
  if (ph >= 0 && ph < LP_MAX) atomic_fetch_add_explicit(&g_lp_bytes[ph], (unsigned long long)n, memory_order_relaxed);
  if (!g_rl_on || fd < 0 || fd >= RL_FDS) return;
  pthread_mutex_lock(&g_rl_mu);
  const int k = atomic_load_explicit(&g_rl_fd[fd], memory_order_acquire);
  if (k > 0) {
    RlEnt *e = &g_rl[k - 1];
    e->reads++;
    e->bytes += (uint64_t)n;
    e->t_last = rl_now();
    if (off >= 0 && (uint64_t)off + (uint64_t)n > e->maxend) e->maxend = (uint64_t)off + (uint64_t)n;
  }
  pthread_mutex_unlock(&g_rl_mu);
}

static int ps4_vfs_open_impl(const char *guest_path, int host_flags, mode_t mode);
int ps4_vfs_open(const char *guest_path, int host_flags, mode_t mode) {
  const int fd = ps4_vfs_open_impl(guest_path, host_flags, mode);
  if (g_rl_on && fd >= 0 && !ps4_vfs_is_virtual_fd(fd)) {
    const int e = errno;
    rl_open(fd, guest_path);
    errno = e;
  }
  return fd;
}
static int ps4_vfs_open_impl(const char *guest_path, int host_flags, mode_t mode) {
  if (!guest_path) {
    errno = EFAULT;
    return -1;
  }
  ps4_vfs_note_level_path(guest_path);

  char resolved[1024];
  // Anything that can create, write or truncate goes through the central write gate (ps4_vfs_resolve_for_write).
  if ((host_flags & O_ACCMODE) != O_RDONLY || (host_flags & (O_CREAT | O_TRUNC | O_APPEND))) {
    int wr = ps4_vfs_resolve_for_write(guest_path, resolved, sizeof(resolved));
    if (wr < 0) {
      errno = -wr;
      return -1;
    }
    return vfs_open_checked(resolved, host_flags, mode ? mode : 0666, (host_flags & O_TRUNC) != 0);
  }

  if (ps4_vfs_resolve(guest_path, resolved, sizeof(resolved)) == 0) {
    if (access(resolved, F_OK) == 0 || (host_flags & O_CREAT)) {
      int hfd = open(resolved, host_flags, mode ? mode : 0666);
      if (getenv("NPS5_VFS_TRACE_INI")) { size_t n = strlen(guest_path); if (n > 4 && !strcasecmp(guest_path + n - 4, ".ini")) fprintf(stderr, "[ps4-vfs] ini open: %s -> fd %d\n", guest_path, hfd); }
      return hfd;
    }
  }

  // Physical disk locale fallback for .res resource bundles (NPS5_PAK_LOCALE_FALLBACK=1 only, like the pak one)
  char cur_guest[1024];
  strncpy(cur_guest, guest_path, sizeof(cur_guest) - 1);
  cur_guest[sizeof(cur_guest) - 1] = '\0';
  char parent_guest[1024];
  while (ps4_pak_locale_fallback_enabled() &&
         ps4_pak_get_parent_locale_path(cur_guest, parent_guest,
                                        sizeof(parent_guest))) {
    if (ps4_vfs_resolve(parent_guest, resolved, sizeof(resolved)) == 0) {
      if (access(resolved, F_OK) == 0) {
        return open(resolved, host_flags, mode);
      }
    }
    strncpy(cur_guest, parent_guest, sizeof(cur_guest) - 1);
    cur_guest[sizeof(cur_guest) - 1] = '\0';
  }

  // Path not on physical disk. Check universal PAK archive provider
  const char *subpath = get_app0_subpath(guest_path);
  if (subpath) {
    ps4_pak_entry_t entry;
    if (ps4_pak_lookup(subpath, &entry)) {
      // PAK archives are strictly read-only
      if ((host_flags & O_ACCMODE) != O_RDONLY) {
        errno = EROFS;
        return -1;
      }

      pthread_mutex_lock(&g_vfile_mutex);
      int slot = -1;
      for (int i = 0; i < MAX_VIRTUAL_FILES; i++) {
        if (!g_vfiles[i].active) {
          slot = i;
          break;
        }
      }

      if (slot < 0) {
        pthread_mutex_unlock(&g_vfile_mutex);
        errno = ENFILE;
        return -1;
      }

      g_vfiles[slot].active = 1;
      g_vfiles[slot].pak_fd = entry.pak_fd;
      g_vfiles[slot].data_offset = entry.data_offset;
      g_vfiles[slot].file_size = entry.size;
      g_vfiles[slot].cur_offset = 0;
      strncpy(g_vfiles[slot].path, guest_path, sizeof(g_vfiles[slot].path) - 1);
      pthread_mutex_unlock(&g_vfile_mutex);

      int vfd = PS4_VFS_VIRTUAL_FD_BASE + slot;
      PS4_LOGF(NPS5_S_KERNEL, NPS5_L_DEBUG, "ps4-vfs", "Virtual open: '%s' -> vfd=%d (pak_offset=0x%llx, "
             "size=%llu)\n",
             guest_path, vfd, (unsigned long long)entry.data_offset,
             (unsigned long long)entry.size);
      return vfd;
    }
  }

  errno = ENOENT;
  return -1;
}

int ps4_vfs_close(int fd) {
  if (ps4_vfs_is_virtual_fd(fd)) {
    int slot = fd - PS4_VFS_VIRTUAL_FD_BASE;
    pthread_mutex_lock(&g_vfile_mutex);
    if (slot >= 0 && slot < MAX_VIRTUAL_FILES && g_vfiles[slot].active) {
      g_vfiles[slot].active = 0;
      pthread_mutex_unlock(&g_vfile_mutex);
      PS4_LOGF(NPS5_S_KERNEL, NPS5_L_DEBUG, "ps4-vfs", "Virtual close: vfd=%d\n", fd);
      return 0;
    }
    pthread_mutex_unlock(&g_vfile_mutex);
    errno = EBADF;
    return -1;
  }
  if (g_rl_on && fd >= 0 && fd < RL_FDS) atomic_store_explicit(&g_rl_fd[fd], 0, memory_order_release);
  return close(fd);
}

// Reads into guest memory must never come back short before EOF. The kernel's copy cannot go through the write watch:
// a page that a host library closes again between nps5_host_write_begin and the copy is EFAULT, and
// nps5_host_write_refault opens it again up to NPS5_KIO_RETRIES times. A long read (a 160 MB model file) into pages
// that the watch keeps closing can run out of retries. Before this, the read then returned the bytes copied so far as
// a successful short read, and the game's file layer accepts a short read as success: the tail of the buffer kept
// stale memory and the model parser walked it (wild chunk pointer, NULL index-buffer and name pointers). Now the rest
// is read into a host buffer and copied with ordinary user-mode stores, which fault into the watch and are resolved
// there like any guest store. NPS5_KIO_BOUNCE=0 = old behaviour (for A/B only).
static _Atomic uint64_t g_kio_bounces, g_kio_bounce_bytes, g_kio_short;
static int kio_bounce_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("NPS5_KIO_BOUNCE");
        on = !(e && *e == '0');
    }
    return on;
}
static void kio_report_exit(void) {
    const uint64_t b = atomic_load(&g_kio_bounces), s = atomic_load(&g_kio_short);
    if (b || s)
        fprintf(stderr,
                "[kio] exit: %llu reads finished through a host buffer after the refaults ran out (%llu bytes), %llu "
                "short reads before EOF\n",
                (unsigned long long)b, (unsigned long long)atomic_load(&g_kio_bounce_bytes), (unsigned long long)s);
}
static void kio_exit_hook_once(void) {
    static atomic_flag done = ATOMIC_FLAG_INIT;
    if (!atomic_flag_test_and_set(&done)) atexit(kio_report_exit);
}
// A read that returns fewer bytes than asked although the file has more (an error after a partial copy).
static void vfs_note_short(int fd, size_t got, size_t want, int err) {
    kio_exit_hook_once();
    const uint64_t n = atomic_fetch_add(&g_kio_short, 1) + 1;
    if (n <= 20 || n % 100 == 0)
        fprintf(stderr, "[kio] SHORT READ fd %d: 0x%zx of 0x%zx bytes before EOF (errno %d, %llu so far)\n", fd, got,
                want, err, (unsigned long long)n);
}
// The rest of a read whose kernel copy kept failing with EFAULT: pread into a host buffer, then user-mode stores.
// Returns the bytes stored, or -1 (errno set) when nothing could be read.
static ssize_t vfs_bounce_pread(int hfd, int fd, char *dst, size_t n, off_t off) {
    kio_exit_hook_once();
    const size_t chunk = (size_t)4 << 20;
    char *tmp = malloc(n < chunk ? (n ? n : 1) : chunk);
    if (!tmp) {
        errno = ENOMEM;
        return -1;
    }
    size_t got = 0;
    int err = 0;
    while (got < n) {
        const size_t want = n - got < chunk ? n - got : chunk;
        const ssize_t r = pread(hfd, tmp, want, off + (off_t)got);
        if (r < 0) {
            if (errno == EINTR) continue;
            err = errno;
            break;
        }
        if (r == 0) break;
        memcpy(dst + got, tmp, (size_t)r); // user-mode stores: a closed page faults into the watch and is opened there
        got += (size_t)r;
    }
    free(tmp);
    const uint64_t k = atomic_fetch_add(&g_kio_bounces, 1) + 1;
    atomic_fetch_add(&g_kio_bounce_bytes, got);
    if (k <= 20 || k % 100 == 0)
        fprintf(stderr,
                "[kio] read fd %d: kernel copy into guest memory still EFAULT after %u refaults, 0x%zx bytes left at "
                "file offset 0x%llx: "
                "read through a host buffer, 0x%zx bytes (%llu so far)\n",
                fd, NPS5_KIO_RETRIES, n, (unsigned long long)off, got, (unsigned long long)k);
    if (got == 0 && err) {
        errno = err;
        return -1;
    }
    return (ssize_t)got;
}

static ssize_t vfs_read_inner(int fd, void *buf, size_t count);
ssize_t ps4_vfs_read(int fd, void *buf, size_t count) {
  nps5_host_write_begin(buf, count);
  const off_t at = g_rl_on ? lseek(fd, 0, SEEK_CUR) : -1;
  const ssize_t r = vfs_read_inner(fd, buf, count);
  const int e = errno;
  ps4_vfs_readlog_note(fd, (long long)at, (long long)r);
  nps5_host_write_end(buf, count);
  errno = e;
  return r;
}

static ssize_t vfs_read_inner(int fd, void *buf, size_t count) {
  unsigned refault = 0; // nps5_mem.h nps5_host_write_refault: EFAULT on a destination that was opened for the kernel
  if (ps4_vfs_is_virtual_fd(fd)) {
    int slot = fd - PS4_VFS_VIRTUAL_FD_BASE;
    pthread_mutex_lock(&g_vfile_mutex);
    if (slot < 0 || slot >= MAX_VIRTUAL_FILES || !g_vfiles[slot].active) {
      pthread_mutex_unlock(&g_vfile_mutex);
      errno = EBADF;
      return -1;
    }
    VirtualPakFile *vf = &g_vfiles[slot];
    if ((uint64_t)vf->cur_offset >= vf->file_size) {
      pthread_mutex_unlock(&g_vfile_mutex);
      return 0; // EOF
    }

    size_t to_read = count;
    if ((uint64_t)vf->cur_offset + to_read > vf->file_size) {
      to_read = (size_t)(vf->file_size - (uint64_t)vf->cur_offset);
    }

    size_t total = 0;
    char *p = (char *)buf;
    while (total < to_read) {
      ssize_t ret = pread(vf->pak_fd, p + total, to_read - total,
                          (off_t)(vf->data_offset + (uint64_t)vf->cur_offset + total));
      if (ret < 0) {
        if (errno == EINTR)
          continue;
        if (errno == EFAULT && nps5_host_write_refault(p + total, to_read - total, &refault))
          continue; // a page of the destination was closed again before the kernel copied: opened again, repeat
        if (errno == EFAULT && refault >= NPS5_KIO_RETRIES && kio_bounce_on()) {
            const ssize_t b = vfs_bounce_pread(vf->pak_fd, fd, p + total, to_read - total,
                                               (off_t)(vf->data_offset + (uint64_t)vf->cur_offset + total));
            if (b > 0) {
                total += (size_t)b;
                continue;
            }
        }
        if (total > 0) {
            vfs_note_short(fd, total, to_read, errno);
            break;
        }
        pthread_mutex_unlock(&g_vfile_mutex);
        return -1;
      }
      if (ret == 0)
        break;
      total += (size_t)ret;
    }
    vf->cur_offset += (off_t)total;
    pthread_mutex_unlock(&g_vfile_mutex);
    return (ssize_t)total;
  }

  struct stat st;
  if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
    size_t total = 0;
    char *p = (char *)buf;
    const off_t pos = nps5_host_access_active() ? lseek(fd, 0, SEEK_CUR) : (off_t)-1;
    while (total < count) {
      ssize_t n = read(fd, p + total, count - total);
      if (n < 0) {
        if (errno == EINTR)
          continue;
        if (errno == EFAULT && nps5_host_write_refault(p + total, count - total, &refault)) {
          if (pos >= 0) lseek(fd, pos + (off_t)total, SEEK_SET); // the failed call may have moved the position
          continue;
        }
        if (errno == EFAULT && refault >= NPS5_KIO_RETRIES && pos >= 0 && kio_bounce_on()) {
            const ssize_t b = vfs_bounce_pread(fd, fd, p + total, count - total, pos + (off_t)total);
            if (b > 0) {
                total += (size_t)b;
                lseek(fd, pos + (off_t)total,
                      SEEK_SET); // pread does not move the position: put it after the bytes read
                continue;
            }
        }
        if (total > 0) {
            vfs_note_short(fd, total, count, errno);
            return (ssize_t)total;
        }
        return -1;
      }
      if (n == 0)
        break;
      total += (size_t)n;
    }
    return (ssize_t)total;
  }

  return read(fd, buf, count);
}

static ssize_t vfs_pread_inner(int fd, void *buf, size_t count, off_t offset);
// The kernel fills buf: the pages must be open for it (nps5_mem.h host access hooks; EFAULT otherwise).
ssize_t ps4_vfs_pread(int fd, void *buf, size_t count, off_t offset) {
  nps5_host_write_begin(buf, count);
  const ssize_t r = vfs_pread_inner(fd, buf, count, offset);
  const int e = errno;
  ps4_vfs_readlog_note(fd, (long long)offset, (long long)r);
  nps5_host_write_end(buf, count);
  errno = e;
  return r;
}

static ssize_t vfs_pread_inner(int fd, void *buf, size_t count, off_t offset) {
  // offset + count must be representable: no signed overflow below, whatever the guest passes
  if (offset < 0) {
    errno = EINVAL;
    return -1;
  }
  // a count that would carry past the largest offset is cut there (a "read to the end" with a huge count still works)
  if ((uint64_t)count > (uint64_t)INT64_MAX - (uint64_t)offset) count = (size_t)((uint64_t)INT64_MAX - (uint64_t)offset);
  unsigned refault = 0;
  if (ps4_vfs_is_virtual_fd(fd)) {
    int slot = fd - PS4_VFS_VIRTUAL_FD_BASE;
    pthread_mutex_lock(&g_vfile_mutex);
    if (slot < 0 || slot >= MAX_VIRTUAL_FILES || !g_vfiles[slot].active) {
      pthread_mutex_unlock(&g_vfile_mutex);
      errno = EBADF;
      return -1;
    }
    VirtualPakFile *vf = &g_vfiles[slot];
    if (offset < 0 || (uint64_t)offset >= vf->file_size) {
      pthread_mutex_unlock(&g_vfile_mutex);
      return 0;
    }

    size_t to_read = count;
    if ((uint64_t)offset + to_read > vf->file_size) {
      to_read = (size_t)(vf->file_size - (uint64_t)offset);
    }

    size_t total = 0;
    char *p = (char *)buf;
    while (total < to_read) {
      ssize_t ret = pread(vf->pak_fd, p + total, to_read - total,
                          (off_t)(vf->data_offset + (uint64_t)offset + total));
      if (ret < 0) {
        if (errno == EINTR)
          continue;
        if (errno == EFAULT && nps5_host_write_refault(p + total, to_read - total, &refault))
          continue;
        if (errno == EFAULT && refault >= NPS5_KIO_RETRIES && kio_bounce_on()) {
            const ssize_t b = vfs_bounce_pread(vf->pak_fd, fd, p + total, to_read - total,
                                               (off_t)(vf->data_offset + (uint64_t)offset + total));
            if (b > 0) {
                total += (size_t)b;
                continue;
            }
        }
        if (total > 0) {
            vfs_note_short(fd, total, to_read, errno);
            break;
        }
        pthread_mutex_unlock(&g_vfile_mutex);
        return -1;
      }
      if (ret == 0)
        break;
      total += (size_t)ret;
    }
    pthread_mutex_unlock(&g_vfile_mutex);
    return (ssize_t)total;
  }

  struct stat st;
  if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
    size_t total = 0;
    char *p = (char *)buf;
    while (total < count) {
      ssize_t n = pread(fd, p + total, count - total, offset + (off_t)total);
      if (n < 0) {
        if (errno == EINTR)
          continue;
        if (errno == EFAULT && nps5_host_write_refault(p + total, count - total, &refault))
          continue;
        if (errno == EFAULT && refault >= NPS5_KIO_RETRIES && kio_bounce_on()) {
            const ssize_t b = vfs_bounce_pread(fd, fd, p + total, count - total, offset + (off_t)total);
            if (b > 0) {
                total += (size_t)b;
                continue;
            }
        }
        if (total > 0) {
            vfs_note_short(fd, total, count, errno);
            return (ssize_t)total;
        }
        return -1;
      }
      if (n == 0)
        break;
      total += (size_t)n;
    }
    return (ssize_t)total;
  }

  return pread(fd, buf, count, offset);
}

off_t ps4_vfs_lseek(int fd, off_t offset, int whence) {
  if (ps4_vfs_is_virtual_fd(fd)) {
    int slot = fd - PS4_VFS_VIRTUAL_FD_BASE;
    pthread_mutex_lock(&g_vfile_mutex);
    if (slot < 0 || slot >= MAX_VIRTUAL_FILES || !g_vfiles[slot].active) {
      pthread_mutex_unlock(&g_vfile_mutex);
      errno = EBADF;
      return -1;
    }
    VirtualPakFile *vf = &g_vfiles[slot];
    off_t new_off = vf->cur_offset;
    bool ovf = false;
    if (whence == SEEK_SET) {
      new_off = offset;
    } else if (whence == SEEK_CUR) {
      ovf = __builtin_add_overflow(new_off, offset, &new_off);
    } else if (whence == SEEK_END) {
      ovf = vf->file_size > (uint64_t)INT64_MAX || __builtin_add_overflow((off_t)vf->file_size, offset, &new_off);
    } else {
      pthread_mutex_unlock(&g_vfile_mutex);
      errno = EINVAL;
      return -1;
    }

    if (ovf || new_off < 0) {
      pthread_mutex_unlock(&g_vfile_mutex);
      errno = EINVAL;
      return -1;
    }

    vf->cur_offset = new_off;
    PS4_LOGF(NPS5_S_KERNEL, NPS5_L_DEBUG, "ps4-vfs", "Virtual lseek: vfd=%d offset=%lld whence=%d -> "
           "new_off=%lld\n",
           fd, (long long)offset, whence, (long long)new_off);
    pthread_mutex_unlock(&g_vfile_mutex);
    return new_off;
  }
  return lseek(fd, offset, whence);
}

int ps4_vfs_fstat(int fd, struct stat *st) {
  if (!st) {
    errno = EFAULT;
    return -1;
  }
  if (ps4_vfs_is_virtual_fd(fd)) {
    int slot = fd - PS4_VFS_VIRTUAL_FD_BASE;
    pthread_mutex_lock(&g_vfile_mutex);
    if (slot < 0 || slot >= MAX_VIRTUAL_FILES || !g_vfiles[slot].active) {
      pthread_mutex_unlock(&g_vfile_mutex);
      errno = EBADF;
      return -1;
    }
    VirtualPakFile *vf = &g_vfiles[slot];
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0644;
    st->st_nlink = 1;
    st->st_size = (off_t)vf->file_size;
    st->st_blksize = 4096;
    st->st_blocks = (off_t)((vf->file_size + 511) / 512);
    PS4_LOGF(NPS5_S_KERNEL, NPS5_L_DEBUG, "ps4-vfs", "Virtual fstat: vfd=%d size=%llu\n", fd,
           (unsigned long long)vf->file_size);
    pthread_mutex_unlock(&g_vfile_mutex);
    return 0;
  }
  return fstat(fd, st);
}

int ps4_vfs_stat(const char *guest_path, struct stat *st) {
  if (!guest_path || !st) {
    errno = EFAULT;
    return -1;
  }
  char resolved[1024];
  if (ps4_vfs_resolve(guest_path, resolved, sizeof(resolved)) == 0) {
    if (stat(resolved, st) == 0) {
      return 0;
    }
  }

  // Physical disk locale fallback for .res resource bundles (NPS5_PAK_LOCALE_FALLBACK=1 only, like the pak one)
  char cur_guest[1024];
  strncpy(cur_guest, guest_path, sizeof(cur_guest) - 1);
  cur_guest[sizeof(cur_guest) - 1] = '\0';
  char parent_guest[1024];
  while (ps4_pak_locale_fallback_enabled() &&
         ps4_pak_get_parent_locale_path(cur_guest, parent_guest,
                                        sizeof(parent_guest))) {
    if (ps4_vfs_resolve(parent_guest, resolved, sizeof(resolved)) == 0) {
      if (stat(resolved, st) == 0) {
        return 0;
      }
    }
    strncpy(cur_guest, parent_guest, sizeof(cur_guest) - 1);
    cur_guest[sizeof(cur_guest) - 1] = '\0';
  }

  const char *subpath = get_app0_subpath(guest_path);
  if (subpath) {
    ps4_pak_entry_t entry;
    if (ps4_pak_lookup(subpath, &entry)) {
      memset(st, 0, sizeof(*st));
      st->st_mode = S_IFREG | 0644;
      st->st_nlink = 1;
      st->st_size = (off_t)entry.size;
      st->st_blksize = 4096;
      st->st_blocks = (off_t)((entry.size + 511) / 512);
      return 0;
    }
    if (ps4_pak_dir_exists(subpath)) {
      memset(st, 0, sizeof(*st));
      st->st_mode = S_IFDIR | 0755;
      st->st_nlink = 2;
      st->st_size = 4096;
      st->st_blksize = 4096;
      return 0;
    }
  }

  errno = ENOENT;
  return -1;
}

int ps4_vfs_lstat(const char *guest_path, struct stat *st) {
  return ps4_vfs_stat(guest_path, st);
}

int ps4_vfs_access(const char *guest_path, int mode) {
  if (!guest_path) {
    errno = EFAULT;
    return -1;
  }
  char resolved[1024];
  if (ps4_vfs_resolve(guest_path, resolved, sizeof(resolved)) == 0) {
    if (access(resolved, mode) == 0) {
      return 0;
    }
  }

  // Physical disk locale fallback for .res resource bundles (NPS5_PAK_LOCALE_FALLBACK=1 only, like the pak one)
  char cur_guest[1024];
  strncpy(cur_guest, guest_path, sizeof(cur_guest) - 1);
  cur_guest[sizeof(cur_guest) - 1] = '\0';
  char parent_guest[1024];
  while (ps4_pak_locale_fallback_enabled() &&
         ps4_pak_get_parent_locale_path(cur_guest, parent_guest,
                                        sizeof(parent_guest))) {
    if (ps4_vfs_resolve(parent_guest, resolved, sizeof(resolved)) == 0) {
      if (access(resolved, mode) == 0) {
        return 0;
      }
    }
    strncpy(cur_guest, parent_guest, sizeof(cur_guest) - 1);
    cur_guest[sizeof(cur_guest) - 1] = '\0';
  }

  const char *subpath = get_app0_subpath(guest_path);
  if (subpath) {
    if (ps4_pak_lookup(subpath, NULL) || ps4_pak_dir_exists(subpath)) {
      if (mode & W_OK) {
        errno = EACCES;
        return -1;
      }
      return 0;
    }
  }

  errno = ENOENT;
  return -1;
}

void *ps4_vfs_mmap(void *addr, size_t len, int prot, int flags, int fd,
                   off_t offset) {
  if (ps4_vfs_is_virtual_fd(fd)) {
    int slot = fd - PS4_VFS_VIRTUAL_FD_BASE;
    pthread_mutex_lock(&g_vfile_mutex);
    if (slot < 0 || slot >= MAX_VIRTUAL_FILES || !g_vfiles[slot].active) {
      pthread_mutex_unlock(&g_vfile_mutex);
      errno = EBADF;
      return MAP_FAILED;
    }
    VirtualPakFile *vf = &g_vfiles[slot];
    void *mem =
        mmap(addr, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) {
      pthread_mutex_unlock(&g_vfile_mutex);
      return MAP_FAILED;
    }
    if (offset >= 0 && (uint64_t)offset < vf->file_size) {
      size_t to_read = len;
      if ((uint64_t)offset + to_read > vf->file_size) {
        to_read = (size_t)(vf->file_size - (uint64_t)offset);
      }
      pread(vf->pak_fd, mem, to_read,
            (off_t)(vf->data_offset + (uint64_t)offset));
    }
    pthread_mutex_unlock(&g_vfile_mutex);
    mprotect(mem, len, prot);
    return mem;
  }
  return mmap(addr, len, prot, flags, fd, offset);
}

void ps4_vfs_destroy(void) {
  pthread_mutex_lock(&g_vfs_mutex);
  if (g_app_root) {
    free(g_app_root);
    g_app_root = NULL;
  }
  vfs_mounts_clear_locked();
  free(g_app_root_real);
  g_app_root_real = NULL;

  pthread_mutex_lock(&g_vfile_mutex);
  memset(g_vfiles, 0, sizeof(g_vfiles));
  pthread_mutex_unlock(&g_vfile_mutex);

  pthread_mutex_unlock(&g_vfs_mutex);

  ps4_pak_destroy();
}

// dirfix (podtrace8): the guest's directory readers keep only DT_REG (8) / DT_DIR (4) entries. A host symlink farm (boot/app0_fast*) comes
// back from readdir as DT_LNK (10), and some file systems report DT_UNKNOWN (0). Resolve both by following the link (fstatat, not lstat) so
// the guest sees the TARGET's type, like stat/sceKernelStat already do. Returns the type to report, or 0xFF = hide the entry (dangling
// link). A link to a device/socket/fifo reports DT_UNKNOWN (never DT_LNK). Results are cached per directory (dev, ino, mtime) so a level
// folder that the game lists many times costs one stat per entry once. *resolved (optional) is set when the entry needed resolving.
//
// Cache rules (review-recomp 2026-11-19):
//  - lookups go through a per-directory hash index: farm folders hold up to ~46k entries and a linear scan made one listing O(n^2)
//    (about 3 s per listing of a 46k folder, under the getdents lock);
//  - only successful stats are cached. A failed one (dangling link, EACCES, ELOOP) is retried next time: the link's target can appear
//    later without the link's own directory changing, so a cached miss would hide the entry for the rest of the run;
//  - a directory whose mtime is within DIRTYPE_RACY_SEC of now is not cached (file systems with 1-2 s mtime granularity could
//    change it again without a visible mtime change);
//  - an entry is added only if the slot still describes the same directory version (dev, ino, mtime) after the unlocked stat.
#define DIRTYPE_CACHE_DIRS 32
#define DIRTYPE_RACY_SEC 2
typedef struct {
    dev_t dev;
    ino_t ino;
    struct timespec mt;
    int live;
    int n, cap;
    char **names;
    uint8_t *types;
    int32_t *idx; // open-addressing hash: entry index + 1, 0 = empty; idxcap = 2 * cap (power of two)
    unsigned long stamp;
} DirTypeCache;
static DirTypeCache g_dtc[DIRTYPE_CACHE_DIRS];
static unsigned long g_dtc_clock;
static pthread_mutex_t g_dtc_lock = PTHREAD_MUTEX_INITIALIZER;

static void dtc_clear(DirTypeCache *c) {
    for (int i = 0; i < c->n; i++)
        free(c->names[i]);
    free(c->names);
    free(c->types);
    free(c->idx);
    memset(c, 0, sizeof(*c));
}

static uint32_t dtc_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static int dtc_find(const DirTypeCache *c, const char *name, uint32_t h) {
    if (!c->idx) return -1;
    uint32_t mask = (uint32_t)c->cap * 2 - 1;
    for (uint32_t k = h & mask;; k = (k + 1) & mask) {
        int32_t v = c->idx[k];
        if (v == 0) return -1;
        if (strcmp(c->names[v - 1], name) == 0) return v - 1;
    }
}

static int dtc_same(const DirTypeCache *c, const struct stat *ds) {
    return c->live && c->dev == ds->st_dev && c->ino == ds->st_ino && c->mt.tv_sec == ds->st_mtimespec.tv_sec &&
           c->mt.tv_nsec == ds->st_mtimespec.tv_nsec;
}

// Adds name -> t; on allocation failure the entry is simply not cached.
static void dtc_add(DirTypeCache *c, const char *name, uint32_t h, uint8_t t) {
    if (dtc_find(c, name, h) >= 0) return; // another thread resolved it meanwhile
    if (c->n == c->cap) {
        int nc = c->cap ? c->cap * 2 : 64;
        char **nn = (char **)realloc(c->names, nc * sizeof(char *));
        if (!nn) return;
        c->names = nn;
        uint8_t *nt = (uint8_t *)realloc(c->types, nc);
        if (!nt) return;
        c->types = nt;
        int32_t *ni = (int32_t *)calloc((size_t)nc * 2, sizeof(int32_t));
        if (!ni) return;
        free(c->idx);
        c->idx = ni;
        c->cap = nc;
        uint32_t mask = (uint32_t)nc * 2 - 1;
        for (int i = 0; i < c->n; i++) { // rehash
            uint32_t k = dtc_hash(c->names[i]) & mask;
            while (c->idx[k])
                k = (k + 1) & mask;
            c->idx[k] = i + 1;
        }
    }
    char *dup = strdup(name);
    if (!dup) return;
    uint32_t mask = (uint32_t)c->cap * 2 - 1;
    uint32_t k = h & mask;
    while (c->idx[k])
        k = (k + 1) & mask;
    c->names[c->n] = dup;
    c->types[c->n] = t;
    c->idx[k] = ++c->n;
}

uint8_t ps4_vfs_resolve_dirent_type(int dirfd_, const char *name, uint8_t d_type, int *resolved) {
    if (d_type != DT_LNK && d_type != DT_UNKNOWN) return d_type;
    if (resolved) *resolved = 1;
    struct stat ds;
    DirTypeCache *c = NULL;
    const uint32_t h = dtc_hash(name);
    int cacheable = fstat(dirfd_, &ds) == 0 && ds.st_mtimespec.tv_sec < time(NULL) - DIRTYPE_RACY_SEC;
    if (cacheable) {
        pthread_mutex_lock(&g_dtc_lock);
        DirTypeCache *victim = &g_dtc[0];
        for (int i = 0; i < DIRTYPE_CACHE_DIRS; i++) {
            DirTypeCache *e = &g_dtc[i];
            if (e->live && e->dev == ds.st_dev && e->ino == ds.st_ino) {
                if (dtc_same(e, &ds)) {
                    c = e;
                    break;
                }
                dtc_clear(e);
                victim = e;
                break; // directory changed: start over
            }
            if (e->stamp < victim->stamp) victim = e;
        }
        if (!c) {
            dtc_clear(victim);
            victim->dev = ds.st_dev;
            victim->ino = ds.st_ino;
            victim->mt = ds.st_mtimespec;
            victim->live = 1;
            c = victim;
        }
        c->stamp = ++g_dtc_clock;
        int i = dtc_find(c, name, h);
        if (i >= 0) {
            uint8_t t = c->types[i];
            pthread_mutex_unlock(&g_dtc_lock);
            return t;
        }
        pthread_mutex_unlock(&g_dtc_lock);
    }
    struct stat ts;
    if (fstatat(dirfd_, name, &ts, 0) != 0)
        return d_type == DT_LNK ? 0xFF
                                : d_type; // dangling link: hide; DT_UNKNOWN that cannot be stat'ed: keep (never cached)
    uint8_t t = S_ISDIR(ts.st_mode) ? DT_DIR : S_ISREG(ts.st_mode) ? DT_REG : DT_UNKNOWN;
    if (c) {
        pthread_mutex_lock(&g_dtc_lock);
        if (dtc_same(c, &ds)) dtc_add(c, name, h, t); // slot not recycled / directory not changed meanwhile
        pthread_mutex_unlock(&g_dtc_lock);
    }
    return t;
}
