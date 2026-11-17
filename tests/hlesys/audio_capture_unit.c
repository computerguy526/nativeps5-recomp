// Unit test of the NPS5_AUDIO_CAPTURE writer (nps5_audio_capture.c): WAV header, exact samples,
// grain CSV, two ports, a flush without close (killed-run behaviour), concurrent appends.
#include "nps5_audio_capture.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static long slurp(const char *path, uint8_t **out) {
  FILE *f = fopen(path, "rb");
  if (!f) { *out = NULL; return -1; }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  *out = malloc(n + 1); n = (long)fread(*out, 1, n, f); fclose(f);
  return n;
}

static void *appender(void *arg) {
  nps5_acap *c = arg;
  float buf[64 * 2];
  for (int g = 0; g < 500; g++) {
    for (int i = 0; i < 128; i++) buf[i] = (float)g;
    nps5_acap_append(c, buf, 64, 1000000 + g * 1333);
  }
  return NULL;
}

int main(void) {
  char dir[] = "/tmp/acap_unit.XXXXXX";
  if (!mkdtemp(dir)) return 2;
  setenv("NPS5_AUDIO_CAPTURE", dir, 1);
  CHECK(nps5_acap_dir() && !strcmp(nps5_acap_dir(), dir));
  nps5_acap *a = nps5_acap_open(nps5_acap_dir(), 1, 0);
  nps5_acap *b = nps5_acap_open(nps5_acap_dir(), 2, 4);
  CHECK(a && b);

  // port 1: 3 grains of 256 frames, a ramp so the content is checkable
  float g[256 * 2];
  for (int k = 0; k < 3; k++) {
    for (int i = 0; i < 256; i++) { g[i * 2] = (k * 256 + i) / 1000.0f; g[i * 2 + 1] = -g[i * 2]; }
    nps5_acap_append(a, g, 256, 5000 + k * 5333);
  }
  nps5_acap_flush_all(); // killed run: no close yet
  uint8_t *w; long n;
  char p[1200]; snprintf(p, sizeof p, "%s/port1_type0.wav", dir);
  n = slurp(p, &w);
  CHECK(n == 44 + 768 * 8);
  CHECK(!memcmp(w, "RIFF", 4) && !memcmp(w + 8, "WAVEfmt ", 8) && !memcmp(w + 36, "data", 4));
  CHECK(w[20] == 3 && w[22] == 2 && rd32(w + 24) == 48000 && w[34] == 32);
  CHECK(rd32(w + 40) == 768 * 8 && rd32(w + 4) == 36 + 768 * 8);
  float *s = (float *)(w + 44);
  CHECK(s[0] == 0.0f && s[1] == 0.0f && fabsf(s[600 * 2] - 0.6f) < 1e-6f && fabsf(s[600 * 2 + 1] + 0.6f) < 1e-6f);
  free(w);

  // port 2: two threads append concurrently to the same capture
  pthread_t t1, t2;
  pthread_create(&t1, NULL, appender, b); pthread_create(&t2, NULL, appender, b);
  pthread_join(t1, NULL); pthread_join(t2, NULL);
  nps5_acap_close(b);
  nps5_acap_close(a);

  snprintf(p, sizeof p, "%s/port2_type4.wav", dir);
  n = slurp(p, &w);
  CHECK(n == 44 + 2 * 500 * 64 * 8);
  CHECK(rd32(w + 40) == 2u * 500 * 64 * 8);
  free(w);
  snprintf(p, sizeof p, "%s/port2_type4.grains.csv", dir);
  n = slurp(p, &w); w[n] = 0;
  int lines = 0; for (char *c = (char *)w; *c; c++) if (*c == '\n') lines++;
  CHECK(lines == 1 + 1000);
  free(w);
  snprintf(p, sizeof p, "%s/port1_type0.grains.csv", dir);
  n = slurp(p, &w); w[n] = 0;
  CHECK(strstr((char *)w, "\n0,0,5000\n1,256,10333\n2,512,15666\n") != NULL);
  free(w);

  // unset dir: capture off
  unsetenv("NPS5_AUDIO_CAPTURE");
  CHECK(nps5_acap_dir() == NULL && nps5_acap_open(NULL, 1, 0) == NULL);

  char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); if (system(cmd)) {}
  printf(fails ? "audio_capture_unit: FAIL\n" : "audio_capture_unit: ok\n");
  return fails ? 1 : 0;
}
