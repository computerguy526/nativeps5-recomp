#ifndef PS4_PAK_H
#define PS4_PAK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int pak_fd;
    uint64_t data_offset;
    uint64_t size;
    uint32_t compression;
} ps4_pak_entry_t;

// Initialize PAK provider by scanning app_root for .pak files
void ps4_pak_init(const char *app_root);

// The scan's decision for one directory entry (readdir d_type, name); see ps4_pak.c. Pure.
enum { PS4_PAK_SCAN_SKIP = 0, PS4_PAK_SCAN_DIR = 1, PS4_PAK_SCAN_STAT = 2 };
int ps4_pak_scan_class(unsigned char d_type, const char *name);
// Totals of all scans so far: directories opened, stat() calls, .pak files handed to the parser (tests, diagnostics).
void ps4_pak_scan_counters(uint64_t *dirs, uint64_t *stats, uint64_t *paks);

// Lookup a relative subpath (e.g. "engine/content/internationalization/icudt64l/lang/en.res")
// Returns true if found and fills out_entry.
/* NPS5_PAK_LOCALE_FALLBACK=1: serve <name>.res for a missing <name>_<loc>.res (off by default, as on console). */
bool ps4_pak_locale_fallback_enabled(void);
bool ps4_pak_lookup(const char *subpath, ps4_pak_entry_t *out_entry);

// Compute parent locale path for CLDR/ICU resource bundle (.res) inheritance (e.g. "en_US.res" -> "en.res")
bool ps4_pak_get_parent_locale_path(const char *path, char *out_path, size_t out_sz);

// Check if a directory exists in any mounted pak archive
bool ps4_pak_dir_exists(const char *subpath);

// Cleanup all loaded pak archives
void ps4_pak_destroy(void);

#ifdef __cplusplus
}
#endif

#endif // PS4_PAK_H
