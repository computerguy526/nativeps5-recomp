// nps5 performance trace: frame-time statistics (always) and CPU attribution (NPS5_PERF=1). See nps5_perf.c and
// docs/m3/perf.md.
#ifndef NPS5_PERF_H
#define NPS5_PERF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 0 = frame statistics only, 1 = attribution (HLE timing, sampler, threads), 2 = also per-frame CSV.
extern int g_nps5_perf_level;

// A shim whose call marks the end of a frame (the flip submission). Up to 4.
void nps5_perf_register_flip_fn(void *shim_fn);

// Set by nps5_perf.c's constructor, called by nps5_diag.c (which also links alone in unit tests):
// the HLE entry hook and the exit-time report into the run bundle.
extern void (*g_nps5_perf_hle_enter)(void *shim_fn);
extern void (*g_nps5_perf_finish)(const char *bundle_dir);
// Called by the shim bridge (_recomp_call_shim) after every HLE call when set (NPS5_PERF=1).
extern void (*g_nps5_hle_exit_hook)(void *ctx, void *shim_fn);

#ifdef __cplusplus
}
#endif
#endif
