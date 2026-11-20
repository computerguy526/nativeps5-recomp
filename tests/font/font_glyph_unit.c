// SPDX-License-Identifier: GPL-2.0-or-later
// Host unit test of the libSceFont glyph API (ps4_font.c): sceFontGetRenderCharGlyphMetrics, sceFontGenerateCharGlyph,
// sceFontGlyphDefineAttribute, sceFontDeleteGlyph, sceFontGetVerticalLayout (skiptext). Builds ps4_font.c into this
// file with a flat fake guest memory; needs a system TrueType font (macOS Helvetica) and FreeType.
#include "../../pkg/runtime/modules/libSceFont/ps4_font.c"
#include <assert.h>
#include <math.h>

// ---- link stubs for the runtime pieces ps4_font.c references
static uint64_t g_bump = 0x100000;
uint64_t recomp_vm_alloc_named(GuestContext *ctx, size_t size, int prot, int flags, const char *name) {
  (void)ctx; (void)prot; (void)flags; (void)name;
  uint64_t a = g_bump; g_bump += (size + 0xfff) & ~(uint64_t)0xfff; return a;
}
int recomp_vm_free(GuestContext *ctx, uint64_t addr, size_t size) { (void)ctx; (void)addr; (void)size; return 0; }
recomp_fn_t **g_dispatch_l1[DISPATCH_LEVEL_SIZE];
uint64_t g_recomp_call_model;
uintptr_t recomp_dispatch_miss(GuestContext *ctx, uint64_t rip, uint32_t reason) { (void)ctx; (void)rip; (void)reason; abort(); }
static int g_notify_calls;
void ps4_gnm_notify_cpu_write(uint64_t addr, uint64_t size) { (void)addr; (void)size; g_notify_calls++; }
int ps4_vfs_open(const char *p, int f, mode_t m) { return open(p, f, m); }
int ps4_vfs_close(int fd) { return close(fd); }
ssize_t ps4_vfs_read(int fd, void *b, size_t n) { return read(fd, b, n); }
int ps4_vfs_fstat(int fd, struct stat *st) { return fstat(fd, st); }

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static GuestContext C;
static uint64_t galloc(size_t n) { return recomp_vm_alloc_named(&C, n, 0, 0, "t"); }
static void *H(uint64_t g) { return C.mem_base + g; }
#define CALL(fn, a, b, c, d) (C.rdi = (a), C.rsi = (b), C.rdx = (c), C.rcx = (d), fn(&C), (int32_t)C.rax)

static int32_t metrics(uint64_t font, uint32_t code, OrbisFontGlyphMetrics *out) {
  uint64_t g = galloc(sizeof(OrbisFontGlyphMetrics));
  memset(H(g), 0x5a, sizeof(OrbisFontGlyphMetrics));
  int32_t rc = CALL(shim_sceFontGetRenderCharGlyphMetrics, font, code, g, 0);
  memcpy(out, H(g), sizeof(*out));
  return rc;
}

int main(void) {
  const char *path = "/System/Library/Fonts/Helvetica.ttc";
  if (access(path, R_OK) != 0) { printf("FONT SKIP (no %s)\n", path); return 0; }
  C.mem_size = 256u << 20;
  C.mem_base = calloc(1, C.mem_size);
  C.rsp = 0x8000;
  CHECK(FT_Init_FreeType(&g_ft_library) == 0);
  FT_Face face = NULL;
  CHECK(FT_New_Face(g_ft_library, path, 0, &face) == 0);
  uint64_t font = 0;
  CHECK(font_register_opened_handle(&C, 0x1234, 0, face, NULL, NULL, &font) == ORBIS_FONT_OK && font);
  InternalFontHandle *h = get_font(font);
  h->render_scale_w = h->render_scale_h = 32.0f;
  h->scale_w = h->scale_h = 24.0f;
  int checks = 0;

  // 1. metrics of printable ASCII at the render scale
  OrbisFontGlyphMetrics m;
  for (uint32_t c = 0x21; c < 0x7f; c++) {
    CHECK(metrics(font, c, &m) == ORBIS_FONT_OK);
    CHECK(m.Horizontal.advance > 4.0f && m.Horizontal.advance < 64.0f);
    CHECK(m.width > 0.0f && m.height > 0.0f && m.width <= 64.0f && m.height <= 64.0f);
    CHECK(m.Vertical.advance > 0.0f);
    checks += 4;
  }
  CHECK(metrics(font, ' ', &m) == ORBIS_FONT_OK && m.Horizontal.advance > 0.0f && m.width == 0.0f);
  // render scale, not the plain scale: 'M' at 32 px is wider than at 24 px
  CHECK(metrics(font, 'M', &m) == ORBIS_FONT_OK);
  float adv32 = m.Horizontal.advance;
  h->render_scale_w = h->render_scale_h = 24.0f;
  CHECK(metrics(font, 'M', &m) == ORBIS_FONT_OK && m.Horizontal.advance < adv32);
  h->render_scale_w = h->render_scale_h = 32.0f;
  checks += 3;

  // 2. errors: code 0, unknown handle, no output, unmapped code (zeroed metrics; the game then retries '_')
  CHECK(metrics(font, 0, &m) == ORBIS_FONT_ERROR_NO_SUPPORT_CODE && m.width == 0.0f && m.Horizontal.advance == 0.0f);
  CHECK(metrics(0xdead000, 'A', &m) == ORBIS_FONT_ERROR_INVALID_FONT_HANDLE && m.Horizontal.advance == 0.0f);
  CHECK(CALL(shim_sceFontGetRenderCharGlyphMetrics, font, 'A', 0, 0) == ORBIS_FONT_ERROR_INVALID_PARAMETER);
  CHECK(metrics(font, 0x10FFFD, &m) == FONT_ERROR_NO_SUPPORT_GLYPH && m.Horizontal.advance == 0.0f && m.height == 0.0f);
  CHECK(metrics(font, '_', &m) == ORBIS_FONT_OK);
  checks += 5;

  // 3. measured advance == drawn advance (font_render_glyph_common), and the draw writes coverage
  {
    uint64_t buf = galloc(256 * 64), surf = galloc(sizeof(OrbisFontRenderSurface));
    uint64_t met = galloc(sizeof(OrbisFontGlyphMetrics)), res = galloc(sizeof(OrbisFontRenderOutput));
    OrbisFontRenderSurface *s = H(surf); memset(s, 0, sizeof(*s));
    s->buffer = (void *)(uintptr_t)buf; s->widthByte = 256; s->pixelSizeByte = 1; s->width = 256; s->height = 64;
    s->sc_x1 = 256; s->sc_y1 = 64;
    const char *word = "Continue";
    float x = 0.0f, measured = 0.0f;
    for (const char *p = word; *p; p++) {
      CHECK(metrics(font, (uint8_t)*p, &m) == ORBIS_FONT_OK);
      measured += m.Horizontal.advance;
      C.xmm[0].f32[0] = x; C.xmm[1].f32[0] = 40.0f;
      C.rdi = font; C.rsi = (uint8_t)*p; C.rdx = surf; C.rcx = met; C.r8 = res;
      shim_sceFontRenderCharGlyphImageHorizontal(&C);
      CHECK((int32_t)C.rax == ORBIS_FONT_OK);
      OrbisFontGlyphMetrics *dm = H(met);
      CHECK(fabsf(dm->Horizontal.advance - m.Horizontal.advance) < 1.0f);
      x += dm->Horizontal.advance;
      checks += 3;
    }
    CHECK(measured > 60.0f && fabsf(measured - x) < 8.0f);
    int lit = 0; for (int i = 0; i < 256 * 64; i++) lit += ((uint8_t *)H(buf))[i] > 128;
    CHECK(lit > 100 && g_notify_calls > 0);
    checks += 2;
  }

  // 4. glyph objects: the game's call (detail id 0xfd3, glyph form 1, metrics form 1) + attribute + delete
  uint64_t det = galloc(32), pg = galloc(8), prev = galloc(4);
  uint8_t *d = H(det); memset(d, 0, 32); d[0] = 0xd3; d[1] = 0x0f; d[6] = 1; d[7] = 1;
  *(uint64_t *)H(pg) = 0x77777777;
  CHECK(CALL(shim_sceFontGenerateCharGlyph, font, 'A', det, pg) == ORBIS_FONT_OK);
  uint64_t g = *(uint64_t *)H(pg);
  CHECK(g != 0 && ((GuestFontGlyph *)H(g))->magic == FONT_GLYPH_MAGIC && ((GuestFontGlyph *)H(g))->glyph_form == 1);
  CHECK(((GuestFontGlyph *)H(g))->em_size == 24 && ((GuestFontGlyph *)H(g))->baseline > 0);
  CHECK(CALL(shim_sceFontGlyphDefineAttribute, g, 0x11, prev, 0) == ORBIS_FONT_OK && *(int32_t *)H(prev) == 0x10);
  CHECK(CALL(shim_sceFontGlyphDefineAttribute, g, 0x11, 0, 0) == ORBIS_FONT_OK);   // the game passes NULL
  CHECK(CALL(shim_sceFontGlyphDefineAttribute, g, 0x10, prev, 0) == ORBIS_FONT_OK && *(int32_t *)H(prev) == 0x11);
  CHECK(CALL(shim_sceFontGlyphDefineAttribute, g, 0x12, prev, 0) == ORBIS_FONT_ERROR_INVALID_PARAMETER && *(int32_t *)H(prev) == 0);
  CHECK(CALL(shim_sceFontGlyphDefineAttribute, 0x1230, 0x11, prev, 0) == FONT_ERROR_INVALID_GLYPH);
  CHECK(CALL(shim_sceFontDeleteGlyph, 0, pg, 0, 0) == ORBIS_FONT_OK && *(uint64_t *)H(pg) == 0);
  CHECK(CALL(shim_sceFontDeleteGlyph, 0, pg, 0, 0) == FONT_ERROR_INVALID_GLYPH);
  *(uint64_t *)H(pg) = g;   // stale pointer after delete
  CHECK(CALL(shim_sceFontDeleteGlyph, 0, pg, 0, 0) == FONT_ERROR_INVALID_GLYPH);
  CHECK(CALL(shim_sceFontGlyphDefineAttribute, g, 0x11, 0, 0) == FONT_ERROR_INVALID_GLYPH);
  *(uint64_t *)H(pg) = g + 8;   // misaligned inside the pool
  CHECK(CALL(shim_sceFontDeleteGlyph, 0, pg, 0, 0) == FONT_ERROR_INVALID_GLYPH);
  checks += 12;
  // generate errors write a null glyph
  d[0] = 0x00; *(uint64_t *)H(pg) = 0x99;
  CHECK(CALL(shim_sceFontGenerateCharGlyph, font, 'A', det, pg) == ORBIS_FONT_ERROR_INVALID_PARAMETER && *(uint64_t *)H(pg) == 0);
  d[0] = 0xd3; d[7] = 0; *(uint64_t *)H(pg) = 0x99;
  CHECK(CALL(shim_sceFontGenerateCharGlyph, font, 'A', det, pg) == ORBIS_FONT_ERROR_INVALID_PARAMETER && *(uint64_t *)H(pg) == 0);
  d[7] = 1; *(uint64_t *)H(pg) = 0x99;
  CHECK(CALL(shim_sceFontGenerateCharGlyph, font, 0, det, pg) == ORBIS_FONT_ERROR_NO_SUPPORT_CODE && *(uint64_t *)H(pg) == 0);
  CHECK(CALL(shim_sceFontGenerateCharGlyph, 0xdead000, 'A', det, pg) == ORBIS_FONT_ERROR_INVALID_FONT_HANDLE && *(uint64_t *)H(pg) == 0);
  CHECK(CALL(shim_sceFontGenerateCharGlyph, font, 0x10FFFD, det, pg) == FONT_ERROR_NO_SUPPORT_GLYPH && *(uint64_t *)H(pg) == 0);
  CHECK(CALL(shim_sceFontGenerateCharGlyph, font, 'A', 0, pg) == ORBIS_FONT_OK && *(uint64_t *)H(pg) != 0);   // NULL detail
  CHECK(CALL(shim_sceFontDeleteGlyph, 0, pg, 0, 0) == ORBIS_FONT_OK);
  CHECK(CALL(shim_sceFontGenerateCharGlyph, font, 'A', det, 0) == ORBIS_FONT_ERROR_INVALID_PARAMETER);
  checks += 8;

  // 5. pool exhaustion and reuse; random generate/delete against a model (no slot handed out twice)
  {
    uint64_t live[FONT_GLYPH_SLOTS]; int n = 0;
    uint64_t slots = galloc(8 * (FONT_GLYPH_SLOTS + 1));
    for (int i = 0; i < FONT_GLYPH_SLOTS; i++) {
      CHECK(CALL(shim_sceFontGenerateCharGlyph, font, 'a' + i % 26, det, slots + 8 * i) == ORBIS_FONT_OK);
      live[n++] = *(uint64_t *)H(slots + 8 * i);
    }
    CHECK(CALL(shim_sceFontGenerateCharGlyph, font, 'A', det, slots + 8 * FONT_GLYPH_SLOTS) == ORBIS_FONT_ERROR_ALLOCATION_FAILED);
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n && j < i + 4; j++) CHECK(live[i] != live[j]);
    srand(12345);
    int ops = 0;
    for (int it = 0; it < 200000; it++) {
      if (n > 0 && (rand() & 1)) {
        int k = rand() % n;
        *(uint64_t *)H(pg) = live[k];
        if (CALL(shim_sceFontDeleteGlyph, 0, pg, 0, 0) != ORBIS_FONT_OK) { fails++; break; }
        live[k] = live[--n];
      } else {
        int32_t rc = CALL(shim_sceFontGenerateCharGlyph, font, 0x21 + rand() % 90, det, pg);
        if (n == FONT_GLYPH_SLOTS) { if (rc != ORBIS_FONT_ERROR_ALLOCATION_FAILED) { fails++; break; } continue; }
        if (rc != ORBIS_FONT_OK) { fails++; break; }
        uint64_t ng = *(uint64_t *)H(pg);
        for (int i = 0; i < n; i++) if (live[i] == ng) { fprintf(stderr, "FAIL slot reused while live\n"); fails++; it = 1 << 30; break; }
        live[n++] = ng;
      }
      ops++;
    }
    while (n > 0) { *(uint64_t *)H(pg) = live[--n]; CHECK(CALL(shim_sceFontDeleteGlyph, 0, pg, 0, 0) == ORBIS_FONT_OK); }
    checks += ops + 3;
  }

  // 6. vertical layout
  {
    uint64_t vl = galloc(12);
    CHECK(CALL(shim_sceFontGetVerticalLayout, font, vl, 0, 0) == ORBIS_FONT_OK);
    float *f = H(vl);
    CHECK(f[1] > 0.0f && f[0] < 0.0f && f[2] == 0.0f);
    CHECK(CALL(shim_sceFontGetVerticalLayout, 0xdead000, vl, 0, 0) == ORBIS_FONT_ERROR_INVALID_FONT_HANDLE && f[1] == 0.0f);
    CHECK(CALL(shim_sceFontGetVerticalLayout, font, 0, 0, 0) == ORBIS_FONT_ERROR_INVALID_PARAMETER);
    checks += 4;
  }

  // 10. fontfix: horizontal layout from the face bbox (as the PS5 FreeType driver): baselineOffset = bbox.yMax and
  //     lineAdvance = bbox height at the font scale, so a line cell holds every glyph: a baseline placed at
  //     baselineOffset below the cell top never cuts the top of any glyph (Astro Bot's 310 px banners lost their cap tops).
  {
    float saved_w = h->scale_w, saved_h = h->scale_h, saved_rw = h->render_scale_w, saved_rh = h->render_scale_h;
    h->scale_w = h->scale_h = h->render_scale_w = h->render_scale_h = 310.0f;
    uint64_t lg = galloc(sizeof(OrbisFontHorizontalLayout));
    C.rdi = font; C.rsi = lg; shim_sceFontGetHorizontalLayout(&C);
    CHECK((int32_t)C.rax == ORBIS_FONT_OK);
    OrbisFontHorizontalLayout L = *(OrbisFontHorizontalLayout *)H(lg);
    FT_Face f = h->ft_face;
    const float want_base = (float)FT_MulFix(f->bbox.yMax, f->size->metrics.y_scale) / 64.0f;
    const float want_adv = (float)(FT_MulFix(f->bbox.yMax, f->size->metrics.y_scale) - FT_MulFix(f->bbox.yMin, f->size->metrics.y_scale)) / 64.0f;
    CHECK(fabsf(L.baselineOffset - want_base) < 0.02f && fabsf(L.lineAdvance - want_adv) < 0.02f && L.decorationExtent == 0.0f);
    CHECK(L.baselineOffset > (float)(f->size->metrics.ascender >> 6)); // Helvetica: bbox top above the hhea ascender
    checks += 2;
    // every printable ASCII glyph drawn at baseline = cell top + baselineOffset stays inside [0, lineAdvance)
    const int cell = (int)ceilf(L.lineAdvance) + 2;
    uint64_t buf = galloc((size_t)400 * cell), surf = galloc(sizeof(OrbisFontRenderSurface));
    uint64_t met = galloc(sizeof(OrbisFontGlyphMetrics)), res = galloc(sizeof(OrbisFontRenderOutput));
    for (uint32_t c = 0x21; c < 0x7f; c++) {
      OrbisFontRenderSurface *s = H(surf); memset(s, 0, sizeof(*s));
      s->buffer = (void *)(uintptr_t)buf; s->widthByte = 400; s->pixelSizeByte = 1; s->width = 400; s->height = cell;
      s->sc_x1 = 400; s->sc_y1 = cell;
      OrbisFontGlyphMetrics gm; CHECK(metrics(font, c, &gm) == ORBIS_FONT_OK);
      CHECK(gm.Horizontal.bearingY <= L.baselineOffset + 0.5f);                       // top inside the cell
      CHECK(gm.height - gm.Horizontal.bearingY <= L.lineAdvance - L.baselineOffset + 0.5f); // bottom inside the cell
      C.xmm[0].f32[0] = 20.0f; C.xmm[1].f32[0] = L.baselineOffset;
      C.rdi = font; C.rsi = c; C.rdx = surf; C.rcx = met; C.r8 = res;
      shim_sceFontRenderCharGlyphImageHorizontal(&C);
      CHECK((int32_t)C.rax == ORBIS_FONT_OK);
      checks += 3;
    }
    h->scale_w = saved_w; h->scale_h = saved_h; h->render_scale_w = saved_rw; h->render_scale_h = saved_rh;
  }

  if (fails) { printf("FONT FAIL %d\n", fails); return 1; }
  printf("FONT OK (%d checks)\n", checks);
  return 0;
}
