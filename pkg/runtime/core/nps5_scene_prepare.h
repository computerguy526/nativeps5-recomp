// SPDX-License-Identifier: GPL-2.0-or-later
// shader_prepare (launch-time shader preparation: all | scene) and the scene tag: the pure parts, header only so
// tools/scene_prepare_unit.sh tests them without the runtime.
#pragma once
#include <string.h>

// "all" -> 0, "scene" -> 1, anything else (NULL, empty, unknown) -> fallback.
static inline int nps5_shader_prepare_parse(const char *text, int fallback) {
  if (!text) return fallback;
  if (!strcmp(text, "all")) return 0;
  if (!strcmp(text, "scene")) return 1;
  return fallback;
}

// 1 when name starts with one of the comma-separated prefixes ("ui_,menu_"); spaces after a comma are ignored.
static inline int nps5_scene_tag_skipped(const char *name, const char *prefixes) {
  if (!name || !prefixes) return 0;
  for (const char *p = prefixes; *p;) {
    const char *e = strchr(p, ',');
    size_t n = e ? (size_t)(e - p) : strlen(p);
    while (n && *p == ' ') { p++; n--; }
    if (n && !strncmp(name, p, n)) return 1;
    if (!e) break;
    p = e + 1;
  }
  return 0;
}
