// SPDX-License-Identifier: GPL-2.0-or-later

#include "ps4_font.h"
#include "ps4_vfs.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FONTS 128
#define MAX_STRINGS 128
#define MAX_WRITINGS 64

typedef struct FontBacking {
    uint8_t *data;
    size_t size;
    uint32_t subfont_index;
    int refs;
} FontBacking;

typedef struct InternalFontHandle {
    int in_use;
    uint64_t guest_addr;
    uint32_t magic; // 0x0F02
    uint32_t scale_unit; // 0 = pixel, 1 = point
    float scale_w;
    float scale_h;
    float render_scale_w;
    float render_scale_h;
    float weight_x;
    float weight_y;
    float slant;
    float render_weight_x;
    float render_weight_y;
    float render_slant;
    uint32_t dpi_x;
    uint32_t dpi_y;
    uint64_t renderer_guest;
    FT_Face ft_face;
    FontBacking *backing;
    uint64_t library_guest;
} InternalFontHandle;

typedef struct InternalStringNode {
    OrbisFontTextCharacter guest_node;
} InternalStringNode;

typedef struct InternalString {
    int in_use;
    uint64_t guest_addr;
    uint64_t memory_guest;
    uint64_t source_guest;
    uint64_t default_font_guest;
    uint32_t writing_form;
    uint32_t terminate_code;
    uint64_t terminate_order;
    uint32_t character_count;
    uint64_t head_node_guest;
} InternalString;

typedef struct InternalWriting {
    int in_use;
    uint64_t guest_addr;
    uint64_t string_guest;
    uint64_t current_node_guest;
    uint64_t step_char_node_guest;
    uint32_t current_index;
    float cursor_x;
    float cursor_y;
    OrbisFontWritingStep current_step;
} InternalWriting;

static pthread_mutex_t g_font_mutex = PTHREAD_MUTEX_INITIALIZER;
static FT_Library g_ft_library = NULL;
static InternalFontHandle g_fonts[MAX_FONTS];
static InternalString g_strings[MAX_STRINGS];
static InternalWriting g_writings[MAX_WRITINGS];

static const char *g_fallback_font_candidates[] = {
    "/System/Library/Fonts/Helvetica.ttc",
    "/System/Library/Fonts/Geneva.dfont",
    "/System/Library/Fonts/SFNS.ttf",
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/System/Library/Fonts/Supplemental/Tahoma.ttf",
    "/System/Library/Fonts/Supplemental/Courier New.ttf",
    "/System/Library/Fonts/AppleSDGothicNeo.ttc",
    NULL
};

static void font_active_weight(const InternalFontHandle *h, int use_render, float *wx, float *wy) {
    *wx = h->weight_x; *wy = h->weight_y;
    if (use_render && (h->render_weight_x != 1.0f || h->render_weight_y != 1.0f)) { *wx = h->render_weight_x; *wy = h->render_weight_y; }
}

// fontfix: the horizontal layout of the PS5 font library's FreeType driver (AnyPS5 libSceFontFt Driver.cpp
// LibraryComputeLayout, a reconstruction of the real driver) is built from the face BOUNDING BOX, not from the
// hhea ascender/height: baselineOffset = bbox.yMax, lineAdvance = bbox.yMax - bbox.yMin, both FT_MulFix'ed by the
// current y scale (26.6), and decorationExtent = half the effect-weight growth (0 at weight 1). Astro Bot places the
// first baseline of its banner/title text with it (guest 0x12d6ede stores baselineOffset; the banner surface puts the
// baseline at (480 - size + baselineOffset) / 2 + ...): with the hhea ascender (0.82 em for LevelNameFont, bbox yMax is
// 1.274 em) the baseline came out 70 px too high at 310 px and the tops of the capitals were cut off.
// NPS5_FONT_LAYOUT=hhea restores the old values.
static int font_layout_hhea(void) { static int v = -1; if (v < 0) { const char *e = getenv("NPS5_FONT_LAYOUT"); v = (e && strcmp(e, "hhea") == 0); } return v; }
// Caller holds g_font_mutex and has applied the size. Returns 1 and fills the three values from the face bbox.
static int font_bbox_layout(const InternalFontHandle *h, float weight_y, float *baseline, float *line_adv, float *deco) {
    if (font_layout_hhea() || !h->ft_face || !h->ft_face->size) return 0;
    FT_Face f = h->ft_face;
    const FT_Fixed ys = f->size->metrics.y_scale;
    FT_Pos ymax = FT_MulFix(f->bbox.yMax, ys), ymin = FT_MulFix(f->bbox.yMin, ys);
    float effect = 0.0f;
    const float d = weight_y - 1.0f;
    if (d != 0.0f && d == d) {
        const FT_Pos em = FT_MulFix((FT_Long)f->units_per_EM, ys);           // em size in 26.6
        const FT_Pos half = (FT_Pos)(d * (float)em) / 2;
        effect = (float)half / 64.0f;
        ymin -= half; ymax += half;
    }
    *baseline = (float)ymax / 64.0f;
    *line_adv = (float)(ymax - ymin) / 64.0f;
    *deco = effect;
    return 1;
}

static FT_Face load_system_fallback_face(FT_Library ft_lib) {
    for (int i = 0; g_fallback_font_candidates[i] != NULL; i++) {
        const char *candidate = g_fallback_font_candidates[i];
        FT_Face face = NULL;
        if (FT_New_Face(ft_lib, candidate, 0, &face) == 0) {
            fprintf(stderr, "[ps4-font] Loaded system fallback font: %s\n", candidate);
            return face;
        }
    }
    return NULL;
}

static FontBacking *backing_retain(FontBacking *b) {
    if (b) b->refs++;
    return b;
}

static void backing_release(FontBacking *b) {
    if (!b) return;
    b->refs--;
    if (b->refs <= 0) {
        free(b->data);
        free(b);
    }
}

static void font_compute_pixel_scale(const InternalFontHandle *h, int use_render,
                                     float *out_w, float *out_h) {
    float w = use_render ? h->render_scale_w : h->scale_w;
    float ht = use_render ? h->render_scale_h : h->scale_h;
    if (!use_render && h->scale_unit != 0) {
        if (h->dpi_x != 0) w *= (float)h->dpi_x / 72.0f;
        if (h->dpi_y != 0) ht *= (float)h->dpi_y / 72.0f;
    }
    if (w <= 0.0f) w = 16.0f;
    if (ht <= 0.0f) ht = 16.0f;
    if (out_w) *out_w = w;
    if (out_h) *out_h = ht;
}

static void font_apply_ft_size(InternalFontHandle *h, int use_render) {
    if (!h || !h->ft_face) return;
    float pw = 16.0f, ph = 16.0f;
    font_compute_pixel_scale(h, use_render, &pw, &ph);
    FT_Set_Pixel_Sizes(h->ft_face, (FT_UInt)(pw + 0.5f), (FT_UInt)(ph + 0.5f));
}

static InternalFontHandle *find_or_create_font(uint64_t guest_addr) {
    for (int i = 0; i < MAX_FONTS; i++) {
        if (g_fonts[i].in_use && g_fonts[i].guest_addr == guest_addr) {
            return &g_fonts[i];
        }
    }
    for (int i = 0; i < MAX_FONTS; i++) {
        if (!g_fonts[i].in_use) {
            memset(&g_fonts[i], 0, sizeof(InternalFontHandle));
            g_fonts[i].in_use = 1;
            g_fonts[i].guest_addr = guest_addr;
            g_fonts[i].magic = 0x0F02;
            g_fonts[i].scale_unit = 0;
            g_fonts[i].scale_w = 16.0f;
            g_fonts[i].scale_h = 16.0f;
            g_fonts[i].render_scale_w = 16.0f;
            g_fonts[i].render_scale_h = 16.0f;
            g_fonts[i].weight_x = 1.0f;
            g_fonts[i].weight_y = 1.0f;
            g_fonts[i].render_weight_x = 1.0f;
            g_fonts[i].render_weight_y = 1.0f;
            g_fonts[i].dpi_x = 72;
            g_fonts[i].dpi_y = 72;
            return &g_fonts[i];
        }
    }
    return NULL;
}

static InternalFontHandle *get_font(uint64_t guest_addr) {
    if (!guest_addr) return NULL;
    for (int i = 0; i < MAX_FONTS; i++) {
        if (g_fonts[i].in_use && g_fonts[i].guest_addr == guest_addr) {
            return &g_fonts[i];
        }
    }
    return NULL;
}

static InternalString *find_or_create_string(uint64_t guest_addr) {
    for (int i = 0; i < MAX_STRINGS; i++) {
        if (g_strings[i].in_use && g_strings[i].guest_addr == guest_addr) {
            return &g_strings[i];
        }
    }
    for (int i = 0; i < MAX_STRINGS; i++) {
        if (!g_strings[i].in_use) {
            memset(&g_strings[i], 0, sizeof(InternalString));
            g_strings[i].in_use = 1;
            g_strings[i].guest_addr = guest_addr;
            return &g_strings[i];
        }
    }
    return NULL;
}

static InternalString *get_string(uint64_t guest_addr) {
    if (!guest_addr) return NULL;
    for (int i = 0; i < MAX_STRINGS; i++) {
        if (g_strings[i].in_use && g_strings[i].guest_addr == guest_addr) {
            return &g_strings[i];
        }
    }
    return NULL;
}

static InternalWriting *find_or_create_writing(uint64_t guest_addr) {
    for (int i = 0; i < MAX_WRITINGS; i++) {
        if (g_writings[i].in_use && g_writings[i].guest_addr == guest_addr) {
            return &g_writings[i];
        }
    }
    for (int i = 0; i < MAX_WRITINGS; i++) {
        if (!g_writings[i].in_use) {
            memset(&g_writings[i], 0, sizeof(InternalWriting));
            g_writings[i].in_use = 1;
            g_writings[i].guest_addr = guest_addr;
            return &g_writings[i];
        }
    }
    return NULL;
}

static InternalWriting *get_writing(uint64_t guest_addr) {
    if (!guest_addr) return NULL;
    for (int i = 0; i < MAX_WRITINGS; i++) {
        if (g_writings[i].in_use && g_writings[i].guest_addr == guest_addr) {
            return &g_writings[i];
        }
    }
    return NULL;
}

// 1. sceFontMemoryInit
void shim_sceFontMemoryInit(GuestContext *ctx) {
    uint64_t memGuest = ctx->rdi;
    uint64_t regionGuest = ctx->rsi;
    uint32_t regionSize = (uint32_t)ctx->rdx;
    uint64_t ifaceGuest = ctx->rcx;
    uint64_t mspaceGuest = ctx->r8;
    uint64_t destroyCbGuest = ctx->r9;
    uint64_t destroyCtx = 0;
    if (ctx->rsp && RECOMP_MEM_OK(ctx)) {
        destroyCtx = *(uint64_t *)(ctx->mem_base + ctx->rsp + 8);
    }

    if (!memGuest || !RECOMP_MEM_OK(ctx)) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_PARAMETER;
        SHIM_RETURN();
    }

    OrbisFontMem *mem = (OrbisFontMem *)(ctx->mem_base + memGuest);
    if (!ifaceGuest && (!regionGuest || regionSize == 0)) {
        mem->mem_kind = 0;
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_PARAMETER;
        SHIM_RETURN();
    }

    mem->mem_kind = 0x0F00;
    mem->attr_bits = 0;
    mem->region_size = regionSize;
    mem->region_base = (void *)(uintptr_t)regionGuest;
    mem->mspace_handle = (void *)(uintptr_t)mspaceGuest;
    mem->iface = (const OrbisFontMemInterface *)(uintptr_t)ifaceGuest;
    mem->on_destroy = (OrbisFontMemDestroyCb)(uintptr_t)destroyCbGuest;
    mem->destroy_ctx = (void *)(uintptr_t)destroyCtx;
    mem->some_ctx1 = NULL;
    mem->some_ctx2 = (void *)(uintptr_t)mspaceGuest;

    fprintf(stderr, "[ps4-font] sceFontMemoryInit: mem=0x%llx region=0x%llx size=0x%x mspace=0x%llx iface=0x%llx\n",
            (unsigned long long)memGuest, (unsigned long long)regionGuest, regionSize,
            (unsigned long long)mspaceGuest, (unsigned long long)ifaceGuest);

    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 2. sceFontMemoryTerm
void shim_sceFontMemoryTerm(GuestContext *ctx) {
    uint64_t memGuest = ctx->rdi;
    if (!memGuest || !RECOMP_MEM_OK(ctx)) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_PARAMETER;
        SHIM_RETURN();
    }
    OrbisFontMem *mem = (OrbisFontMem *)(ctx->mem_base + memGuest);
    if (mem->mem_kind != 0x0F00) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_MEMORY;
        SHIM_RETURN();
    }
    if (mem->on_destroy) {
        mem->mem_kind = 0;
        uint64_t cb = (uint64_t)(uintptr_t)mem->on_destroy;
        GuestContext sub = *ctx;
        sub.rdi = memGuest;
        sub.rsi = (uint64_t)(uintptr_t)mem->mspace_handle;
        sub.rdx = (uint64_t)(uintptr_t)mem->destroy_ctx;
        recomp_call_guest(&sub, cb);
    }
    memset(mem, 0, sizeof(*mem));
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 3. sceFontCreateLibraryWithEdition
void shim_sceFontCreateLibraryWithEdition(GuestContext *ctx) {
    uint64_t memGuest = ctx->rdi;
    uint64_t createParamsGuest = ctx->rsi;
    uint64_t edition = ctx->rdx;
    uint64_t pLibraryGuest = ctx->rcx;

    (void)edition;
    if (!memGuest || !createParamsGuest || !pLibraryGuest || !RECOMP_MEM_OK(ctx)) {
        if (pLibraryGuest && RECOMP_MEM_OK(ctx)) {
            *(uint64_t *)(ctx->mem_base + pLibraryGuest) = 0;
        }
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_PARAMETER;
        SHIM_RETURN();
    }

    pthread_mutex_lock(&g_font_mutex);
    if (!g_ft_library) {
        if (FT_Init_FreeType(&g_ft_library) != 0) {
            pthread_mutex_unlock(&g_font_mutex);
            ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_ALLOCATION_FAILED;
            SHIM_RETURN();
        }
    }
    pthread_mutex_unlock(&g_font_mutex);

    uint64_t libGuest = recomp_vm_alloc_named(ctx, 0x1000, PROT_READ | PROT_WRITE, 0, "font_lib");
    if (!libGuest) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_ALLOCATION_FAILED;
        SHIM_RETURN();
    }

    uint8_t *libPtr = (uint8_t *)(ctx->mem_base + libGuest);
    memset(libPtr, 0, 0x1000);
    *(uint16_t *)(libPtr + 0x00) = 0x0F01; // magic

    *(uint64_t *)(ctx->mem_base + pLibraryGuest) = libGuest;

    fprintf(stderr, "[ps4-font] sceFontCreateLibraryWithEdition: created lib=0x%llx\n", (unsigned long long)libGuest);
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 4. sceFontDestroyLibrary
void shim_sceFontDestroyLibrary(GuestContext *ctx) {
    uint64_t pLibraryGuest = ctx->rdi;
    if (pLibraryGuest && RECOMP_MEM_OK(ctx)) {
        *(uint64_t *)(ctx->mem_base + pLibraryGuest) = 0;
    }
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 5. sceFontSupportSystemFonts
void shim_sceFontSupportSystemFonts(GuestContext *ctx) {
    uint64_t libGuest = ctx->rdi;
    if (!libGuest || !RECOMP_MEM_OK(ctx)) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_LIBRARY;
        SHIM_RETURN();
    }
    uint8_t *libPtr = (uint8_t *)(ctx->mem_base + libGuest);
    *(uint64_t *)(libPtr + 0xA0) = 0x1; // sysfonts_ctx non-null
    fprintf(stderr, "[ps4-font] sceFontSupportSystemFonts: enabled system fonts for lib=0x%llx\n", (unsigned long long)libGuest);
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 6. sceFontSupportExternalFonts
void shim_sceFontSupportExternalFonts(GuestContext *ctx) {
    uint64_t libGuest = ctx->rdi;
    if (!libGuest || !RECOMP_MEM_OK(ctx)) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_LIBRARY;
        SHIM_RETURN();
    }
    uint8_t *libPtr = (uint8_t *)(ctx->mem_base + libGuest);
    *(uint64_t *)(libPtr + 0xA8) = 0x1; // external_fonts_ctx non-null
    fprintf(stderr, "[ps4-font] sceFontSupportExternalFonts: enabled external fonts (max=%u, formats=0x%x)\n",
            (unsigned int)ctx->rsi, (unsigned int)ctx->rdx);
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 7. sceFontCreateRendererWithEdition
void shim_sceFontCreateRendererWithEdition(GuestContext *ctx) {
    uint64_t pRendererGuest = ctx->rcx;
    if (!pRendererGuest || !RECOMP_MEM_OK(ctx)) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_PARAMETER;
        SHIM_RETURN();
    }
    uint64_t rendGuest = recomp_vm_alloc_named(ctx, 0x1000, PROT_READ | PROT_WRITE, 0, "font_renderer");
    if (!rendGuest) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_ALLOCATION_FAILED;
        SHIM_RETURN();
    }
    uint8_t *rendPtr = (uint8_t *)(ctx->mem_base + rendGuest);
    memset(rendPtr, 0, 0x1000);
    *(uint16_t *)(rendPtr + 0x00) = 0x0F07; // renderer magic
    *(uint64_t *)(ctx->mem_base + pRendererGuest) = rendGuest;
    fprintf(stderr, "[ps4-font] sceFontCreateRendererWithEdition: created renderer=0x%llx\n", (unsigned long long)rendGuest);
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 8. sceFontDestroyRenderer
void shim_sceFontDestroyRenderer(GuestContext *ctx) {
    uint64_t pRendererGuest = ctx->rdi;
    if (pRendererGuest && RECOMP_MEM_OK(ctx)) {
        *(uint64_t *)(ctx->mem_base + pRendererGuest) = 0;
    }
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 9. sceFontAttachDeviceCacheBuffer
void shim_sceFontAttachDeviceCacheBuffer(GuestContext *ctx) {
    uint64_t libGuest = ctx->rdi;
    uint64_t bufGuest = ctx->rsi;
    uint32_t size = (uint32_t)ctx->rdx;
    if (libGuest && RECOMP_MEM_OK(ctx)) {
        uint8_t *libPtr = (uint8_t *)(ctx->mem_base + libGuest);
        *(uint64_t *)(libPtr + 0xB0) = bufGuest;
    }
    fprintf(stderr, "[ps4-font] sceFontAttachDeviceCacheBuffer: lib=0x%llx buf=0x%llx size=0x%x\n",
            (unsigned long long)libGuest, (unsigned long long)bufGuest, size);
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 10. sceFontOpenFontSet
void shim_sceFontOpenFontSet(GuestContext *ctx) {
    uint64_t libGuest = ctx->rdi;
    uint32_t fontSetType = (uint32_t)ctx->rsi;
    uint32_t openMode = (uint32_t)ctx->rdx;
    uint64_t openParamsGuest = ctx->rcx;
    uint64_t pFontHandleGuest = ctx->r8;

    (void)openMode;
    (void)openParamsGuest;

    if (!libGuest || !pFontHandleGuest || !RECOMP_MEM_OK(ctx)) {
        if (pFontHandleGuest && RECOMP_MEM_OK(ctx)) {
            *(uint64_t *)(ctx->mem_base + pFontHandleGuest) = 0;
        }
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_PARAMETER;
        SHIM_RETURN();
    }

    pthread_mutex_lock(&g_font_mutex);
    if (!g_ft_library) {
        FT_Init_FreeType(&g_ft_library);
    }
    FT_Face face = load_system_fallback_face(g_ft_library);

    uint64_t fontGuest = recomp_vm_alloc_named(ctx, 0x1000, PROT_READ | PROT_WRITE, 0, "font_handle");
    if (!fontGuest) {
        pthread_mutex_unlock(&g_font_mutex);
        *(uint64_t *)(ctx->mem_base + pFontHandleGuest) = 0;
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_ALLOCATION_FAILED;
        SHIM_RETURN();
    }

    uint8_t *hPtr = (uint8_t *)(ctx->mem_base + fontGuest);
    memset(hPtr, 0, 0x1000);
    *(uint16_t *)(hPtr + 0x00) = 0x0F02; // native font magic
    *(uint64_t *)(hPtr + 0x28) = libGuest; // library

    InternalFontHandle *h = find_or_create_font(fontGuest);
    if (h) {
        h->library_guest = libGuest;
        h->ft_face = face;
    }
    pthread_mutex_unlock(&g_font_mutex);

    *(uint64_t *)(ctx->mem_base + pFontHandleGuest) = fontGuest;
    fprintf(stderr, "[ps4-font] sceFontOpenFontSet: fontSetType=0x%x -> handle=0x%llx\n",
            fontSetType, (unsigned long long)fontGuest);
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 11. sceFontCloseFont
void shim_sceFontCloseFont(GuestContext *ctx) {
    uint64_t fontGuest = ctx->rdi;
    pthread_mutex_lock(&g_font_mutex);
    InternalFontHandle *h = get_font(fontGuest);
    if (h) {
        if (h->ft_face) {
            FT_Done_Face(h->ft_face);
            h->ft_face = NULL;
        }
        if (h->backing) {
            backing_release(h->backing);
            h->backing = NULL;
        }
        h->in_use = 0;
    }
    if (fontGuest && RECOMP_MEM_OK(ctx)) {
        *(uint16_t *)(ctx->mem_base + fontGuest) = 0;
    }
    pthread_mutex_unlock(&g_font_mutex);
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 12. sceFontBindRenderer
void shim_sceFontBindRenderer(GuestContext *ctx) {
    uint64_t fontGuest = ctx->rdi;
    uint64_t rendGuest = ctx->rsi;
    pthread_mutex_lock(&g_font_mutex);
    InternalFontHandle *h = get_font(fontGuest);
    if (h) {
        h->renderer_guest = rendGuest;
    }
    if (fontGuest && RECOMP_MEM_OK(ctx)) {
        uint8_t *hPtr = (uint8_t *)(ctx->mem_base + fontGuest);
        *(uint64_t *)(hPtr + 0x30) = rendGuest;
    }
    pthread_mutex_unlock(&g_font_mutex);
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 13. sceFontUnbindRenderer
void shim_sceFontUnbindRenderer(GuestContext *ctx) {
    uint64_t fontGuest = ctx->rdi;
    pthread_mutex_lock(&g_font_mutex);
    InternalFontHandle *h = get_font(fontGuest);
    if (h) {
        h->renderer_guest = 0;
    }
    if (fontGuest && RECOMP_MEM_OK(ctx)) {
        uint8_t *hPtr = (uint8_t *)(ctx->mem_base + fontGuest);
        *(uint64_t *)(hPtr + 0x30) = 0;
    }
    pthread_mutex_unlock(&g_font_mutex);
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 14. sceFontRebindRenderer
void shim_sceFontRebindRenderer(GuestContext *ctx) {
    shim_sceFontBindRenderer(ctx);
}

// 15. sceFontSetupRenderScalePixel
void shim_sceFontSetupRenderScalePixel(GuestContext *ctx) {
    uint64_t fontGuest = ctx->rdi;
    float scale_w = ctx->xmm[0].f32[0];
    float scale_h = ctx->xmm[1].f32[0];
    pthread_mutex_lock(&g_font_mutex);
    InternalFontHandle *h = get_font(fontGuest);
    if (h) {
        h->render_scale_w = scale_w > 0.0f ? scale_w : 16.0f;
        h->render_scale_h = scale_h > 0.0f ? scale_h : 16.0f;
        font_apply_ft_size(h, 1);
    }
    pthread_mutex_unlock(&g_font_mutex);
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 16. sceFontSetScalePixel
void shim_sceFontSetScalePixel(GuestContext *ctx) {
    uint64_t fontGuest = ctx->rdi;
    float scale_w = ctx->xmm[0].f32[0];
    float scale_h = ctx->xmm[1].f32[0];
    pthread_mutex_lock(&g_font_mutex);
    InternalFontHandle *h = get_font(fontGuest);
    if (h) {
        h->scale_unit = 0;
        h->scale_w = scale_w > 0.0f ? scale_w : 16.0f;
        h->scale_h = scale_h > 0.0f ? scale_h : 16.0f;
        font_apply_ft_size(h, 0);
    }
    pthread_mutex_unlock(&g_font_mutex);
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 17. sceFontRenderSurfaceInit
void shim_sceFontRenderSurfaceInit(GuestContext *ctx) {
    uint64_t surfGuest = ctx->rdi;
    uint64_t bufGuest = ctx->rsi;
    int32_t bufWidthByte = (int32_t)ctx->rdx;
    int8_t pixelSizeByte = (int8_t)ctx->rcx;
    int32_t widthPixel = (int32_t)ctx->r8;
    int32_t heightPixel = (int32_t)ctx->r9;

    if (!surfGuest || !RECOMP_MEM_OK(ctx)) {
        SHIM_RETURN();
    }

    OrbisFontRenderSurface *surf = (OrbisFontRenderSurface *)(ctx->mem_base + surfGuest);
    memset(surf, 0, sizeof(*surf));
    surf->buffer = (void *)(uintptr_t)bufGuest;
    surf->widthByte = bufWidthByte;
    surf->pixelSizeByte = pixelSizeByte;
    surf->width = widthPixel >= 0 ? widthPixel : 0;
    surf->height = heightPixel >= 0 ? heightPixel : 0;
    surf->sc_x0 = 0;
    surf->sc_y0 = 0;
    surf->sc_x1 = (uint32_t)surf->width;
    surf->sc_y1 = (uint32_t)surf->height;

    SHIM_RETURN();
}

// 18. sceFontTextSourceInit
void shim_sceFontTextSourceInit(GuestContext *ctx) {
    uint64_t sourceGuest = ctx->rdi;
    uint64_t textAddress = ctx->rsi;
    uint32_t textSizeByte = (uint32_t)ctx->rdx;
    uint64_t textParser = ctx->rcx;
    uint64_t textObject = ctx->r8;

    if (!sourceGuest || !RECOMP_MEM_OK(ctx)) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_PARAMETER;
        SHIM_RETURN();
    }

    OrbisFontTextSource *src = (OrbisFontTextSource *)(ctx->mem_base + sourceGuest);
    memset(src, 0, sizeof(*src));
    src->systemUse0 = (1ULL << 32) | 0x54585352ULL; // 'TXSR'
    src->start = (const void *)(uintptr_t)textAddress;
    src->end = textSizeByte ? (const void *)(uintptr_t)(textAddress + textSizeByte) : NULL;
    src->current = src->start;
    src->textParser = (OrbisFontTextParseFunction)(uintptr_t)textParser;
    src->textObject = (void *)(uintptr_t)textObject;

    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 19. sceFontTextSourceSetDefaultFont
void shim_sceFontTextSourceSetDefaultFont(GuestContext *ctx) {
    uint64_t sourceGuest = ctx->rdi;
    uint64_t fontGuest = ctx->rsi;
    if (sourceGuest && RECOMP_MEM_OK(ctx)) {
        OrbisFontTextSource *src = (OrbisFontTextSource *)(ctx->mem_base + sourceGuest);
        src->defaultFont = (OrbisFontHandle)(uintptr_t)fontGuest;
    }
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 20. sceFontTextSourceSetWritingForm
void shim_sceFontTextSourceSetWritingForm(GuestContext *ctx) {
    uint64_t sourceGuest = ctx->rdi;
    uint32_t form = (uint32_t)ctx->rsi;
    if (sourceGuest && RECOMP_MEM_OK(ctx)) {
        OrbisFontTextSource *src = (OrbisFontTextSource *)(ctx->mem_base + sourceGuest);
        src->systemUse0 = ((uint64_t)form << 32) | (src->systemUse0 & 0xFFFFFFFFULL);
    }
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 21. sceFontCreateString
void shim_sceFontCreateString(GuestContext *ctx) {
    uint64_t memGuest = ctx->rdi;
    uint64_t srcGuest = ctx->rsi;
    uint64_t detailGuest = ctx->rdx;
    uint64_t pStringGuest = ctx->rcx;

    (void)detailGuest;
    if (!memGuest || !srcGuest || !pStringGuest || !RECOMP_MEM_OK(ctx)) {
        if (pStringGuest && RECOMP_MEM_OK(ctx)) {
            *(uint64_t *)(ctx->mem_base + pStringGuest) = 0;
        }
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_PARAMETER;
        SHIM_RETURN();
    }

    OrbisFontTextSource *src = (OrbisFontTextSource *)(ctx->mem_base + srcGuest);
    uint64_t strGuest = recomp_vm_alloc_named(ctx, 0x10000, PROT_READ | PROT_WRITE, 0, "font_string");
    if (!strGuest) {
        *(uint64_t *)(ctx->mem_base + pStringGuest) = 0;
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_ALLOCATION_FAILED;
        SHIM_RETURN();
    }

    pthread_mutex_lock(&g_font_mutex);
    InternalString *s = find_or_create_string(strGuest);
    if (s) {
        s->memory_guest = memGuest;
        s->source_guest = srcGuest;
        s->default_font_guest = (uint64_t)(uintptr_t)src->defaultFont;
        s->writing_form = (uint32_t)(src->systemUse0 >> 32);
        s->character_count = 0;
        s->head_node_guest = 0;

        // Parse text if parser is provided
        if (src->textParser && src->start) {
            uint64_t parserFn = (uint64_t)(uintptr_t)src->textParser;
            uint64_t nodeAllocGuest = strGuest + 0x200;
            uint64_t prevNodeGuest = 0;
            uint64_t orderPtrGuest = strGuest + 0x80;
            uint64_t parseResultGuest = strGuest + 0x100;

            *(uint64_t *)(ctx->mem_base + orderPtrGuest) = 0;
            memset(ctx->mem_base + parseResultGuest, 0, sizeof(OrbisFontTextParseResult));

            pthread_mutex_unlock(&g_font_mutex);

            for (uint32_t i = 0; i < 256; i++) {
                memset(ctx->mem_base + parseResultGuest, 0, sizeof(OrbisFontTextParseResult));

                GuestContext sub = *ctx;
                sub.rdi = srcGuest;
                sub.rsi = orderPtrGuest;
                sub.rdx = parseResultGuest;
                recomp_call_guest(&sub, parserFn);

                int32_t state = (int32_t)sub.rax;
                if (state == 1) { // kTextParserResultFontCode
                    OrbisFontTextParseResult *res = (OrbisFontTextParseResult *)(ctx->mem_base + parseResultGuest);
                    uint64_t order = *(uint64_t *)(ctx->mem_base + orderPtrGuest);
                    uint64_t nodeGuest = nodeAllocGuest + (i * sizeof(OrbisFontTextCharacter));

                    OrbisFontTextCharacter *node = (OrbisFontTextCharacter *)(ctx->mem_base + nodeGuest);
                    memset(node, 0, sizeof(*node));
                    node->prev = prevNodeGuest ? (OrbisFontTextCharacter *)(uintptr_t)prevNodeGuest : NULL;
                    node->next = NULL;
                    node->textOrder = (void *)(uintptr_t)order;
                    node->font = res->FontCode.font ? res->FontCode.font : (OrbisFontHandle)(uintptr_t)s->default_font_guest;
                    node->shapeEntry = NULL;
                    node->characterCode = res->FontCode.code;
                    node->clusterSpan = 1;
                    node->clusterIndex = 0;
                    node->clusterKind = 0;
                    node->synthetic = 0;
                    uint32_t code = res->FontCode.code;
                    bool is_ws = (code == 0x20 || code == 0x09 || code == 0x0a || code == 0x0b || code == 0x0c || code == 0x0d || code == 0x3000 || (code >= 0x2000 && code <= 0x200a));
                    node->flags = (is_ws ? 0x0EULL : 0ULL) << 8;

                    if (prevNodeGuest) {
                        OrbisFontTextCharacter *prev = (OrbisFontTextCharacter *)(ctx->mem_base + prevNodeGuest);
                        prev->next = (OrbisFontTextCharacter *)(uintptr_t)nodeGuest;
                    } else {
                        s->head_node_guest = nodeGuest;
                    }
                    prevNodeGuest = nodeGuest;
                    s->character_count++;
                } else if (state == 0) { // kTextParserResultTerminate
                    OrbisFontTextParseResult *res = (OrbisFontTextParseResult *)(ctx->mem_base + parseResultGuest);
                    s->terminate_code = res->Terminate.terminateCode;
                    s->terminate_order = *(uint64_t *)(ctx->mem_base + orderPtrGuest);
                    break;
                } else {
                    break;
                }
            }

            pthread_mutex_lock(&g_font_mutex);
        }
        fprintf(stderr, "[ps4-font] sceFontCreateString: src=0x%llx parser=0x%llx count=%u\n",
                (unsigned long long)srcGuest, (unsigned long long)(src ? (uintptr_t)src->textParser : 0),
                s ? s->character_count : 0);
    }
    pthread_mutex_unlock(&g_font_mutex);

    *(uint64_t *)(ctx->mem_base + pStringGuest) = strGuest;
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 22. sceFontDestroyString
void shim_sceFontDestroyString(GuestContext *ctx) {
    uint64_t pStringGuest = ctx->rdi;
    if (pStringGuest && RECOMP_MEM_OK(ctx)) {
        uint64_t strGuest = *(uint64_t *)(ctx->mem_base + pStringGuest);
        pthread_mutex_lock(&g_font_mutex);
        InternalString *s = get_string(strGuest);
        if (s) {
            s->in_use = 0;
        }
        pthread_mutex_unlock(&g_font_mutex);
        if (strGuest) {
            recomp_vm_free(ctx, strGuest, 0x10000);
        }
        *(uint64_t *)(ctx->mem_base + pStringGuest) = 0;
    }
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 23. sceFontStringRefersTextCharacters
void shim_sceFontStringRefersTextCharacters(GuestContext *ctx) {
    uint64_t strGuest = ctx->rdi;
    uint64_t pCountGuest = ctx->rsi;

    pthread_mutex_lock(&g_font_mutex);
    InternalString *s = get_string(strGuest);
    uint64_t head = 0;
    uint32_t count = 0;
    if (s) {
        head = s->head_node_guest;
        count = s->character_count;
    }
    pthread_mutex_unlock(&g_font_mutex);

    if (pCountGuest && RECOMP_MEM_OK(ctx)) {
        *(uint32_t *)(ctx->mem_base + pCountGuest) = count;
    }
    ctx->rax = head;
    SHIM_RETURN();
}

// 24. sceFontStringGetTerminateOrder
void shim_sceFontStringGetTerminateOrder(GuestContext *ctx) {
    uint64_t strGuest = ctx->rdi;
    pthread_mutex_lock(&g_font_mutex);
    InternalString *s = get_string(strGuest);
    uint64_t order = s ? s->terminate_order : 0;
    pthread_mutex_unlock(&g_font_mutex);
    ctx->rax = order;
    SHIM_RETURN();
}

// 25. sceFontStringGetTerminateCode
void shim_sceFontStringGetTerminateCode(GuestContext *ctx) {
    uint64_t strGuest = ctx->rdi;
    pthread_mutex_lock(&g_font_mutex);
    InternalString *s = get_string(strGuest);
    uint32_t code = s ? s->terminate_code : 0;
    pthread_mutex_unlock(&g_font_mutex);
    ctx->rax = code;
    SHIM_RETURN();
}

// 26. sceFontStringRefersRenderCharacters
void shim_sceFontStringRefersRenderCharacters(GuestContext *ctx) {
    uint64_t strGuest = ctx->rdi;
    uint64_t startCharGuest = ctx->rsi;
    uint64_t lastCharGuest = ctx->rdx;
    uint64_t pCountGuest = ctx->rcx;

    pthread_mutex_lock(&g_font_mutex);
    InternalString *s = get_string(strGuest);
    uint64_t head = s ? s->head_node_guest : 0;
    uint64_t retNode = startCharGuest ? startCharGuest : head;
    uint32_t count = 0;

    if (s && retNode && RECOMP_MEM_OK(ctx)) {
        uint64_t curr = retNode;
        while (curr) {
            OrbisFontTextCharacter *node = (OrbisFontTextCharacter *)(ctx->mem_base + curr);
            if (node->clusterIndex == 0) {
                count++;
            }
            if (curr == lastCharGuest) {
                break;
            }
            curr = (uint64_t)(uintptr_t)node->next;
        }
        if (lastCharGuest && curr != lastCharGuest) {
            count = 0;
            retNode = 0;
        }
    }
    pthread_mutex_unlock(&g_font_mutex);

    if (pCountGuest && RECOMP_MEM_OK(ctx)) {
        *(uint32_t *)(ctx->mem_base + pCountGuest) = count;
    }
    fprintf(stderr, "[ps4-font] sceFontStringRefersRenderCharacters: str=0x%llx count=%u ret=0x%llx start=0x%llx last=0x%llx\n",
            (unsigned long long)strGuest, count, (unsigned long long)retNode,
            (unsigned long long)startCharGuest, (unsigned long long)lastCharGuest);
    ctx->rax = retNode;
    SHIM_RETURN();
}

// 27. sceFontStringGetWritingForm
void shim_sceFontStringGetWritingForm(GuestContext *ctx) {
    uint64_t strGuest = ctx->rdi;
    pthread_mutex_lock(&g_font_mutex);
    InternalString *s = get_string(strGuest);
    uint32_t form = s ? s->writing_form : 0;
    pthread_mutex_unlock(&g_font_mutex);
    ctx->rax = form;
    SHIM_RETURN();
}

// 28. sceFontCreateWritingLine
void shim_sceFontCreateWritingLine(GuestContext *ctx) {
    uint64_t pWritingLineGuest = ctx->rdx;
    if (pWritingLineGuest && RECOMP_MEM_OK(ctx)) {
        uint64_t lineGuest = recomp_vm_alloc_named(ctx, 0x1000, PROT_READ | PROT_WRITE, 0, "font_writing_line");
        *(uint64_t *)(ctx->mem_base + pWritingLineGuest) = lineGuest;
    }
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 29. sceFontDestroyWritingLine
void shim_sceFontDestroyWritingLine(GuestContext *ctx) {
    uint64_t pWritingLineGuest = ctx->rdi;
    if (pWritingLineGuest && RECOMP_MEM_OK(ctx)) {
        *(uint64_t *)(ctx->mem_base + pWritingLineGuest) = 0;
    }
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 30. sceFontWritingLineClear
void shim_sceFontWritingLineClear(GuestContext *ctx) {
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 31. sceFontWritingLineGetOrderingSpace
void shim_sceFontWritingLineGetOrderingSpace(GuestContext *ctx) {
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 32. sceFontWritingLineGetRenderMetrics
void shim_sceFontWritingLineGetRenderMetrics(GuestContext *ctx) {
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 33. sceFontWritingLineWritesOrder
void shim_sceFontWritingLineWritesOrder(GuestContext *ctx) {
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 34. sceFontWritingLineRefersRenderStep
void shim_sceFontWritingLineRefersRenderStep(GuestContext *ctx) {
    ctx->rax = 0;
    SHIM_RETURN();
}

// 35. sceFontWritingInit
void shim_sceFontWritingInit(GuestContext *ctx) {
    uint64_t writingGuest = ctx->rdi;
    uint64_t stringGuest = ctx->rsi;
    uint64_t startCharGuest = ctx->rdx;

    pthread_mutex_lock(&g_font_mutex);
    InternalWriting *w = find_or_create_writing(writingGuest);
    if (w) {
        w->string_guest = stringGuest;
        InternalString *s = get_string(stringGuest);
        w->current_node_guest = startCharGuest ? startCharGuest : (s ? s->head_node_guest : 0);
        w->step_char_node_guest = 0;
        w->current_index = 0;
        w->cursor_x = 0.0f;
        w->cursor_y = 0.0f;
    }
    pthread_mutex_unlock(&g_font_mutex);

    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 36. sceFontWritingGetRenderMetrics
void shim_sceFontWritingGetRenderMetrics(GuestContext *ctx) {
    uint64_t writingGuest = ctx->rdi;
    uint64_t metricsGuest = ctx->rsi;

    if (metricsGuest && RECOMP_MEM_OK(ctx)) {
        OrbisFontWritingMetrics *m = (OrbisFontWritingMetrics *)(ctx->mem_base + metricsGuest);
        memset(m, 0, sizeof(*m));

        pthread_mutex_lock(&g_font_mutex);
        InternalWriting *w = get_writing(writingGuest);
        if (w) {
            InternalString *s = get_string(w->string_guest);
            float totalAdvance = (s ? (float)s->character_count : 0.0f) * 16.0f;
            m->advanceX = totalAdvance;
            m->advanceY = 0.0f;
            m->Extent.left = 0.0f;
            m->Extent.right = totalAdvance;
            m->Extent.top = 16.0f;
            m->Extent.bottom = 0.0f;
        }
        pthread_mutex_unlock(&g_font_mutex);
    }

    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 37. sceFontWritingRefersRenderStep
void shim_sceFontWritingRefersRenderStep(GuestContext *ctx) {
    uint64_t writingGuest = ctx->rdi;
    uint64_t retStepGuest = 0;

    pthread_mutex_lock(&g_font_mutex);
    InternalWriting *w = get_writing(writingGuest);
    if (w && w->current_node_guest && RECOMP_MEM_OK(ctx)) {
        OrbisFontTextCharacter *node = (OrbisFontTextCharacter *)(ctx->mem_base + w->current_node_guest);
        InternalFontHandle *h = get_font((uint64_t)(uintptr_t)node->font);

        float adv = h ? h->scale_w : 16.0f;
        memset(&w->current_step, 0, sizeof(w->current_step));
        w->current_step.x = w->cursor_x;
        w->current_step.y = w->cursor_y;
        w->current_step.advanceX = adv;
        w->current_step.advanceY = 0.0f;
        w->current_step.font = node->font;
        w->current_step.glyphCode = node->characterCode;
        w->current_step.Profile.characterCount = 1;
        w->current_step.Positioning.x = w->cursor_x;
        w->current_step.Positioning.y = w->cursor_y;
        w->current_step.GlyphMetrics.width = adv;
        w->current_step.GlyphMetrics.height = h ? h->scale_h : 16.0f;
        w->current_step.GlyphMetrics.Horizontal.advance = adv;
        w->current_step.GlyphMetrics.Horizontal.bearingX = 0.0f;
        w->current_step.GlyphMetrics.Horizontal.bearingY = 0.0f;

        w->cursor_x += adv;
        w->step_char_node_guest = w->current_node_guest;
        w->current_node_guest = (uint64_t)(uintptr_t)node->next;
        w->current_index++;

        // Step buffer inside writing guest memory at offset 0x80
        retStepGuest = writingGuest + 0x80;
        memcpy(ctx->mem_base + retStepGuest, &w->current_step, sizeof(OrbisFontWritingStep));
    }
    pthread_mutex_unlock(&g_font_mutex);

    ctx->rax = retStepGuest;
    SHIM_RETURN();
}

// 38. sceFontWritingRefersRenderStepCharacter
void shim_sceFontWritingRefersRenderStepCharacter(GuestContext *ctx) {
    uint64_t writingGuest = ctx->rdi;
    uint64_t letterStepGuest = ctx->rsi;
    uint64_t retCharGuest = 0;

    pthread_mutex_lock(&g_font_mutex);
    InternalWriting *w = get_writing(writingGuest);
    if (w && RECOMP_MEM_OK(ctx)) {
        retCharGuest = w->step_char_node_guest;
        if (letterStepGuest) {
            OrbisFontWritingLetterStep *ls = (OrbisFontWritingLetterStep *)(ctx->mem_base + letterStepGuest);
            memset(ls, 0, sizeof(*ls));
            ls->x = w->current_step.x;
            ls->y = w->current_step.y;
            ls->advanceX = w->current_step.advanceX;
            ls->advanceY = w->current_step.advanceY;
            ls->Components.textsCount = 1;
            ls->Components.textsIndex = w->current_index > 0 ? w->current_index - 1 : 0;
            ls->Components.glyphsCount = 1;
            ls->Components.glyphsIndex = ls->Components.textsIndex;
            ls->Components.characterTextCount = 1;
            ls->Components.baseTextCount = 1;
        }
    }
    pthread_mutex_unlock(&g_font_mutex);

    ctx->rax = retCharGuest;
    SHIM_RETURN();
}

// 39. sceFontCharacterGetBidiLevel
void shim_sceFontCharacterGetBidiLevel(GuestContext *ctx) {
    uint64_t bidiGuest = ctx->rsi;
    if (bidiGuest && RECOMP_MEM_OK(ctx)) {
        *(int32_t *)(ctx->mem_base + bidiGuest) = 0;
    }
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 40. sceFontCharacterGetSyllableStringState
void shim_sceFontCharacterGetSyllableStringState(GuestContext *ctx) {
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 41. sceFontCharacterGetTextFontCode
void shim_sceFontCharacterGetTextFontCode(GuestContext *ctx) {
    uint64_t nodeGuest = ctx->rdi;
    uint64_t pFontGuest = ctx->rsi;
    uint64_t pCodeGuest = ctx->rdx;

    if (!nodeGuest || !pFontGuest || !pCodeGuest || !RECOMP_MEM_OK(ctx)) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_PARAMETER;
        SHIM_RETURN();
    }

    OrbisFontTextCharacter *node = (OrbisFontTextCharacter *)(ctx->mem_base + nodeGuest);
    *(uint64_t *)(ctx->mem_base + pFontGuest) = (uint64_t)(uintptr_t)node->font;
    *(uint32_t *)(ctx->mem_base + pCodeGuest) = node->characterCode;

    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 42. sceFontCharacterGetTextOrder
void shim_sceFontCharacterGetTextOrder(GuestContext *ctx) {
    uint64_t nodeGuest = ctx->rdi;
    uint64_t pOrderGuest = ctx->rsi;

    if (!nodeGuest || !pOrderGuest || !RECOMP_MEM_OK(ctx)) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_PARAMETER;
        SHIM_RETURN();
    }

    OrbisFontTextCharacter *node = (OrbisFontTextCharacter *)(ctx->mem_base + nodeGuest);
    *(uint64_t *)(ctx->mem_base + pOrderGuest) = (uint64_t)(uintptr_t)node->textOrder;

    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 43. sceFontCharacterLooksWhiteSpace
void shim_sceFontCharacterLooksWhiteSpace(GuestContext *ctx) {
    uint64_t nodeGuest = ctx->rdi;
    uint32_t ret = 0;
    if (nodeGuest && RECOMP_MEM_OK(ctx)) {
        OrbisFontTextCharacter *node = (OrbisFontTextCharacter *)(ctx->mem_base + nodeGuest);
        if (node->characterCode == ' ' || node->characterCode == '\t' || node->characterCode == 0x3000) {
            ret = node->characterCode;
        }
    }
    ctx->rax = ret;
    SHIM_RETURN();
}

// 44. sceFontCharacterRefersTextNext
void shim_sceFontCharacterRefersTextNext(GuestContext *ctx) {
    uint64_t nodeGuest = ctx->rdi;
    uint64_t next = 0;
    if (nodeGuest && RECOMP_MEM_OK(ctx)) {
        OrbisFontTextCharacter *node = (OrbisFontTextCharacter *)(ctx->mem_base + nodeGuest);
        next = (uint64_t)(uintptr_t)node->next;
    }
    ctx->rax = next;
    SHIM_RETURN();
}

// 45. sceFontCharactersRefersTextCodes
void shim_sceFontCharactersRefersTextCodes(GuestContext *ctx) {
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 46. sceFontTextCodesStepNext
void shim_sceFontTextCodesStepNext(GuestContext *ctx) {
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 47. sceFontWordsFindWordCharacters
void shim_sceFontWordsFindWordCharacters(GuestContext *ctx) {
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

static int32_t font_register_opened_handle(GuestContext *ctx, uint64_t libGuest,
                                           uint64_t preallocFontGuest, FT_Face face,
                                           FontBacking *backing, const InternalFontHandle *copy_from,
                                           uint64_t *out_handle_guest) {
    uint64_t fontGuest = preallocFontGuest;
    if (!fontGuest) {
        fontGuest = recomp_vm_alloc_named(ctx, 0x1000, PROT_READ | PROT_WRITE, 0, "font_handle");
        if (!fontGuest) {
            return ORBIS_FONT_ERROR_ALLOCATION_FAILED;
        }
    }
    uint8_t *hPtr = (uint8_t *)(ctx->mem_base + fontGuest);
    memset(hPtr, 0, 0x100);
    *(uint16_t *)(hPtr + 0x00) = 0x0F02;
    *(uint64_t *)(hPtr + 0x28) = libGuest;

    InternalFontHandle *h = find_or_create_font(fontGuest);
    if (!h) {
        return ORBIS_FONT_ERROR_ALLOCATION_FAILED;
    }
    h->library_guest = libGuest;
    h->ft_face = face;
    h->backing = backing;
    if (copy_from) {
        h->scale_unit = copy_from->scale_unit;
        h->scale_w = copy_from->scale_w;
        h->scale_h = copy_from->scale_h;
        h->render_scale_w = copy_from->render_scale_w;
        h->render_scale_h = copy_from->render_scale_h;
        h->weight_x = copy_from->weight_x;
        h->weight_y = copy_from->weight_y;
        h->slant = copy_from->slant;
        h->render_weight_x = copy_from->render_weight_x;
        h->render_weight_y = copy_from->render_weight_y;
        h->render_slant = copy_from->render_slant;
        h->dpi_x = copy_from->dpi_x;
        h->dpi_y = copy_from->dpi_y;
    }
    font_apply_ft_size(h, 0);
    *out_handle_guest = fontGuest;
    return ORBIS_FONT_OK;
}

static void font_render_glyph_common(GuestContext *ctx, int add_baseline) {
    uint64_t fontGuest = ctx->rdi;
    uint32_t code = (uint32_t)ctx->rsi;
    uint64_t surfGuest = ctx->rdx;
    float x = ctx->xmm[0].f32[0];
    float y = ctx->xmm[1].f32[0];
    uint64_t metricsGuest = ctx->rcx;
    uint64_t resultGuest = ctx->r8;

    (void)resultGuest;

    if (!fontGuest || !RECOMP_MEM_OK(ctx)) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_FONT_HANDLE;
        SHIM_RETURN();
    }
    if (code == 0) {
        if (metricsGuest) memset(ctx->mem_base + metricsGuest, 0, sizeof(OrbisFontGlyphMetrics));
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_NO_SUPPORT_CODE;
        SHIM_RETURN();
    }
    if (!surfGuest) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_PARAMETER;
        SHIM_RETURN();
    }

    pthread_mutex_lock(&g_font_mutex);
    InternalFontHandle *h = get_font(fontGuest);
    if (!h) {
        pthread_mutex_unlock(&g_font_mutex);
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_FONT_HANDLE;
        SHIM_RETURN();
    }

    float adv = h->render_scale_w;
    float hgt = h->render_scale_h;
    float gw = adv;
    float gh = hgt;
    float bx = 0.0f;
    float by = hgt;

    uint32_t upd_x = 0, upd_y = 0, upd_w = 0, upd_h = 0;
    uint64_t surf_buf_guest = 0;
    uint32_t surf_wb = 0;
    uint8_t surf_bpp = 0;

    if (h->ft_face) {
        font_apply_ft_size(h, 1);
        float active_slant = h->render_slant != 0.0f ? h->render_slant : h->slant;
        if (active_slant != 0.0f) {
            FT_Matrix mat;
            mat.xx = 0x10000L;
            mat.xy = (FT_Fixed)(active_slant * 65536.0f);
            mat.yx = 0;
            mat.yy = 0x10000L;
            FT_Set_Transform(h->ft_face, &mat, NULL);
        } else {
            FT_Set_Transform(h->ft_face, NULL, NULL);
        }

        float y_used = y;
        if (add_baseline) {
            float asc = (float)(h->ft_face->size->metrics.ascender >> 6);
            if (asc <= 0.0f) asc = hgt * 0.8f;
            {   // the library adds the layout baseline (render scale, render weight), see font_bbox_layout
                float b, la, de, wx, wy;
                font_active_weight(h, 1, &wx, &wy);
                if (font_bbox_layout(h, wy, &b, &la, &de)) asc = b;
            }
            y_used = y + asc;
        }

        FT_UInt glyph_idx = FT_Get_Char_Index(h->ft_face, code);
        if (glyph_idx != 0 && FT_Load_Glyph(h->ft_face, glyph_idx, FT_LOAD_DEFAULT) == 0) {
            FT_Render_Glyph(h->ft_face->glyph, FT_RENDER_MODE_NORMAL);
            FT_GlyphSlot slot = h->ft_face->glyph;
            adv = (float)(slot->advance.x >> 6);
            if (adv <= 0.0f) adv = h->render_scale_w;
            gw = (float)slot->bitmap.width;
            gh = (float)slot->bitmap.rows;
            bx = (float)slot->bitmap_left;
            by = (float)slot->bitmap_top;

            OrbisFontRenderSurface *surf = (OrbisFontRenderSurface *)(ctx->mem_base + surfGuest);
            if (surf && surf->buffer) {
                surf_buf_guest = (uint64_t)(uintptr_t)surf->buffer;
                surf_wb = (uint32_t)surf->widthByte;
                surf_bpp = (uint8_t)(surf->pixelSizeByte > 0 ? surf->pixelSizeByte : 4);

                int sx0 = (int)surf->sc_x0;
                int sy0 = (int)surf->sc_y0;
                int sx1 = (int)surf->sc_x1;
                int sy1 = (int)surf->sc_y1;
                if (sx0 < 0) sx0 = 0;
                if (sy0 < 0) sy0 = 0;
                if (sx1 > surf->width) sx1 = surf->width;
                if (sy1 > surf->height) sy1 = surf->height;

                int dst_x0 = (int)x + slot->bitmap_left;
                int dst_y0 = (int)y_used - slot->bitmap_top;
                int start_col = dst_x0 > sx0 ? dst_x0 : sx0;
                int end_col = (dst_x0 + (int)slot->bitmap.width) < sx1 ? (dst_x0 + (int)slot->bitmap.width) : sx1;
                int start_row = dst_y0 > sy0 ? dst_y0 : sy0;
                int end_row = (dst_y0 + (int)slot->bitmap.rows) < sy1 ? (dst_y0 + (int)slot->bitmap.rows) : sy1;

                upd_x = (uint32_t)(start_col > 0 ? start_col : 0);
                upd_y = (uint32_t)(start_row > 0 ? start_row : 0);
                upd_w = (uint32_t)(end_col > start_col ? end_col - start_col : 0);
                upd_h = (uint32_t)(end_row > start_row ? end_row - start_row : 0);

                if (slot->bitmap.buffer && upd_w > 0 && upd_h > 0) {
                    uint8_t *dst_buf = (uint8_t *)(ctx->mem_base + surf_buf_guest);
                    int bpp = (int)surf_bpp;
                    for (int py = start_row; py < end_row; py++) {
                        int row = py - dst_y0;
                        for (int px = start_col; px < end_col; px++) {
                            int col = px - dst_x0;
                            uint8_t alpha = slot->bitmap.buffer[row * slot->bitmap.pitch + col];
                            uint8_t *dst_pixel = dst_buf + py * surf->widthByte + px * bpp;
                            if (bpp == 4) {
                                dst_pixel[0] = alpha;
                                dst_pixel[1] = alpha;
                                dst_pixel[2] = alpha;
                                dst_pixel[3] = alpha;
                            } else if (bpp == 1) {
                                dst_pixel[0] = alpha;
                            }
                        }
                    }
                    extern void ps4_gnm_notify_cpu_write(uint64_t addr, uint64_t size) __attribute__((weak));
                    if (ps4_gnm_notify_cpu_write && surf->widthByte > 0) {
                        uint64_t base = surf_buf_guest + (uint64_t)upd_y * (uint64_t)surf->widthByte;
                        uint64_t sz = (uint64_t)upd_h * (uint64_t)surf->widthByte;
                        ps4_gnm_notify_cpu_write(base, sz);
                    }
                }
            }
        }
    }
    pthread_mutex_unlock(&g_font_mutex);

    if (metricsGuest) {
        OrbisFontGlyphMetrics *m = (OrbisFontGlyphMetrics *)(ctx->mem_base + metricsGuest);
        memset(m, 0, sizeof(*m));
        m->width = gw;
        m->height = gh;
        m->Horizontal.advance = adv;
        m->Horizontal.bearingX = bx;
        m->Horizontal.bearingY = by;
    }

    if (resultGuest) {
        OrbisFontRenderOutput *res = (OrbisFontRenderOutput *)(ctx->mem_base + resultGuest);
        memset(res, 0, sizeof(*res));
        res->stage = NULL;
        res->SurfaceImage.widthByte = surf_wb;
        res->SurfaceImage.pixelSizeByte = surf_bpp;
        res->SurfaceImage.pixelFormat = 0;
        res->UpdateRect.x = upd_x;
        res->UpdateRect.y = upd_y;
        res->UpdateRect.w = upd_w;
        res->UpdateRect.h = upd_h;
        if (surf_buf_guest) {
            res->SurfaceImage.address = (void *)(uintptr_t)(
                surf_buf_guest + (uint64_t)upd_y * (uint64_t)surf_wb + (uint64_t)upd_x * (uint64_t)surf_bpp);
        }
        res->ImageMetrics.bearingX = bx;
        res->ImageMetrics.bearingY = by;
        res->ImageMetrics.advance = adv;
        res->ImageMetrics.stride = adv;
        res->ImageMetrics.width = upd_w;
        res->ImageMetrics.height = upd_h;
    }

    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 48. sceFontRenderCharGlyphImageHorizontal
void shim_sceFontRenderCharGlyphImageHorizontal(GuestContext *ctx) {
    font_render_glyph_common(ctx, 0);
}

// 49. sceFontRenderCharGlyphImage
void shim_sceFontRenderCharGlyphImage(GuestContext *ctx) {
    font_render_glyph_common(ctx, 1);
}

// 50. sceFontOpenFontFile
void shim_sceFontOpenFontFile(GuestContext *ctx) {
    uint64_t libGuest = ctx->rdi;
    uint64_t pathGuest = ctx->rsi;
    uint32_t openMode = (uint32_t)ctx->rdx;
    uint64_t openDetailGuest = ctx->rcx;
    uint64_t pFontHandleGuest = ctx->r8;
    (void)openMode;

    if (!libGuest || !RECOMP_MEM_OK(ctx)) {
        if (pFontHandleGuest && RECOMP_MEM_OK(ctx)) *(uint64_t *)(ctx->mem_base + pFontHandleGuest) = 0;
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_LIBRARY;
        SHIM_RETURN();
    }
    if (!pFontHandleGuest || !pathGuest) {
        if (pFontHandleGuest) *(uint64_t *)(ctx->mem_base + pFontHandleGuest) = 0;
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_PARAMETER;
        SHIM_RETURN();
    }

    const char *guestPath = (const char *)(ctx->mem_base + pathGuest);
    uint32_t subfont_index = 0;
    if (openDetailGuest) {
        const OrbisFontOpenParams *params = (const OrbisFontOpenParams *)(ctx->mem_base + openDetailGuest);
        subfont_index = params->subfont_index;
    }

    uint64_t prealloc = *(uint64_t *)(ctx->mem_base + pFontHandleGuest);
    uint8_t *fileBuf = NULL;
    size_t fileSize = 0;

    int fd = ps4_vfs_open(guestPath, O_RDONLY, 0);
    if (fd >= 0) {
        struct stat st;
        if (ps4_vfs_fstat(fd, &st) == 0 && st.st_size > 0) {
            fileSize = (size_t)st.st_size;
            fileBuf = (uint8_t *)malloc(fileSize);
            if (fileBuf) {
                size_t got = 0;
                while (got < fileSize) {
                    ssize_t r = ps4_vfs_read(fd, fileBuf + got, fileSize - got);
                    if (r <= 0) break;
                    got += (size_t)r;
                }
                if (got != fileSize) {
                    free(fileBuf);
                    fileBuf = NULL;
                }
            }
        }
        ps4_vfs_close(fd);
    }

    pthread_mutex_lock(&g_font_mutex);
    if (!g_ft_library) {
        FT_Init_FreeType(&g_ft_library);
    }
    FT_Face face = NULL;
    FontBacking *backing = NULL;
    if (fileBuf) {
        if (FT_New_Memory_Face(g_ft_library, fileBuf, (FT_Long)fileSize, (FT_Long)subfont_index, &face) == 0) {
            backing = (FontBacking *)calloc(1, sizeof(FontBacking));
            if (backing) {
                backing->data = fileBuf;
                backing->size = fileSize;
                backing->subfont_index = subfont_index;
                backing->refs = 1;
            } else {
                FT_Done_Face(face);
                face = NULL;
                free(fileBuf);
            }
        } else {
            free(fileBuf);
        }
    }
    if (!face) {
        pthread_mutex_unlock(&g_font_mutex);
        *(uint64_t *)(ctx->mem_base + pFontHandleGuest) = 0;
        fprintf(stderr, "[ps4-font] sceFontOpenFontFile failed: path=%s\n", guestPath);
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_FS_OPEN_FAILED;
        SHIM_RETURN();
    }

    uint64_t outHandle = 0;
    int32_t rc = font_register_opened_handle(ctx, libGuest, prealloc, face, backing, NULL, &outHandle);
    if (rc != ORBIS_FONT_OK) {
        FT_Done_Face(face);
        backing_release(backing);
        outHandle = 0;
    }
    pthread_mutex_unlock(&g_font_mutex);

    *(uint64_t *)(ctx->mem_base + pFontHandleGuest) = outHandle;
    fprintf(stderr, "[ps4-font] sceFontOpenFontFile: path=%s sub=%u -> handle=0x%llx (rc=0x%x)\n",
            guestPath, subfont_index, (unsigned long long)outHandle, (unsigned int)rc);
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

// 51. sceFontOpenFontMemory
void shim_sceFontOpenFontMemory(GuestContext *ctx) {
    uint64_t libGuest = ctx->rdi;
    uint64_t fontAddr = ctx->rsi;
    uint32_t fontSize = (uint32_t)ctx->rdx;
    uint64_t openParamsGuest = ctx->rcx;
    uint64_t pFontHandleGuest = ctx->r8;

    if (!libGuest || !RECOMP_MEM_OK(ctx)) {
        if (pFontHandleGuest && RECOMP_MEM_OK(ctx)) *(uint64_t *)(ctx->mem_base + pFontHandleGuest) = 0;
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_LIBRARY;
        SHIM_RETURN();
    }
    if (!fontAddr || fontSize == 0 || !pFontHandleGuest || fontAddr >= ctx->mem_size ||
        fontSize > ctx->mem_size - fontAddr) {
        if (pFontHandleGuest) *(uint64_t *)(ctx->mem_base + pFontHandleGuest) = 0;
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_PARAMETER;
        SHIM_RETURN();
    }

    uint32_t subfont_index = 0;
    if (openParamsGuest) {
        const OrbisFontOpenParams *params = (const OrbisFontOpenParams *)(ctx->mem_base + openParamsGuest);
        subfont_index = params->subfont_index;
    }

    uint8_t *memCopy = (uint8_t *)malloc(fontSize);
    if (!memCopy) {
        *(uint64_t *)(ctx->mem_base + pFontHandleGuest) = 0;
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_ALLOCATION_FAILED;
        SHIM_RETURN();
    }
    memcpy(memCopy, ctx->mem_base + fontAddr, fontSize);

    uint64_t prealloc = *(uint64_t *)(ctx->mem_base + pFontHandleGuest);

    pthread_mutex_lock(&g_font_mutex);
    if (!g_ft_library) {
        FT_Init_FreeType(&g_ft_library);
    }
    FT_Face face = NULL;
    FontBacking *backing = NULL;
    if (FT_New_Memory_Face(g_ft_library, memCopy, (FT_Long)fontSize, (FT_Long)subfont_index, &face) == 0) {
        backing = (FontBacking *)calloc(1, sizeof(FontBacking));
        if (backing) {
            backing->data = memCopy;
            backing->size = fontSize;
            backing->subfont_index = subfont_index;
            backing->refs = 1;
        } else {
            FT_Done_Face(face);
            face = NULL;
            free(memCopy);
        }
    } else {
        free(memCopy);
    }

    if (!face) {
        pthread_mutex_unlock(&g_font_mutex);
        *(uint64_t *)(ctx->mem_base + pFontHandleGuest) = 0;
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_FONT_OPEN_FAILED;
        SHIM_RETURN();
    }

    uint64_t outHandle = 0;
    int32_t rc = font_register_opened_handle(ctx, libGuest, prealloc, face, backing, NULL, &outHandle);
    if (rc != ORBIS_FONT_OK) {
        FT_Done_Face(face);
        backing_release(backing);
        outHandle = 0;
    }
    pthread_mutex_unlock(&g_font_mutex);

    *(uint64_t *)(ctx->mem_base + pFontHandleGuest) = outHandle;
    fprintf(stderr, "[ps4-font] sceFontOpenFontMemory: addr=0x%llx size=%u sub=%u -> handle=0x%llx\n",
            (unsigned long long)fontAddr, fontSize, subfont_index, (unsigned long long)outHandle);
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

// 52. sceFontOpenFontInstance
void shim_sceFontOpenFontInstance(GuestContext *ctx) {
    uint64_t fontGuest = ctx->rdi;
    uint64_t setupFontGuest = ctx->rsi;
    uint64_t pFontHandleGuest = ctx->rdx;

    if (!fontGuest || !RECOMP_MEM_OK(ctx)) {
        if (pFontHandleGuest && RECOMP_MEM_OK(ctx)) *(uint64_t *)(ctx->mem_base + pFontHandleGuest) = 0;
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_FONT_HANDLE;
        SHIM_RETURN();
    }
    if (!setupFontGuest && !pFontHandleGuest) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_PARAMETER;
        SHIM_RETURN();
    }

    pthread_mutex_lock(&g_font_mutex);
    InternalFontHandle *src = get_font(fontGuest);
    if (!src) {
        pthread_mutex_unlock(&g_font_mutex);
        if (pFontHandleGuest) *(uint64_t *)(ctx->mem_base + pFontHandleGuest) = 0;
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_FONT_HANDLE;
        SHIM_RETURN();
    }

    InternalFontHandle copy_cfg = *src;
    FT_Face new_face = NULL;
    FontBacking *backing = NULL;
    if (src->backing && g_ft_library) {
        if (FT_New_Memory_Face(g_ft_library, src->backing->data, (FT_Long)src->backing->size,
                               (FT_Long)src->backing->subfont_index, &new_face) == 0) {
            backing = backing_retain(src->backing);
        }
    }
    if (!new_face && src->ft_face) {
        FT_Reference_Face(src->ft_face);
        new_face = src->ft_face;
    }

    uint64_t outHandle = 0;
    int32_t rc = font_register_opened_handle(ctx, copy_cfg.library_guest, setupFontGuest,
                                             new_face, backing, &copy_cfg, &outHandle);
    if (rc != ORBIS_FONT_OK) {
        if (new_face) FT_Done_Face(new_face);
        backing_release(backing);
        outHandle = 0;
    }
    pthread_mutex_unlock(&g_font_mutex);

    if (pFontHandleGuest) {
        *(uint64_t *)(ctx->mem_base + pFontHandleGuest) = outHandle;
    }
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

// 53. sceFontSetResolutionDpi
void shim_sceFontSetResolutionDpi(GuestContext *ctx) {
    uint64_t fontGuest = ctx->rdi;
    uint32_t h_dpi = (uint32_t)ctx->rsi;
    uint32_t v_dpi = (uint32_t)ctx->rdx;

    pthread_mutex_lock(&g_font_mutex);
    InternalFontHandle *h = get_font(fontGuest);
    if (!h) {
        pthread_mutex_unlock(&g_font_mutex);
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_FONT_HANDLE;
        SHIM_RETURN();
    }
    h->dpi_x = h_dpi ? h_dpi : 72;
    h->dpi_y = v_dpi ? v_dpi : 72;
    font_apply_ft_size(h, 0);
    pthread_mutex_unlock(&g_font_mutex);

    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 54. sceFontSetScalePoint
void shim_sceFontSetScalePoint(GuestContext *ctx) {
    uint64_t fontGuest = ctx->rdi;
    float w = ctx->xmm[0].f32[0];
    float h_pt = ctx->xmm[1].f32[0];

    pthread_mutex_lock(&g_font_mutex);
    InternalFontHandle *h = get_font(fontGuest);
    if (!h) {
        pthread_mutex_unlock(&g_font_mutex);
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_FONT_HANDLE;
        SHIM_RETURN();
    }
    h->scale_unit = 1;
    h->scale_w = w > 0.0f ? w : 16.0f;
    h->scale_h = h_pt > 0.0f ? h_pt : 16.0f;
    font_apply_ft_size(h, 0);
    pthread_mutex_unlock(&g_font_mutex);

    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 55. sceFontGetScalePixel
void shim_sceFontGetScalePixel(GuestContext *ctx) {
    uint64_t fontGuest = ctx->rdi;
    uint64_t outWGuest = ctx->rsi;
    uint64_t outHGuest = ctx->rdx;

    if (!fontGuest || !RECOMP_MEM_OK(ctx)) {
        if (outWGuest && RECOMP_MEM_OK(ctx)) *(float *)(ctx->mem_base + outWGuest) = 0.0f;
        if (outHGuest && RECOMP_MEM_OK(ctx)) *(float *)(ctx->mem_base + outHGuest) = 0.0f;
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_FONT_HANDLE;
        SHIM_RETURN();
    }
    if (!outWGuest && !outHGuest) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_PARAMETER;
        SHIM_RETURN();
    }

    pthread_mutex_lock(&g_font_mutex);
    InternalFontHandle *h = get_font(fontGuest);
    if (!h) {
        pthread_mutex_unlock(&g_font_mutex);
        if (outWGuest) *(float *)(ctx->mem_base + outWGuest) = 0.0f;
        if (outHGuest) *(float *)(ctx->mem_base + outHGuest) = 0.0f;
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_FONT_HANDLE;
        SHIM_RETURN();
    }
    float px_w = 0.0f, px_h = 0.0f;
    font_compute_pixel_scale(h, 0, &px_w, &px_h);
    pthread_mutex_unlock(&g_font_mutex);

    if (outWGuest) *(float *)(ctx->mem_base + outWGuest) = px_w;
    if (outHGuest) *(float *)(ctx->mem_base + outHGuest) = px_h;
    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 56. sceFontSetEffectWeight
void shim_sceFontSetEffectWeight(GuestContext *ctx) {
    uint64_t fontGuest = ctx->rdi;
    float weightX = ctx->xmm[0].f32[0];
    float weightY = ctx->xmm[1].f32[0];
    uint32_t mode = (uint32_t)ctx->rsi;

    if (mode != 0) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_PARAMETER;
        SHIM_RETURN();
    }
    pthread_mutex_lock(&g_font_mutex);
    InternalFontHandle *h = get_font(fontGuest);
    if (!h) {
        pthread_mutex_unlock(&g_font_mutex);
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_FONT_HANDLE;
        SHIM_RETURN();
    }
    h->weight_x = weightX;
    h->weight_y = weightY;
    pthread_mutex_unlock(&g_font_mutex);

    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 57. sceFontSetEffectSlant
void shim_sceFontSetEffectSlant(GuestContext *ctx) {
    uint64_t fontGuest = ctx->rdi;
    float slantRatio = ctx->xmm[0].f32[0];

    pthread_mutex_lock(&g_font_mutex);
    InternalFontHandle *h = get_font(fontGuest);
    if (!h) {
        pthread_mutex_unlock(&g_font_mutex);
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_FONT_HANDLE;
        SHIM_RETURN();
    }
    h->slant = slantRatio;
    pthread_mutex_unlock(&g_font_mutex);

    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 58. sceFontSetupRenderEffectWeight
void shim_sceFontSetupRenderEffectWeight(GuestContext *ctx) {
    uint64_t fontGuest = ctx->rdi;
    float weightX = ctx->xmm[0].f32[0];
    float weightY = ctx->xmm[1].f32[0];
    uint32_t mode = (uint32_t)ctx->rsi;

    pthread_mutex_lock(&g_font_mutex);
    InternalFontHandle *h = get_font(fontGuest);
    if (!h) {
        pthread_mutex_unlock(&g_font_mutex);
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_FONT_HANDLE;
        SHIM_RETURN();
    }
    if (!h->renderer_guest) {
        pthread_mutex_unlock(&g_font_mutex);
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_NOT_BOUND_RENDERER;
        SHIM_RETURN();
    }
    if (mode != 0) {
        pthread_mutex_unlock(&g_font_mutex);
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_PARAMETER;
        SHIM_RETURN();
    }
    h->render_weight_x = weightX;
    h->render_weight_y = weightY;
    pthread_mutex_unlock(&g_font_mutex);

    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 59. sceFontSetupRenderEffectSlant
void shim_sceFontSetupRenderEffectSlant(GuestContext *ctx) {
    uint64_t fontGuest = ctx->rdi;
    float slantRatio = ctx->xmm[0].f32[0];

    pthread_mutex_lock(&g_font_mutex);
    InternalFontHandle *h = get_font(fontGuest);
    if (!h) {
        pthread_mutex_unlock(&g_font_mutex);
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_FONT_HANDLE;
        SHIM_RETURN();
    }
    if (!h->renderer_guest) {
        pthread_mutex_unlock(&g_font_mutex);
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_NOT_BOUND_RENDERER;
        SHIM_RETURN();
    }
    h->render_slant = slantRatio;
    pthread_mutex_unlock(&g_font_mutex);

    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 60. sceFontGetHorizontalLayout
void shim_sceFontGetHorizontalLayout(GuestContext *ctx) {
    uint64_t fontGuest = ctx->rdi;
    uint64_t layoutGuest = ctx->rsi;

    if (!fontGuest || !RECOMP_MEM_OK(ctx)) {
        if (layoutGuest && RECOMP_MEM_OK(ctx)) memset(ctx->mem_base + layoutGuest, 0, sizeof(OrbisFontHorizontalLayout));
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_FONT_HANDLE;
        SHIM_RETURN();
    }
    if (!layoutGuest) {
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_PARAMETER;
        SHIM_RETURN();
    }

    pthread_mutex_lock(&g_font_mutex);
    InternalFontHandle *h = get_font(fontGuest);
    if (!h) {
        pthread_mutex_unlock(&g_font_mutex);
        memset(ctx->mem_base + layoutGuest, 0, sizeof(OrbisFontHorizontalLayout));
        ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_FONT_HANDLE;
        SHIM_RETURN();
    }

    float px_w = 16.0f, px_h = 16.0f;
    font_compute_pixel_scale(h, 0, &px_w, &px_h);
    float asc = px_h * 0.8f;
    float desc = -px_h * 0.2f;
    float line_adv = px_h;

    if (h->ft_face) {
        font_apply_ft_size(h, 0);
        if (h->ft_face->size) {
            float ft_asc = (float)h->ft_face->size->metrics.ascender / 64.0f;
            float ft_desc = (float)h->ft_face->size->metrics.descender / 64.0f;
            float ft_hgt = (float)h->ft_face->size->metrics.height / 64.0f;
            if (ft_asc > 0.0f) asc = ft_asc;
            if (ft_desc <= 0.0f) desc = ft_desc;
            line_adv = (ft_hgt > 0.0f) ? ft_hgt : (asc - desc);
        }
    }
    float bb_base = 0.0f, bb_adv = 0.0f, bb_deco = 0.0f;
    const int bbox_ok = font_bbox_layout(h, h->weight_y, &bb_base, &bb_adv, &bb_deco);
    pthread_mutex_unlock(&g_font_mutex);

    OrbisFontHorizontalLayout *layout = (OrbisFontHorizontalLayout *)(ctx->mem_base + layoutGuest);
    layout->baselineOffset = asc;
    layout->lineAdvance = line_adv;
    layout->decorationExtent = (desc < 0.0f) ? -desc : 0.0f;
    if (bbox_ok) {
        layout->baselineOffset = bb_base;
        layout->lineAdvance = bb_adv;
        layout->decorationExtent = bb_deco;
    }

    ctx->rax = ORBIS_FONT_OK;
    SHIM_RETURN();
}

// 61. sceFontRenderSurfaceSetScissor
void shim_sceFontRenderSurfaceSetScissor(GuestContext *ctx) {
    uint64_t surfGuest = ctx->rdi;
    int32_t x0 = (int32_t)ctx->rsi;
    int32_t y0 = (int32_t)ctx->rdx;
    int32_t w = (int32_t)ctx->rcx;
    int32_t h = (int32_t)ctx->r8;

    if (!surfGuest || !RECOMP_MEM_OK(ctx)) {
        SHIM_RETURN();
    }

    OrbisFontRenderSurface *surf = (OrbisFontRenderSurface *)(ctx->mem_base + surfGuest);
    const uint32_t surface_w = (uint32_t)surf->width;
    if (surface_w != 0) {
        uint32_t x1, x0_out;
        uint32_t w_u = (uint32_t)w;
        if (x0 < 0) {
            x1 = w_u + (uint32_t)x0;
            if (surface_w < x1) x1 = surface_w;
            if (w_u <= (uint32_t)(-x0)) x1 = 0;
            x0_out = 0;
        } else {
            x1 = surface_w;
            x0_out = surface_w;
            if ((uint32_t)x0 <= surface_w) {
                if (surface_w < w_u) w_u = surface_w;
                x1 = w_u + (uint32_t)x0;
                x0_out = (uint32_t)x0;
                if (surface_w < x1) x1 = surface_w;
            }
        }
        surf->sc_x0 = x0_out;
        surf->sc_x1 = x1;
    }

    const uint32_t surface_h = (uint32_t)surf->height;
    if (surface_h == 0) {
        SHIM_RETURN();
    }

    uint32_t y0_out;
    uint32_t y1 = surface_h;
    uint32_t h_u = (uint32_t)h;
    if (y0 < 0) {
        y0_out = 0;
        if (h_u <= (uint32_t)(-y0)) {
            surf->sc_y0 = 0;
            surf->sc_y1 = 0;
            SHIM_RETURN();
        }
    } else {
        y0_out = surface_h;
        if (surface_h < (uint32_t)y0) {
            surf->sc_y0 = y0_out;
            surf->sc_y1 = y1;
            SHIM_RETURN();
        }
        y0_out = (uint32_t)y0;
        if (surface_h < h_u) h_u = surface_h;
    }

    const uint32_t y1_candidate = h_u + (uint32_t)y0;
    if (y1_candidate <= surface_h) {
        y1 = y1_candidate;
    }
    surf->sc_y0 = y0_out;
    surf->sc_y1 = y1;
    SHIM_RETURN();
}

// ---------------------------------------------------------------------------------------------
// Glyph API used by Astro Bot (PPSA21564): sceFontGetRenderCharGlyphMetrics, sceFontGenerateCharGlyph,
// sceFontGlyphDefineAttribute, sceFontDeleteGlyph, sceFontGetVerticalLayout. These were stubs returning an
// error (libSceAstroStubs). The game's text layout (guest 0x12d9d90) measures every character with
// sceFontGetRenderCharGlyphMetrics, retries with '_' on an error, and DROPS the character when both fail,
// so with the stub every string laid out empty and no UI text was drawn at all (skiptext).
// Layouts and argument checks follow AnyPS5 core/libs/prx/libSceFont (Render.cpp/Layout.cpp).
#define FONT_ERROR_INVALID_GLYPH     ((int32_t)0x80460006)
#define FONT_ERROR_NO_SUPPORT_GLYPH  ((int32_t)0x80460042)
#define FONT_GLYPH_MAGIC             0x0F03
#define FONT_GENERATE_DETAIL_ID      0x0FD3
#define FONT_GLYPH_SLOT              0x40
#define FONT_GLYPH_SLOTS             1024

typedef struct GuestFontGlyph {      // FontGlyphOpaque (guest memory, the game holds a pointer to it)
    uint16_t magic;
    uint16_t flags;
    uint8_t glyph_form;
    uint8_t metrics_form;
    uint16_t em_size;
    uint16_t baseline;
    uint16_t height_px;
    uint16_t origin_x;
    uint16_t origin_y;
    float scale_x;
    float base_scale;
    uint64_t memory;
    uint32_t codepoint;              // host bookkeeping (inside the 0x40 slot, past the public part)
    uint32_t pad;
    uint64_t owner_font;
} GuestFontGlyph;
_Static_assert(sizeof(GuestFontGlyph) <= FONT_GLYPH_SLOT, "glyph slot");

static uint64_t g_glyph_pool_guest;
static uint8_t g_glyph_used[FONT_GLYPH_SLOTS];
static uint32_t g_glyph_next;

static uint16_t font_clamp_u16(float v) {
    if (!(v > 0.0f)) return 0;
    if (v > 65535.0f) return 65535;
    return (uint16_t)(v + 0.5f);
}

// Metrics of one character at the font's render scale (use_render=1) or its plain scale (0).
// Caller holds g_font_mutex. Returns ORBIS_FONT_OK or an error; *m is zeroed on error.
static int32_t font_char_metrics_locked(InternalFontHandle *h, uint32_t code, int use_render,
                                        OrbisFontGlyphMetrics *m, float *used_w, float *used_h) {
    memset(m, 0, sizeof(*m));
    float pw = 16.0f, ph = 16.0f;
    font_compute_pixel_scale(h, use_render, &pw, &ph);
    if (used_w) *used_w = pw;
    if (used_h) *used_h = ph;
    if (!h->ft_face) {
        // no face: same fallback box the render path reports
        m->width = pw; m->height = ph;
        m->Horizontal.bearingY = ph; m->Horizontal.advance = pw;
        m->Vertical.bearingX = -pw * 0.5f; m->Vertical.advance = ph;
        return ORBIS_FONT_OK;
    }
    font_apply_ft_size(h, use_render);
    FT_UInt gi = FT_Get_Char_Index(h->ft_face, code);
    if (gi == 0) return FONT_ERROR_NO_SUPPORT_GLYPH;
    FT_Set_Transform(h->ft_face, NULL, NULL);
    // FT_LOAD_DEFAULT like font_render_glyph_common, so measured and drawn advances agree
    if (FT_Load_Glyph(h->ft_face, gi, FT_LOAD_DEFAULT) != 0 || !h->ft_face->glyph) return FONT_ERROR_NO_SUPPORT_GLYPH;
    const FT_Glyph_Metrics *gm = &h->ft_face->glyph->metrics;
    m->width = (float)gm->width / 64.0f;
    m->height = (float)gm->height / 64.0f;
    m->Horizontal.bearingX = (float)gm->horiBearingX / 64.0f;
    m->Horizontal.bearingY = (float)gm->horiBearingY / 64.0f;
    m->Horizontal.advance = (float)gm->horiAdvance / 64.0f;
    if (FT_HAS_VERTICAL(h->ft_face)) {
        m->Vertical.bearingX = (float)gm->vertBearingX / 64.0f;
        m->Vertical.bearingY = (float)gm->vertBearingY / 64.0f;
        m->Vertical.advance = (float)gm->vertAdvance / 64.0f;
    } else {
        m->Vertical.bearingX = (float)(gm->horiBearingX - gm->horiAdvance / 2) / 64.0f;
        m->Vertical.bearingY = (float)(gm->vertAdvance - gm->height) / 2.0f / 64.0f;
        m->Vertical.advance = (float)gm->vertAdvance / 64.0f;
    }
    return ORBIS_FONT_OK;
}

// int sceFontGetRenderCharGlyphMetrics(SceFontHandle font, uint32_t code, SceFontGlyphMetrics *metrics)
void shim_sceFontGetRenderCharGlyphMetrics(GuestContext *ctx) {
    uint64_t fontGuest = ctx->rdi;
    uint32_t code = (uint32_t)ctx->rsi;
    uint64_t metricsGuest = ctx->rdx;
    if (!RECOMP_MEM_OK(ctx)) { ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_FONT_HANDLE; SHIM_RETURN(); }
    OrbisFontGlyphMetrics *out = metricsGuest ? (OrbisFontGlyphMetrics *)(ctx->mem_base + metricsGuest) : NULL;
    pthread_mutex_lock(&g_font_mutex);
    InternalFontHandle *h = get_font(fontGuest);
    int32_t rc;
    if (!h) rc = ORBIS_FONT_ERROR_INVALID_FONT_HANDLE;
    else if (code == 0) rc = ORBIS_FONT_ERROR_NO_SUPPORT_CODE;
    else if (!out) rc = ORBIS_FONT_ERROR_INVALID_PARAMETER;
    else {
        OrbisFontGlyphMetrics m;
        rc = font_char_metrics_locked(h, code, 1, &m, NULL, NULL);
        *out = m;
    }
    pthread_mutex_unlock(&g_font_mutex);
    if (rc != ORBIS_FONT_OK && out) memset(out, 0, sizeof(*out));
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

// int sceFontGetVerticalLayout(SceFontHandle font, SceFontVerticalLayout *layout)  {baselineOffsetX, columnAdvance, decorationSpan}
void shim_sceFontGetVerticalLayout(GuestContext *ctx) {
    uint64_t fontGuest = ctx->rdi;
    uint64_t layoutGuest = ctx->rsi;
    if (!RECOMP_MEM_OK(ctx)) { ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_FONT_HANDLE; SHIM_RETURN(); }
    float *out = layoutGuest ? (float *)(ctx->mem_base + layoutGuest) : NULL;
    pthread_mutex_lock(&g_font_mutex);
    InternalFontHandle *h = get_font(fontGuest);
    int32_t rc = ORBIS_FONT_OK;
    float col = 16.0f, base_x = -8.0f;
    if (!h) rc = ORBIS_FONT_ERROR_INVALID_FONT_HANDLE;
    else if (!out) rc = ORBIS_FONT_ERROR_INVALID_PARAMETER;
    else {
        float pw = 16.0f, ph = 16.0f;
        font_compute_pixel_scale(h, 0, &pw, &ph);
        col = pw;
        if (h->ft_face) {
            font_apply_ft_size(h, 0);
            if (h->ft_face->size && h->ft_face->size->metrics.max_advance > 0)
                col = (float)h->ft_face->size->metrics.max_advance / 64.0f;
        }
        base_x = -col * 0.5f;
    }
    pthread_mutex_unlock(&g_font_mutex);
    if (out) {
        if (rc == ORBIS_FONT_OK) { out[0] = base_x; out[1] = col; out[2] = 0.0f; }
        else { out[0] = out[1] = out[2] = 0.0f; }
    }
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

static GuestFontGlyph *font_glyph_slot(GuestContext *ctx, uint64_t g, int *index) {
    if (!g || !g_glyph_pool_guest || g < g_glyph_pool_guest) return NULL;
    uint64_t off = g - g_glyph_pool_guest;
    if (off % FONT_GLYPH_SLOT || off / FONT_GLYPH_SLOT >= FONT_GLYPH_SLOTS) return NULL;
    int i = (int)(off / FONT_GLYPH_SLOT);
    if (!g_glyph_used[i]) return NULL;
    GuestFontGlyph *gl = (GuestFontGlyph *)(ctx->mem_base + g);
    if (gl->magic != FONT_GLYPH_MAGIC) return NULL;
    if (index) *index = i;
    return gl;
}

// int sceFontGenerateCharGlyph(SceFontHandle font, uint32_t code, const SceFontGenerateGlyphDetail *detail, SceFontGlyph *pGlyph)
void shim_sceFontGenerateCharGlyph(GuestContext *ctx) {
    uint64_t fontGuest = ctx->rdi;
    uint32_t code = (uint32_t)ctx->rsi;
    uint64_t detailGuest = ctx->rdx;
    uint64_t outGuest = ctx->rcx;
    if (!RECOMP_MEM_OK(ctx) || !outGuest) { ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_PARAMETER; SHIM_RETURN(); }
    uint64_t *out = (uint64_t *)(ctx->mem_base + outGuest);
    *out = 0;
    uint16_t form_options = 0; uint8_t glyph_form = 0, metrics_form = 0; uint64_t mem = 0;
    if (detailGuest) {
        const uint8_t *d = ctx->mem_base + detailGuest;
        uint16_t id; memcpy(&id, d, 2);
        memcpy(&form_options, d + 4, 2);
        glyph_form = d[6]; metrics_form = d[7];
        memcpy(&mem, d + 8, 8);
        if (id != FONT_GENERATE_DETAIL_ID || (form_options & ~0x11u) != 0 ||
            (glyph_form == 0 && metrics_form != 0) || (glyph_form != 0 && metrics_form == 0) ||
            glyph_form > 1 || metrics_form > 4) {
            ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_PARAMETER;
            SHIM_RETURN();
        }
    }
    if (!fontGuest) { ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_FONT_HANDLE; SHIM_RETURN(); }
    if (code == 0) { ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_NO_SUPPORT_CODE; SHIM_RETURN(); }

    pthread_mutex_lock(&g_font_mutex);
    int32_t rc = ORBIS_FONT_OK;
    InternalFontHandle *h = get_font(fontGuest);
    OrbisFontGlyphMetrics m;
    float sw = 16.0f, sh = 16.0f;
    if (!h) rc = ORBIS_FONT_ERROR_INVALID_FONT_HANDLE;
    else rc = font_char_metrics_locked(h, code, 0, &m, &sw, &sh);
    if (rc == ORBIS_FONT_OK && !g_glyph_pool_guest) {
        g_glyph_pool_guest = recomp_vm_alloc_named(ctx, FONT_GLYPH_SLOT * FONT_GLYPH_SLOTS, PROT_READ | PROT_WRITE, 0, "font_glyphs");
        if (!g_glyph_pool_guest) rc = ORBIS_FONT_ERROR_ALLOCATION_FAILED;
    }
    int slot = -1;
    if (rc == ORBIS_FONT_OK) {
        for (uint32_t k = 0; k < FONT_GLYPH_SLOTS; k++) {
            uint32_t i = (g_glyph_next + k) % FONT_GLYPH_SLOTS;
            if (!g_glyph_used[i]) { slot = (int)i; g_glyph_next = i + 1; break; }
        }
        if (slot < 0) rc = ORBIS_FONT_ERROR_ALLOCATION_FAILED;
    }
    if (rc == ORBIS_FONT_OK) {
        g_glyph_used[slot] = 1;
        uint64_t g = g_glyph_pool_guest + (uint64_t)slot * FONT_GLYPH_SLOT;
        GuestFontGlyph *gl = (GuestFontGlyph *)(ctx->mem_base + g);
        memset(gl, 0, FONT_GLYPH_SLOT);
        gl->magic = FONT_GLYPH_MAGIC;
        gl->flags = form_options;
        gl->glyph_form = glyph_form;
        gl->metrics_form = metrics_form;
        gl->em_size = font_clamp_u16(sh);
        gl->baseline = font_clamp_u16(m.Horizontal.bearingY);
        gl->height_px = font_clamp_u16(m.height);
        gl->origin_y = gl->baseline;
        gl->scale_x = sw;
        gl->base_scale = sh;
        gl->memory = mem;
        gl->codepoint = code;
        gl->owner_font = fontGuest;
        *out = g;
    }
    pthread_mutex_unlock(&g_font_mutex);
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

// int sceFontDeleteGlyph(const SceFontMemory *memory, SceFontGlyph *pGlyph)
void shim_sceFontDeleteGlyph(GuestContext *ctx) {
    uint64_t pGuest = ctx->rsi;
    if (!RECOMP_MEM_OK(ctx) || !pGuest) { ctx->rax = (uint64_t)(int64_t)ORBIS_FONT_ERROR_INVALID_PARAMETER; SHIM_RETURN(); }
    uint64_t *p = (uint64_t *)(ctx->mem_base + pGuest);
    pthread_mutex_lock(&g_font_mutex);
    int i = -1;
    GuestFontGlyph *gl = font_glyph_slot(ctx, *p, &i);
    int32_t rc = FONT_ERROR_INVALID_GLYPH;
    if (gl) {
        gl->magic = 0;
        g_glyph_used[i] = 0;
        *p = 0;
        rc = ORBIS_FONT_OK;
    }
    pthread_mutex_unlock(&g_font_mutex);
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

// int sceFontGlyphDefineAttribute(SceFontGlyph glyph, int attribute, int *previousAttribute)  (0x10 / 0x11 = bit 1 of flags)
void shim_sceFontGlyphDefineAttribute(GuestContext *ctx) {
    uint64_t g = ctx->rdi;
    int32_t attr = (int32_t)ctx->rsi;
    uint64_t prevGuest = ctx->rdx;
    if (!RECOMP_MEM_OK(ctx)) { ctx->rax = (uint64_t)(int64_t)FONT_ERROR_INVALID_GLYPH; SHIM_RETURN(); }
    int32_t *prev = prevGuest ? (int32_t *)(ctx->mem_base + prevGuest) : NULL;
    pthread_mutex_lock(&g_font_mutex);
    GuestFontGlyph *gl = font_glyph_slot(ctx, g, NULL);
    int32_t rc = ORBIS_FONT_OK;
    if (!gl) { rc = FONT_ERROR_INVALID_GLYPH; if (prev) *prev = 0; }
    else if (attr != 0x10 && attr != 0x11) { rc = ORBIS_FONT_ERROR_INVALID_PARAMETER; if (prev) *prev = 0; }
    else {
        int32_t was = (gl->flags & 2) ? 0x11 : 0x10;
        gl->flags = (uint16_t)((gl->flags & ~2u) | ((uint32_t)(attr - 0x10) << 1));
        if (prev) *prev = was;
    }
    pthread_mutex_unlock(&g_font_mutex);
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}
