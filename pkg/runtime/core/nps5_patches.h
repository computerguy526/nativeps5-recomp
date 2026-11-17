// SPDX-License-Identifier: GPL-2.0-or-later
// Game patches as run-time settings (translator side: pkg/emitter/patchpoints.go).
//
// A title translated with a patch profile carries nps5_patches.c: the flag bytes its code tests
// (nps5_guest_patch_flags, one per setting, initialised to the profile defaults) and a table that
// a constructor registers here before main. The code pages are never modified: a setting only
// changes its flag byte, and it does so once, before the guest starts (nps5_patches_resolve).
//
// Value of a setting, later wins: profile default; "patch.<key>=0|1" in the per-game settings file
// (nps5_patches_cfg_line, written back by nps5_patches_cfg_write); the environment: the profile's
// own variable (e.g. NPS5_ASTRO_FAST_LIGHTING) and NPS5_PATCH_<KEY>, "0" = off, anything else = on.
#pragma once
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPS5_PATCH_MAX 32 // emitter.MaxPatches
#define NPS5_PATCH_ABI 1

// One setting. Must match the struct the translator spells out in nps5_patches.c.
typedef struct {
    const char *key, *title, *on, *off, *description, *env;
    uint32_t slot, def, nsites;
    const uint64_t *sites; // guest addresses of the patched instructions
} Nps5PatchDesc;

// Called by the constructor of the generated nps5_patches.c. A table of another abi is ignored
// (the flags keep the profile defaults).
void nps5_patches_register(int abi, const char *profile, const Nps5PatchDesc *descs, int n, uint8_t *flags, int nflags);

int nps5_patches_count(void);
const char *nps5_patches_profile(void);       // "" when the title has none
const Nps5PatchDesc *nps5_patches_desc(int i); // NULL when out of range
int nps5_patches_get(int i);                   // the pending value (before resolve) or the applied one
void nps5_patches_set(int i, int on);          // settings file / dialog; ignored after resolve

// One "key=value" line of the settings file: returns 1 when key is "patch.<key>" of a known patch
// (the value is taken), 0 otherwise.
int nps5_patches_cfg_line(const char *key, int value);
// Appends "patch.<key>=<0|1>" lines for every patch.
void nps5_patches_cfg_write(FILE *f);
// Applies the environment overrides, writes the flag bytes and logs one line per setting to
// stderr. The first call wins; later calls and later nps5_patches_set calls change nothing.
void nps5_patches_resolve(void);

#ifdef __cplusplus
}
#endif
