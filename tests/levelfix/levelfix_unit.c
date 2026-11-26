// Host unit test of pkg/runtime/kernel/vfs/nps5_levelfix.h with SYNTHETIC level data (invented names/values, no game
// content) and a fake file layer: gating by title, opt-out, sha mismatch skip, already applied, overlay present, wrong
// value / two matches skips, same-size served copy, game file never written. Built by tools/levelfix_unit.sh.
#include "nps5_levelfix.h"
#include <dirent.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define SUFFIX "|fakegate|fake_delay"
static char HASH[17];
static char SHA[65];
static nps5_lf_entry E;

static char *gate(const char *name, const char *cls, const char *delay) {
  char *b = malloc(1024);
  snprintf(b, 1024, "\t\t<LevelObject>\r\n\t\t\t<Name>%s</Name>\r\n\t\t\t<Lox>%s</Lox>\r\n\t\t\t<Attributes>\r\n"
           "\t\t\t\t<EnterDelay type=\"Float\">%s</EnterDelay>\r\n\t\t\t\t<LeaveDelay type=\"Float\">%s</LeaveDelay>\r\n"
           "\t\t\t</Attributes>\r\n\t\t</LevelObject>\r\n", name, cls, delay, delay);
  return b;
}
// a level with one unrelated object and one gate named n2 (default: ends with SUFFIX) carrying delay d2 (default 0.6)
static char *level(const char *n2, const char *d2) {
  char *a = gate("|r|coin", "pickup", "0.6"), *b = gate(n2 ? n2 : "|r|x" SUFFIX, "delay_trigger", d2 ? d2 : "0.6");
  char *o = malloc(8192);
  snprintf(o, 8192, "<?xml version=\"1.0\"?>\r\n<Level>\r\n%s%s</Level>\r\n", a, b);
  free(a); free(b);
  return o;
}

typedef struct { char app[600], ovl[600], cache[600]; int mounted; char mguest[600], mhost[1100]; int have_ovl; } Fake;
static const char *REL = "lvl/level.lvx";
static int f_resolve(void *u, const char *g, char *h, size_t n) {
  Fake *f = u; snprintf(h, n, "%s/%s", f->have_ovl ? f->ovl : f->app, REL); return g ? 0 : -1;
}
static int f_in_root(void *u, const char *h) { Fake *f = u; return strncmp(h, f->app, strlen(f->app)) == 0; }
static int f_cache(void *u, char *d, size_t n) { Fake *f = u; snprintf(d, n, "%s", f->cache); return 1; }
static int f_mount(void *u, const char *g, const char *h) {
  Fake *f = u; f->mounted++; snprintf(f->mguest, sizeof f->mguest, "%s", g); snprintf(f->mhost, sizeof f->mhost, "%s", h); return 0;
}

static void put(const char *root, const char *data) {
  char p[800]; snprintf(p, sizeof p, "%s/lvl", root); mkdir(p, 0755);
  snprintf(p, sizeof p, "%s/%s", root, REL);
  FILE *f = fopen(p, "wb"); fwrite(data, 1, strlen(data), f); fclose(f);
}
static char *slurp(const char *p, size_t *n) {
  char *b = NULL; size_t k = 0;
  if (!nps5_lf_read_file(p, &b, &k)) return NULL;
  b[k] = 0; if (n) *n = k; return b;
}

static int run(Fake *f, const char *title, const char *optout) {
  nps5_lf_env env = { title, optout, f_resolve, f_in_root, f_cache, f_mount, f };
  f->mounted = 0;
  return nps5_levelfix_run(&env, &E, 1);
}

static void setup(Fake *f, const char *tmp, const char *data) {
  memset(f, 0, sizeof *f);
  snprintf(f->app, sizeof f->app, "%s/app", tmp); snprintf(f->ovl, sizeof f->ovl, "%s/ovl", tmp); snprintf(f->cache, sizeof f->cache, "%s/cache", tmp);
  mkdir(tmp, 0755); mkdir(f->app, 0755); mkdir(f->ovl, 0755); mkdir(f->cache, 0755);
  put(f->app, data);
  nps5_lf_sha256_hex((const uint8_t *)data, strlen(data), SHA);
  E.src = SHA;
}

int main(void) {
  char t0[] = "/tmp/levelfix_unit.XXXXXX";
  if (!mkdtemp(t0)) return 2;
  char tmp[700]; int seq = 0;
#define FRESH() snprintf(tmp, sizeof tmp, "%s/c%d", t0, seq++)
  char h[65];
  nps5_lf_sha256_hex((const uint8_t *)"abc", 3, h);
  CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  nps5_lf_sha256_hex((const uint8_t *)"", 0, h);
  CHECK(!strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  nps5_lf_sha256_hex((const uint8_t *)SUFFIX, strlen(SUFFIX), h); memcpy(HASH, h, 16); HASH[16] = 0;
  E = (nps5_lf_entry){ "PPSATEST1", "lvl", REL, "", "delay_trigger", (int)strlen(SUFFIX), HASH, "EnterDelay", "0.6", "8.0" };
  Fake f;
  char *d = level(NULL, NULL); size_t dn = strlen(d);

  // applied: same size, only the value changed, game file untouched, mounted over the guest path
  FRESH(); setup(&f, tmp, d);
  CHECK(run(&f, "PPSATEST1", NULL) == 1 && f.mounted == 1);
  { size_t n; char *o = slurp(f.mhost, &n); CHECK(o && n == dn);
    char *want = strdup(d); char *p = strstr(want, "|r|x" SUFFIX); p = strstr(p, ">0.6<"); memcpy(p, ">8.0<", 5);
    CHECK(o && !memcmp(o, want, dn)); CHECK(o && strstr(o, "<LeaveDelay type=\"Float\">0.6<") != NULL); free(o); free(want);
    char gp[800]; snprintf(gp, sizeof gp, "%s/%s", f.app, REL); char *g = slurp(gp, NULL); CHECK(g && !strcmp(g, d)); free(g); }
  CHECK(!strcmp(f.mguest, "/app0/lvl/level.lvx"));
  { char first[1100]; snprintf(first, sizeof first, "%s", f.mhost); CHECK(run(&f, "PPSATEST1", NULL) == 1 && !strcmp(first, f.mhost)); } // cache reused

  // gating: another title untouched, no mount, no cache file
  FRESH(); setup(&f, tmp, d);
  CHECK(run(&f, "PPSAOTHER", NULL) == 0 && f.mounted == 0);
  CHECK(run(&f, "", NULL) == 0 && run(&f, NULL, NULL) == 0);
  { DIR *dr = opendir(f.cache); int n = 0; struct dirent *e; while ((e = readdir(dr))) if (e->d_name[0] != '.') n++; closedir(dr); CHECK(n == 0); }

  // opt-out
  CHECK(run(&f, "PPSATEST1", "0") == 0 && f.mounted == 0);
  CHECK(run(&f, "PPSATEST1", "1") == 1);

  // sha mismatch from the game folder: skipped
  FRESH(); setup(&f, tmp, d); E.src = "0000000000000000000000000000000000000000000000000000000000000000";
  CHECK(run(&f, "PPSATEST1", NULL) == 0 && f.mounted == 0);

  // already applied in the overlay file (launcher-fixed): nothing mounted, no failure
  FRESH(); setup(&f, tmp, d);
  { char *fx = level(NULL, "8.0"); put(f.ovl, fx); free(fx); f.have_ovl = 1; }
  CHECK(run(&f, "PPSATEST1", NULL) == 0 && f.mounted == 0);

  // overlay present with the old value and another sha (scenery-thinned): patched on top, sha not required
  FRESH(); setup(&f, tmp, d);
  { char *th = level(NULL, NULL); strcat(th, "<!-- thinned -->\r\n"); put(f.ovl, th); free(th); f.have_ovl = 1; }
  CHECK(run(&f, "PPSATEST1", NULL) == 1 && f.mounted == 1);
  { char *o = slurp(f.mhost, NULL); CHECK(o && strstr(o, "<!-- thinned -->") && strstr(o, ">8.0<")); free(o); }

  // wrong old value (another version), two matches, class mismatch, no such object: skipped
  char *bad;
  FRESH(); bad = level(NULL, "0.7"); setup(&f, tmp, bad); CHECK(run(&f, "PPSATEST1", NULL) == 0 && f.mounted == 0); free(bad);
  { char *a = level("|r|y" SUFFIX, NULL), *b = level(NULL, NULL); char *two = malloc(20000);
    snprintf(two, 20000, "%s%s", a, b); free(a); free(b);
    FRESH(); setup(&f, tmp, two); CHECK(run(&f, "PPSATEST1", NULL) == 0 && f.mounted == 0); free(two); }
  FRESH(); bad = level("|r|other", NULL); setup(&f, tmp, bad); CHECK(run(&f, "PPSATEST1", NULL) == 0); free(bad);
  FRESH(); setup(&f, tmp, d); E.cls = "other_class"; CHECK(run(&f, "PPSATEST1", NULL) == 0); E.cls = "delay_trigger";
  // missing level file
  FRESH(); setup(&f, tmp, d); { char gp[800]; snprintf(gp, sizeof gp, "%s/%s", f.app, REL); unlink(gp); }
  CHECK(run(&f, "PPSATEST1", NULL) == 0);
  // a row that would change the length is refused
  FRESH(); setup(&f, tmp, d); E.new_v = "12.5"; CHECK(run(&f, "PPSATEST1", NULL) == 0); E.new_v = "8.0";

  // built-in table: shape only
  CHECK(!strcmp(nps5_lf_table[0].title, "PPSA21564") && nps5_lf_table[0].obj_len == 59 && strlen(nps5_lf_table[0].src) == 64 &&
        strlen(nps5_lf_table[0].old_v) == strlen(nps5_lf_table[0].new_v));
  free(d);
  char cmd[800]; snprintf(cmd, sizeof cmd, "rm -rf %s", t0); if (system(cmd)) {}
  if (fails) printf("levelfix_unit: %d FAILED\n", fails); else printf("levelfix_unit: all passed\n");
  return fails ? 1 : 0;
}
