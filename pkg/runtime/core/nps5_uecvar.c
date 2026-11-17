// SPDX-License-Identifier: GPL-2.0-or-later
//
// Unreal console-variable read-back (docs/m3/uecvar.md). Read-only; nothing is written to guest memory.
//
//   NPS5_UECVAR_DUMP=<file>   output file (appended), required to enable
//   NPS5_UECVAR_LIST=<file>   one cvar per line: "<name> <kind> <type> <guest address hex>" (cmd/uecvar dumplist)
//   NPS5_UECVAR_AT=<s[,s...]> seconds after start to dump (default 30)
//
// kind "tacv": the address is a TAutoConsoleVariable object ([+0] vtable, [+8] IConsoleVariable*); the
//   value is read from the IConsoleVariable (FConsoleVariable<T>: TConsoleVariableData at
//   NPS5_UECVAR_DATA_OFF, game-thread slot first). kind "ref": the address is the bound global itself.
// Output line: "<name> <kind> <type> <value> raw=<hex of the first 0x58 bytes of the object>".
// Reads go through mach_vm_read_overwrite, so an unmapped address reports "unmapped" instead of faulting.
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "nps5_mem.h"

#ifndef NPS5_UECVAR_DATA_OFF
#define NPS5_UECVAR_DATA_OFF 0x50 // TConsoleVariableData<T>::ShadowedValue[0] (game thread; [1] at +4 is the render thread copy) in FConsoleVariable<T>, from its constructor (stores the default at +0x50 and +0x54)
#endif

typedef struct {
  char *name, *kind, *type;
  uint64_t addr;
} UcEntry;

static UcEntry *g_ents;
static size_t g_nents;
static char *g_out;
static double g_at[16];
static int g_nat;

// Copies guest [guest, guest+n) into buf; false when it is outside the arena or not readable
// (mach_vm_read_overwrite reports an error instead of faulting).
static bool uc_read(uint64_t guest, void *buf, size_t n) {
  if (!g_nps5_arena.bias || !nps5_guest_range_ok(guest, n)) return false;
  mach_vm_size_t got = 0;
  kern_return_t kr = mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)(g_nps5_arena.bias + guest), n,
                                            (mach_vm_address_t)(uintptr_t)buf, &got);
  return kr == KERN_SUCCESS && got == n;
}

// Formats the value of a scalar of the given type from p (>= 8 readable bytes).
static void nps5_uecvar_format(const char *type, const uint8_t *p, char *out, size_t n) {
  if (!strcmp(type, "float")) {
    float f;
    memcpy(&f, p, 4);
    snprintf(out, n, "%g", (double)f);
  } else if (!strcmp(type, "bool")) {
    snprintf(out, n, "%d", p[0] != 0);
  } else if (!strcmp(type, "int")) {
    int32_t v;
    memcpy(&v, p, 4);
    snprintf(out, n, "%d", v);
  } else {
    snprintf(out, n, "?");
  }
}

static void uc_dump(double t) {
  FILE *f = fopen(g_out, "a");
  if (!f) return;
  fprintf(f, "# t=%.1fs entries=%zu\n", t, g_nents);
  for (size_t i = 0; i < g_nents; i++) {
    UcEntry *e = &g_ents[i];
    char val[64] = "unmapped";
    char raw[0x58 * 2 + 1] = "";
    uint8_t buf[0x60];
    bool have = false;
    if (!strcmp(e->kind, "tacv")) {
      uint64_t ref = 0;
      if (uc_read(e->addr + 8, &ref, 8) && ref) {
        have = uc_read(ref, buf, sizeof buf);
        if (have) nps5_uecvar_format(e->type, buf + NPS5_UECVAR_DATA_OFF, val, sizeof val);
      } else if (ref == 0 && uc_read(e->addr, buf, 8)) {
        snprintf(val, sizeof val, "unregistered");
      }
    } else {
      have = uc_read(e->addr, buf, 8);
      if (have) nps5_uecvar_format(e->type, buf, val, sizeof val);
    }
    if (have)
      for (int k = 0; k < (!strcmp(e->kind, "tacv") ? 0x58 : 8); k++) snprintf(raw + 2 * k, 3, "%02x", buf[k]);
    fprintf(f, "%s %s %s %s raw=%s\n", e->name, e->kind, e->type, val, raw);
  }
  fclose(f);
}

// ---- NPS5_UECVAR_SET: write cvar values (docs/m3/uecvar.md "Setting values"). ----
// The title's project ini sets r.DynamicRes.* at priority ConsoleVariablesIni and the platform ini cannot outrank it, so the
// runtime stores the value itself: both thread copies of a TAutoConsoleVariable (+0x50 game, +0x54 render) or the bound global.
typedef struct {
  char *name;
  double val;
} UcSet;
static UcSet g_sets[32];
static int g_nsets;
// Re-applying stops after this many seconds BY DESIGN (default 120; NPS5_UECVAR_SET_FOR=<s> changes it, a large value keeps going for the whole
// run): the title may re-set a cvar from code, and an endless 10 Hz store loop into game memory is not wanted by default. After that
// the title's own value applies again (a scene resolution reverts to its dynamic resolution); a line on stderr says so.
static double g_set_for = 120;

static bool uc_write(uint64_t guest, const void *buf, size_t n) {
  uint8_t probe[8];
  if (!uc_read(guest, probe, n < 8 ? n : 8) || !nps5_guest_range_ok(guest, n)) return false;
  // Not a bare memcpy: the cvar may sit in a page the guest made read-only (RELRO), and this thread has no fault resolver. The write
  // goes through the kernel (mach_vm_write), which fails cleanly on a page that is not writable; a page a host library keeps
  // protected for its own bookkeeping (the write watch) is opened through the host access bracket first.
  uint8_t *host = g_nps5_arena.bias + guest;
  nps5_host_write_begin(host, n);
  const kern_return_t kr = mach_vm_write(mach_task_self(), (mach_vm_address_t)(uintptr_t)host, (vm_offset_t)(uintptr_t)buf, (mach_msg_type_number_t)n);
  nps5_host_write_end(host, n);
  return kr == KERN_SUCCESS;
}

static void uc_store(const char *type, uint64_t at, double v, bool both) {
  uint8_t b[4] = {0};
  size_t n = 4;
  if (!strcmp(type, "float")) {
    float f = (float)v;
    memcpy(b, &f, 4);
  } else if (!strcmp(type, "bool")) {
    b[0] = v != 0;
    n = 1;
  } else {
    int32_t i = (int32_t)v;
    memcpy(b, &i, 4);
  }
  uc_write(at, b, n);
  if (both) uc_write(at + 4, b, n);
}

static int uc_apply_sets(void) {
  int applied = 0;
  for (int s = 0; s < g_nsets; s++) {
    for (size_t i = 0; i < g_nents; i++) {
      UcEntry *e = &g_ents[i];
      if (strcmp(e->name, g_sets[s].name)) continue;
      if (!strcmp(e->kind, "tacv")) {
        uint64_t ref = 0;
        if (uc_read(e->addr + 8, &ref, 8) && ref) { uc_store(e->type, ref + NPS5_UECVAR_DATA_OFF, g_sets[s].val, true); applied++; }
      } else {
        uc_store(e->type, e->addr, g_sets[s].val, false);
        applied++;
      }
    }
  }
  return applied;
}

static void *uc_set_thread(void *arg) {
  (void)arg;
  struct timespec ts = {0, 100 * 1000 * 1000};
  int logged = 0;
  for (double t = 0; t < g_set_for; t += 0.1) {
    int n = uc_apply_sets();
    if (n && !logged) { fprintf(stderr, "[uecvar] set: %d cvar stores applied (t=%.1fs)\n", n, t); logged = 1; }
    nanosleep(&ts, NULL);
  }
  fprintf(stderr, "[uecvar] set: stopped re-applying after %.0f s (NPS5_UECVAR_SET_FOR); the title's own values apply from now on\n", g_set_for);
  return NULL;
}

static void *uc_thread(void *arg) {
  (void)arg;
  double done = 0;
  for (int i = 0; i < g_nat; i++) {
    double d = g_at[i] - done;
    if (d > 0) {
      struct timespec ts = {(time_t)d, (long)((d - (time_t)d) * 1e9)};
      nanosleep(&ts, NULL);
    }
    done = g_at[i];
    uc_dump(done);
  }
  return NULL;
}

static int uc_load_list(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  char line[512];
  size_t cap = 0;
  while (fgets(line, sizeof line, f)) {
    char name[256], kind[16], type[16];
    unsigned long long a;
    if (line[0] == '#' || sscanf(line, "%255s %15s %15s %llx", name, kind, type, &a) != 4) continue;
    if (g_nents == cap) {
      cap = cap ? cap * 2 : 256;
      g_ents = realloc(g_ents, cap * sizeof *g_ents);
    }
    g_ents[g_nents++] = (UcEntry){strdup(name), strdup(kind), strdup(type), a};
  }
  fclose(f);
  return 0;
}

__attribute__((constructor)) static void nps5_uecvar_init(void) {
  const char *out = getenv("NPS5_UECVAR_DUMP"), *list = getenv("NPS5_UECVAR_LIST");
  const char *set = getenv("NPS5_UECVAR_SET"), *res = getenv("NPS5_TITLE_RESOLUTION");
  char setbuf[1024] = "";
  if (set && *set) snprintf(setbuf, sizeof setbuf, "%s", set);
  if (res && *res) {
    // NPS5_TITLE_RESOLUTION=WxH: the title presents a fixed 3840x2160 and renders the scene at r.DynamicRes.{Min,Max}ScreenPercentage of it.
    unsigned w = 0, h = 0;
    if (sscanf(res, "%ux%u", &w, &h) == 2 && w && h && w <= 3840 && h <= 2160) {
      double pct = 100.0 * (w * 2160.0 > h * 3840.0 ? (double)w / 3840.0 : (double)h / 2160.0);
      size_t l = strlen(setbuf);
      snprintf(setbuf + l, sizeof setbuf - l, "%sr.ScreenPercentage=%.4f,r.DynamicRes.MinScreenPercentage=%.4f,r.DynamicRes.MaxScreenPercentage=%.4f", l ? "," : "", pct, pct, pct);
    } else {
      fprintf(stderr, "[uecvar] NPS5_TITLE_RESOLUTION=%s ignored (want WxH within 3840x2160)\n", res);
    }
  }
  if (setbuf[0] && !(list && *list)) fprintf(stderr, "[uecvar] NPS5_UECVAR_SET / NPS5_TITLE_RESOLUTION need NPS5_UECVAR_LIST (the cvar list of the title); nothing is set\n");
  if (setbuf[0] && list && *list) {
    for (char *s = strtok(setbuf, ","); s && g_nsets < 32; s = strtok(NULL, ",")) {
      char *eq = strchr(s, '=');
      if (!eq) continue;
      *eq = 0;
      g_sets[g_nsets].name = strdup(s);
      g_sets[g_nsets++].val = atof(eq + 1);
    }
    if (uc_load_list(list) != 0 || !g_nents) fprintf(stderr, "[uecvar] cannot read the cvar list %s\n", list);
    else {
      const char *sf = getenv("NPS5_UECVAR_SET_FOR");
      if (sf && *sf) g_set_for = atof(sf);
      pthread_t st;
      if (pthread_create(&st, NULL, uc_set_thread, NULL) == 0) pthread_detach(st);
      fprintf(stderr, "[uecvar] setting %d cvars from %s\n", g_nsets, list);
    }
  }
  if (!out || !*out || !list || !*list) return;
  if (g_nents) { /* list already loaded by the setter */ } else
  if (uc_load_list(list) != 0 || !g_nents) {
    fprintf(stderr, "[uecvar] cannot read the cvar list %s\n", list);
    return;
  }
  g_out = strdup(out);
  const char *at = getenv("NPS5_UECVAR_AT");
  char buf[128];
  snprintf(buf, sizeof buf, "%s", at && *at ? at : "30");
  for (char *s = strtok(buf, ","); s && g_nat < 16; s = strtok(NULL, ",")) g_at[g_nat++] = atof(s);
  pthread_t t;
  if (pthread_create(&t, NULL, uc_thread, NULL) == 0) pthread_detach(t);
  fprintf(stderr, "[uecvar] %zu cvars, dump to %s at %s s\n", g_nents, out, buf);
}
