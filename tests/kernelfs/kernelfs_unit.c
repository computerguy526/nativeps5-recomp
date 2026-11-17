// Host unit test: (1) the sceKernel* file family returns SCE error codes (0x80020000 | errno) while the
// POSIX shims keep -1/errno; (2) the pak overlay does not invent en.res for a missing en_US.res;
// (3) PS5 sce_sys/param.json feeds sceAppContent / the SFO accessors. Fake GuestContext, shims called
// directly, everything under a temp dir. Built by tools/kernelfs_unit.sh. No guest code runs.
#include "recomp_runtime.h"
#include "nps5_diag.h"
#include "ps4_vfs.h"
#include "ps4_pak.h"
#include "ps4_sfo.h"
#include "ps4_app_content.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

uint8_t g_nps5_level[NPS5_S_COUNT];
void nps5_log(int s, int l, const char *fmt, ...) { (void)s; (void)l; (void)fmt; }
void nps5_log_compat(int s, int l, const char *tag, const char *fmt, ...) { (void)s; (void)l; (void)tag; (void)fmt; }

#define ARENA (16ULL << 20)
static GuestContext *C;
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define H(a) ((uint8_t *)C->mem_base + (a))
#define CALL(fn) do { uint64_t rsp0 = C->rsp; fn(C); assert(C->rsp == rsp0 + 8); C->rsp = rsp0; } while (0)
#define RAX32 ((uint32_t)C->rax)
static uint64_t bump = 0x100000;
static uint64_t galloc(size_t n) { uint64_t r = bump; bump += (n + 63) & ~63ULL; return r; }
static uint64_t gstr(const char *s) { uint64_t r = galloc(strlen(s) + 1); strcpy((char *)H(r), s); return r; }
static void reset(void) { C->rdi = C->rsi = C->rdx = C->rcx = C->r8 = C->r9 = 0; C->rax = 0; *(int *)H(C->fs_base + 0x100) = 0; }
static int guest_errno(void) { return *(int *)H(C->fs_base + 0x100); }

#define SHIM(n) void shim_##n(GuestContext *)
SHIM(sceKernelMkdir); SHIM(sceKernelUnlink); SHIM(sceKernelRename); SHIM(sceKernelRmdir); SHIM(sceKernelTruncate);
SHIM(sceKernelStat); SHIM(sceKernelFstat); SHIM(sceKernelOpen); SHIM(sceKernelClose); SHIM(sceKernelChmod);
SHIM(sceKernelUtimes); SHIM(sceKernelGetdents); SHIM(sceKernelFsync); SHIM(sceKernelFtruncate);
SHIM(mkdir); SHIM(unlink); SHIM(rename); SHIM(rmdir); SHIM(truncate); SHIM(open); SHIM(stat); SHIM(chmod);

static void call2(void (*fn)(GuestContext *), uint64_t a, uint64_t b) {
  reset(); C->rdi = a; C->rsi = b; CALL(fn);
}

static void test_sce_codes(const char *tmp) {
  char d[700];
  snprintf(d, sizeof d, "%s.dt", tmp); mkdir(d, 0755); // a mount directory must not lie inside the application root (the write gate)
  ps4_vfs_mount("/dt", d);
  uint64_t missing = gstr("/dt/nope/sub"), nofile = gstr("/dt/nofile"), dir = gstr("/dt/dir");
  uint64_t f1 = gstr("/dt/f1"), f2 = gstr("/dt/f2");

  // success paths
  call2(shim_sceKernelMkdir, dir, 0755); CHECK(RAX32 == 0);
  // EEXIST
  call2(shim_sceKernelMkdir, dir, 0755); CHECK(RAX32 == 0x80020011u);
  call2(shim_mkdir, dir, 0755); CHECK((int64_t)C->rax == -1 && guest_errno() == EEXIST);
  // ENOENT on a missing parent
  call2(shim_sceKernelMkdir, missing, 0755); CHECK(RAX32 == 0x80020002u);
  call2(shim_mkdir, missing, 0755); CHECK((int64_t)C->rax == -1 && guest_errno() == ENOENT);
  // unlink / rmdir / rename / truncate / stat / open of something missing
  call2(shim_sceKernelUnlink, nofile, 0); CHECK(RAX32 == 0x80020002u);
  call2(shim_unlink, nofile, 0); CHECK((int64_t)C->rax == -1 && guest_errno() == ENOENT);
  call2(shim_sceKernelRmdir, nofile, 0); CHECK(RAX32 == 0x80020002u);
  call2(shim_rmdir, nofile, 0); CHECK((int64_t)C->rax == -1);
  call2(shim_sceKernelRename, nofile, f2); CHECK(RAX32 == 0x80020002u);
  call2(shim_rename, nofile, f2); CHECK((int64_t)C->rax == -1 && guest_errno() == ENOENT);
  call2(shim_sceKernelTruncate, nofile, 0); CHECK(RAX32 == 0x80020002u);
  call2(shim_truncate, nofile, 0); CHECK((int64_t)C->rax == -1);
  call2(shim_sceKernelChmod, nofile, 0644); CHECK(RAX32 == 0x80020002u);
  uint64_t st = galloc(256);
  call2(shim_sceKernelStat, nofile, st); CHECK(RAX32 == 0x80020002u);
  call2(shim_stat, nofile, st); CHECK((int64_t)C->rax == -1 && guest_errno() == ENOENT);
  reset(); C->rdi = nofile; C->rsi = O_RDONLY; CALL(shim_sceKernelOpen); CHECK(RAX32 == 0x80020002u);
  reset(); C->rdi = nofile; C->rsi = O_RDONLY; CALL(shim_open); CHECK((int64_t)C->rax == -1 && guest_errno() == ENOENT);
  call2(shim_sceKernelFstat, 999, st); CHECK(RAX32 == 0x80020009u);   // EBADF
  call2(shim_sceKernelFsync, 999, 0); CHECK(RAX32 == 0x80020009u);
  call2(shim_sceKernelFtruncate, 999, 0); CHECK(RAX32 == 0x80020009u);
  call2(shim_sceKernelGetdents, 999, 0); CHECK((int64_t)(int32_t)RAX32 < 0 && (RAX32 & 0xFFFF0000u) == 0x80020000u);
  call2(shim_sceKernelUtimes, nofile, 0); CHECK(RAX32 == 0x80020002u);

  // a real create / rename / unlink / rmdir round trip returns 0
  reset(); C->rdi = f1; C->rsi = O_WRONLY | O_CREAT; C->rdx = 0644; CALL(shim_sceKernelOpen);
  int fd = (int)C->rax; CHECK(fd >= 0);
  call2(shim_sceKernelClose, fd, 0); CHECK(RAX32 == 0);
  call2(shim_sceKernelRename, f1, f2); CHECK(RAX32 == 0);
  call2(shim_sceKernelUnlink, f2, 0); CHECK(RAX32 == 0);
  call2(shim_sceKernelRmdir, dir, 0); CHECK(RAX32 == 0);
  // read-only image: EROFS (30) as an SCE code from the sceKernel forms, -1/errno from POSIX
  uint64_t ro = gstr("/app0/x");
  call2(shim_sceKernelMkdir, ro, 0755); CHECK(RAX32 == 0x8002001eu);
  call2(shim_sceKernelUnlink, ro, 0); CHECK(RAX32 == 0x8002001eu);
  call2(shim_sceKernelRename, ro, f1); CHECK(RAX32 == 0x8002001eu);
  call2(shim_sceKernelRmdir, ro, 0); CHECK(RAX32 == 0x8002001eu);
  call2(shim_mkdir, ro, 0755); CHECK((int64_t)C->rax == -1 && guest_errno() == EROFS);
  printf("kernelfs: SCE error codes ok\n");
}

static void test_pak_fallback(void) {
  extern void nps5_test_pak_insert(const char *path);
  nps5_test_pak_insert("Engine/Content/Internationalization/ICU/icudt/en.res");
  ps4_pak_entry_t e;
  unsetenv("NPS5_PAK_LOCALE_FALLBACK");
  CHECK(ps4_pak_lookup("Engine/Content/Internationalization/ICU/icudt/en.res", &e));
  CHECK(!ps4_pak_lookup("Engine/Content/Internationalization/ICU/icudt/en_US.res", &e));
  CHECK(!ps4_pak_lookup("Engine/Content/Internationalization/ICU/icudt/en_US_POSIX.res", &e));
  setenv("NPS5_PAK_LOCALE_FALLBACK", "1", 1);
  CHECK(ps4_pak_lookup("Engine/Content/Internationalization/ICU/icudt/en_US.res", &e));
  unsetenv("NPS5_PAK_LOCALE_FALLBACK");
  CHECK(!ps4_pak_lookup("Engine/Content/Internationalization/ICU/icudt/en_US.res", &e));
  printf("kernelfs: pak locale fallback gated ok\n");
}

static void test_param_json(const char *tmp) {
  static const char js[] =
      "{\n \"applicationCategoryType\": 0,\n \"contentId\": \"UP0000-PPSA30552_00-DIGGINGGAME00000\",\n"
      " \"contentVersion\": \"01.002.000\",\n \"masterVersion\": \"01.00\",\n \"titleId\": \"PPSA30552\",\n"
      " \"userDefinedParam1\": 7,\n"
      " \"localizedParameters\": {\n  \"defaultLanguage\": \"en-US\",\n"
      "  \"de-DE\": {\"titleName\": \"Falsch\"},\n  \"en-US\": {\"titleName\": \"Digging Game\"}\n }\n}\n";
  Ps4SfoContext sfo = {0};
  char v[128];
  CHECK(ps4_sfo_from_param_json(&sfo, js, sizeof js - 1));
  CHECK(ps4_sfo_get_string(&sfo, "TITLE_ID", v, sizeof v) && !strcmp(v, "PPSA30552"));
  CHECK(ps4_sfo_get_string(&sfo, "CONTENT_ID", v, sizeof v) && !strcmp(v, "UP0000-PPSA30552_00-DIGGINGGAME00000"));
  CHECK(ps4_sfo_get_string(&sfo, "APP_VER", v, sizeof v) && !strcmp(v, "01.002.000"));
  CHECK(ps4_sfo_get_string(&sfo, "VERSION", v, sizeof v) && !strcmp(v, "01.00"));
  CHECK(ps4_sfo_get_string(&sfo, "TITLE", v, sizeof v) && !strcmp(v, "Digging Game"));
  int32_t n = -1;
  CHECK(ps4_sfo_get_int(&sfo, "APPLICATION_CATEGORY_TYPE", &n) && n == 0);
  CHECK(ps4_sfo_get_int(&sfo, "USER_DEFINED_PARAM_1", &n) && n == 7);
  ps4_sfo_free(&sfo);
  CHECK(!ps4_sfo_from_param_json(&sfo, "{\"x\":1}", 7));

  // end to end through the app root: no param.sfo, only sce_sys/param.json
  char root[700], p[800];
  snprintf(root, sizeof root, "%s/ps5app", tmp); mkdir(root, 0755);
  snprintf(p, sizeof p, "%s/sce_sys", root); mkdir(p, 0755);
  snprintf(p, sizeof p, "%s/sce_sys/param.json", root);
  FILE *f = fopen(p, "w"); fputs(js, f); fclose(f);
  ps4_vfs_init(root);
  CHECK(ps4_sfo_find_and_load(&sfo));
  CHECK(ps4_sfo_get_string(&sfo, "TITLE_ID", v, sizeof v) && !strcmp(v, "PPSA30552"));
  ps4_sfo_free(&sfo);
  char ud[800]; snprintf(ud, sizeof ud, "%s/ud", tmp); setenv("NPS5_USERDATA_DIR", ud, 1);
  OrbisAppContentBootParam bp;
  CHECK(sceAppContentInitialize(NULL, &bp) == 0);
  char buf[128] = {0};
  CHECK(sceAppContentAppParamGetString(0, buf, sizeof buf) == 0 && !strcmp(buf, "PPSA30552"));
  CHECK(sceAppContentAppParamGetString(1, buf, sizeof buf) == 0 && !strcmp(buf, "UP0000-PPSA30552_00-DIGGINGGAME00000"));
  int32_t u = 0;
  CHECK(sceAppContentAppParamGetInt(ORBIS_APP_CONTENT_APPPARAM_ID_USER_DEFINED_PARAM_1, &u) == 0 && u == 7);
  CHECK(!strcmp(ps4_vfs_title_id(), "PPSA30552"));
  printf("kernelfs: param.json ok\n");
}

// ---------------------------------------------------------------------------------------------------------------
// The dump must never be written. Table-driven: every mutating entry point (VFS API and guest shims, POSIX and SCE
// forms) is run against /app0 paths in several spellings and must fail with EROFS, leaving the file's bytes, mode,
// mtime and xattrs unchanged.
#include <sys/xattr.h>
#include <sys/time.h>
#include <signal.h>
#include <dirent.h>
#include "ps4_syscalls.h"

typedef struct { const char *name; int (*fn)(const char *p, const char *other); int is_sce; } EroTest;
static int rc_errno(int r) { return r == -1 ? errno : (r == 0 ? 0 : -1); }

static int t_open_w(const char *p, const char *o)      { (void)o; int fd = ps4_vfs_open(p, O_WRONLY, 0); if (fd >= 0) close(fd); return rc_errno(fd < 0 ? -1 : 0); }
static int t_open_rw(const char *p, const char *o)     { (void)o; int fd = ps4_vfs_open(p, O_RDWR, 0); if (fd >= 0) close(fd); return rc_errno(fd < 0 ? -1 : 0); }
static int t_open_trunc(const char *p, const char *o)  { (void)o; int fd = ps4_vfs_open(p, O_RDONLY | O_TRUNC, 0); if (fd >= 0) close(fd); return rc_errno(fd < 0 ? -1 : 0); }
static int t_open_creat(const char *p, const char *o)  { (void)o; int fd = ps4_vfs_open(p, O_RDONLY | O_CREAT, 0644); if (fd >= 0) close(fd); return rc_errno(fd < 0 ? -1 : 0); }
static int t_open_append(const char *p, const char *o) { (void)o; int fd = ps4_vfs_open(p, O_RDONLY | O_APPEND, 0); if (fd >= 0) close(fd); return rc_errno(fd < 0 ? -1 : 0); }
static int t_unlink(const char *p, const char *o)      { (void)o; return rc_errno(ps4_vfs_unlink(p)); }
static int t_rmdir(const char *p, const char *o)       { (void)o; return rc_errno(ps4_vfs_rmdir(p)); }
static int t_remove(const char *p, const char *o)      { (void)o; return rc_errno(ps4_vfs_remove(p)); }
static int t_mkdir(const char *p, const char *o)       { (void)o; return rc_errno(ps4_vfs_mkdir(p, 0755)); }
static int t_mkfifo(const char *p, const char *o)      { (void)o; return rc_errno(ps4_vfs_mkfifo(p, 0644)); }
static int t_truncate(const char *p, const char *o)    { (void)o; return rc_errno(ps4_vfs_truncate(p, 0)); }
static int t_chmod(const char *p, const char *o)       { (void)o; return rc_errno(ps4_vfs_chmod(p, 0)); }
static int t_chown(const char *p, const char *o)       { (void)o; return rc_errno(ps4_vfs_chown(p, getuid(), getgid())); }
static int t_chflags(const char *p, const char *o)     { (void)o; return rc_errno(ps4_vfs_chflags(p, 0x2 /*UF_IMMUTABLE*/)); }
static int t_utimes(const char *p, const char *o)      { (void)o; struct timeval tv[2] = {{1000, 0}, {1000, 0}}; return rc_errno(ps4_vfs_utimes(p, tv)); }
static int t_setxattr(const char *p, const char *o)    { (void)o; return rc_errno(ps4_vfs_setxattr(p, "user.evil", "1", 1, 0)); }
static int t_rmxattr(const char *p, const char *o)     { (void)o; return rc_errno(ps4_vfs_removexattr(p, "user.keep")); }
static int t_rename_from(const char *p, const char *o) { return rc_errno(ps4_vfs_rename(p, o)); }   // other = a writable /data path
static int t_rename_to(const char *p, const char *o)   { return rc_errno(ps4_vfs_rename(o, p)); }
static int t_link_src(const char *p, const char *o)    { return rc_errno(ps4_vfs_link(p, o)); }     // a writable name for the image inode
static int t_link_dst(const char *p, const char *o)    { return rc_errno(ps4_vfs_link(o, p)); }
static int t_symlink_at(const char *p, const char *o)  { return rc_errno(ps4_vfs_symlink(o, p)); }
static int t_symlink_to(const char *p, const char *o)  { return rc_errno(ps4_vfs_symlink(p, o)); } // a link in /data to the image
static int t_fopen_w(const char *p, const char *o)     { (void)o; FILE *f = ps4_vfs_fopen(p, "w"); if (f) { fclose(f); return 0; } return errno; }
static int t_fopen_a(const char *p, const char *o)     { (void)o; FILE *f = ps4_vfs_fopen(p, "r+"); if (f) { fclose(f); return 0; } return errno; }
// fd forms: a read-only descriptor of an image file must not change the file either
static int fd_ro(const char *p) { int fd = ps4_vfs_open(p, O_RDONLY, 0); return fd; }
#define FD_TEST(nm, call) static int nm(const char *p, const char *o) { (void)o; int fd = fd_ro(p); if (fd < 0) return -2; int r = (call); int e = errno; ps4_vfs_close(fd); return rc_errno(r) == 0 ? 0 : e; }
FD_TEST(t_fchmod, ps4_vfs_fchmod(fd, 0))
FD_TEST(t_fchown, ps4_vfs_fchown(fd, getuid(), getgid()))
FD_TEST(t_futimes, ({ struct timeval tv[2] = {{1000, 0}, {1000, 0}}; ps4_vfs_futimes(fd, tv); }))
FD_TEST(t_ftruncate, ps4_vfs_ftruncate(fd, 0))
FD_TEST(t_fsetxattr, ps4_vfs_fsetxattr(fd, "user.evil", "1", 1, 0))
// guest shims, called with registers set up like the real thing
static int shim_rc(GuestContext *c) { return ((int64_t)c->rax == -1) ? guest_errno() : ((uint32_t)c->rax >> 16 == 0x8002 ? (int)(c->rax & 0xffff) : (c->rax == 0 || (int64_t)c->rax >= 0 ? 0 : -1)); }
static int s_open(const char *p, int fl, int sce, int sc) {
  reset(); uint64_t g = gstr(p);
  if (sc) { C->rdi = 5; C->rsi = g; C->rdx = (uint64_t)fl; C->rcx = 0644; CALL(shim_syscall); }
  else { C->rdi = g; C->rsi = (uint64_t)fl; C->rdx = 0644; if (sce) CALL(shim_sceKernelOpen); else CALL(shim_open); }
  int e = shim_rc(C);
  if ((int64_t)C->rax >= 0 && !(C->rax >> 16 == 0x8002)) close((int)C->rax);
  return e;
}
static int sh_open_w(const char *p, const char *o)       { (void)o; return s_open(p, O_WRONLY, 0, 0); }
static int sh_open_trunc(const char *p, const char *o)   { (void)o; return s_open(p, O_RDONLY | O_TRUNC, 0, 0); }
static int sh_open_creat(const char *p, const char *o)   { (void)o; return s_open(p, O_RDWR | O_CREAT, 0, 0); }
static int sh_kopen_w(const char *p, const char *o)      { (void)o; return s_open(p, O_WRONLY | O_TRUNC, 1, 0); }
static int sh_syscall5_w(const char *p, const char *o)   { (void)o; return s_open(p, O_WRONLY | O_TRUNC, 0, 1); }
static int sh_syscall5_c(const char *p, const char *o)   { (void)o; return s_open(p, O_RDWR | O_CREAT, 0, 1); }
#define SHIM1(nm, fn, setup) static int nm(const char *p, const char *o) { (void)o; reset(); C->rdi = gstr(p); setup; CALL(fn); return shim_rc(C); }
SHIM1(sh_unlink, shim_unlink, (void)0)
SHIM1(sh_kunlink, shim_sceKernelUnlink, (void)0)
SHIM1(sh_rmdir, shim_rmdir, (void)0)
SHIM1(sh_krmdir, shim_sceKernelRmdir, (void)0)
SHIM1(sh_mkdir, shim_mkdir, C->rsi = 0755)
SHIM1(sh_kmkdir, shim_sceKernelMkdir, C->rsi = 0755)
SHIM1(sh_truncate, shim_truncate, C->rsi = 0)
SHIM1(sh_ktruncate, shim_sceKernelTruncate, C->rsi = 0)
SHIM1(sh_chmod, shim_chmod, C->rsi = 0)
SHIM1(sh_kchmod, shim_sceKernelChmod, C->rsi = 0)
SHIM1(sh_utimes_now, shim_utimes, C->rsi = 0)
SHIM1(sh_kutimes_now, shim_sceKernelUtimes, C->rsi = 0)
static int sh_utimes_old(const char *p, const char *o) { (void)o; reset(); uint64_t tv = galloc(32); int64_t *t = (int64_t *)H(tv); t[0] = 1000; t[1] = 0; t[2] = 1000; t[3] = 0; C->rdi = gstr(p); C->rsi = tv; CALL(shim_utimes); return shim_rc(C); }
static int sh_rename_from(const char *p, const char *o) { reset(); C->rdi = gstr(p); C->rsi = gstr(o); CALL(shim_rename); return shim_rc(C); }
static int sh_rename_to(const char *p, const char *o)   { reset(); C->rdi = gstr(o); C->rsi = gstr(p); CALL(shim_rename); return shim_rc(C); }
static int sh_krename_from(const char *p, const char *o) { reset(); C->rdi = gstr(p); C->rsi = gstr(o); CALL(shim_sceKernelRename); return shim_rc(C); }
static int sh_krename_to(const char *p, const char *o)   { reset(); C->rdi = gstr(o); C->rsi = gstr(p); CALL(shim_sceKernelRename); return shim_rc(C); }
#define SHFD(nm, fn, setup) static int nm(const char *p, const char *o) { (void)o; int fd = fd_ro(p); if (fd < 0) return -2; reset(); C->rdi = (uint64_t)fd; setup; CALL(fn); int e = shim_rc(C); ps4_vfs_close(fd); return e; }
SHFD(sh_fchmod, shim_fchmod, C->rsi = 0)
SHFD(sh_futimes, shim_futimes, C->rsi = 0)
SHFD(sh_ftruncate, shim_ftruncate, C->rsi = 0)
SHFD(sh_kftruncate, shim_sceKernelFtruncate, C->rsi = 0)

static const EroTest ero_tests[] = {
  {"vfs open O_WRONLY", t_open_w, 0}, {"vfs open O_RDWR", t_open_rw, 0}, {"vfs open O_RDONLY|O_TRUNC", t_open_trunc, 0},
  {"vfs open O_RDONLY|O_CREAT", t_open_creat, 0}, {"vfs open O_RDONLY|O_APPEND", t_open_append, 0},
  {"vfs unlink", t_unlink, 0}, {"vfs rmdir", t_rmdir, 0}, {"vfs remove", t_remove, 0}, {"vfs mkdir", t_mkdir, 0},
  {"vfs mkfifo", t_mkfifo, 0}, {"vfs truncate", t_truncate, 0}, {"vfs chmod", t_chmod, 0}, {"vfs chown", t_chown, 0},
  {"vfs chflags", t_chflags, 0}, {"vfs utimes", t_utimes, 0}, {"vfs setxattr", t_setxattr, 0}, {"vfs removexattr", t_rmxattr, 0},
  {"vfs rename (source)", t_rename_from, 0}, {"vfs rename (destination)", t_rename_to, 0},
  {"vfs link (source)", t_link_src, 0}, {"vfs link (destination)", t_link_dst, 0},
  {"vfs symlink (at)", t_symlink_at, 0}, {"vfs symlink (to)", t_symlink_to, 0},
  {"vfs fopen w", t_fopen_w, 0}, {"vfs fopen r+", t_fopen_a, 0},
  {"vfs fchmod(ro fd)", t_fchmod, 0}, {"vfs fchown(ro fd)", t_fchown, 0}, {"vfs futimes(ro fd)", t_futimes, 0},
  {"vfs ftruncate(ro fd)", t_ftruncate, 0}, {"vfs fsetxattr(ro fd)", t_fsetxattr, 0},
  {"shim open O_WRONLY", sh_open_w, 0}, {"shim open O_TRUNC", sh_open_trunc, 0}, {"shim open O_CREAT", sh_open_creat, 0},
  {"shim sceKernelOpen", sh_kopen_w, 0}, {"shim syscall(SYS_open) O_TRUNC", sh_syscall5_w, 0}, {"shim syscall(SYS_open) O_CREAT", sh_syscall5_c, 0},
  {"shim unlink", sh_unlink, 0}, {"shim sceKernelUnlink", sh_kunlink, 1}, {"shim rmdir", sh_rmdir, 0}, {"shim sceKernelRmdir", sh_krmdir, 1},
  {"shim mkdir", sh_mkdir, 0}, {"shim sceKernelMkdir", sh_kmkdir, 1}, {"shim truncate", sh_truncate, 0}, {"shim sceKernelTruncate", sh_ktruncate, 1},
  {"shim chmod", sh_chmod, 0}, {"shim sceKernelChmod", sh_kchmod, 1}, {"shim utimes (now)", sh_utimes_now, 0},
  {"shim sceKernelUtimes (now)", sh_kutimes_now, 1}, {"shim utimes (1000)", sh_utimes_old, 0},
  {"shim rename (source)", sh_rename_from, 0}, {"shim rename (destination)", sh_rename_to, 0},
  {"shim sceKernelRename (source)", sh_krename_from, 1}, {"shim sceKernelRename (destination)", sh_krename_to, 1},
  {"shim fchmod(ro fd)", sh_fchmod, 0}, {"shim futimes(ro fd)", sh_futimes, 0}, {"shim ftruncate(ro fd)", sh_ftruncate, 0},
  {"shim sceKernelFtruncate(ro fd)", sh_kftruncate, 1},
};

typedef struct { char bytes[64]; size_t len; mode_t mode; struct timespec mtime; uid_t uid; char xattrs[256]; ssize_t xlen; nlink_t nlink; } Snap;
static void snap_take(const char *path, Snap *s) {
  memset(s, 0, sizeof *s);
  struct stat st;
  if (stat(path, &st) != 0) { s->len = (size_t)-1; return; }
  s->mode = st.st_mode; s->mtime = st.st_mtimespec; s->uid = st.st_uid; s->nlink = st.st_nlink;
  FILE *f = fopen(path, "rb");
  if (f) { s->len = fread(s->bytes, 1, sizeof s->bytes, f); fclose(f); }
  s->xlen = listxattr(path, s->xattrs, sizeof s->xattrs, 0);
}
static bool snap_same(const Snap *a, const Snap *b) {
  return a->len == b->len && !memcmp(a->bytes, b->bytes, sizeof a->bytes) && a->mode == b->mode && a->mtime.tv_sec == b->mtime.tv_sec &&
         a->mtime.tv_nsec == b->mtime.tv_nsec && a->uid == b->uid && a->xlen == b->xlen && !memcmp(a->xattrs, b->xattrs, a->xlen > 0 ? (size_t)a->xlen : 0) && a->nlink == b->nlink;
}

static void test_erofs_table(const char *tmp) {
  char app[700], ud[700], victim[800], dirp[800], dvictim[800];
  snprintf(app, sizeof app, "%s/dump", tmp); mkdir(app, 0755);
  snprintf(ud, sizeof ud, "%s/ud_ero", tmp);
  snprintf(victim, sizeof victim, "%s/victim.txt", app);
  snprintf(dirp, sizeof dirp, "%s/sub", app); mkdir(dirp, 0755);
  snprintf(dvictim, sizeof dvictim, "%s/sub/inner.txt", app);
  for (int i = 0; i < 2; i++) {
    FILE *f = fopen(i ? dvictim : victim, "w"); fputs("DUMP-BYTES", f); fclose(f);
    chmod(i ? dvictim : victim, 0644);
    struct timeval tv[2] = {{1234567, 0}, {1234567, 0}};
    utimes(i ? dvictim : victim, tv);
  }
  setxattr(victim, "user.keep", "k", 1, 0, 0);
  setenv("NPS5_USERDATA_DIR", ud, 1); setenv("NPS5_TITLE_ID", "TESTERO00", 1);
  ps4_vfs_init(app);
  char realapp[1100]; realpath(app, realapp);
  char host_raw[1200], host_dotdot[1300], host_link[900];
  snprintf(host_raw, sizeof host_raw, "%s/victim.txt", realapp);
  snprintf(host_dotdot, sizeof host_dotdot, "%s/x/../victim.txt", realapp);
  snprintf(host_link, sizeof host_link, "%s/ud_ero/lnk", tmp);
  const char *spell_file[] = {"/app0/victim.txt", "/APP0/Victim.TXT", "/app0/./sub/../victim.txt", "/hostapp/victim.txt", "victim.txt",
                              "./victim.txt", host_raw, host_dotdot, "/app0/sub/inner.txt", "/app0/sub", "/app0/new-file.txt", "/app0/newdir",
                              // through a symlink planted in /data that points at an ANCESTOR of the dump (review #1): the mount kind must not exempt it
                              "/data/alnk/dump/victim.txt", "/data/alnk/dump/sub/inner.txt", "/data/alnk/dump/sub", "/data/alnk/dump/new-file.txt", "/data/alnk/dump/newdir"};
  char other[900]; snprintf(other, sizeof other, "/data/other.txt");
  { char al[900]; snprintf(al, sizeof al, "%s/ud_ero/data/alnk", tmp); CHECK(symlink(tmp, al) == 0); }
  { FILE *f = ps4_vfs_fopen(other, "w"); CHECK(f != NULL); if (f) fclose(f); }
  Snap s0, s1, sd0, sd1; snap_take(victim, &s0); snap_take(dvictim, &sd0);
  int checked = 0;
  for (size_t t = 0; t < sizeof ero_tests / sizeof *ero_tests; t++) {
    for (size_t s = 0; s < sizeof spell_file / sizeof *spell_file; s++) {
      const char *sp = spell_file[s];
      int e = ero_tests[t].fn(sp, other);
      if (e == -2) continue; // the read-only fd could not be opened (a directory / missing path): nothing to test
      checked++;
      if (e != EROFS) { printf("FAIL erofs table: [%s] on '%s' -> errno %d (want EROFS)\n", ero_tests[t].name, sp, e); fails++; }
      snap_take(victim, &s1); snap_take(dvictim, &sd1);
      if (!snap_same(&s0, &s1) || !snap_same(&sd0, &sd1)) { printf("FAIL erofs table: [%s] on '%s' CHANGED the image file\n", ero_tests[t].name, sp); fails++; s0 = s1; sd0 = sd1; }
    }
  }
  char nf[900];
  snprintf(nf, sizeof nf, "%s/new-file.txt", app); CHECK(access(nf, F_OK) != 0);
  snprintf(nf, sizeof nf, "%s/newdir", app); CHECK(access(nf, F_OK) != 0);
  snprintf(nf, sizeof nf, "%s/ud_ero/data/other.txt", tmp); CHECK(access(nf, F_OK) == 0); // the writable root was untouched by the refusals
  CHECK(access(host_link, F_OK) != 0);
  CHECK(checked > 500);
  // the same operations on the writable /data root work (the guard is not simply refusing everything)
  CHECK(ps4_vfs_mkdir("/data/d1", 0755) == 0);
  CHECK(ps4_vfs_chmod("/data/other.txt", 0600) == 0);
  CHECK(ps4_vfs_rename("/data/other.txt", "/data/o2.txt") == 0);
  CHECK(ps4_vfs_link("/data/o2.txt", "/data/o3.txt") == 0);
  CHECK(ps4_vfs_symlink("/data/o2.txt", "/data/o4.txt") == 0);
  CHECK(ps4_vfs_truncate("/data/o2.txt", 0) == 0);
  CHECK(ps4_vfs_unlink("/data/o3.txt") == 0 && ps4_vfs_unlink("/data/o4.txt") == 0 && ps4_vfs_unlink("/data/o2.txt") == 0 && ps4_vfs_rmdir("/data/d1") == 0);
  { int fd = ps4_vfs_open("/data/w.txt", O_RDWR | O_CREAT, 0644); CHECK(fd >= 0); if (fd >= 0) { CHECK(ps4_vfs_fchmod(fd, 0600) == 0); ps4_vfs_close(fd); } ps4_vfs_unlink("/data/w.txt"); }
  // case-insensitive guard on the helper itself
  CHECK(ps4_vfs_is_readonly("/App0/x") && ps4_vfs_is_readonly("/HOSTAPP") && ps4_vfs_is_readonly("rel") && !ps4_vfs_is_readonly("/data/x") && !ps4_vfs_is_readonly("/app0x"));
  printf("kernelfs: EROFS table ok (%d cases)\n", checked);
  unsetenv("NPS5_USERDATA_DIR"); unsetenv("NPS5_TITLE_ID");
}

// ---------------------------------------------------------------------------------------------------------------
// Write-gate attacks (review 2026-11-15 #1, #2, #11, #12): a symlink planted in a writable mount that points at the dump or
// one of its ancestors, rename out of the dump, the read-only DLC mount, an unresolvable application root, and a userdata
// root inside the dump. The dump here is a temp dir; nothing outside the temp dir is touched.
static bool file_is(const char *p, const char *want) { char b[64] = {0}; FILE *f = fopen(p, "rb"); if (!f) return false; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0; return !strcmp(b, want); }
static void test_gate_attacks(const char *tmp) {
  char dump[700], ud[700], ebo[800], dlc[800], datalink[900], f[1000];
  snprintf(dump, sizeof dump, "%s/gdump", tmp); mkdir(dump, 0755);
  snprintf(ud, sizeof ud, "%s/ud_gate", tmp);
  snprintf(ebo, sizeof ebo, "%s/eboot.bin", dump);
  snprintf(f, sizeof f, "%s/addcont", dump); mkdir(f, 0755);
  snprintf(dlc, sizeof dlc, "%s/addcont/dlc.pak", dump);
  for (int i = 0; i < 2; i++) { FILE *fp = fopen(i ? dlc : ebo, "w"); fputs("GAMEBYTES", fp); fclose(fp); }
  setenv("NPS5_USERDATA_DIR", ud, 1); setenv("NPS5_TITLE_ID", "TESTGATE0", 1);
  ps4_vfs_init(dump);
  snprintf(datalink, sizeof datalink, "%s/data/lnk", ud);

  // 1. a symlink to an ancestor of the dump (and to the dump itself, and a relative one) is refused at creation
  CHECK(ps4_vfs_symlink(tmp, "/data/lnk") == -1 && errno == EROFS);
  CHECK(access(datalink, F_OK) != 0);
  CHECK(ps4_vfs_symlink(dump, "/data/lnk") == -1 && errno == EROFS);
  CHECK(ps4_vfs_symlink("../../gdump", "/data/rel") == -1 && errno == EROFS);
  CHECK(ps4_vfs_symlink("/", "/data/rootlink") == -1 && errno == EROFS);
  CHECK(ps4_vfs_symlink("/data/ok-target", "/data/fine") == 0);   // a harmless link still works
  ps4_vfs_unlink("/data/fine");

  // 2. a link that exists anyway (planted on the host, as a race would): every mutation through it is refused
  CHECK(symlink(tmp, datalink) == 0);
  const char *via_ebo = "/data/lnk/gdump/eboot.bin";
  int fd = ps4_vfs_open(via_ebo, O_WRONLY | O_TRUNC, 0); CHECK(fd < 0 && errno == EROFS); if (fd >= 0) close(fd);
  fd = ps4_vfs_open(via_ebo, O_RDWR, 0); CHECK(fd < 0 && errno == EROFS); if (fd >= 0) close(fd);
  CHECK(file_is(ebo, "GAMEBYTES"));
  CHECK(ps4_vfs_rename(via_ebo, "/data/stolen") == -1 && errno == EROFS);   // rename OUT of the dump
  CHECK(ps4_vfs_rename("/data/stolen2", via_ebo) == -1);                    // and INTO it
  CHECK(ps4_vfs_unlink(via_ebo) == -1 && errno == EROFS);
  CHECK(ps4_vfs_mkdir("/data/lnk/gdump/newdir", 0755) == -1 && errno == EROFS);
  CHECK(ps4_vfs_chmod(via_ebo, 0) == -1 && errno == EROFS);
  CHECK(ps4_vfs_truncate(via_ebo, 0) == -1 && errno == EROFS);
  CHECK(ps4_vfs_rename("/data/lnk/gdump", "/data/gone") == -1 && errno == EROFS); // the dump directory itself
  { FILE *fp = ps4_vfs_fopen(via_ebo, "w"); CHECK(fp == NULL && errno == EROFS); if (fp) fclose(fp); }
  CHECK(ps4_vfs_rmdir("/data/lnk/gdump") == -1 && errno == EROFS);
  CHECK(ps4_vfs_link(via_ebo, "/data/hard") == -1);
  CHECK(file_is(ebo, "GAMEBYTES"));
  snprintf(f, sizeof f, "%s/data/stolen", ud); CHECK(access(f, F_OK) != 0);
  snprintf(f, sizeof f, "%s/data/hard", ud); CHECK(access(f, F_OK) != 0);
  snprintf(f, sizeof f, "%s/newdir", dump); CHECK(access(f, F_OK) != 0);
  // an ancestor of the dump reached as a raw host path or through the link: never renamed, removed or truncated
  CHECK(ps4_vfs_rename("/data/lnk", "/data/lnk2") == -1); // conservative: the link resolves to an ancestor
  unlink(datalink);
  CHECK(ps4_vfs_rename(tmp, "/data/anc") == -1 && errno == EROFS);
  CHECK(ps4_vfs_rmdir(tmp) == -1 && errno == EROFS);
  CHECK(access(ebo, F_OK) == 0);
  // the guest shims see the same refusal
  CHECK(symlink(tmp, datalink) == 0);
  call2(shim_open, gstr(via_ebo), O_WRONLY | O_TRUNC); CHECK((int64_t)C->rax == -1 && guest_errno() == EROFS);
  CHECK(file_is(ebo, "GAMEBYTES"));
  unlink(datalink);

  // 3. DLC: <dump>/addcont mounted read-only. Nothing below it can be written, whatever kind the path is
  snprintf(f, sizeof f, "%s/addcont", dump);
  CHECK(ps4_vfs_mount_ro("/addcont0", f) == 0);
  fd = ps4_vfs_open("/addcont0/dlc.pak", O_RDONLY, 0); CHECK(fd >= 0); if (fd >= 0) close(fd);   // reads work
  fd = ps4_vfs_open("/addcont0/dlc.pak", O_WRONLY | O_TRUNC, 0); CHECK(fd < 0 && errno == EROFS); if (fd >= 0) close(fd);
  CHECK(ps4_vfs_unlink("/addcont0/dlc.pak") == -1 && errno == EROFS);
  CHECK(ps4_vfs_rename("/addcont0/dlc.pak", "/data/dlc") == -1 && errno == EROFS);
  CHECK(ps4_vfs_mkdir("/addcont0/x", 0755) == -1 && errno == EROFS);
  CHECK(file_is(dlc, "GAMEBYTES"));
  // an explicit read-only mount OUTSIDE the dump is read-only too (the flag, not the location, decides)
  char rodir[800]; snprintf(rodir, sizeof rodir, "%s/rodir", tmp); mkdir(rodir, 0755);
  snprintf(f, sizeof f, "%s/rodir/a.txt", tmp); { FILE *fp = fopen(f, "w"); fputs("RO", fp); fclose(fp); }
  CHECK(ps4_vfs_mount_ro("/ro0", rodir) == 0);
  CHECK(ps4_vfs_unlink("/ro0/a.txt") == -1 && errno == EROFS);
  fd = ps4_vfs_open("/ro0/b.txt", O_WRONLY | O_CREAT, 0644); CHECK(fd < 0 && errno == EROFS); if (fd >= 0) close(fd);
  CHECK(ps4_vfs_mount("/ro0", rodir) == 0);                 // re-mounting read-write clears the flag
  CHECK(ps4_vfs_unlink("/ro0/a.txt") == 0);
  ps4_vfs_unmount("/ro0");
  // writable mounts keep working
  fd = ps4_vfs_open("/data/ok.txt", O_RDWR | O_CREAT | O_TRUNC, 0644); CHECK(fd >= 0); if (fd >= 0) { CHECK(write(fd, "hi", 2) == 2); ps4_vfs_close(fd); }
  snprintf(f, sizeof f, "%s/data/ok.txt", ud); CHECK(file_is(f, "hi"));
  fd = ps4_vfs_open("/data/ok.txt", O_WRONLY | O_TRUNC, 0); CHECK(fd >= 0); if (fd >= 0) ps4_vfs_close(fd);   // O_TRUNC still truncates
  CHECK(file_is(f, ""));
  ps4_vfs_unlink("/data/ok.txt");

  // 4. fail closed: an application root that cannot be resolved makes every host path read-only
  char moved[800]; snprintf(moved, sizeof moved, "%s/gdump_moved", tmp);
  ps4_vfs_init(dump);
  CHECK(rename(dump, moved) == 0); // the root can no longer be resolved
  snprintf(f, sizeof f, "%s/failclosed.txt", tmp);
  fd = ps4_vfs_open(f, O_WRONLY | O_CREAT, 0644); CHECK(fd < 0 && errno == EROFS); if (fd >= 0) close(fd);
  CHECK(access(f, F_OK) != 0);
  CHECK(ps4_vfs_mkdir(f, 0755) == -1);
  CHECK(rename(moved, dump) == 0);

  // 5. a userdata root inside the dump is refused (no mount, nothing created below the dump)
  char bad[900]; snprintf(bad, sizeof bad, "%s/inner_ud", dump);
  setenv("NPS5_USERDATA_DIR", bad, 1);
  ps4_vfs_init(dump);
  fd = ps4_vfs_open("/data/x.txt", O_WRONLY | O_CREAT, 0644); CHECK(fd < 0); if (fd >= 0) close(fd);
  CHECK(access(bad, F_OK) != 0);
  // ... also through a symlink that resolves into the dump
  char viaq[900]; snprintf(viaq, sizeof viaq, "%s/ud_symlink", tmp); symlink(dump, viaq);
  snprintf(bad, sizeof bad, "%s/ud_symlink/x", tmp); setenv("NPS5_USERDATA_DIR", bad, 1);
  ps4_vfs_init(dump);
  fd = ps4_vfs_open("/data/x.txt", O_WRONLY | O_CREAT, 0644); CHECK(fd < 0); if (fd >= 0) close(fd);
  snprintf(f, sizeof f, "%s/x", dump); CHECK(access(f, F_OK) != 0);
  CHECK(file_is(ebo, "GAMEBYTES"));
  unlink(viaq);
  printf("kernelfs: write-gate attacks refused ok\n");
  unsetenv("NPS5_USERDATA_DIR"); unsetenv("NPS5_TITLE_ID");
}

// Unmounted absolute host paths are never written (EACCES); allow-listed devices and the mounted roots still are.
static void test_raw_write(const char *tmp) {
  char dump[700], ud[700], f[1000];
  snprintf(dump, sizeof dump, "%s/rdump", tmp); mkdir(dump, 0755);
  snprintf(f, sizeof f, "%s/eboot.bin", dump); { FILE *fp = fopen(f, "w"); fputs("GAMEBYTES", fp); fclose(fp); }
  snprintf(ud, sizeof ud, "%s/ud_raw", tmp);
  setenv("NPS5_USERDATA_DIR", ud, 1); setenv("NPS5_TITLE_ID", "TESTRAW00", 1);
  ps4_vfs_init(dump);
  // raw (unmounted) host paths are never written; devices on the allow-list and the mounted roots still are
  {
    char raw[900], rawf[1000], mine[1000];
    snprintf(raw, sizeof raw, "%s/rawdir", tmp); mkdir(raw, 0755);
    snprintf(rawf, sizeof rawf, "%s/rawdir/victim", tmp);
    { FILE *fp = fopen(rawf, "w"); fputs("HOSTBYTES", fp); fclose(fp); }
    unsetenv("NPS5_VFS_RAW_WRITE");
    int rfd = ps4_vfs_open(rawf, O_WRONLY | O_TRUNC, 0); CHECK(rfd < 0 && errno == EACCES); if (rfd >= 0) close(rfd);
    rfd = ps4_vfs_open(rawf, O_RDWR, 0); CHECK(rfd < 0 && errno == EACCES); if (rfd >= 0) close(rfd);
    snprintf(f, sizeof f, "%s/rawdir/created", tmp);
    rfd = ps4_vfs_open(f, O_WRONLY | O_CREAT, 0644); CHECK(rfd < 0 && errno == EACCES); if (rfd >= 0) close(rfd);
    CHECK(access(f, F_OK) != 0);
    { FILE *fp = ps4_vfs_fopen(f, "w"); CHECK(fp == NULL && errno == EACCES); if (fp) fclose(fp); }
    CHECK(file_is(rawf, "HOSTBYTES"));
    CHECK(ps4_vfs_unlink(rawf) == -1 && errno == EACCES);
    CHECK(ps4_vfs_mkdir(f, 0755) == -1 && errno == EACCES);
    CHECK(ps4_vfs_rmdir(raw) == -1 && errno == EACCES);
    CHECK(ps4_vfs_truncate(rawf, 0) == -1 && errno == EACCES);
    CHECK(ps4_vfs_rename(rawf, "/data/taken") == -1 && errno == EACCES);
    CHECK(ps4_vfs_rename("/data/nothing", rawf) == -1);
    CHECK(ps4_vfs_symlink("/etc", "/data/to_etc") == -1 && errno == EACCES);   // no link out of the mounts
    CHECK(file_is(rawf, "HOSTBYTES"));
    // a link planted inside the mount that points at a raw directory does not carry a write out
    snprintf(mine, sizeof mine, "%s/data/outlink", ud);
    CHECK(symlink(raw, mine) == 0);
    rfd = ps4_vfs_open("/data/outlink/victim", O_WRONLY | O_TRUNC, 0); CHECK(rfd < 0 && errno == EACCES); if (rfd >= 0) close(rfd);
    CHECK(ps4_vfs_unlink("/data/outlink/victim") == -1 && errno == EACCES);
    CHECK(file_is(rawf, "HOSTBYTES"));
    unlink(mine);
    // ".." cannot climb out of a mount
    rfd = ps4_vfs_open("/data/../../rawdir/victim", O_WRONLY | O_TRUNC, 0); CHECK(rfd < 0 || file_is(rawf, "HOSTBYTES")); if (rfd >= 0) close(rfd);
    CHECK(file_is(rawf, "HOSTBYTES"));
    // the allow-list
    rfd = ps4_vfs_open("/dev/null", O_WRONLY, 0); CHECK(rfd >= 0); if (rfd >= 0) close(rfd);
    // mounted roots keep working
    rfd = ps4_vfs_open("/data/w.bin", O_WRONLY | O_CREAT, 0644); CHECK(rfd >= 0); if (rfd >= 0) close(rfd);
    CHECK(ps4_vfs_mkdir("/temp0/d", 0755) == 0);
    CHECK(ps4_vfs_rmdir("/temp0/d") == 0);
    CHECK(ps4_vfs_rename("/data/w.bin", "/data/w2.bin") == 0);
    CHECK(ps4_vfs_unlink("/data/w2.bin") == 0);
    // the escape hatch restores the old behaviour
    setenv("NPS5_VFS_RAW_WRITE", "1", 1);
    rfd = ps4_vfs_open(rawf, O_WRONLY | O_TRUNC, 0); CHECK(rfd >= 0); if (rfd >= 0) close(rfd);
    CHECK(ps4_vfs_unlink(rawf) == 0);
    unsetenv("NPS5_VFS_RAW_WRITE");
    rfd = ps4_vfs_open(rawf, O_WRONLY | O_CREAT, 0644); CHECK(rfd < 0 && errno == EACCES); if (rfd >= 0) close(rfd);
  }
  // pread rejects offsets whose end is not representable
  { char b[4]; int rfd = ps4_vfs_open("/dev/null", O_RDONLY, 0); CHECK(rfd >= 0);
    CHECK(ps4_vfs_pread(rfd, b, 4, INT64_MAX - 2) == 0); // past the end: nothing, no overflow
    CHECK(ps4_vfs_pread(rfd, b, 4, -1) == -1 && errno == EINVAL);
    if (rfd >= 0) close(rfd); }
  printf("kernelfs: raw host writes refused ok\n");
  unsetenv("NPS5_USERDATA_DIR"); unsetenv("NPS5_TITLE_ID");
}

// Raw syscall(SYS_read/SYS_close) with a pak virtual fd number must go through the VFS table: a host fd that happens
// to carry that number is never read or closed (review #4).
static void test_raw_syscall_virtual_fd(const char *tmp) {
  char p[900]; snprintf(p, sizeof p, "%s/vfdhost.txt", tmp);
  FILE *fp = fopen(p, "w"); fputs("HOSTDATA", fp); fclose(fp);
  int src = open(p, O_RDONLY); CHECK(src >= 0);
  int vnum = PS4_VFS_VIRTUAL_FD_BASE + 200;
  CHECK(dup2(src, vnum) == vnum);
  close(src);
  uint64_t buf = galloc(32);
  reset(); C->rdi = 3; C->rsi = (uint64_t)vnum; C->rdx = buf; C->rcx = 8; CALL(shim_syscall);
  CHECK((int64_t)C->rax == -1 && guest_errno() == EBADF);                       // not the host file's bytes
  reset(); C->rdi = 6; C->rsi = (uint64_t)vnum; CALL(shim_syscall);
  CHECK((int64_t)C->rax == -1 && guest_errno() == EBADF);
  CHECK(fcntl(vnum, F_GETFD) != -1);                                            // still open
  reset(); C->rdi = 4; C->rsi = (uint64_t)vnum; C->rdx = buf; C->rcx = 1; CALL(shim_syscall);
  CHECK((int64_t)C->rax == -1);
  CHECK(fcntl(vnum, F_GETFD) != -1);
  close(vnum);
  // an ordinary host fd still works through the same syscalls
  src = open(p, O_RDONLY);
  reset(); C->rdi = 3; C->rsi = (uint64_t)src; C->rdx = buf; C->rcx = 8; CALL(shim_syscall);
  CHECK((int64_t)C->rax == 8 && !memcmp(H(buf), "HOSTDATA", 8));
  reset(); C->rdi = 6; C->rsi = (uint64_t)src; CALL(shim_syscall);
  CHECK(C->rax == 0 && fcntl(src, F_GETFD) == -1);
  printf("kernelfs: raw SYS_read/SYS_close with virtual fds ok\n");
}

// /data must fail closed when the per-title mount cannot be made: never fall back into the dump.
static void test_userdata_fail_closed(const char *tmp) {
  char app[700], d[800];
  snprintf(app, sizeof app, "%s/dump_fc", tmp); mkdir(app, 0755);
  snprintf(d, sizeof d, "%s/data", app); mkdir(d, 0755);
  setenv("NPS5_USERDATA_DIR", "/dev/null/nope", 1); setenv("NPS5_TITLE_ID", "TESTFC000", 1);
  ps4_vfs_init(app);
  const char *paths[] = {"/data/evil.txt", "/DATA/evil.txt", "/temp0/evil.txt", "/download0/evil.txt"};
  for (size_t i = 0; i < 4; i++) {
    int fd = ps4_vfs_open(paths[i], O_WRONLY | O_CREAT, 0644);
    CHECK(fd < 0);
    if (fd >= 0) close(fd);
    CHECK(ps4_vfs_mkdir(paths[i], 0755) == -1);
    char h[1024]; CHECK(ps4_vfs_resolve(paths[i], h, sizeof h) < 0);
  }
  char f[900]; snprintf(f, sizeof f, "%s/data/evil.txt", app); CHECK(access(f, F_OK) != 0);
  call2(shim_open, gstr("/data/evil.txt"), O_WRONLY | O_CREAT); CHECK((int64_t)C->rax == -1);
  printf("kernelfs: /data fails closed ok\n");
  unsetenv("NPS5_USERDATA_DIR"); unsetenv("NPS5_TITLE_ID");
}

static int g_fs_ci;
#define HAS(h, name) (g_fs_ci || strstr((h), (name)) != NULL)
static void test_dircache(const char *tmp) {
  char app[700], p[900];
  snprintf(app, sizeof app, "%s/dump_dc", tmp); mkdir(app, 0755);
  snprintf(p, sizeof p, "%s/gfx", app); mkdir(p, 0755);
  const char *names[] = {"Alpha.GNF", "beta.gnf", "Gamma.Dat"};
  for (int i = 0; i < 3; i++) { snprintf(p, sizeof p, "%s/gfx/%s", app, names[i]); FILE *f = fopen(p, "w"); fputs("x", f); fclose(f); }
  // many files, so the scan matters
  for (int i = 0; i < 3000; i++) { snprintf(p, sizeof p, "%s/gfx/f%05d.bin", app, i); FILE *f = fopen(p, "w"); fclose(f); }
  // age the directory so it is not "racy"
  { snprintf(p, sizeof p, "%s/gfx", app); struct timeval tv[2]; gettimeofday(&tv[0], 0); tv[0].tv_sec -= 100; tv[1] = tv[0]; utimes(p, tv); }
  { char a[900], b[900]; snprintf(a, sizeof a, "%s/CsProbe", app); snprintf(b, sizeof b, "%s/csprobe", app); FILE *f = fopen(a, "w"); fclose(f); g_fs_ci = access(b, F_OK) == 0; unlink(a); }
  char ud[800]; snprintf(ud, sizeof ud, "%s/ud_dc", tmp);
  setenv("NPS5_USERDATA_DIR", ud, 1); setenv("NPS5_TITLE_ID", "TESTDC000", 1);
  ps4_vfs_init(app);
  const char *probes[] = {"/app0/gfx/alpha.gnf", "/app0/GFX/BETA.GNF", "/app0/gfx/GAMMA.dat", "/app0/gfx/F00042.BIN"};
  const char *expect[] = {"Alpha.GNF", "beta.gnf", "Gamma.Dat", "f00042.bin"};
  char h_off[4][1024], h[1024];
  ps4_vfs_dircache_set(0);
  for (int i = 0; i < 4; i++) { CHECK(ps4_vfs_resolve(probes[i], h_off[i], sizeof h) == 0); CHECK(HAS(h_off[i], expect[i])); }
  char miss_off[1024]; CHECK(ps4_vfs_resolve("/app0/gfx/missing.gnf", miss_off, sizeof miss_off) == 0);
  uint64_t hits0, scans0, hits1, scans1;
  ps4_vfs_dircache_set(1);
  ps4_vfs_dircache_stats(&hits0, &scans0);
  for (int round = 0; round < 3; round++)
    for (int i = 0; i < 4; i++) { CHECK(ps4_vfs_resolve(probes[i], h, sizeof h) == 0); CHECK(strcmp(h, h_off[i]) == 0); }
  char miss[1024]; for (int i = 0; i < 100; i++) { CHECK(ps4_vfs_resolve("/app0/gfx/missing.gnf", miss, sizeof miss) == 0); CHECK(strcmp(miss, miss_off) == 0); }
  ps4_vfs_dircache_stats(&hits1, &scans1);
  CHECK(scans1 - scans0 == 1);          // one listing, then hash hits
  CHECK(hits1 - hits0 >= 99);
  // A file created in the directory behind the cache's back (the dir mtime changes) is found after the change,
  // and a file made through the write gate also.
  snprintf(p, sizeof p, "%s/gfx/NewFile.Gnf", app); { FILE *f = fopen(p, "w"); fclose(f); }
  { snprintf(p, sizeof p, "%s/gfx", app); struct timeval tv[2]; gettimeofday(&tv[0], 0); tv[0].tv_sec -= 50; tv[1] = tv[0]; utimes(p, tv); }
  CHECK(ps4_vfs_resolve("/app0/gfx/newfile.gnf", h, sizeof h) == 0); CHECK(HAS(h, "NewFile.Gnf"));
  // unlink: the name is gone from the next lookup (resolves to the lowercase request path again).
  snprintf(p, sizeof p, "%s/gfx/NewFile.Gnf", app); unlink(p);
  { snprintf(p, sizeof p, "%s/gfx", app); struct timeval tv[2]; gettimeofday(&tv[0], 0); tv[0].tv_sec -= 40; tv[1] = tv[0]; utimes(p, tv); }
  CHECK(ps4_vfs_resolve("/app0/gfx/newfile.gnf", h, sizeof h) == 0); CHECK(HAS(h, "newfile.gnf"));
  // Through the gate: userdata directory create / rename / unlink are seen at once (generation bump).
  char ddir[900]; snprintf(ddir, sizeof ddir, "%s/ud_dc", tmp);
  int fd = ps4_vfs_open("/data/Save1.BIN", O_WRONLY | O_CREAT, 0644); CHECK(fd >= 0); if (fd >= 0) close(fd);
  CHECK(ps4_vfs_resolve("/data/save1.bin", h, sizeof h) == 0); CHECK(HAS(h, "Save1.BIN"));
  CHECK(ps4_vfs_rename("/data/Save1.BIN", "/data/Other.BIN") == 0);
  CHECK(ps4_vfs_resolve("/data/other.bin", h, sizeof h) == 0); CHECK(HAS(h, "Other.BIN"));
  CHECK(ps4_vfs_resolve("/data/save1.bin", h, sizeof h) == 0); CHECK(HAS(h, "save1.bin"));
  CHECK(ps4_vfs_unlink("/data/Other.BIN") == 0);
  CHECK(ps4_vfs_resolve("/data/other.bin", h, sizeof h) == 0); CHECK(HAS(h, "other.bin"));
  // Two case variants in one directory: the first in readdir order wins, like the uncached loop.
  snprintf(p, sizeof p, "%s/gfx/dup.txt", app); { FILE *f = fopen(p, "w"); fclose(f); }
  snprintf(p, sizeof p, "%s/gfx/DUP.TXT", app); { FILE *f = fopen(p, "w"); fclose(f); }
  { snprintf(p, sizeof p, "%s/gfx", app); struct timeval tv[2]; gettimeofday(&tv[0], 0); tv[0].tv_sec -= 30; tv[1] = tv[0]; utimes(p, tv); }
  char d_on[1024], d_off[1024];
  ps4_vfs_dircache_set(1); CHECK(ps4_vfs_resolve("/app0/gfx/Dup.Txt", d_on, sizeof d_on) == 0);
  ps4_vfs_dircache_set(0); CHECK(ps4_vfs_resolve("/app0/gfx/Dup.Txt", d_off, sizeof d_off) == 0);
  CHECK(strcmp(d_on, d_off) == 0);
  // Racy directory (modified just now): never cached, still correct.
  snprintf(p, sizeof p, "%s/gfx/Fresh.Gnf", app); { FILE *f = fopen(p, "w"); fclose(f); }
  ps4_vfs_dircache_set(1);
  CHECK(ps4_vfs_resolve("/app0/gfx/fresh.gnf", h, sizeof h) == 0); CHECK(HAS(h, "Fresh.Gnf"));
  ps4_vfs_dircache_set(0);
  printf("kernelfs: dircache ok\n");
  unsetenv("NPS5_USERDATA_DIR"); unsetenv("NPS5_TITLE_ID");
}

static void test_locale_gate(const char *tmp) {
  char app[700], p[800];
  snprintf(app, sizeof app, "%s/dump_loc", tmp); mkdir(app, 0755);
  snprintf(p, sizeof p, "%s/loc", app); mkdir(p, 0755);
  snprintf(p, sizeof p, "%s/loc/en.res", app); FILE *f = fopen(p, "w"); fputs("x", f); fclose(f);
  char ud[800]; snprintf(ud, sizeof ud, "%s/ud_loc", tmp);
  setenv("NPS5_USERDATA_DIR", ud, 1); setenv("NPS5_TITLE_ID", "TESTLOC00", 1);
  ps4_vfs_init(app);
  unsetenv("NPS5_PAK_LOCALE_FALLBACK");
  struct stat st;
  int fd = ps4_vfs_open("/app0/loc/en_US.res", O_RDONLY, 0); CHECK(fd < 0 && errno == ENOENT); if (fd >= 0) close(fd);
  CHECK(ps4_vfs_stat("/app0/loc/en_US.res", &st) != 0); CHECK(ps4_vfs_access("/app0/loc/en_US.res", F_OK) != 0);
  fd = ps4_vfs_open("/app0/loc/en.res", O_RDONLY, 0); CHECK(fd >= 0); if (fd >= 0) close(fd);
  setenv("NPS5_PAK_LOCALE_FALLBACK", "1", 1);
  fd = ps4_vfs_open("/app0/loc/en_US.res", O_RDONLY, 0); CHECK(fd >= 0); if (fd >= 0) close(fd);
  unsetenv("NPS5_PAK_LOCALE_FALLBACK");
  printf("kernelfs: disk locale fallback gated ok\n");
  unsetenv("NPS5_USERDATA_DIR"); unsetenv("NPS5_TITLE_ID");
}

// More overlay files than the old fixed 96-entry table: all must be visible; an over-long path is reported, not mounted.
static void test_overlay_many(const char *tmp) {
  char app[700], ud[700], p[900];
  snprintf(app, sizeof app, "%s/dump_ov", tmp); mkdir(app, 0755);
  snprintf(ud, sizeof ud, "%s/ud_ov", tmp); mkdir(ud, 0755);
  snprintf(p, sizeof p, "%s/overlay", ud); mkdir(p, 0755);
  snprintf(p, sizeof p, "%s/overlay/Engine", ud); mkdir(p, 0755);
  for (int i = 0; i < 300; i++) {
    snprintf(p, sizeof p, "%s/overlay/Engine/f%03d.ini", ud, i);
    FILE *f = fopen(p, "w"); fprintf(f, "v%d", i); fclose(f);
  }
  setenv("NPS5_USERDATA_DIR", ud, 1); setenv("NPS5_TITLE_ID", "TESTOV000", 1);
  ps4_vfs_init(app);
  int ok = 0;
  for (int i = 0; i < 300; i++) {
    char g[100], buf[16] = {0}; snprintf(g, sizeof g, "/APP0/engine/F%03d.INI", i);
    int fd = ps4_vfs_open(g, O_RDONLY, 0);
    if (fd >= 0) { ssize_t n = read(fd, buf, sizeof buf - 1); char want[16]; snprintf(want, sizeof want, "v%d", i); if (n > 0 && !strcmp(buf, want)) ok++; close(fd); }
  }
  CHECK(ok == 300);
  CHECK(ps4_vfs_mount("/zz/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "/tmp") == 0); // long guest prefixes are kept whole
  printf("kernelfs: overlay with 300 files ok (%d visible)\n", ok);
  unsetenv("NPS5_USERDATA_DIR"); unsetenv("NPS5_TITLE_ID");
}

// /temp0 is per process: another instance's directory is not wiped, a dead process's is swept.
static void test_temp_per_process(const char *tmp) {
  char app[700], ud[700], live[900], dead[900], f1[1000], f2[1000];
  snprintf(app, sizeof app, "%s/dump_tmp", tmp); mkdir(app, 0755);
  snprintf(ud, sizeof ud, "%s/ud_tmp", tmp); mkdir(ud, 0755);
  char t[800]; snprintf(t, sizeof t, "%s/temp", ud); mkdir(t, 0755);
  snprintf(live, sizeof live, "%s/temp/p%d", ud, (int)getppid()); mkdir(live, 0755);
  snprintf(dead, sizeof dead, "%s/temp/p2147483", ud); mkdir(dead, 0755);
  snprintf(f1, sizeof f1, "%s/keep.txt", live); FILE *f = fopen(f1, "w"); fputs("x", f); fclose(f);
  snprintf(f2, sizeof f2, "%s/gone.txt", dead); f = fopen(f2, "w"); fputs("x", f); fclose(f);
  setenv("NPS5_USERDATA_DIR", ud, 1); unsetenv("NPS5_TITLE_ID");
  ps4_vfs_init(app); // unknown title id: still no sharing with another instance
  CHECK(access(f1, F_OK) == 0);
  CHECK(access(f2, F_OK) != 0);
  int fd = ps4_vfs_open("/temp0/mine.txt", O_WRONLY | O_CREAT, 0644); CHECK(fd >= 0); if (fd >= 0) close(fd);
  char mine[1000]; snprintf(mine, sizeof mine, "%s/temp/p%d/mine.txt", ud, (int)getpid()); CHECK(access(mine, F_OK) == 0);
  printf("kernelfs: per-process /temp0 ok\n");
  unsetenv("NPS5_USERDATA_DIR");
}

static void on_alarm(int s) { (void)s; }
static void test_usleep_resumes(void) {
  struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_alarm; sigemptyset(&sa.sa_mask); sa.sa_flags = 0; // no SA_RESTART
  sigaction(SIGALRM, &sa, NULL);
  struct itimerval it = {{0, 0}, {0, 20000}};
  struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
  setitimer(ITIMER_REAL, &it, NULL);
  reset(); C->rdi = 200000; CALL(shim_sceKernelUsleep);
  clock_gettime(CLOCK_MONOTONIC, &b);
  double ms = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
  CHECK(ms >= 195.0 && ms < 400.0);
  if (ms < 195.0) printf("  usleep(200ms) returned after %.1f ms\n", ms);
  printf("kernelfs: sceKernelUsleep resumes after a signal ok (%.1f ms)\n", ms);
}

// NPS5_TRACE_CONDWAIT trace of sceKernelUsleep (review #14): the callers block belongs to the threshold test (it used to run on every
// sleep) and a wild rbp must not fault the runtime. NPS5_TRACE_CONDWAIT=50 is set in main before the first sleep (the value is cached).
static void test_usleep_trace(const char *tmp) {
  char lf[900]; snprintf(lf, sizeof lf, "%s/usleep_trace.txt", tmp);
  fflush(stderr);
  int saved = dup(2), lfd = open(lf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  dup2(lfd, 2); close(lfd);
  reset(); C->rbp = 0x100000000ull; C->rdi = 1000; CALL(shim_sceKernelUsleep);   // 1 ms: below the threshold, wild rbp
  reset(); C->rbp = 0x100000000ull; C->rdi = 60000; CALL(shim_sceKernelUsleep);  // 60 ms: reported; the wild rbp ends the chain
  fflush(stderr); dup2(saved, 2); close(saved);
  char buf[4096] = {0}; FILE *f = fopen(lf, "r"); size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0; if (f) fclose(f); buf[n] = 0;
  int reports = 0, callers = 0; for (char *p = buf; (p = strstr(p, "[usleep] req_ms")); p++) reports++;
  for (char *p = buf; (p = strstr(p, "callers:")); p++) callers++;
  CHECK(reports == 1 && callers == 1);   // only the sleeps that pass the threshold
  printf("kernelfs: usleep trace braces ok\n");
}


// ---- pak scan: no stat() per file, each root walked once; load accounting ----
#include <stdatomic.h>
#include <sys/wait.h>
static void put_file(const char *dir, const char *name, const char *text) {
  char p[1200]; snprintf(p, sizeof p, "%s/%s", dir, name);
  FILE *f = fopen(p, "w"); CHECK(f != NULL); if (f) { fputs(text, f); fclose(f); }
}
static void test_pak_scan(const char *tmp) {
  // the decision per directory entry, from readdir's type and the name
  CHECK(ps4_pak_scan_class(DT_DIR, "data") == PS4_PAK_SCAN_DIR && ps4_pak_scan_class(DT_DIR, "a.pak") == PS4_PAK_SCAN_DIR);
  CHECK(ps4_pak_scan_class(DT_REG, "a.bin") == PS4_PAK_SCAN_SKIP && ps4_pak_scan_class(DT_REG, "noext") == PS4_PAK_SCAN_SKIP);
  CHECK(ps4_pak_scan_class(DT_REG, "a.pak") == PS4_PAK_SCAN_STAT && ps4_pak_scan_class(DT_REG, "A.PAK") == PS4_PAK_SCAN_STAT);
  CHECK(ps4_pak_scan_class(DT_REG, ".pak") == PS4_PAK_SCAN_SKIP && ps4_pak_scan_class(DT_REG, "xpak") == PS4_PAK_SCAN_SKIP);
  CHECK(ps4_pak_scan_class(DT_LNK, "a.pak") == PS4_PAK_SCAN_STAT);
  CHECK(ps4_pak_scan_class(DT_LNK, "linkdir") == PS4_PAK_SCAN_STAT);   // may point at a directory
  CHECK(ps4_pak_scan_class(DT_LNK, "a.bin") == PS4_PAK_SCAN_SKIP);     // a link tree over an image: no stat per file
  // a file system that does not report types: every entry is looked at, as before
  CHECK(ps4_pak_scan_class(DT_UNKNOWN, "a.bin") == PS4_PAK_SCAN_STAT && ps4_pak_scan_class(DT_UNKNOWN, "dir") == PS4_PAK_SCAN_STAT &&
        ps4_pak_scan_class(DT_UNKNOWN, "a.pak") == PS4_PAK_SCAN_STAT);
  CHECK(ps4_pak_scan_class(DT_FIFO, "a.pak") == PS4_PAK_SCAN_SKIP && ps4_pak_scan_class(DT_SOCK, "x") == PS4_PAK_SCAN_SKIP);

  char root[1100], sub[1150], out[1100], p[1300], q[1300];
  snprintf(root, sizeof root, "%s/pakscan", tmp); mkdir(root, 0755);
  snprintf(sub, sizeof sub, "%s/sub", root); mkdir(sub, 0755);
  snprintf(out, sizeof out, "%s/pakscan_outside", tmp); mkdir(out, 0755);
  snprintf(p, sizeof p, "%s/sce_sys", root); mkdir(p, 0755); put_file(p, "skipped.pak", "x");
  for (int i = 0; i < 300; i++) { char n[32]; snprintf(n, sizeof n, "f%03d.bin", i); put_file(i % 2 ? root : sub, n, "data"); }
  put_file(root, "real.pak", "not a pak: the parser refuses it, the scan still found it");
  put_file(out, "in.pak", "x"); put_file(out, "other.bin", "x");
  snprintf(p, sizeof p, "%s/linkdir", root); CHECK(symlink(out, p) == 0);           // a linked directory is walked
  snprintf(p, sizeof p, "%s/link.d", root); CHECK(symlink(out, p) == 0);            // ... unless its name has a '.'
  snprintf(q, sizeof q, "%s/other.bin", out);
  snprintf(p, sizeof p, "%s/l.pak", root); CHECK(symlink(q, p) == 0);               // a linked pak
  for (int i = 0; i < 50; i++) { snprintf(p, sizeof p, "%s/lf%02d.bin", root, i); CHECK(symlink(q, p) == 0); }

  ps4_pak_destroy();
  uint64_t d0, s0, k0, d1, s1, k1;
  ps4_pak_scan_counters(&d0, &s0, &k0);
  ps4_pak_init(root);
  ps4_pak_scan_counters(&d1, &s1, &k1);
  CHECK(d1 - d0 == 3);   // root, sub, linkdir; not sce_sys, not link.d
  CHECK(k1 - k0 == 3);   // real.pak, l.pak, linkdir/in.pak
  CHECK(s1 - s0 == 4);   // the three paks and linkdir: none of the 350 other files and links
  ps4_pak_init(root);    // the guest thread inits again: the image is not walked twice
  ps4_pak_init(root);
  ps4_pak_scan_counters(&d0, &s0, &k0);
  CHECK(d0 == d1 && s0 == s1 && k0 == k1);
  ps4_pak_init(sub);     // another root is walked
  ps4_pak_scan_counters(&d1, &s1, &k1);
  CHECK(d1 - d0 == 1 && s1 == s0 && k1 == k0);
  ps4_pak_destroy();     // forgotten with the index
  ps4_pak_init(sub);
  ps4_pak_scan_counters(&d0, &s0, &k0);
  CHECK(d0 - d1 == 1);
  ps4_pak_destroy();
  printf("kernelfs: pak scan ok\n");
}

// The counters a loading card reads are kept without any switch; the files are written only when asked for.
extern _Atomic unsigned long long nps5_vfs_bytes_read;
extern _Atomic int nps5_vfs_phase;
static void load_accounting_body(const char *tmp, int expect_files) {
  char app[1100], p[1300];
  snprintf(app, sizeof app, "%s/acct", tmp); mkdir(app, 0755);
  snprintf(p, sizeof p, "%s/levels", app); mkdir(p, 0755);
  snprintf(p, sizeof p, "%s/levels/acct_scene_%d", app, expect_files); mkdir(p, 0755);
  put_file(p, "a.bin", "0123456789abcdef0123456789abcdef");   // 32 bytes
  ps4_vfs_init(app);
  const unsigned long long b0 = atomic_load(&nps5_vfs_bytes_read);
  const int ph0 = atomic_load(&nps5_vfs_phase);
  char g[200]; snprintf(g, sizeof g, "/app0/levels/acct_scene_%d/a.bin", expect_files);
  char buf[64];
  int fd = ps4_vfs_open(g, O_RDONLY, 0); CHECK(fd >= 0);
  CHECK(ps4_vfs_read(fd, buf, 10) == 10);
  CHECK(ps4_vfs_pread(fd, buf, 8, 20) == 8);
  CHECK(ps4_vfs_read(fd, buf, 64) == 22);
  CHECK(ps4_vfs_read(fd, buf, 64) == 0);       // end of file: nothing counted
  CHECK(ps4_vfs_pread(fd, buf, 4, -1) == -1);  // an error: nothing counted
  CHECK(ps4_vfs_close(fd) == 0);
  CHECK(atomic_load(&nps5_vfs_bytes_read) - b0 == 40);
  CHECK(atomic_load(&nps5_vfs_phase) == ph0 + 1);   // the scene's first open started a phase
  fd = ps4_vfs_open(g, O_RDONLY, 0); CHECK(fd >= 0);
  CHECK(ps4_vfs_read(fd, buf, 4) == 4);
  CHECK(ps4_vfs_close(fd) == 0);
  CHECK(atomic_load(&nps5_vfs_phase) == ph0 + 1);
  nps5_vfs_mark_first_picture();
}
static void test_load_accounting(const char *tmp, const char *self) {
  char log[1100], prof[1100], line[900];
  snprintf(log, sizeof log, "%s/readlog.tsv", tmp);
  snprintf(prof, sizeof prof, "%s/loadprofile.tsv", tmp);
  load_accounting_body(tmp, 0);
  CHECK(access(log, F_OK) != 0 && access(prof, F_OK) != 0);   // nothing is written unless asked for
  // The switches are read when the process starts: a child of this test with them set.
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) {
    setenv("NPS5_VFS_READLOG", log, 1);
    setenv("NPS5_VFS_LOADPROFILE", prof, 1);
    execl(self, self, "--load-accounting", tmp, (char *)NULL);
    _exit(127);
  }
  int st = -1;
  CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
  FILE *f = fopen(log, "r"); CHECK(f != NULL);
  int rows = 0, good = 0;
  while (f && fgets(line, sizeof line, f)) {
    unsigned long long bytes, size, opens, reads, maxend; double t0, t1; char path[400];
    if (sscanf(line, "%llu\t%llu\t%llu\t%llu\t%llu\t%lf\t%lf\t%399s", &bytes, &size, &opens, &reads, &maxend, &t0, &t1, path) != 8) continue;
    rows++;
    if (!strcmp(path, "/app0/levels/acct_scene_1/a.bin") && bytes == 44 && size == 32 && opens == 2 && reads == 4 && maxend == 32 && t1 >= t0) good++;
    CHECK(strstr(path, tmp) == NULL);   // guest paths only, never where the image is on the host
  }
  if (f) fclose(f);
  CHECK(rows == 1 && good == 1);
  f = fopen(prof, "r"); CHECK(f != NULL);
  int boot = 0, scene = 0;
  while (f && fgets(line, sizeof line, f)) {
    int ph; char name[100]; double t, pic; unsigned long long bytes;
    if (sscanf(line, "%d\t%99s\t%lf\t%llu\t%lf", &ph, name, &t, &bytes, &pic) != 5) continue;
    if (ph == 0 && !strcmp(name, "boot")) boot++;
    if (!strcmp(name, "acct_scene_1") && bytes == 44 && pic >= 0) scene++;
  }
  if (f) fclose(f);
  CHECK(boot == 1 && scene == 1);
  printf("kernelfs: load accounting ok\n");
}

int main(int argc, char **argv) {
  if (argc == 3 && !strcmp(argv[1], "--load-accounting")) {   // the child of test_load_accounting
    C = calloc(1, sizeof(GuestContext));
    load_accounting_body(argv[2], 1);
    return fails ? 1 : 0;   // the files are written at exit
  }
  setenv("NPS5_TRACE_CONDWAIT", "50", 1);
  char tmp[512]; snprintf(tmp, sizeof tmp, "%s/kernelfs_unit.XXXXXX", getenv("KERNELFS_TMP") ? getenv("KERNELFS_TMP") : "/tmp");
  if (!mkdtemp(tmp)) return 2;
  C = calloc(1, sizeof(GuestContext));
  C->mem_size = ARENA;
  C->mem_base = calloc(1, ARENA);
  C->fs_base = 0x2000;
  C->rsp = 0x6000;
  { char ud0[700]; snprintf(ud0, sizeof ud0, "%s.ud0", tmp); setenv("NPS5_USERDATA_DIR", ud0, 1); setenv("NPS5_TITLE_ID", "TESTKFS00", 1); }
  ps4_vfs_init(tmp);
  test_sce_codes(tmp);
  test_pak_fallback();
  test_pak_scan(tmp);
  test_load_accounting(tmp, argv[0]);
  test_erofs_table(tmp);
  test_userdata_fail_closed(tmp);
  test_raw_syscall_virtual_fd(tmp);
  alarm(60); test_gate_attacks(tmp); alarm(0); // a hang (the unfixed gate) is a failure, not a stall
  test_raw_write(tmp);
  test_locale_gate(tmp);
  test_dircache(tmp);
  test_overlay_many(tmp);
  test_temp_per_process(tmp);
  test_usleep_resumes();
  test_usleep_trace(tmp);
  test_param_json(tmp);
  char cmd[700]; snprintf(cmd, sizeof cmd, "rm -rf %s", tmp); system(cmd);
  snprintf(cmd, sizeof cmd, "rm -rf %s.ud0 %s.dt", tmp, tmp); system(cmd);
  printf(fails ? "kernelfs_unit: %d FAILURES\n" : "kernelfs_unit: all passed\n", fails);
  return fails ? 1 : 0;
}
