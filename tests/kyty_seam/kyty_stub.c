// SPDX-License-Identifier: GPL-2.0-or-later
//
// A stand-in for kyty_gpu.dylib: implements kytyGpuApi.h ABI 1.6 and records every call. It is the unit-test double
// of the seam (pkg/runtime/kyty_seam_darwin_test.go) and a minimal reference of what the library side must provide.
//
// Environment (read at KytyGpuGetApi):
//   KYTY_STUB_POINTERS=host     pointer_mode = KYTY_GPU_POINTERS_HOST (default: guest)
//   KYTY_STUB_MISSING=<nid>     nid_entry returns null for that NID
//   KYTY_STUB_ABI=<minor>       report ABI 1.<minor> (to test the host's version check)
//   KYTY_STUB_NO_NID_ENTRY=1    nid_entry is null: the host falls back to dlsym(nid) (only "t7PlZ9nt5Lc" is exported)

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kyty_stub.h"

static KytyStubState g;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static const char *g_missing;

__attribute__((visibility("default"))) KytyStubState *KytyStubGetState(void) { return &g; }

static KytyStubCall *record(uint32_t kind) {
  pthread_mutex_lock(&g_mu);
  KytyStubCall *c = &g.calls[g.ncalls < KYTY_STUB_MAX_CALLS ? g.ncalls++ : KYTY_STUB_MAX_CALLS - 1];
  pthread_mutex_unlock(&g_mu);
  memset(c, 0, sizeof *c);
  c->kind = kind;
  c->entry = -1;
  return c;
}

// ---- exports ------------------------------------------------------------------------------------------------------

#define MAX_ENTRIES 128
static char g_entry_nid[MAX_ENTRIES][16];
static int g_nentries;

static uint64_t generic(int entry, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
  KytyStubCall *c = record(STUB_EXPORT);
  c->entry = entry;
  snprintf(c->nid, sizeof c->nid, "%s", g_entry_nid[entry]);
  c->a[0] = a0; c->a[1] = a1; c->a[2] = a2; c->a[3] = a3; c->a[4] = a4; c->a[5] = a5; c->a[6] = a6; c->a[7] = a7;
  const uint64_t r = g.next_result ? g.next_result : 0x1000u + (uint64_t)entry;
  g.next_result = 0;
  return r;
}
// One generic entry per slot: an arm64 function that takes the first eight integer arguments as they arrive.
#define E(i) static uint64_t e##i(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) { return generic(i, a0, a1, a2, a3, a4, a5, a6, a7); }
#define ENTRIES E(0) E(1) E(2) E(3) E(4) E(5) E(6) E(7) E(8) E(9) E(10) E(11) E(12) E(13) E(14) E(15) E(16) E(17) E(18) E(19) E(20) E(21) E(22) E(23) E(24) E(25) E(26) E(27) E(28) E(29) E(30) E(31) E(32) E(33) E(34) E(35) E(36) E(37) E(38) E(39) E(40) E(41) E(42) E(43) E(44) E(45) E(46) E(47) E(48) E(49) E(50) E(51) E(52) E(53) E(54) E(55) E(56) E(57) E(58) E(59) E(60) E(61) E(62) E(63) E(64) E(65) E(66) E(67) E(68) E(69) E(70) E(71) E(72) E(73) E(74) E(75) E(76) E(77) E(78) E(79) E(80) E(81) E(82) E(83) E(84) E(85) E(86) E(87) E(88) E(89) E(90) E(91) E(92) E(93) E(94) E(95) E(96) E(97) E(98) E(99) E(100) E(101) E(102) E(103) E(104) E(105) E(106) E(107) E(108) E(109) E(110) E(111) E(112) E(113) E(114) E(115) E(116) E(117) E(118) E(119) E(120) E(121) E(122) E(123) E(124) E(125) E(126) E(127)
ENTRIES
#undef E
#define E(i) e##i,
static void *const g_generic[MAX_ENTRIES] = {ENTRIES};
#undef E

// Typed entries, with KytyPS5's own prototypes (src/libs/agc.cpp): they prove that arguments the guest passed on its
// stack, floats and pointer results cross the seam.
// uint32_t* AgcDcbDmaData(CommandBuffer* buf, uint8_t engine, uint8_t dst, uint8_t dst_cache_policy, uint64_t dst_address_or_offset, uint8_t src,
//                         uint8_t src_cache_policy, uint64_t src_address_or_offset_or_immediate, uint32_t num_bytes, uint8_t wait_for_previous,
//                         uint8_t write_confirm, uint8_t block_engine)
static uint32_t *AgcDcbDmaData(void *buf, uint8_t engine, uint8_t dst, uint8_t dst_cache_policy, uint64_t dst_address, uint8_t src, uint8_t src_cache_policy,
                               uint64_t src_address, uint32_t num_bytes, uint8_t wait_for_previous, uint8_t write_confirm, uint8_t block_engine) {
  KytyStubCall *c = record(STUB_EXPORT);
  snprintf(c->nid, sizeof c->nid, "WmAc2MEj6Io");
  c->a[0] = (uint64_t)(uintptr_t)buf; c->a[1] = engine; c->a[2] = dst; c->a[3] = dst_cache_policy; c->a[4] = dst_address; c->a[5] = src;
  c->a[6] = src_cache_policy; c->a[7] = src_address; c->a[8] = num_bytes; c->a[9] = wait_for_previous; c->a[10] = write_confirm; c->a[11] = block_engine;
  return buf ? (uint32_t *)((uintptr_t)buf + 0x40) : NULL; // the advanced cursor, in whatever address domain buf came in
}
// int VideoOutColorSettingsSetGamma(VideoOutColorSettings* settings, float gamma)
static int VideoOutColorSettingsSetGamma(void *settings, float gamma) {
  KytyStubCall *c = record(STUB_EXPORT);
  snprintf(c->nid, sizeof c->nid, "DYhhWbJSeRg");
  c->a[0] = (uint64_t)(uintptr_t)settings;
  c->f[0] = gamma;
  return -7;
}
// uint32_t AgcCbNopGetSize(uint32_t num_dwords): also exported by name for the dlsym fallback.
__attribute__((visibility("default"))) uint32_t AgcCbNopGetSize(uint32_t dw) __asm__("_t7PlZ9nt5Lc");
uint32_t AgcCbNopGetSize(uint32_t dw) {
  KytyStubCall *c = record(STUB_EXPORT);
  snprintf(c->nid, sizeof c->nid, "t7PlZ9nt5Lc");
  c->a[0] = dw;
  return dw * 4;
}

static void *nid_entry(const char *nid) {
  KytyStubCall *c = record(STUB_NID_ENTRY);
  snprintf(c->nid, sizeof c->nid, "%s", nid);
  if (g_missing && strcmp(g_missing, nid) == 0) return NULL;
  if (strcmp(nid, "WmAc2MEj6Io") == 0) return (void *)AgcDcbDmaData;
  if (strcmp(nid, "DYhhWbJSeRg") == 0) return (void *)VideoOutColorSettingsSetGamma;
  if (strcmp(nid, "t7PlZ9nt5Lc") == 0) return (void *)AgcCbNopGetSize;
  for (int i = 0; i < g_nentries; i++)
    if (strcmp(g_entry_nid[i], nid) == 0) return g_generic[i];
  if (g_nentries == MAX_ENTRIES) return NULL;
  snprintf(g_entry_nid[g_nentries], sizeof g_entry_nid[0], "%s", nid);
  return g_generic[g_nentries++];
}

// ---- API ----------------------------------------------------------------------------------------------------------

static int api_init(const KytyGpuHostCallbacks *host) {
  record(STUB_INIT);
  if (!host || (host->abi_version >> 16) != KYTY_GPU_ABI_VERSION_MAJOR) return KYTY_GPU_E_VERSION;
  if (host->struct_size < sizeof(KytyGpuHostCallbacks)) return KYTY_GPU_E_VERSION; // this library needs a 1.6 host
  if (!host->host_info || !host->protect_guest) return KYTY_GPU_E_HOST_CALLBACK;
  g.host = host;
  return KYTY_GPU_OK;
}
static void on_main(void *arg) { *(int *)arg = pthread_main_np(); }
static int api_start(void) {
  record(STUB_START);
  // What the real library does here: create the window and the renderer on the main thread.
  if (g.host && g.host->run_on_main_thread) g.host->run_on_main_thread(g.host->user, on_main, &g.in_main_thread, 1);
  return KYTY_GPU_OK;
}
static void api_shutdown(void) { record(STUB_SHUTDOWN); }
static int api_register(uint64_t base, uint64_t size, uint32_t flags, const void *alias) {
  KytyStubCall *c = record(STUB_REGISTER);
  c->a[0] = base; c->a[1] = size; c->a[2] = flags; c->a[3] = (uint64_t)(uintptr_t)alias;
  return KYTY_GPU_OK;
}
static int api_unregister(uint64_t base, uint64_t size) {
  KytyStubCall *c = record(STUB_UNREGISTER);
  c->a[0] = base; c->a[1] = size;
  return KYTY_GPU_OK;
}
static int api_invalidate(uint64_t base, uint64_t size) { (void)base; (void)size; return KYTY_GPU_OK; }
static int api_protect(uint64_t base, uint64_t size, uint32_t flags) {
  KytyStubCall *c = record(STUB_PROTECT);
  c->a[0] = base; c->a[1] = size; c->a[2] = flags;
  return KYTY_GPU_OK;
}
static int api_fault(uint64_t address, int is_write) {
  const uint64_t page = address & ~4095ull;
  KytyStubCall *c = record(STUB_FAULT);
  c->a[0] = page; c->a[1] = (uint64_t)is_write; c->a[2] = address;
  if (!is_write && g.serve_loads && g.host && g.host->struct_size > KYTY_GPU_HOST_CALLBACKS_SIZE_1_6 && g.host->fault_load) {
    uint64_t addr = 0;
    uint32_t size = 0;
    if (g.host->fault_load(g.host->user, &addr, &size) == 1 && size <= 16) {
      uint8_t bytes[16];
      for (uint32_t i = 0; i < size; i++) bytes[i] = (uint8_t)(0xA0 + i);
      if (g.host->fault_load_apply(g.host->user, bytes, size) == 1) {
        KytyStubCall *sv = record(STUB_SERVED);
        sv->a[0] = addr; sv->a[1] = size;
        return 1; // the page stays armed
      }
    }
  }
  if (g.fault_disarms && g.host) g.host->protect_guest(g.host->user, page, 4096, KYTY_GPU_WATCH_NONE);
  return 1;
}
static int api_submit_dcb(const KytyGpuPacket *p) { (void)p; return KYTY_GPU_E_NOT_IMPLEMENTED; }
static int api_submit_acb(uint32_t q, const KytyGpuPacket *p) { (void)q; (void)p; return KYTY_GPU_E_NOT_IMPLEMENTED; }
static uint32_t api_nid_count(void) { return 0; }
static const KytyGpuNidInfo *api_nid_info(uint32_t i) { (void)i; return NULL; }
static const KytyGpuNidInfo *api_find_nid(const char *n) { (void)n; return NULL; }

static KytyGpuApi g_api = {
    .struct_size = sizeof(KytyGpuApi),
    .abi_version = KYTY_GPU_ABI_VERSION,
    .build = "kyty-stub",
    .init = api_init,
    .start_graphics = api_start,
    .shutdown = api_shutdown,
    .register_memory = api_register,
    .unregister_memory = api_unregister,
    .invalidate_memory = api_invalidate,
    .submit_dcb = api_submit_dcb,
    .submit_acb = api_submit_acb,
    .nid_count = api_nid_count,
    .nid_info = api_nid_info,
    .find_nid = api_find_nid,
    .pointer_mode = KYTY_GPU_POINTERS_GUEST,
    .nid_entry = nid_entry,
    .handle_fault = api_fault,
    .protect_memory = api_protect,
};

KYTY_GPU_EXPORT const KytyGpuApi *KytyGpuGetApi(uint32_t abi_version) {
  if ((abi_version >> 16) != KYTY_GPU_ABI_VERSION_MAJOR) return NULL;
  g.fault_disarms = 1;
  const char *e = getenv("KYTY_STUB_POINTERS");
  if (e && strcmp(e, "host") == 0) g_api.pointer_mode = KYTY_GPU_POINTERS_HOST;
  g_missing = getenv("KYTY_STUB_MISSING");
  if ((e = getenv("KYTY_STUB_ABI"))) g_api.abi_version = (KYTY_GPU_ABI_VERSION_MAJOR << 16) | (uint32_t)atoi(e);
  if (getenv("KYTY_STUB_NO_NID_ENTRY")) g_api.nid_entry = NULL;
  return &g_api;
}
