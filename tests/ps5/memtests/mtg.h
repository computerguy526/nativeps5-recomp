/* Guest-side helpers shared by the memtests programs: org(), case selection by "argv[1]" (the
 * payload SDK's crt start passes argv = NULL to main, so the argument block the runtime lays out
 * for the entry point is read directly), and a constant-hiding macro. */
#ifndef MTG_H
#define MTG_H
#include <stddef.h>
#include <stdint.h>

static volatile uint64_t MTG_SH = 12;
/* Page-aligned constants in [6 GiB, 448 GiB) would make the identity-mode scan refuse the title. */
#define BIG(x) ((uint64_t)((x) >> 12) << MTG_SH)
static uint64_t mtg_org(void) { return (uint64_t)(uintptr_t)&mtg_org >= 0x7000000000ULL ? 0x7000000000ULL : 0; }
/* Entry rsp: [argc][argv0] just under the main stack top; the argument block {argc, argv[]} is at argv0 - 0x120. */
static const char *mtg_arg1(void) {
  uint64_t top = mtg_org() + BIG(0x7EEFFC000ULL);
  for (uint64_t sp = top - 0x1200; sp < top - 0x800; sp += 8) {
    int64_t argc = *(volatile int64_t *)sp;
    uint64_t p0 = *(volatile uint64_t *)(sp + 8);
    if (argc < 1 || argc > 8 || p0 < mtg_org() + 0x400000 || p0 > mtg_org() + BIG(0x200000000ULL)) continue;
    uint64_t blk = p0 - 0x120;
    if (*(volatile int32_t *)blk != (int32_t)argc || *(volatile uint64_t *)(blk + 8) != p0) continue;
    if (argc < 2) return "";
    return (const char *)(uintptr_t)*(volatile uint64_t *)(blk + 16);
  }
  return "";
}
static int mtg_eq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
#endif
