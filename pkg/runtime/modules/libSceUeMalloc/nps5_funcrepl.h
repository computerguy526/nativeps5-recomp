// SPDX-License-Identifier: GPL-2.0-or-later
//
// Parameters the translator hands to the host side of a function-level replacement (docs/m3/function-hle.md):
// addresses and sizes that are only known at translate time (the guest address of a vtable, the size of an
// object). The generated file funcrepl_params.c defines nps5_repl_params; a build without replacements links
// the empty weak definition from nps5_uemalloc.c.

#ifndef NPS5_FUNCREPL_H
#define NPS5_FUNCREPL_H

#include <stdbool.h>
#include <stdint.h>

typedef struct Nps5ReplParam {
  const char *key;
  uint64_t value;
} Nps5ReplParam;

// Terminated by an entry with a NULL key.
extern const Nps5ReplParam nps5_repl_params[];

// Looks key up; false when the translation did not set it.
bool nps5_repl_param(const char *key, uint64_t *out);

#endif
