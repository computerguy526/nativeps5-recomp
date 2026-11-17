// SPDX-License-Identifier: GPL-2.0-or-later
// CFBundleExecutable of a nps5 title bundle. Reads Contents/Resources/launch.env (KEY=VALUE lines;
// ${CONTENTS} ${SUPPORT} ${HOME} are expanded), creates the per-title directories, points stdout/stderr at
// logs/last.log, makes run/ the working directory and execs Contents/MacOS/nps5-title.
// Variables already in the environment win (open --env K=V, tests, a shell).
#include <errno.h>
#include <libgen.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

static void mkdir_p(const char *path) {
  char tmp[2048];
  snprintf(tmp, sizeof tmp, "%s", path);
  for (char *p = tmp + 1; *p; p++)
    if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
  mkdir(tmp, 0755);
}

static void expand(char *out, size_t cap, const char *in, const char *contents, const char *support) {
  const char *home = getenv("HOME");
  size_t n = 0;
  while (*in && n + 1 < cap) {
    const char *rep = NULL; size_t skip = 0;
    if (!strncmp(in, "${CONTENTS}", 11)) { rep = contents; skip = 11; }
    else if (!strncmp(in, "${SUPPORT}", 10)) { rep = support; skip = 10; }
    else if (!strncmp(in, "${HOME}", 7)) { rep = home ? home : ""; skip = 7; }
    if (rep) { size_t l = strlen(rep); if (n + l + 1 >= cap) break; memcpy(out + n, rep, l); n += l; in += skip; }
    else out[n++] = *in++;
  }
  out[n] = 0;
}

int main(int argc, char **argv) {
  char exe[2048], real[2048];
  uint32_t sz = sizeof exe;
  if (_NSGetExecutablePath(exe, &sz) || !realpath(exe, real)) return 70;
  char d1[2048], contents[2048];
  snprintf(d1, sizeof d1, "%s", real);
  snprintf(contents, sizeof contents, "%s", dirname(dirname(d1)));  // .../X.app/Contents

  char envf[2200];
  snprintf(envf, sizeof envf, "%s/Resources/launch.env", contents);
  FILE *f = fopen(envf, "r");
  if (!f) { fprintf(stderr, "nps5: cannot read %s: %s\n", envf, strerror(errno)); return 71; }
  char lines[256][1536]; int nl = 0;
  while (nl < 256 && fgets(lines[nl], sizeof lines[0], f)) {
    size_t l = strlen(lines[nl]);
    while (l && (lines[nl][l - 1] == '\n' || lines[nl][l - 1] == '\r')) lines[nl][--l] = 0;
    if (l && lines[nl][0] != '#' && strchr(lines[nl], '=')) nl++;
  }
  fclose(f);

  const char *id = NULL;
  for (int i = 0; i < nl; i++) if (!strncmp(lines[i], "NPS5_TITLE_ID=", 14)) id = lines[i] + 14;
  if (!id) { fprintf(stderr, "nps5: launch.env has no NPS5_TITLE_ID\n"); return 72; }
  char support[2048];
  const char *nhome = getenv("NPS5_HOME");
  if (nhome && *nhome) snprintf(support, sizeof support, "%s/%s", nhome, id);
  else snprintf(support, sizeof support, "%s/Library/Application Support/nativeps5/%s", getenv("HOME") ? getenv("HOME") : "/tmp", id);

  char logs[2200], run[2200], log[2300], prev[2300];
  snprintf(logs, sizeof logs, "%s/logs", support);
  snprintf(run, sizeof run, "%s/run", support);
  mkdir_p(logs); mkdir_p(run);
  setenv("NPS5_SUPPORT_DIR", support, 0);
  for (int i = 0; i < nl; i++) {
    char *eq = strchr(lines[i], '='); *eq = 0;
    char val[2048];
    expand(val, sizeof val, eq + 1, contents, support);
    if (!strcmp(lines[i], "NPS5_MKDIR")) { mkdir_p(val); continue; }
    setenv(lines[i], val, 0);
  }
  snprintf(log, sizeof log, "%s/last.log", logs);
  snprintf(prev, sizeof prev, "%s/previous.log", logs);
  rename(log, prev);
  int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
  int nul = open("/dev/null", O_RDONLY); if (nul >= 0) { dup2(nul, 0); if (nul > 2) close(nul); }
  if (chdir(run)) fprintf(stderr, "nps5: chdir %s: %s\n", run, strerror(errno));
  const char *dump = getenv("NPS5_APP0_DIR");
  if (!dump || !*dump) dump = getenv("PS4_APP_DIR");
  if (dump && access(dump, R_OK)) fprintf(stderr, "nps5: WARNING: the game dump %s is not readable (moved or unmounted?)\n", dump);

  char title[2300];
  snprintf(title, sizeof title, "%s/MacOS/nps5-title", contents);
  argv[0] = title;
  execv(title, argv);
  fprintf(stderr, "nps5: exec %s: %s\n", title, strerror(errno));
  return 73;
}
