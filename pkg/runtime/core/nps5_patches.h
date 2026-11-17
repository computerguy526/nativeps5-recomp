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
// own variable (e.g. NPS5_ASTRO_FAST_LIGHTING) and NPS5_PATCH_<KEY>: 0|false|off|no = off,
// 1|true|on|yes = on (case-insensitive); any other value is ignored with one warning line.
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

// Called by the constructor of the generated nps5_patches.c. A table of another abi, or one that
// is inconsistent (slot >= nflags, duplicate slots, n > nflags, missing sites), registers nothing
// and logs one line: the flags keep the profile defaults.
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

// ---- Choice settings (value patch points) -------------------------------------------------
// A choice setting picks one of several constants for its value sites: the translated code loads
// the site's register from a 64-bit slot of nps5_guest_patch_values (initialised to the default
// option). Value, later wins: profile default; "patch.<key>=<option name>" in the settings file
// (an option's position, 0-based, is accepted too); the environment (the profile's variable and
// NPS5_PATCH_<KEY>, an option name or position; an unknown value is reported and ignored).
// An option may imply host environment variables ("K=V" lines of setenv[option]): resolve sets
// each one that the process environment does not already have, before the guest and the GPU
// backend start (ASTRO BOT "Upscaling": the render size sites + KYTY_METALFX).
#define NPS5_CHOICE_MAX 16
#define NPS5_CHOICE_OPTIONS_MAX 8 // emitter.MaxPatchOptions
#define NPS5_PATCH_VALUES_MAX 64  // emitter.MaxPatchValues
#define NPS5_CHOICE_ABI 1

// Must match the structs the translator spells out in nps5_patches.c.
typedef struct {
    uint32_t slot, f32;     // slot of nps5_guest_patch_values; f32: the low 32 bits are a float's bits
    uint64_t site;          // guest address of the instruction
    const uint64_t *values; // one per option
} Nps5ChoiceValue;
typedef struct {
    const char *key, *title, *description, *env;
    uint32_t noptions, def, nvalues;
    const char *const *names, *const *labels, *const *setenv; // per option
    const Nps5ChoiceValue *values;
} Nps5ChoiceDesc;

void nps5_patch_choices_register(int abi, const char *profile, const Nps5ChoiceDesc *descs, int n, uint64_t *values, int nvalues);
int nps5_choices_count(void);
const Nps5ChoiceDesc *nps5_choices_desc(int i); // NULL when out of range
int nps5_choices_get(int i);                    // the pending option (before resolve) or the applied one
void nps5_choices_set(int i, int option);       // settings file / dialog; ignored after resolve or when out of range
// The option of a name or a decimal position; -1 when it is neither.
int nps5_choices_option(int i, const char *text);
// One "key=text" line of the settings file: returns 1 when key is "patch.<key>" of a choice
// setting (a known option is taken, anything else is ignored), 0 otherwise.
int nps5_patches_cfg_text(const char *key, const char *text);

#ifdef __cplusplus
}
#endif
