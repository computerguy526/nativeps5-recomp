// SPDX-License-Identifier: GPL-2.0-or-later
//
// nps5_diag fault resolvers (docs/m3/gpu-followups.md): a SIGSEGV/SIGBUS that a registered resolver claims (the AGC
// driver's write watch unprotecting a page) is not a crash: nothing is reported, execution resumes, and a later real
// crash is still reported as the first one (g_crashed was not consumed). Built by pkg/runtime/mem_vm_darwin_test.go.
#include "nps5_diag.h"
#include "nps5_mem.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

Nps5Arena g_nps5_arena; // data the diag module reads (never consulted: no fault reaches the crash report here)
static char *g_page;
static volatile int g_resolved;
static bool resolve(uint64_t a) {
  if (a < (uint64_t)(uintptr_t)g_page || a >= (uint64_t)(uintptr_t)g_page + 16384) return false;
  g_resolved++;
  return mprotect(g_page, 16384, PROT_READ | PROT_WRITE) == 0; // what the watch does: unprotect and record
}

// nps5_diag.c consults the flat call model runtime (nps5_shadow.c), which this test does not link: nested, no shadow stacks.
#include <stdint.h>
uint64_t g_recomp_call_model = 0;
int recomp_shadow_handle_fault(void *uctx, uint64_t addr) { (void)uctx; (void)addr; return 0; }

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  nps5_diag_install_crash_handlers();
  if (!nps5_diag_add_fault_resolver(resolve)) { puts("FAIL add"); return 1; }
  g_page = mmap(NULL, 16384, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
  for (int i = 0; i < 3; i++) { // re-arm and write three times: each is resolved, none is a crash
    mprotect(g_page, 16384, PROT_READ);
    *(volatile char *)g_page = (char)i;
    if (g_page[0] != i) { puts("FAIL value"); return 1; }
  }
  if (g_resolved != 3) { printf("FAIL resolved=%d\n", g_resolved); return 1; }
  if (argc > 1) { *(volatile int *)8 = 1; puts("FAIL: wild write survived"); return 1; } // not claimed: must die
  puts("RESOLVER OK");
  return 0;
}
