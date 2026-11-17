// Host unit test for the HLE libc shims (pkg/runtime/modules/libc). Builds natively with
// tools/libc_unit.sh: a fake GuestContext over a malloc'ed arena, shims called directly.
// No guest code runs; comparator callbacks are host functions in the dispatch table.
#include "nps5_libc.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include "ps4_vfs.h"

// ---- runtime stubs the module links against
uint8_t g_nps5_level[NPS5_S_COUNT];
void nps5_log(int s, int l, const char *fmt, ...) { (void)s; (void)l; (void)fmt; }
recomp_fn_t **g_dispatch_l1[DISPATCH_LEVEL_SIZE];
uint64_t g_recomp_call_model = 0; // flat call model flag read by nps5_libc_call_guest
uintptr_t recomp_dispatch_miss(GuestContext *ctx, uint64_t rip, uint32_t reason) { (void)ctx; (void)rip; (void)reason; return 0; }
void ps4_host_quit(int c) { exit(c); }
static uint64_t bump = 0x100000;
uint64_t recomp_vm_alloc_named_aligned(GuestContext *c, size_t size, size_t al, int prot, int fl, const char *n) {
  (void)c; (void)prot; (void)fl; (void)n;
  bump = (bump + al - 1) & ~(uint64_t)(al - 1);
  uint64_t r = bump; bump += (size + al - 1) & ~(uint64_t)(al - 1); return r;
}
int recomp_vm_free(GuestContext *c, uint64_t a, size_t s) { (void)c; (void)a; (void)s; return 0; }

#define ARENA (256ULL << 20)
static GuestContext *C;
static void reg_host_fn(uint64_t addr, recomp_fn_t fn) {
  if (!g_dispatch_l1[0]) g_dispatch_l1[0] = calloc(DISPATCH_LEVEL_SIZE, sizeof(void *));
  if (!g_dispatch_l1[0][(addr >> 16) & 0xffff]) g_dispatch_l1[0][(addr >> 16) & 0xffff] = calloc(DISPATCH_LEVEL_SIZE, sizeof(void *));
  g_dispatch_l1[0][(addr >> 16) & 0xffff][addr & 0xffff] = fn;
}
// "guest" comparator for long: compares the longs at ctx->rdi / ctx->rsi, returns via ret like RET
static int g_cmp_calls;
static void host_cmp(GuestContext *ctx) {
  int64_t a = *(int64_t *)(ctx->mem_base + ctx->rdi), b = *(int64_t *)(ctx->mem_base + ctx->rsi);
  g_cmp_calls++;
  assert((ctx->rsp & 15) == 8);  // SysV: rsp+8 is 16-aligned at function entry
  ctx->rax = (uint64_t)(int64_t)(a < b ? -1 : a > b);
  ctx->rsp += 8;  // RET
}
static uint64_t gstr(const char *s) {  // copy a host string into scratch guest memory
  static uint64_t cur = 0x40000;
  uint64_t r = cur; strcpy((char *)C->mem_base + r, s); cur += (strlen(s) + 16) & ~15ULL; return r;
}
static uint64_t scratch(size_t n) { static uint64_t cur = 0x800000; uint64_t r = cur; cur += (n + 15) & ~15ULL; return r; }
#define H(a) ((char *)C->mem_base + (a))
#define CALL(fn) do { uint64_t rsp0 = C->rsp; fn(C); assert(C->rsp == rsp0 + 8); C->rsp = rsp0; } while (0)
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static int fails;

void shim_malloc(GuestContext *); void shim_free(GuestContext *); void shim_realloc(GuestContext *);
void shim_calloc(GuestContext *); void shim_aligned_alloc(GuestContext *); void shim_malloc_usable_size(GuestContext *);
void shim_div(GuestContext *); void shim_ldiv(GuestContext *); void shim_lldiv(GuestContext *);
void shim_strstr(GuestContext *); void shim_strtok_r(GuestContext *); void shim_strtol(GuestContext *);
void shim_strtod(GuestContext *); void shim_qsort(GuestContext *); void shim_bsearch(GuestContext *);
void shim_snprintf(GuestContext *); void shim_vsnprintf(GuestContext *); void shim_sscanf(GuestContext *);
void shim_frexp(GuestContext *); void shim_modf(GuestContext *); void shim_lround(GuestContext *);
void shim_toupper(GuestContext *); void shim_isxdigit(GuestContext *); void shim_strchr(GuestContext *);
void shim_strrchr(GuestContext *); void shim_memchr(GuestContext *); void shim_strncmp(GuestContext *);
void shim_strdup(GuestContext *); void shim_strlcpy(GuestContext *); void shim_atan2(GuestContext *);
void shim_gmtime(GuestContext *); void shim_fopen(GuestContext *); void shim_remove(GuestContext *); void shim_fclose(GuestContext *); void shim_rand(GuestContext *); void shim_srand(GuestContext *);

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  C = calloc(1, sizeof *C);
  C->mem_base = calloc(1, ARENA); C->mem_size = ARENA;
  C->fs_base = 0x200000;
  uint64_t stack = 0x3000000;
  C->rsp = stack;  // [rsp] = return address slot
  reg_host_fn(0x1000, host_cmp);

  // ---- heap
  C->rdi = 100; CALL(shim_malloc); uint64_t p = C->rax; CHECK(p && p % 16 == 0);
  memset(H(p), 0xAA, 100);
  C->rdi = p; CALL(shim_malloc_usable_size); CHECK(C->rax >= 100);
  C->rdi = p; C->rsi = 5000; CALL(shim_realloc); uint64_t q = C->rax; CHECK(q && (uint8_t)H(q)[99] == 0xAA);
  C->rdi = q; CALL(shim_free);
  C->rdi = 4; C->rsi = 8; CALL(shim_calloc); CHECK(C->rax && *(uint64_t *)H(C->rax) == 0);
  C->rdi = 4096; C->rsi = 10; CALL(shim_aligned_alloc); CHECK(C->rax % 4096 == 0);
  uint64_t al = C->rax; C->rdi = al; CALL(shim_free);
  C->rdi = 100; CALL(shim_malloc); CHECK(C->rax != 0);
  C->rdi = 1 << 20; CALL(shim_malloc); CHECK(C->rax != 0);  // large path

  // ---- div family: struct returns
  C->rdi = (uint64_t)(int64_t)-17; C->rsi = 5; CALL(shim_div);
  CHECK((int32_t)(C->rax & 0xffffffff) == -3 && (int32_t)(C->rax >> 32) == -2);
  C->rdi = (uint64_t)(int64_t)-1000000000000LL; C->rsi = 7; CALL(shim_ldiv);
  CHECK((int64_t)C->rax == -142857142857LL && (int64_t)C->rdx == -1);
  C->rdi = 1LL << 62; C->rsi = 3; CALL(shim_lldiv);
  CHECK((int64_t)C->rax == (1LL << 62) / 3 && (int64_t)C->rdx == (1LL << 62) % 3);

  // ---- strings
  C->rdi = gstr("hello world"); C->rsi = gstr("o w"); CALL(shim_strstr); CHECK(C->rax == C->rdi + 4);
  C->rdi = gstr("abcabc"); C->rsi = 'c'; CALL(shim_strchr); CHECK(C->rax == C->rdi + 2);
  C->rdi = gstr("abcabc"); C->rsi = 'c'; CALL(shim_strrchr); CHECK(C->rax == C->rdi + 5);
  C->rdi = gstr("abcabc"); C->rsi = 'z'; CALL(shim_strchr); CHECK(C->rax == 0);
  C->rdi = gstr("abc"); C->rsi = 'c'; C->rdx = 2; CALL(shim_memchr); CHECK(C->rax == 0);
  C->rdi = gstr("abcd"); C->rsi = gstr("abxd"); C->rdx = 2; CALL(shim_strncmp); CHECK((int32_t)C->rax == 0);
  {
    uint64_t s = gstr("  a,b;;c "), sv = scratch(8), d = gstr(",; ");
    const char *want[] = {"a", "b", "c"};
    C->rdi = s;
    for (int i = 0; i < 3; i++) {
      C->rdi = i ? 0 : s; C->rsi = d; C->rdx = sv; CALL(shim_strtok_r);
      CHECK(C->rax && !strcmp(H(C->rax), want[i]));
    }
    C->rdi = 0; C->rsi = d; C->rdx = sv; CALL(shim_strtok_r); CHECK(C->rax == 0);
  }
  C->rdi = gstr("dup"); CALL(shim_strdup); CHECK(!strcmp(H(C->rax), "dup"));
  { uint64_t d = scratch(8); C->rdi = d; C->rsi = gstr("abcdefghij"); C->rdx = 4; CALL(shim_strlcpy); CHECK(C->rax == 10 && !strcmp(H(d), "abc")); }
  C->rdi = 'q'; CALL(shim_toupper); CHECK(C->rax == 'Q');
  C->rdi = 'g'; CALL(shim_isxdigit); CHECK(C->rax == 0);

  // ---- strto*
  { uint64_t s = gstr("  -12.5e-1xyz"), e = scratch(8);
    C->rdi = s; C->rsi = e; CALL(shim_strtod);
    CHECK(C->xmm[0].f64[0] == -1.25 && *(uint64_t *)H(e) == s + 10); }
  { uint64_t s = gstr("0x1fz"), e = scratch(8);
    C->rdi = s; C->rsi = e; C->rdx = 16; CALL(shim_strtol); CHECK((int64_t)C->rax == 31 && *(uint64_t *)H(e) == s + 4); }
  { uint64_t s = gstr("99999999999999999999999"), e = scratch(8);
    C->rdi = s; C->rsi = e; C->rdx = 10; CALL(shim_strtol); CHECK((int64_t)C->rax == INT64_MAX && *(int *)H(C->fs_base + 0x100) == 34); }

  // ---- qsort / bsearch with guest comparator callbacks
  {
    int64_t v[] = {5, -2, 99, 0, 17, 17, -50, 3, 8, 12, 1, 1, 70, -3, 40, 41, 42, 2, 6, 7, 9, 11, 13, 14, 15, 16, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36};
    int n = sizeof v / sizeof v[0];
    uint64_t a = scratch(sizeof v); memcpy(H(a), v, sizeof v);
    C->rdi = a; C->rsi = n; C->rdx = 8; C->rcx = 0x1000; CALL(shim_qsort);
    int64_t *r = (int64_t *)H(a); int ok = 1;
    for (int i = 1; i < n; i++) ok &= r[i - 1] <= r[i];
    CHECK(ok); CHECK(g_cmp_calls > 0);
    int64_t key = 17; uint64_t k = scratch(8); *(int64_t *)H(k) = key;
    C->rdi = k; C->rsi = a; C->rdx = n; C->rcx = 8; C->r8 = 0x1000; CALL(shim_bsearch);
    CHECK(C->rax && *(int64_t *)H(C->rax) == 17);
    key = 1000; *(int64_t *)H(k) = key;
    C->rdi = k; C->rsi = a; C->rdx = n; C->rcx = 8; C->r8 = 0x1000; CALL(shim_bsearch); CHECK(C->rax == 0);
  }

  // ---- printf engine: register-passed varargs
  {
    uint64_t buf = scratch(256), fmt = gstr("%d|%5d|%-5d|%05d|%x|%s|%c|%lld|%u|%5.2f|%e");
    // snprintf(buf, 256, fmt, ...) : gp args rdi,rsi,rdx then rcx,r8,r9 + stack ; doubles in xmm0..
    // Layout it manually: first vararg is rcx, then r8, r9, then stack (rsp+8..).
    C->rdi = buf; C->rsi = 256; C->rdx = fmt;
    C->rcx = (uint64_t)(int64_t)-42; C->r8 = 42; C->r9 = 42;
    uint64_t *st = (uint64_t *)H(C->rsp + 8);
    st[0] = 42; st[1] = 0x5eef; st[2] = gstr("hi"); st[3] = 'z'; st[4] = (uint64_t)1099511627776LL; st[5] = 4000000000u;
    C->xmm[0].f64[0] = 3.14159; C->xmm[1].f64[0] = 12345.678;
    C->rax = 2;
    CALL(shim_snprintf);
    CHECK(!strcmp(H(buf), "-42|   42|42   |00042|5eef|hi|z|1099511627776|4000000000| 3.14|1.234568e+04"));
    CHECK((int32_t)C->rax == (int)strlen(H(buf)));
  }
  // vsnprintf with a guest va_list (gp_offset/fp_offset/overflow/reg_save), mixing reg and overflow
  {
    uint64_t buf = scratch(256), fmt = gstr("%d %s %f %ld %d %d %d %.1f"), regsave = scratch(176), ovf = scratch(64), va = scratch(24);
    uint64_t *gp = (uint64_t *)H(regsave);
    gp[0] = 7; gp[1] = gstr("str"); gp[2] = 1111111111111LL; gp[3] = 1; gp[4] = 2; gp[5] = 3;  // 6 gp slots (48 bytes)
    *(double *)H(regsave + 48) = 2.5;       // xmm0
    *(double *)H(regsave + 48 + 16) = 9.75; // xmm1
    *(uint64_t *)H(ovf) = 4;                // 7th integer arg -> overflow area
    // consumption order: %d=gp0 %s=gp1 %f=fp0 %ld=gp2 %d=gp3 %d=gp4 %d=gp5 %.1f=fp1
    uint32_t *v = (uint32_t *)H(va); v[0] = 0; v[1] = 48; *(uint64_t *)H(va + 8) = ovf; *(uint64_t *)H(va + 16) = regsave;
    C->rdi = buf; C->rsi = 256; C->rdx = fmt; C->rcx = va;
    CALL(shim_vsnprintf);
    CHECK(!strcmp(H(buf), "7 str 2.500000 1111111111111 1 2 3 9.8"));
    CHECK(v[0] == 48 && v[1] == 48 + 32);
    // truncation: returns full length, writes cap-1 bytes
    uint64_t b2 = scratch(16); memset(H(b2), 'X', 16);
    v[0] = 0; v[1] = 48;
    C->rdi = b2; C->rsi = 4; C->rdx = fmt; C->rcx = va; CALL(shim_vsnprintf);
    CHECK((int32_t)C->rax == 38 && !strcmp(H(b2), "7 s") && H(b2)[4] == 'X');
    // overflow-area for the 7th integer: bump consumed beyond reg save
    uint64_t fmt2 = gstr("%d%d%d%d%d%d%d");
    v[0] = 0; v[1] = 48;
    C->rdi = buf; C->rsi = 256; C->rdx = fmt2; C->rcx = va; CALL(shim_vsnprintf);
    CHECK(H(buf)[strlen(H(buf)) - 1] == '4');  // 7th integer comes from the overflow area
    CHECK(*(uint64_t *)H(va + 8) == ovf + 8);
  }
  // ---- sscanf
  {
    uint64_t s = gstr("  42 -7 ff 2.5 word [abc]rest 12345"), f = gstr("%d %hd %x %lf %s [%[^]]]%*s %lld");
    uint64_t i = scratch(8), h = scratch(8), x = scratch(8), d = scratch(8), w = scratch(16), set = scratch(16), ll = scratch(8);
    C->rdi = s; C->rsi = f; C->rdx = i; C->rcx = h; C->r8 = x; C->r9 = d;
    uint64_t *st = (uint64_t *)H(C->rsp + 8); st[0] = w; st[1] = set; st[2] = ll;
    CALL(shim_sscanf);
    CHECK((int32_t)C->rax == 7);
    CHECK(*(int32_t *)H(i) == 42 && *(int16_t *)H(h) == -7 && *(int32_t *)H(x) == 255 && *(double *)H(d) == 2.5);
    CHECK(!strcmp(H(w), "word") && !strcmp(H(set), "abc") && *(int64_t *)H(ll) == 12345);
  }

  // ---- math out-pointers and conversions
  { uint64_t ep = scratch(8); C->xmm[0].f64[0] = 48.0; C->rdi = ep; CALL(shim_frexp); CHECK(C->xmm[0].f64[0] == 0.75 && *(int32_t *)H(ep) == 6); }
  { uint64_t ip = scratch(8); C->xmm[0].f64[0] = -7.625; C->rdi = ip; CALL(shim_modf); CHECK(C->xmm[0].f64[0] == -0.625 && *(double *)H(ip) == -7.0); }
  C->xmm[0].f64[0] = 2.5; CALL(shim_lround); CHECK((int64_t)C->rax == 3);
  C->xmm[0].f64[0] = 1.0; C->xmm[1].f64[0] = -2.0; CALL(shim_atan2); CHECK(fabs(C->xmm[0].f64[0] - atan2(1.0, -2.0)) < 1e-15);
  // ---- rand follows the FreeBSD sequence
  C->rdi = 1; CALL(shim_srand); CALL(shim_rand); CHECK((int32_t)C->rax == 16807);
  // ---- gmtime
  { uint64_t t = scratch(8); *(int64_t *)H(t) = 86400 * 365; C->rdi = t; CALL(shim_gmtime);
    struct tm *tm = (struct tm *)H(C->rax); CHECK(tm->tm_year == 71 && tm->tm_mon == 0 && tm->tm_mday == 1);
    CHECK(!strcmp((char *)C->mem_base + (uint64_t)(uintptr_t)tm->tm_zone, "UTC")); }

  // ---- fopen / remove against the real VFS: /app0 is read-only (the dump must never be written), /data is writable
  {
    char tmp[] = "/tmp/libc_unit_vfs.XXXXXX";
    CHECK(mkdtemp(tmp) != NULL);
    char app[600], ud[600], victim[700];
    snprintf(app, sizeof app, "%s/app", tmp); mkdir(app, 0755);
    snprintf(ud, sizeof ud, "%s/ud", tmp);
    snprintf(victim, sizeof victim, "%s/victim.txt", app);
    FILE *vf = fopen(victim, "w"); fputs("DATA", vf); fclose(vf);
    chmod(victim, 0644);
    struct stat before; stat(victim, &before);
    setenv("NPS5_USERDATA_DIR", ud, 1); setenv("NPS5_TITLE_ID", "TEST00000", 1);
    ps4_vfs_init(app);
    static const char *modes[] = {"w", "wb", "a", "r+", "w+", "ab+", "wx"};
    for (size_t i = 0; i < sizeof modes / sizeof *modes; i++) {
      C->rdi = gstr("/app0/victim.txt"); C->rsi = gstr(modes[i]); CALL(shim_fopen);
      CHECK(C->rax == 0 && *(int *)H(C->fs_base + 0x100) == EROFS);
      C->rdi = gstr("/APP0/Victim.txt"); C->rsi = gstr(modes[i]); CALL(shim_fopen); CHECK(C->rax == 0);
    }
    C->rdi = gstr("/app0/new.txt"); C->rsi = gstr("w"); CALL(shim_fopen); CHECK(C->rax == 0);
    snprintf(victim, sizeof victim, "%s/new.txt", app); CHECK(access(victim, F_OK) != 0);
    C->rdi = gstr("/app0/victim.txt"); CALL(shim_remove); CHECK((int32_t)C->rax == -1 && *(int *)H(C->fs_base + 0x100) == EROFS);
    C->rdi = gstr("victim.txt"); C->rsi = gstr("w"); CALL(shim_fopen); CHECK(C->rax == 0); // relative = the image
    // a read still works, and the file is untouched
    C->rdi = gstr("/app0/victim.txt"); C->rsi = gstr("rb"); CALL(shim_fopen); CHECK(C->rax != 0);
    if (C->rax) { C->rdi = C->rax; CALL(shim_fclose); }
    snprintf(victim, sizeof victim, "%s/victim.txt", app);
    struct stat after; CHECK(stat(victim, &after) == 0 && after.st_size == 4 && after.st_mtime == before.st_mtime && after.st_mode == before.st_mode);
    // the per-title /data root is writable
    C->rdi = gstr("/data/x.txt"); C->rsi = gstr("w"); CALL(shim_fopen); CHECK(C->rax != 0);
    if (C->rax) { C->rdi = C->rax; CALL(shim_fclose); }
    C->rdi = gstr("/data/x.txt"); CALL(shim_remove); CHECK((int32_t)C->rax == 0);
    char cmd[700]; snprintf(cmd, sizeof cmd, "rm -rf %s", tmp); system(cmd);
    unsetenv("NPS5_USERDATA_DIR"); unsetenv("NPS5_TITLE_ID");
  }

  printf(fails ? "libc_unit: %d FAILED\n" : "libc_unit: all passed\n", fails);
  return fails != 0;
}
