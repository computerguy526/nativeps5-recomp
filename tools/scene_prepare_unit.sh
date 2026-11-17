#!/bin/bash
# scene_prepare_unit.sh: shader_prepare value parsing and the scene-tag skip list (pkg/runtime/core/nps5_scene_prepare.h).
set -eu
cd "$(dirname "$0")/.."; T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cat > "$T/t.c" <<'EOT'
#include "nps5_scene_prepare.h"
#include <stdio.h>
static int bad;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s\n", #x); bad++; } } while (0)
int main(void) {
  CHECK(nps5_shader_prepare_parse("all", 1) == 0);
  CHECK(nps5_shader_prepare_parse("scene", 0) == 1);
  CHECK(nps5_shader_prepare_parse(NULL, 1) == 1 && nps5_shader_prepare_parse("", 0) == 0);
  CHECK(nps5_shader_prepare_parse("Scene", 0) == 0 && nps5_shader_prepare_parse("bogus", 1) == 1);   // unknown: fallback
  CHECK(nps5_scene_tag_skipped("ui_title", "ui_"));
  CHECK(!nps5_scene_tag_skipped("hoover_beach", "ui_"));
  CHECK(nps5_scene_tag_skipped("menu_x", "ui_, menu_") && nps5_scene_tag_skipped("ui_x", "ui_, menu_"));
  CHECK(!nps5_scene_tag_skipped("x", "") && !nps5_scene_tag_skipped("x", ",,") && !nps5_scene_tag_skipped(NULL, "ui_"));
  CHECK(!nps5_scene_tag_skipped("u", "ui_"));   // shorter than the prefix
  printf(bad ? "scene_prepare_unit: %d FAILED\n" : "scene_prepare_unit: ok\n", bad);
  return bad != 0;
}
EOT
cc -Wall -Werror -I pkg/runtime/core -o "$T/t" "$T/t.c" && "$T/t"
