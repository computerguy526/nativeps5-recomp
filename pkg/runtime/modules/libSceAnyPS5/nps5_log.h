// nps5_log.h: the shared log-line convention of the nativeps5 runtime, Kyty and the drivers (header only, C11). The runtime
// core already has a structured logger (core/nps5_diag.h: NPS5_LOG(subsys enum, level), JSONL, coalescing); this header is the
// same convention for the free-text [tag] lines of the libraries that are not part of the core (the Kyty seam, Kyty, KosmicKrisp).
//
//   [subsystem] LEVEL message        LEVEL in E (error), W (warning), I (info, the default), D (debug)
//
// An I line is written without a level token (`[kyty-seam] watch interval: ...`), exactly as before, so every tool that
// greps the logs keeps working; E / W / D lines carry it (`[kyty-seam] W slow page`). `nps5 log` and `nps5 bug-report`
// (scripts/nps5lib/logsummary.py) read the same convention, and also infer E / W from the words ERROR, FATAL, WARN, [RED].
//
//   NPS5_LOG=subsys:level,...   the lowest level a subsystem prints: kyty-seam:W,kk-archive:D (`=` works as well as `:`, the core's
//                               form hle=debug; a level is one of E W I D or error warn info debug trace). `*:W` or a bare
//                               `W` sets the default for every subsystem. Unset = I and above (D and trace are off).
//   NPS5_LOG_RATE=FIRST,EVERY   rate limit for NPS5_SLOGRL lines (the periodic ones): print the first FIRST, then every EVERYth,
//                               and a "(xK repeated; N not shown)" line with each one printed after the first FIRST. Unset or 0 = print all.
//
// Usage (NPS5_SLOG, not NPS5_LOG: that name is the core's macro):
//   NPS5_SLOG("kyty-seam", 'W', "slow page %d", n);                 // one line if the level is enabled
//   static nps5_log_rl rl; NPS5_SLOGRL("kyty-seam", 'I', &rl, ...);  // a periodic line, rate limited when NPS5_LOG_RATE is set
// Thread safe; the settings are read from the environment once. Written for this repository (the convention follows the
// level-and-subsystem filters of most logging libraries; no code is taken from another project).
#ifndef NPS5_LOG_H
#define NPS5_LOG_H

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NPS5_LOG_MAX_RULES 16

typedef struct {
  char name[32];
  int min;  // 0 = D, 1 = I, 2 = W, 3 = E
} nps5_log_rule;

typedef struct {
  atomic_ullong seen;       // lines offered
  atomic_ullong last_shown;  // value of seen at the last printed line after the first FIRST
} nps5_log_rl;

typedef struct {
  int ready, def, nrules;
  unsigned first, every;
  nps5_log_rule rules[NPS5_LOG_MAX_RULES];
} nps5_log_cfg;

// Level of a word or letter: D (debug, trace), I (info), W (warn, warning), E (error); -1 = not a level.
static inline int nps5_log_level_word(const char *w) {
  if (!w || !*w) return -1;
  char l = (char)toupper((unsigned char)w[0]);
  if (w[1]) {
    if (strcasecmp(w, "trace") == 0 || strcasecmp(w, "debug") == 0) return 0;
    if (strcasecmp(w, "info") == 0) return 1;
    if (strcasecmp(w, "warn") == 0 || strcasecmp(w, "warning") == 0) return 2;
    if (strcasecmp(w, "error") == 0) return 3;
    return -1;
  }
  return l == 'D' || l == 'T' ? 0 : l == 'I' ? 1 : l == 'W' ? 2 : l == 'E' ? 3 : -1;
}
static inline int nps5_log_level_index(char c) { const char w[2] = {c, 0}; return nps5_log_level_word(w); }

// Parse NPS5_LOG / NPS5_LOG_RATE texts into c (testable: no environment access).
static inline void nps5_log_parse(nps5_log_cfg *c, const char *spec, const char *rate) {
  memset(c, 0, sizeof *c);
  c->def = 1;
  if (spec) {
    char buf[256];
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (char *save = NULL, *tok = strtok_r(buf, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save)) {
      char *colon = strpbrk(tok, ":=");
      const int lv = nps5_log_level_word(colon ? colon + 1 : tok);
      if (lv < 0) continue;  // not a level, not "name:level": ignored
      if (!colon || strncmp(tok, "*:", 2) == 0 || strncmp(tok, "*=", 2) == 0 || strncmp(tok, "all", 3) == 0) {
        c->def = lv;
      } else if (c->nrules < NPS5_LOG_MAX_RULES) {
        *colon = 0;
        strncpy(c->rules[c->nrules].name, tok, sizeof c->rules[0].name - 1);
        c->rules[c->nrules++].min = lv;
      }
    }
  }
  if (rate) {
    unsigned f = 0, e = 0;
    if (sscanf(rate, "%u,%u", &f, &e) == 2 && e > 0) {
      c->first = f;
      c->every = e;
    }
  }
  c->ready = 1;
}

static inline const nps5_log_cfg *nps5_log_config(void) {
  static nps5_log_cfg cfg;  // filled once; a racing second fill writes the same values
  if (!cfg.ready) {
    nps5_log_cfg t;
    nps5_log_parse(&t, getenv("NPS5_LOG"), getenv("NPS5_LOG_RATE"));
    cfg = t;
  }
  return &cfg;
}

static inline int nps5_log_enabled_in(const nps5_log_cfg *c, const char *subsys, char level) {
  const int lv = nps5_log_level_index(level);
  int min = c->def;
  for (int i = 0; i < c->nrules; i++)
    if (strcmp(c->rules[i].name, subsys) == 0) min = c->rules[i].min;
  return lv >= min;
}

static inline int nps5_log_enabled(const char *subsys, char level) { return nps5_log_enabled_in(nps5_log_config(), subsys, level); }

// Rate limit decision for one offered line: 1 = print it, 0 = skip. *hidden = lines skipped since the last printed one (for the
// "repeated" note) when the line is printed after the first FIRST.
static inline int nps5_log_rl_pass_in(const nps5_log_cfg *c, nps5_log_rl *rl, unsigned long long *hidden) {
  const unsigned long long n = atomic_fetch_add(&rl->seen, 1) + 1;
  *hidden = 0;
  if (c->every == 0 || n <= c->first) return 1;
  if ((n - c->first) % c->every != 0) return 0;
  const unsigned long long last = atomic_exchange(&rl->last_shown, n);
  *hidden = n - (last > c->first ? last : c->first) - 1;
  return 1;
}

#define NPS5_SLOG_EMIT(subsys, level, ...)                              \
  do {                                                                 \
    if ((level) == 'I') fprintf(stderr, "[%s] ", subsys);              \
    else fprintf(stderr, "[%s] %c ", subsys, (char)(level));           \
    fprintf(stderr, __VA_ARGS__);                                      \
    fputc('\n', stderr);                                               \
  } while (0)

#define NPS5_SLOG(subsys, level, ...)                                   \
  do {                                                                 \
    if (nps5_log_enabled((subsys), (level))) NPS5_SLOG_EMIT(subsys, level, __VA_ARGS__); \
  } while (0)

// A periodic line: subject to NPS5_LOG_RATE. `rl` is a static nps5_log_rl, one per call site.
#define NPS5_SLOGRL(subsys, level, rl, ...)                                                       \
  do {                                                                                           \
    if (nps5_log_enabled((subsys), (level))) {                                                   \
      unsigned long long hidden_ = 0;                                                            \
      if (nps5_log_rl_pass_in(nps5_log_config(), (rl), &hidden_)) {                              \
        if (hidden_) NPS5_SLOG_EMIT(subsys, 'I', "(x%llu repeated; %llu not shown)", hidden_ + 1, hidden_); \
        NPS5_SLOG_EMIT(subsys, level, __VA_ARGS__);                                               \
      }                                                                                          \
    }                                                                                            \
  } while (0)

#endif
