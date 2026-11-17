// Host-test stand-ins for the parts of the runtime the memory manager only calls out to.
#include <stdint.h>
#include "nps5_diag.h"
uint8_t g_nps5_level[NPS5_S_COUNT];
void nps5_log(int s, int l, const char *fmt, ...) { (void)s; (void)l; (void)fmt; }
void nps5_log_compat(int s, int l, const char *t, const char *fmt, ...) { (void)s; (void)l; (void)t; (void)fmt; }
uint64_t ps4_hitch_now_ns(void) { return 0; }
void ps4_hitch_add_ns(int kind, uint64_t ns) { (void)kind; (void)ns; }
int sceKernelIsNeoMode(void) { return 0; }
void ps4_gnm_notify_cpu_write(uint64_t a, uint64_t b) { (void)a; (void)b; }
