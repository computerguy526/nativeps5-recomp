// SPDX-License-Identifier: GPL-2.0-or-later
// When the inherited PS4Recomp settings dialog (PS4 Pro mode, MetalFX, HUD, language) may appear.
// Its options do not apply to nativeps5 (PS5 / AnyPS5 path), so it is opt-in: NPS5_SETTINGS_DIALOG=1.
// By default it behaves as if PS4_NO_DIALOGS were set. PS4_NO_DIALOGS and PS4_LANG still suppress it.
#pragma once
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static inline bool ps4_settings_dialog_wanted(bool stored, bool option_held) {
  const char *on = getenv("NPS5_SETTINGS_DIALOG");
  if (!on || strcmp(on, "1") != 0) return false;
  return (!stored || option_held) && !getenv("PS4_NO_DIALOGS") && !getenv("PS4_LANG");
}
