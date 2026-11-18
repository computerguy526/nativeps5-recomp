// Host unit test of the m2-hlesys system libraries (SaveData, AudioOut2, Ampr/Apr, small kernel
// calls). Builds natively with tools/hlesys_unit.sh: a fake GuestContext over a malloc'ed arena,
// shims called directly. No guest code runs, no audio device is opened (NPS5_AUDIO_NULL), and
// everything is written under a temp directory.
#include "recomp_runtime.h"
#include "nps5_diag.h"
#include "ps4_equeue.h"
#include "ps4_vfs.h"
#include "ps4_savedata.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// ---- runtime stubs the modules link against
uint8_t g_nps5_level[NPS5_S_COUNT];
void nps5_log(int s, int l, const char *fmt, ...) { (void)s; (void)l; (void)fmt; }
void nps5_log_compat(int s, int l, const char *tag, const char *fmt, ...) { (void)s; (void)l; (void)tag; (void)fmt; }
// AudioOut2 links the haptics port (nps5_haptics.c); its HD sink calls the Kyty seam (nps5_kyty.c, not linked here):
// no pad audio device, so the haptics take the rumble path.
uint64_t nps5_kyty_queue_pad_audio(uint32_t freq, const float *lr, uint32_t frames) { (void)freq; (void)lr; (void)frames; return 0; }

#include <sys/stat.h>
void nps5_fill_guest_stat(void *g, const struct stat *st) { memset(g, 0, 0x78); *(int64_t *)((char *)g + 72) = st->st_size; }
uint64_t sceKernelGetProcessTimeCounter(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
void shim_scePthreadCondBroadcast(GuestContext *ctx) { ctx->rax = 0; ctx->rsp += 8; }
uint64_t recomp_vm_alloc(GuestContext *c, size_t n) { (void)c; static uint64_t b = 0x3000000; uint64_t r = b; b += (n + 63) & ~63ull; return r; }
void shim_scePthreadRwlockTryrdlock(GuestContext *ctx) { ctx->rax = 16; ctx->rsp += 8; }
void shim_scePthreadRwlockTrywrlock(GuestContext *ctx) { ctx->rax = 0; ctx->rsp += 8; }
#define ARENA (64ULL << 20)
static GuestContext *C;
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define H(a) ((uint8_t *)C->mem_base + (a))
#define CALL(fn) do { uint64_t rsp0 = C->rsp; fn(C); assert(C->rsp == rsp0 + 8); C->rsp = rsp0; } while (0)
#define RAX32 ((int32_t)C->rax)

static uint64_t bump = 0x100000;
static uint64_t galloc(size_t n) { uint64_t r = bump; bump += (n + 63) & ~63ULL; return r; }
static uint64_t gstr(const char *s) { uint64_t r = galloc(strlen(s) + 1); strcpy((char *)H(r), s); return r; }

#define SHIM(n) void shim_##n(GuestContext *)
SHIM(sceSaveDataInitialize3); SHIM(sceSaveDataTerminate); SHIM(sceSaveDataMount3); SHIM(sceSaveDataUmount2);
SHIM(sceSaveDataPrepare); SHIM(sceSaveDataCommit); SHIM(sceSaveDataCreateTransactionResource);
SHIM(sceSaveDataDeleteTransactionResource); SHIM(sceSaveDataSetParam); SHIM(sceSaveDataGetParam);
SHIM(sceSaveDataSaveIcon); SHIM(sceSaveDataLoadIcon); SHIM(sceSaveDataDirNameSearch); SHIM(sceSaveDataDelete);
SHIM(sceAudioOut2Initialize); SHIM(sceAudioOut2UserCreate); SHIM(sceAudioOut2UserDestroy);
SHIM(sceAudioOut2ContextResetParam); SHIM(sceAudioOut2ContextQueryMemory); SHIM(sceAudioOut2ContextCreate);
SHIM(sceAudioOut2ContextDestroy); SHIM(sceAudioOut2ContextPush); SHIM(sceAudioOut2ContextAdvance);
SHIM(sceAudioOut2PortCreate); SHIM(sceAudioOut2PortDestroy); SHIM(sceAudioOut2PortGetState);
SHIM(sceAudioOut2PortSetAttributes);
SHIM(sceAmprCommandBufferConstructor); SHIM(sceAmprCommandBufferDestructor); SHIM(sceAmprCommandBufferReset);
SHIM(sceAmprCommandBufferSetBuffer); SHIM(sceAmprCommandBufferClearBuffer); SHIM(sceAmprCommandBufferGetSize);
SHIM(sceAmprCommandBufferGetCurrentOffset); SHIM(sceAmprCommandBufferWriteKernelEventQueue_04_00);
SHIM(sceAmprAprCommandBufferConstructor); SHIM(sceAmprAprCommandBufferDestructor);
SHIM(sceAmprAprCommandBufferReadFile); SHIM(sceAmprMeasureCommandSizeReadFile);
SHIM(sceAmprMeasureCommandSizeWriteKernelEventQueue_04_00);
SHIM(sceKernelAprResolveFilepathsToIdsAndFileSizes); SHIM(sceKernelAprResolveFilepathsToIds);
SHIM(sceKernelAprSubmitCommandBufferAndGetResult); SHIM(sceKernelAprWaitCommandBuffer);
SHIM(sceKernelAprSubmitCommandBuffer); SHIM(sceKernelAprSubmitCommandBufferAndGetId);
SHIM(sceKernelAprGetFileSize); SHIM(sceKernelAddAmprEvent);
SHIM(sceKernelIsTrinityMode); SHIM(sceKernelSetPrtAperture); SHIM(pthread_attr_setsolosched_np);
SHIM(scePadSetVibrationMode);
SHIM(sceAudioPropagationSystemQueryMemory); SHIM(sceAudioPropagationSystemCreate); SHIM(sceAudioPropagationSourceCreate);
SHIM(sceAudioPropagationSourceRender); SHIM(sceAudioPropagationSourceGetAudioPathCount);
SHIM(_ZN3sce4Json5Array8iteratorppEv);
SHIM(_ZN3sce4Json5ArrayC1Ev);
SHIM(_ZN3sce4Json5Value10referValueERKNS0_6StringE);
SHIM(_ZN3sce4Json5Value3setEb);
SHIM(_ZN3sce4Json5Value9serializeERNS0_6StringE);
SHIM(_ZN3sce4Json5ValueC1ERKNS0_6ObjectE);
SHIM(_ZN3sce4Json5ValueC1Em);
SHIM(_ZN3sce4Json5ValueC1Ev);
SHIM(_ZN3sce4Json6Object8iteratorppEv);
SHIM(_ZN3sce4Json6ObjectC1Ev);
SHIM(_ZN3sce4Json6ObjectixERKNS0_6StringE);
SHIM(_ZN3sce4Json6Parser5parseERNS0_5ValueEPKcm);
SHIM(_ZN3sce4Json6StringC1EPKc);
SHIM(_ZN3sce4Json6StringC1Ev);
SHIM(_ZNK3sce4Json5Array3endEv);
SHIM(_ZNK3sce4Json5Array5beginEv);
SHIM(_ZNK3sce4Json5Array5emptyEv);
SHIM(_ZNK3sce4Json5Array8iteratordeEv);
SHIM(_ZNK3sce4Json5Array8iteratorneERKS2_);
SHIM(_ZNK3sce4Json5Value10getBooleanEv);
SHIM(_ZNK3sce4Json5Value10getIntegerEv);
SHIM(_ZNK3sce4Json5Value11getUIntegerEv);
SHIM(_ZNK3sce4Json5Value5countEv);
SHIM(_ZNK3sce4Json5Value7getRealEv);
SHIM(_ZNK3sce4Json5Value7getTypeEv);
SHIM(_ZNK3sce4Json5Value8getArrayEv);
SHIM(_ZNK3sce4Json5Value9getObjectEv);
SHIM(_ZNK3sce4Json5Value9getStringEv);
SHIM(_ZNK3sce4Json5ValueixEPKc);
SHIM(_ZNK3sce4Json5ValueixEm);
SHIM(_ZNK3sce4Json6Object3endEv);
SHIM(_ZNK3sce4Json6Object5beginEv);
SHIM(_ZNK3sce4Json6Object8iteratordeEv);
SHIM(_ZNK3sce4Json6Object8iteratorneERKS2_);
SHIM(_ZNK3sce4Json6String5c_strEv);
SHIM(_ZNK3sce4Json6String6lengthEv);
SHIM(_ZNK3sce4Json6StringeqEPKc);
SHIM(sceVoiceChatCreateRequest); SHIM(sceHttp2SendRequest); SHIM(scePngEncQueryMemorySize); SHIM(scePngEncCreate);
SHIM(sceShareCaptureScreenshot); SHIM(scePthreadRwlockTimedrdlock); SHIM(scePthreadRwlockTimedwrlock);
SHIM(sceSystemServiceGetNoticeScreenSkipFlag); SHIM(sceAudioOut2GetSpeakerInfo); SHIM(sceKernelNanosleep);
SHIM(sceAmprCommandBufferNop); SHIM(sceAmprCommandBufferNopWithData); SHIM(sceAmprCommandBufferConstructNop);
SHIM(sceAmprCommandBufferPushMarker); SHIM(sceAmprCommandBufferPopMarker); SHIM(sceAmprCommandBufferSetMarker);
SHIM(sceAmprCommandBufferConstructMarker); SHIM(sceAmprAprCommandBufferMapBegin); SHIM(sceAmprAprCommandBufferMapEnd);
SHIM(sceAmprAprCommandBufferReadFileGather); SHIM(sceAmprAprCommandBufferReadFileScatter);
SHIM(sceAmprAprCommandBufferReadFileGatherScatter); SHIM(sceAmprAprCommandBufferResetGatherScatterState);
SHIM(sceAmprCommandBufferWriteAddress_04_00); SHIM(sceAmprCommandBufferWriteCounter_04_00);
SHIM(sceAmprCommandBufferWaitOnCounter_04_00); SHIM(sceAmprCommandBufferWaitOnAddress_04_00);
SHIM(sceAmprCommandBufferWriteAddressFromCounter_04_00); SHIM(sceAmprCommandBufferWriteAddressFromCounterPair_04_00);
SHIM(sceAmprCommandBufferWriteAddressFromTimeCounter_04_00);
SHIM(sceAmprCommandBufferGetNumCommands); SHIM(sceAmprCommandBufferGetType); SHIM(sceAmprCommandBufferGetBufferBaseAddress);
SHIM(sceAmprMeasureCommandSizePushMarker); SHIM(sceAmprMeasureCommandSizeNop); SHIM(sceAmprMeasureCommandSizeMapBegin);
SHIM(sceAmprMeasureCommandSizeReadFileGather); SHIM(sceAmprMeasureCommandSizeWriteCounter_04_00);
SHIM(sceKernelAprResolveFilepathsWithPrefixToIds); SHIM(sceKernelAprResolveFilepathsWithPrefixToIdsAndFileSizes);
SHIM(sceKernelAprResolveFilepathsToIdsForEach); SHIM(sceKernelAprResolveFilepathsToIdsAndFileSizesForEach);
SHIM(sceKernelAprResolveFilepathsWithPrefixToIdsForEach); SHIM(sceKernelAprResolveFilepathsWithPrefixToIdsAndFileSizesForEach);
SHIM(sceKernelAprGetFileStat); SHIM(sceKernelClearVirtualRangeName); SHIM(sceKernelGetOpenPsId);

static void reset_regs(void) { C->rdi = C->rsi = C->rdx = C->rcx = C->r8 = C->r9 = 0; }

// ------------------------------------------------------------------------------------ SaveData

static int rd_file(const char *guest_path, void *buf, size_t n) {
  int fd = ps4_vfs_open(guest_path, O_RDONLY, 0);
  if (fd < 0) return -1;
  ssize_t r = ps4_vfs_read(fd, buf, n);
  ps4_vfs_close(fd);
  return (int)r;
}

static int mount3(const char *dir, uint32_t mode, uint64_t blocks, uint64_t result) {
  uint64_t dn = gstr(dir), m = galloc(0x50);
  memset(H(m), 0, 0x50);
  *(int32_t *)H(m) = 0x10000000;       // userId
  *(uint64_t *)H(m + 8) = dn;          // dirName
  *(uint64_t *)H(m + 16) = blocks;     // blocks
  *(uint32_t *)H(m + 32) = mode;       // mountMode
  reset_regs(); C->rdi = m; C->rsi = result;
  CALL(shim_sceSaveDataMount3);
  return RAX32;
}

// /app0 is the read-only package image: reads work, every open that could change it fails with
// EROFS and leaves the host directory untouched.
static void test_app0_readonly(const char *tmp) {
  char p[700]; snprintf(p, sizeof p, "%s/present.txt", tmp);
  FILE *f = fopen(p, "w"); CHECK(f != NULL); if (f) { fputs("x", f); fclose(f); }
  CHECK(ps4_vfs_is_readonly("/app0") && ps4_vfs_is_readonly("/app0/a/b") && !ps4_vfs_is_readonly("/app0x/a") &&
        !ps4_vfs_is_readonly("/download0/a"));
  int fd = ps4_vfs_open("/app0/present.txt", O_RDONLY, 0);
  CHECK(fd >= 0); if (fd >= 0) ps4_vfs_close(fd);
  errno = 0; CHECK(ps4_vfs_open("/app0/present.txt", O_WRONLY, 0) == -1 && errno == EROFS);
  errno = 0; CHECK(ps4_vfs_open("/app0/present.txt", O_RDONLY | O_TRUNC, 0) == -1 && errno == EROFS);
  errno = 0; CHECK(ps4_vfs_open("/app0/new.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644) == -1 && errno == EROFS);
  snprintf(p, sizeof p, "%s/new.txt", tmp);
  CHECK(access(p, F_OK) != 0);
  snprintf(p, sizeof p, "%s/present.txt", tmp);
  struct stat st; CHECK(stat(p, &st) == 0 && st.st_size == 1);
}

static int host_exists(const char *fmt, const char *a, const char *b) {
  char p[1400]; snprintf(p, sizeof p, fmt, a, b);
  return access(p, F_OK) == 0;
}

// Per-title writable roots (ps4_vfs.h): /data, /temp0, /download0 land below one host directory, never in
// the application image; /temp0 is emptied when the VFS starts; the title id comes from param.json.
static void test_userdata_roots(const char *tmp) {
  char app[700], ud[700], p[1400];
  snprintf(app, sizeof app, "%s/image", tmp); snprintf(ud, sizeof ud, "%s/ud", tmp);
  snprintf(p, sizeof p, "%s/sce_sys", app); mkdir(app, 0755); mkdir(p, 0755);
  snprintf(p, sizeof p, "%s/sce_sys/param.json", app);
  FILE *f = fopen(p, "w"); CHECK(f != NULL);
  if (f) { fputs("{\n  \"contentId\": \"IV0000-PPSA99999_00-UNITTEST00000000\",\n  \"titleId\" : \"PPSA99999\"\n}\n", f); fclose(f); }
  char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
  char *old_dd = getenv("PS4RECOMP_DATA_DIR") ? strdup(getenv("PS4RECOMP_DATA_DIR")) : NULL;
  unsetenv("NPS5_TITLE_ID");

  // 1. NPS5_USERDATA_DIR: <root>/<kind>.
  setenv("NPS5_USERDATA_DIR", ud, 1);
  ps4_vfs_init(app);
  CHECK(strcmp(ps4_vfs_title_id(), "PPSA99999") == 0);
  char tp[100]; snprintf(tp, sizeof tp, "%%s/temp/p%d/t.bin%%s", (int)getpid()); // /temp0 is per process
  static const char *const files[] = {"/data/a.bin", "/temp0/t.bin", "/download0/d.bin"};
  for (int i = 0; i < 3; i++) {
    int fd = ps4_vfs_open(files[i], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    if (fd >= 0) { CHECK(write(fd, "x", 1) == 1); ps4_vfs_close(fd); }
  }
  CHECK(host_exists("%s/data/a.bin%s", ud, "") && host_exists(tp, ud, "") && host_exists("%s/download0/d.bin%s", ud, ""));
  CHECK(!host_exists("%s/data%s", app, "") && !host_exists("%s/a.bin%s", app, ""));
  errno = 0; CHECK(ps4_vfs_open("relative.bin", O_WRONLY | O_CREAT, 0644) == -1 && errno == EROFS);
  CHECK(!host_exists("%s/relative.bin%s", app, ""));
  char dir[1100];
  CHECK(ps4_vfs_userdata_dir("savedata", dir, sizeof dir) && strstr(dir, "/ud/savedata") != NULL);
  // A new start empties /temp0 and keeps the rest.
  ps4_vfs_init(app);
  CHECK(!host_exists(tp, ud, "") && host_exists("%s/temp%s", ud, ""));
  CHECK(host_exists("%s/data/a.bin%s", ud, "") && host_exists("%s/download0/d.bin%s", ud, ""));

  // 2. Default: ~/Library/Application Support/nativeps5/<titleId>/<kind>.
  unsetenv("NPS5_USERDATA_DIR"); unsetenv("PS4RECOMP_DATA_DIR");
  setenv("HOME", tmp, 1);
  ps4_vfs_init(app);
  int fd = ps4_vfs_open("/download0/h.bin", O_WRONLY | O_CREAT, 0644);
  CHECK(fd >= 0); if (fd >= 0) ps4_vfs_close(fd);
  CHECK(host_exists("%s/Library/Application Support/nativeps5/PPSA99999/download0/h.bin%s", tmp, ""));

  // 3. PS4Recomp layout requested: only /data moves (it used to live inside the image).
  setenv("PS4RECOMP_DATA_DIR", ud, 1);
  ps4_vfs_init(app);
  CHECK(!ps4_vfs_userdata_dir("download0", dir, sizeof dir));
  CHECK(ps4_vfs_userdata_dir("data", dir, sizeof dir) && strstr(dir, "/ud/data/PPSA99999") != NULL);

  if (old_home) { setenv("HOME", old_home, 1); free(old_home); }
  if (old_dd) { setenv("PS4RECOMP_DATA_DIR", old_dd, 1); free(old_dd); } else unsetenv("PS4RECOMP_DATA_DIR");
  ps4_vfs_unmount("/data"); ps4_vfs_unmount("/temp0"); ps4_vfs_unmount("/download0");
}

// Mount modes (RPS5E ps5hle/src/savedata.rs mount()): CREATE=4 fails with EXISTS when the save exists, CREATE2=32 opens or
// creates, a plain open mode on a missing save is NOT_FOUND, and a mode that names nothing is a parameter error.
static void test_savedata_modes(void) {
  uint64_t res = galloc(0x40);
  const int32_t NOT_FOUND = (int32_t)0x809f0008, EXISTS = (int32_t)0x809f0007, PARAM = (int32_t)0x809f0000;
  CHECK(mount3("MODES", 0x2 /*RDWR*/, 0, res) == NOT_FOUND);
  CHECK(mount3("MODES", 0x1 /*RDONLY*/, 0, res) == NOT_FOUND);
  CHECK(mount3("MODES", 0x0, 96, res) == PARAM);
  CHECK(mount3("MODES", 0x10 /*COPY_ICON only*/, 96, res) == PARAM);
  CHECK(mount3("MODES", 0x20 | 0x2 /*CREATE2|RDWR*/, 96, res) == 0 && *(uint32_t *)H(res + 28) == 1); // created
  reset_regs(); C->rdi = 0; C->rsi = res; CALL(shim_sceSaveDataUmount2); CHECK(RAX32 == 0);
  CHECK(mount3("MODES", 0x20 | 0x2, 96, res) == 0 && *(uint32_t *)H(res + 28) == 0);                // opened, not created
  reset_regs(); C->rdi = 0; C->rsi = res; CALL(shim_sceSaveDataUmount2); CHECK(RAX32 == 0);
  CHECK(mount3("MODES", 0x4 | 0x2 /*CREATE|RDWR*/, 96, res) == EXISTS);                             // CREATE on an existing save
  CHECK(mount3("MODES", 0x4 | 0x20 | 0x2, 96, res) == EXISTS);                                      // CREATE wins over CREATE2 (as RPS5E)
  CHECK(mount3("MODES", 0x2, 0, res) == 0);                                                          // plain open works now
  reset_regs(); C->rdi = 0; C->rsi = res; CALL(shim_sceSaveDataUmount2); CHECK(RAX32 == 0);
  printf("hlesys: savedata mount modes ok\n");
}

static void test_savedata(const char *tmp) {
  char root[600]; snprintf(root, sizeof root, "%s/save", tmp);
  setenv("NPS5_SAVEDATA_DIR", root, 1);
  reset_regs(); CALL(shim_sceSaveDataInitialize3); CHECK(RAX32 == 0);

  uint64_t res = galloc(0x40);
  CHECK(mount3("SLOT0", 0x2 /*RDWR*/, 0, res) == (int32_t)0x809f0008);            // NOT_FOUND
  CHECK(mount3("SLOT0", 0x4 | 0x2 /*CREATE|RDWR*/, 96, res) == 0);
  CHECK(strcmp((char *)H(res), "/savedata0") == 0);
  CHECK(*(uint32_t *)H(res + 28) == 1);                                            // mountStatus CREATED
  CHECK(mount3("SLOT0", 0x2, 0, galloc(0x40)) == (int32_t)0x809f0003);            // BUSY while mounted
  CHECK(mount3("SLOT0", 0x4, 96, galloc(0x40)) != 0);                              // EXISTS/BUSY: create over a mount

  // transaction: resource, prepare, write through the VFS, commit
  reset_regs(); C->rdi = 1 << 20; CALL(shim_sceSaveDataCreateTransactionResource);
  int32_t resid = RAX32; CHECK(resid > 0);
  reset_regs(); C->rdi = res; C->rsi = galloc(0x28); CALL(shim_sceSaveDataPrepare); CHECK(RAX32 == 0);
  uint8_t payload[3000]; for (size_t i = 0; i < sizeof payload; i++) payload[i] = (uint8_t)(i * 31 + 7);
  int fd = ps4_vfs_open("/savedata0/progress.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  CHECK(fd >= 0);
  if (fd >= 0) { CHECK(write(fd, payload, sizeof payload) == (ssize_t)sizeof payload); ps4_vfs_close(fd); }
  uint64_t cp = galloc(0x28); memset(H(cp), 0, 0x28); *(int32_t *)H(cp) = resid;
  reset_regs(); C->rdi = cp; CALL(shim_sceSaveDataCommit); CHECK(RAX32 == 0);
  reset_regs(); C->rdi = resid; CALL(shim_sceSaveDataDeleteTransactionResource); CHECK(RAX32 == 0);

  // param (title) and icon
  uint64_t title = gstr("Dig deeper");
  reset_regs(); C->rdi = res; C->rsi = 1 /*TITLE*/; C->rdx = title; C->rcx = 11; CALL(shim_sceSaveDataSetParam); CHECK(RAX32 == 0);
  uint64_t icon = galloc(0x40), ibuf = galloc(64);
  memset(H(ibuf), 0xAB, 64);
  memset(H(icon), 0, 0x40); *(uint64_t *)H(icon) = ibuf; *(uint64_t *)H(icon + 8) = 64; *(uint64_t *)H(icon + 16) = 64;
  reset_regs(); C->rdi = res; C->rsi = icon; CALL(shim_sceSaveDataSaveIcon); CHECK(RAX32 == 0);

  // the files are in <root>/user_10000000/SLOT0
  char p[800]; struct stat st;
  snprintf(p, sizeof p, "%s/user_10000000/SLOT0/progress.bin", root);
  CHECK(stat(p, &st) == 0 && st.st_size == (off_t)sizeof payload);
  snprintf(p, sizeof p, "%s/user_10000000/SLOT0/sce_sys/param.sfo", root);
  CHECK(stat(p, &st) == 0);

  reset_regs(); C->rdi = 0; C->rsi = res; CALL(shim_sceSaveDataUmount2); CHECK(RAX32 == 0);
  reset_regs(); C->rdi = 0; C->rsi = res; CALL(shim_sceSaveDataUmount2); CHECK(RAX32 == (int32_t)0x809f0004); // NOT_MOUNTED

  // remount read-only and read back
  CHECK(mount3("SLOT0", 0x1 /*RDONLY*/, 0, res) == 0);
  uint8_t back[3000]; memset(back, 0, sizeof back);
  CHECK(rd_file("/savedata0/progress.bin", back, sizeof back) == (int)sizeof back);
  CHECK(memcmp(back, payload, sizeof payload) == 0);
  uint64_t out = galloc(64);
  reset_regs(); C->rdi = res; C->rsi = 1; C->rdx = out; C->rcx = 64; C->r8 = 0; CALL(shim_sceSaveDataGetParam);
  CHECK(RAX32 == 0 && strcmp((char *)H(out), "Dig deeper") == 0);
  // a read-only mount refuses Prepare
  reset_regs(); C->rdi = res; C->rsi = galloc(0x28); CALL(shim_sceSaveDataPrepare); CHECK(RAX32 != 0);
  reset_regs(); C->rdi = 0; C->rsi = res; CALL(shim_sceSaveDataUmount2);

  // a second slot, listed by name search, then deleted
  CHECK(mount3("SLOT1", 0x4 | 0x2, 96, res) == 0);
  reset_regs(); C->rdi = 0; C->rsi = res; CALL(shim_sceSaveDataUmount2);
  snprintf(p, sizeof p, "%s/user_10000000/SLOT1", root);
  CHECK(stat(p, &st) == 0 && S_ISDIR(st.st_mode));
  CHECK(mount3("SLOT1", 0x2, 0, res) == 0);
  reset_regs(); C->rdi = 0; C->rsi = res; CALL(shim_sceSaveDataUmount2);
  printf("savedata: round trip under %s ok\n", root);
}

// sceSaveDataTransferringMount opens ANOTHER title's save: <root>/transfer/<titleId>/user_<id>/<dir>, read-only,
// NOT_FOUND when absent; never the running title's own <root>/user_<id>/<dir> (the v1.0.1 save-load crash).
static int transfer_mount(const char *title, const char *dir, uint64_t result) {
  uint64_t m = galloc(sizeof(SdTransferringMount));
  SdTransferringMount *tm = (SdTransferringMount *)H(m);
  memset(tm, 0, sizeof *tm);
  tm->userId = 0x10000000;
  tm->dirName = gstr(dir);
  tm->titleId = title ? gstr(title) : 0;
  return sceSaveDataTransferringMount(C, tm, (SdMountResult *)H(result));
}

static void test_savedata_transfer(const char *tmp) {
  const int32_t NOT_FOUND = (int32_t)0x809f0008, PARAM = (int32_t)0x809f0000, BUSY = (int32_t)0x809f0003;
  char root[600]; snprintf(root, sizeof root, "%s/save", tmp);
  uint64_t res = galloc(0x40), res2 = galloc(0x40);
  // the running title's own sce_sdmemory exists and must not be what a transferring mount opens
  CHECK(mount3("sce_sdmemory", 0x20 | 0x2, 96, res) == 0);
  { int fd = ps4_vfs_open("/savedata0/memory.dat", O_WRONLY | O_CREAT | O_TRUNC, 0644); CHECK(fd >= 0); if (fd >= 0) ps4_vfs_close(fd); }
  reset_regs(); C->rdi = 0; C->rsi = res; CALL(shim_sceSaveDataUmount2); CHECK(RAX32 == 0);
  CHECK(transfer_mount("PPSA01325", "sce_sdmemory", res) == NOT_FOUND);          // no source save: NOT_FOUND
  char p[900]; struct stat st;
  snprintf(p, sizeof p, "%s/transfer", root);
  CHECK(stat(p, &st) != 0);                                                       // and nothing was created
  // a title id that is not one path component is refused ('..' would reach <root>/user_<id>/sce_sdmemory)
  CHECK(transfer_mount("..", "sce_sdmemory", res) == PARAM);
  CHECK(transfer_mount("../x", "sce_sdmemory", res) == PARAM);
  CHECK(transfer_mount("", "sce_sdmemory", res) == PARAM);
  CHECK(transfer_mount(NULL, "sce_sdmemory", res) == PARAM);
  CHECK(transfer_mount("PPSA01325", "..", res) == PARAM);
  // a source save present: mounted read-only
  char cmd[1400];
  snprintf(cmd, sizeof cmd, "mkdir -p '%s/transfer/PPSA01325/user_10000000/sce_sdmemory/sce_sys' && printf x > "
           "'%s/transfer/PPSA01325/user_10000000/sce_sdmemory/sce_sys/param.sfo' && printf PLAYROOM > "
           "'%s/transfer/PPSA01325/user_10000000/sce_sdmemory/memory.dat'", root, root, root);
  CHECK(system(cmd) == 0);
  CHECK(transfer_mount("PPSA01325", "sce_sdmemory", res) == 0);
  char mp[32]; snprintf(mp, sizeof mp, "%s", (char *)H(res));
  char path[64], back[16] = {0};
  snprintf(path, sizeof path, "%s/memory.dat", mp);
  CHECK(rd_file(path, back, sizeof back) == 8 && memcmp(back, "PLAYROOM", 8) == 0);
  errno = 0; CHECK(ps4_vfs_open(path, O_WRONLY, 0) == -1 && errno == EROFS);
  snprintf(path, sizeof path, "%s/new.dat", mp);
  errno = 0; CHECK(ps4_vfs_open(path, O_WRONLY | O_CREAT, 0644) == -1 && errno == EROFS);
  uint64_t t = gstr("x"), icon = galloc(sizeof(SdIcon));
  CHECK(sceSaveDataSetParam(C, res, 1 /*TITLE*/, t, 2) != 0);
  SdIcon *ic = (SdIcon *)H(icon); memset(ic, 0, sizeof *ic); ic->buf = t; ic->bufSize = ic->dataSize = 1;
  CHECK(sceSaveDataSaveIcon(C, res, ic) != 0);
  snprintf(p, sizeof p, "%s/transfer/PPSA01325/user_10000000/sce_sdmemory/sce_sys/icon0.png", root);
  CHECK(stat(p, &st) != 0);                                                       // the other title's save is untouched
  // the same source twice is BUSY; the own directory of the same name is not
  CHECK(transfer_mount("PPSA01325", "sce_sdmemory", res2) == BUSY);
  CHECK(mount3("sce_sdmemory", 0x2, 0, res2) == 0);
  reset_regs(); C->rdi = 0; C->rsi = res2; CALL(shim_sceSaveDataUmount2); CHECK(RAX32 == 0);
  reset_regs(); C->rdi = 0; C->rsi = res; CALL(shim_sceSaveDataUmount2); CHECK(RAX32 == 0);
  // APS5_SAVEDATA_TRANSFER_FALLBACK=self: the old own-directory read/write mount
  setenv("APS5_SAVEDATA_TRANSFER_FALLBACK", "self", 1);
  CHECK(transfer_mount("PPSA01325", "sce_sdmemory", res) == 0);
  snprintf(path, sizeof path, "%s/memory.dat", (char *)H(res));
  CHECK(rd_file(path, back, sizeof back) == 0);                                    // the own (empty) memory.dat
  unsetenv("APS5_SAVEDATA_TRANSFER_FALLBACK");
  reset_regs(); C->rdi = 0; C->rsi = res; CALL(shim_sceSaveDataUmount2); CHECK(RAX32 == 0);
  printf("hlesys: savedata transferring mount ok\n");
}

// -------------------------------------------------------------------------------------- Apr

static void test_apr(const char *tmp) {
  // a 1 MiB file with a position-dependent pattern, resolved through a VFS mount
  char dir[600]; snprintf(dir, sizeof dir, "%s/pak", tmp);
  mkdir(dir, 0755);
  char fp[700]; snprintf(fp, sizeof fp, "%s/data.ucas", dir);
  size_t fsz = 1 << 20;
  uint8_t *pat = malloc(fsz);
  for (size_t i = 0; i < fsz; i++) pat[i] = (uint8_t)((i * 2654435761u) >> 13);
  FILE *f = fopen(fp, "wb"); fwrite(pat, 1, fsz, f); fclose(f);
  ps4_vfs_mount("/apprt", dir);

  uint64_t paths = galloc(16), p0 = gstr("/apprt/data.ucas"), p1 = gstr("/apprt/missing.bin");
  uint64_t ids = galloc(16), sizes = galloc(16), erridx = galloc(8);
  *(uint64_t *)H(paths) = p0; *(uint64_t *)H(paths + 8) = p1;
  reset_regs(); C->rdi = paths; C->rsi = 1; C->rdx = ids; C->rcx = sizes; C->r8 = erridx;
  CALL(shim_sceKernelAprResolveFilepathsToIdsAndFileSizes);
  CHECK(RAX32 == 0 && *(uint64_t *)H(sizes) == fsz);
  uint32_t fid = *(uint32_t *)H(ids);
  reset_regs(); C->rdi = paths; C->rsi = 2; C->rdx = ids; C->rcx = sizes; C->r8 = erridx;
  *(uint32_t *)H(erridx) = 99;
  CALL(shim_sceKernelAprResolveFilepathsToIdsAndFileSizes);
  CHECK(RAX32 == -1 && *(uint32_t *)H(erridx) == 1 && *(uint32_t *)(H(ids) + 4) == 0xFFFFFFFFu);
  CHECK(*(int *)H(C->fs_base + 0x100) == 2); // errno ENOENT
  // the same path resolves to the same id
  reset_regs(); C->rdi = paths; C->rsi = 1; C->rdx = ids; C->rcx = sizes; C->r8 = erridx;
  CALL(shim_sceKernelAprResolveFilepathsToIdsAndFileSizes);
  CHECK(*(uint32_t *)H(ids) == fid);

  // command buffer: 2 reads + an event
  uint64_t cb = galloc(0x18), gs = galloc(8), ss = galloc(8), mem = galloc(256);
  memset(H(cb), 0xEE, 0x18);
  reset_regs(); C->rdi = cb; C->rsi = gs; C->rdx = ss; CALL(shim_sceAmprAprCommandBufferConstructor); CHECK(RAX32 == 0);
  CHECK(*(uint16_t *)H(cb) == 1);
  reset_regs(); C->rdi = cb; C->rsi = mem; C->rdx = 4 * 24 + 8; CALL(shim_sceAmprCommandBufferSetBuffer); CHECK(RAX32 == 0);
  reset_regs(); C->rdi = cb; CALL(shim_sceAmprCommandBufferGetSize); CHECK((uint32_t)C->rax == 4 * 24 + 8);
  reset_regs(); CALL(shim_sceAmprMeasureCommandSizeReadFile); CHECK(C->rax == 24);
  reset_regs(); CALL(shim_sceAmprMeasureCommandSizeWriteKernelEventQueue_04_00); CHECK(C->rax == 24);

  uint64_t d0 = galloc(70000), d1 = galloc(9000), d2 = galloc(4096);
  memset(H(d0), 0x55, 70000); memset(H(d1), 0x55, 9000); memset(H(d2), 0x55, 4096);
  uint64_t stack = galloc(256); C->rsp = stack + 64;
  struct { uint64_t dst, size, off; } reads[3] = {{d0, 65536, 0x1234}, {d1, 8192, 0x80000}, {d2, 4096, fsz - 1000}};
  for (int i = 0; i < 3; i++) {
    *(uint64_t *)H(C->rsp + 8) = reads[i].off;
    C->rdi = cb; C->rsi = gs; C->rdx = ss; C->rcx = fid; C->r8 = reads[i].dst; C->r9 = reads[i].size;
    CALL(shim_sceAmprAprCommandBufferReadFile);
    CHECK(RAX32 == 0);
  }
  reset_regs(); C->rdi = cb; CALL(shim_sceAmprCommandBufferGetCurrentOffset); CHECK((uint32_t)C->rax == 72);

  int eq = 0;
  CHECK(sceKernelCreateEqueue(&eq, "apr") == 0 && eq > 0);
  reset_regs(); C->rdi = eq; C->rsi = 7; C->rdx = 0xCAFE; CALL(shim_sceKernelAddAmprEvent); CHECK(RAX32 == 0);
  reset_regs(); C->rdi = cb; C->rsi = (uint64_t)eq; C->rdx = 7; C->rcx = 0x1122334455667788ull; C->r8 = 0;
  CALL(shim_sceAmprCommandBufferWriteKernelEventQueue_04_00); CHECK(RAX32 == 0);
  // 4 records of 24 bytes = 96 of the 104 bytes; a fifth no longer fits: EBUSY, nothing recorded
  reset_regs(); C->rdi = cb; C->rsi = (uint64_t)eq; C->rdx = 7; C->rcx = 1; CALL(shim_sceAmprCommandBufferWriteKernelEventQueue_04_00);
  CHECK(RAX32 == (int32_t)0x80020010);

  uint64_t resp = galloc(8), idp = galloc(8);
  reset_regs(); C->rdi = cb; C->rsi = 0; C->rdx = resp; C->rcx = idp;
  CALL(shim_sceKernelAprSubmitCommandBufferAndGetResult);
  CHECK(RAX32 == 0 && *(uint32_t *)H(resp) == 0 && *(uint32_t *)H(idp) >= 1);
  reset_regs(); C->rdi = *(uint32_t *)H(idp); CALL(shim_sceKernelAprWaitCommandBuffer); CHECK(RAX32 == 0);

  CHECK(memcmp(H(d0), pat + 0x1234, 65536) == 0);
  CHECK(H(d0)[65536] == 0x55);                       // nothing written past the request
  CHECK(memcmp(H(d1), pat + 0x80000, 8192) == 0);
  CHECK(memcmp(H(d2), pat + fsz - 1000, 1000) == 0);
  int zeros = 1; for (int i = 1000; i < 4096; i++) if (H(d2)[i] != 0) zeros = 0;
  CHECK(zeros);                                       // past EOF reads as zeros

  OrbisKernelEvent ev; int n = 0; uint32_t tmo = 2000000;
  CHECK(sceKernelWaitEqueue(eq, &ev, 1, &n, &tmo) == 0 && n == 1);
  CHECK(ev.filter == -25 && ev.ident == 7 && ev.data == 0x1122334455667788ll && (uintptr_t)ev.udata == 0xCAFE);

  // size query + reset + clear
  reset_regs(); C->rdi = fid; C->rsi = galloc(8); CALL(shim_sceKernelAprGetFileSize);
  CHECK(RAX32 == 0 && *(uint64_t *)H(C->rsi) == fsz);
  reset_regs(); C->rdi = cb; CALL(shim_sceAmprCommandBufferReset); CHECK(RAX32 == 0);
  reset_regs(); C->rdi = cb; CALL(shim_sceAmprCommandBufferGetCurrentOffset); CHECK(C->rax == 0);
  reset_regs(); C->rdi = cb; CALL(shim_sceAmprCommandBufferClearBuffer); CHECK(C->rax == mem);
  // recording with no memory bound is EPERM
  *(uint64_t *)H(C->rsp + 8) = 0;
  C->rdi = cb; C->rsi = gs; C->rdx = ss; C->rcx = fid; C->r8 = d0; C->r9 = 16; CALL(shim_sceAmprAprCommandBufferReadFile);
  CHECK(RAX32 == (int32_t)0x80020001);
  // many submissions complete in order and the waits do not hang
  for (int k = 0; k < 20; k++) {
    reset_regs(); C->rdi = cb; C->rsi = mem; C->rdx = 256; CALL(shim_sceAmprCommandBufferSetBuffer);
    *(uint64_t *)H(C->rsp + 8) = (uint64_t)k * 4096;
    C->rdi = cb; C->rsi = gs; C->rdx = ss; C->rcx = fid; C->r8 = d1; C->r9 = 4096; CALL(shim_sceAmprAprCommandBufferReadFile);
    reset_regs(); C->rdi = cb; C->rsi = 0; C->rdx = resp; C->rcx = idp; CALL(shim_sceKernelAprSubmitCommandBufferAndGetResult);
    reset_regs(); C->rdi = *(uint32_t *)H(idp); CALL(shim_sceKernelAprWaitCommandBuffer);
    CHECK(memcmp(H(d1), pat + (size_t)k * 4096, 4096) == 0);
  }
  printf("apr: reads, event, ordering ok\n");
  free(pat);
}


// ---- astro-hle2: the rest of the AMPR command set and the Apr path resolution
static int cbcall(void (*fn)(GuestContext *)) { CALL(fn); return RAX32; }
static void apr_run(uint64_t cb) {
  uint64_t resp = galloc(8), idp = galloc(8);
  reset_regs(); C->rdi = cb; C->rsi = 0; C->rdx = resp; C->rcx = idp;
  CALL(shim_sceKernelAprSubmitCommandBufferAndGetResult); CHECK(RAX32 == 0);
  reset_regs(); C->rdi = *(uint32_t *)H(idp); CALL(shim_sceKernelAprWaitCommandBuffer); CHECK(RAX32 == 0);
}
static void test_apr_more(const char *tmp) {
  char dir[600]; snprintf(dir, sizeof dir, "%s/pak2", tmp); mkdir(dir, 0755);
  size_t fsz = 100000; uint8_t *pat = malloc(fsz);
  for (size_t i = 0; i < fsz; i++) pat[i] = (uint8_t)((i * 40503u) >> 5);
  char fp[700];
  snprintf(fp, sizeof fp, "%s/pre_a.bin", dir); FILE *f = fopen(fp, "wb"); fwrite(pat, 1, fsz, f); fclose(f);
  snprintf(fp, sizeof fp, "%s/pre_b.bin", dir); f = fopen(fp, "wb"); fwrite(pat, 1, 5000, f); fclose(f);
  ps4_vfs_mount("/apprt2", dir);
  uint64_t stack = galloc(256); C->rsp = stack + 64;

  // prefix resolution, with and without sizes, ForEach, GetFileStat
  uint64_t pre = gstr("/apprt2/pre_"), na = gstr("a.bin"), nb = gstr("b.bin"), nm = gstr("zz.bin");
  uint64_t names = galloc(24), ids = galloc(12), sz = galloc(24), res = galloc(12), eix = galloc(4);
  *(uint64_t *)H(names) = na; *(uint64_t *)H(names + 8) = nb; *(uint64_t *)H(names + 16) = nm;
  reset_regs(); C->rdi = pre; C->rsi = names; C->rdx = 2; C->rcx = ids; C->r8 = eix;
  CALL(shim_sceKernelAprResolveFilepathsWithPrefixToIds); CHECK(RAX32 == 0);
  uint32_t ida = ((uint32_t *)H(ids))[0], idb = ((uint32_t *)H(ids))[1];
  CHECK(ida != idb && ida != 0xFFFFFFFFu);
  reset_regs(); C->rdi = pre; C->rsi = names; C->rdx = 3; C->rcx = ids; C->r8 = sz; C->r9 = eix; *(uint32_t *)H(eix) = 77;
  CALL(shim_sceKernelAprResolveFilepathsWithPrefixToIdsAndFileSizes);
  CHECK(RAX32 == -1 && *(uint32_t *)H(eix) == 2 && ((uint32_t *)H(ids))[2] == 0xFFFFFFFFu);
  CHECK(((uint64_t *)H(sz))[0] == fsz && ((uint64_t *)H(sz))[1] == 5000);
  reset_regs(); C->rdi = pre; C->rsi = names; C->rdx = 3; C->rcx = ids; C->r8 = res;
  CALL(shim_sceKernelAprResolveFilepathsWithPrefixToIdsForEach);
  CHECK(RAX32 == 1 && ((int32_t *)H(res))[0] == 0 && ((int32_t *)H(res))[2] == (int32_t)0x80020002);
  reset_regs(); C->rdi = pre; C->rsi = names; C->rdx = 3; C->rcx = ids; C->r8 = sz; C->r9 = res;
  CALL(shim_sceKernelAprResolveFilepathsWithPrefixToIdsAndFileSizesForEach);
  CHECK(RAX32 == 1 && ((uint64_t *)H(sz))[2] == 0 && ((uint32_t *)H(ids))[1] == idb);
  uint64_t full = galloc(16), p0 = gstr("/apprt2/pre_a.bin"), p1 = gstr("/apprt2/none");
  *(uint64_t *)H(full) = p0; *(uint64_t *)H(full + 8) = p1;
  reset_regs(); C->rdi = full; C->rsi = 2; C->rdx = ids; C->rcx = res;
  CALL(shim_sceKernelAprResolveFilepathsToIdsForEach);
  CHECK(RAX32 == 1 && ((uint32_t *)H(ids))[0] == ida && ((int32_t *)H(res))[1] == (int32_t)0x80020002);
  reset_regs(); C->rdi = full; C->rsi = 2; C->rdx = ids; C->rcx = sz; C->r8 = res;
  CALL(shim_sceKernelAprResolveFilepathsToIdsAndFileSizesForEach); CHECK(RAX32 == 1 && ((uint64_t *)H(sz))[0] == fsz);
  uint64_t st = galloc(256);
  reset_regs(); C->rdi = idb; C->rsi = st; CALL(shim_sceKernelAprGetFileStat); CHECK(RAX32 == 0);
  { int found = 0; for (int o = 0; o + 8 <= 256; o += 8) if (*(int64_t *)H(st + o) == 5000) found = 1; CHECK(found); }
  reset_regs(); C->rdi = 9999999; C->rsi = st; CALL(shim_sceKernelAprGetFileStat); CHECK(RAX32 == -1);
  reset_regs(); C->rdi = idb; C->rsi = 0; CALL(shim_sceKernelAprGetFileStat); CHECK(RAX32 == -1);

  // command buffer: markers, nops, map, counters, gather/scatter, address writes
  uint64_t cb = galloc(0x18), gs = galloc(8), ss = galloc(8), mem = galloc(4096);
  reset_regs(); C->rdi = cb; C->rsi = gs; C->rdx = ss; CALL(shim_sceAmprAprCommandBufferConstructor);
  reset_regs(); C->rdi = cb; C->rsi = mem; C->rdx = 4096; CALL(shim_sceAmprCommandBufferSetBuffer);
  reset_regs(); C->rdi = cb; CALL(shim_sceAmprCommandBufferGetType); CHECK(C->rax == 1);
  reset_regs(); C->rdi = cb; CALL(shim_sceAmprCommandBufferGetBufferBaseAddress); CHECK(C->rax == mem);
  uint64_t txt = gstr("load level");
  reset_regs(); C->rdi = txt; CALL(shim_sceAmprMeasureCommandSizePushMarker); uint64_t msz = C->rax; CHECK(msz == 8 + 16);
  reset_regs(); C->rdi = cb; C->rsi = txt; CALL(shim_sceAmprCommandBufferPushMarker); CHECK(RAX32 == 0);
  reset_regs(); C->rdi = cb; CALL(shim_sceAmprCommandBufferPopMarker); CHECK(RAX32 == 0);
  reset_regs(); C->rdi = cb; C->rsi = 3; CALL(shim_sceAmprCommandBufferNop); CHECK(RAX32 == 0);
  reset_regs(); C->rdi = 3; CALL(shim_sceAmprMeasureCommandSizeNop); CHECK(C->rax == 12);
  uint64_t nd = galloc(16);
  reset_regs(); C->rdi = cb; C->rsi = 2; C->rdx = nd; CALL(shim_sceAmprCommandBufferNopWithData); CHECK(RAX32 == 0);
  reset_regs(); C->rdi = cb; CALL(shim_sceAmprCommandBufferGetCurrentOffset);
  CHECK((uint32_t)C->rax == msz + 8 + 12 + 12);
  reset_regs(); C->rdi = cb; CALL(shim_sceAmprCommandBufferGetNumCommands); CHECK((uint32_t)C->rax == 4);
  // map begin/end pairing
  reset_regs(); C->rdi = cb; CALL(shim_sceAmprAprCommandBufferMapEnd); CHECK(RAX32 == (int32_t)0x80020001);
  reset_regs(); C->rdi = cb; C->rsi = 0x300000000ull; C->rdx = 0x4000; C->rcx = 0; C->r8 = 3;
  CALL(shim_sceAmprAprCommandBufferMapBegin); CHECK(RAX32 == 0);
  reset_regs(); C->rdi = cb; C->rsi = 0x300000001ull; C->rdx = 0x4000; CALL(shim_sceAmprAprCommandBufferMapBegin); CHECK(RAX32 == (int32_t)0x80020016);
  reset_regs(); C->rdi = cb; CALL(shim_sceAmprAprCommandBufferMapEnd); CHECK(RAX32 == 0);

  // counters: store 7 into counter 5 (access 1 = 32 bit, op 0), add 3 -> 10; write it to an address
  uint64_t out = galloc(32);
  reset_regs(); C->rdi = cb; C->rsi = 5; C->rdx = 1; C->rcx = 7; C->r8 = 0; C->r9 = 0; CALL(shim_sceAmprCommandBufferWriteCounter_04_00); CHECK(RAX32 == 0);
  reset_regs(); C->rdi = cb; C->rsi = 5; C->rdx = 1; C->rcx = 3; C->r8 = 4; C->r9 = 0; CALL(shim_sceAmprCommandBufferWriteCounter_04_00); CHECK(RAX32 == 0);
  reset_regs(); C->rdi = cb; C->rsi = out; C->rdx = 5; C->rcx = 0; CALL(shim_sceAmprCommandBufferWriteAddressFromCounter_04_00); CHECK(RAX32 == 0);
  reset_regs(); C->rdi = cb; C->rsi = out + 8; C->rdx = 0x1122334455667788ull; C->rcx = 0; CALL(shim_sceAmprCommandBufferWriteAddress_04_00); CHECK(RAX32 == 0);
  reset_regs(); C->rdi = cb; C->rsi = out + 16; C->rdx = 0; CALL(shim_sceAmprCommandBufferWriteAddressFromTimeCounter_04_00); CHECK(RAX32 == 0);
  reset_regs(); C->rdi = cb; C->rsi = out + 3; C->rdx = 5; CALL(shim_sceAmprCommandBufferWriteAddressFromCounter_04_00); CHECK(RAX32 == (int32_t)0x80020016); // unaligned
  // wait on counter 5 == 10 (compare 0, mask unused): satisfied already, so must not block
  reset_regs(); C->rdi = cb; C->rsi = 5; C->rdx = 1; C->rcx = 10; C->r8 = 0; C->r9 = 0;
  *(uint64_t *)H(C->rsp + 8) = 0; *(uint64_t *)H(C->rsp + 16) = 0; CALL(shim_sceAmprCommandBufferWaitOnCounter_04_00); CHECK(RAX32 == 0);
  // gather/scatter: a Gather before any ReadFile is EINVAL at record time
  reset_regs(); C->rdi = cb; C->rsi = 0; C->rdx = 0; C->rcx = 100; C->r8 = 0; CALL(shim_sceAmprAprCommandBufferReadFileGather); CHECK(RAX32 == (int32_t)0x80020016);
  uint64_t d0 = galloc(20000); memset(H(d0), 0x55, 20000);
  // ReadFile(ida, d0, 1000, off 100) ; Gather(500, off 5000) -> dst d0+1000 ; Scatter(dst d0+8000, 300) -> off 5500
  *(uint64_t *)H(C->rsp + 8) = 100;
  reset_regs(); C->rdi = cb; C->rsi = gs; C->rdx = ss; C->rcx = ida; C->r8 = d0; C->r9 = 1000;
  CALL(shim_sceAmprAprCommandBufferReadFile); CHECK(RAX32 == 0);
  reset_regs(); C->rdi = cb; C->rsi = 0; C->rdx = 0; C->rcx = 500; C->r8 = 5000; CALL(shim_sceAmprAprCommandBufferReadFileGather); CHECK(RAX32 == 0);
  reset_regs(); C->rdi = cb; C->rsi = 0; C->rdx = 0; C->rcx = d0 + 8000; C->r8 = 300; CALL(shim_sceAmprAprCommandBufferReadFileScatter); CHECK(RAX32 == 0);
  reset_regs(); C->rdi = cb; C->rsi = 0; C->rdx = 0; C->rcx = d0 + 12000; C->r8 = 64; C->r9 = 90000;
  CALL(shim_sceAmprAprCommandBufferReadFileGatherScatter); CHECK(RAX32 == 0);
  reset_regs(); C->rdi = cb; CALL(shim_sceAmprAprCommandBufferResetGatherScatterState); CHECK(RAX32 == 0);
  reset_regs(); C->rdi = cb; C->rsi = 0; C->rdx = 0; C->rcx = 1; C->r8 = 1; CALL(shim_sceAmprAprCommandBufferReadFileGather); CHECK(RAX32 == (int32_t)0x80020016);

  apr_run(cb);
  CHECK(memcmp(H(d0), pat + 100, 1000) == 0);
  CHECK(memcmp(H(d0) + 1000, pat + 5000, 500) == 0);
  CHECK(memcmp(H(d0) + 8000, pat + 5500, 300) == 0);
  CHECK(memcmp(H(d0) + 12000, pat + 90000, 64) == 0);
  CHECK(H(d0)[1500] == 0x55 && H(d0)[8300] == 0x55);
  CHECK(*(uint64_t *)H(out) == 10);
  CHECK(*(uint64_t *)H(out + 8) == 0x1122334455667788ull);
  CHECK(*(uint64_t *)H(out + 16) != 0);

  // a wait that is released by a later submission: command buffer 1 waits for counter 9 == 1, then writes a flag
  uint64_t cb2 = galloc(0x18), mem2 = galloc(512), flag = galloc(8);
  reset_regs(); C->rdi = cb2; CALL(shim_sceAmprCommandBufferConstructor);
  reset_regs(); C->rdi = cb2; C->rsi = mem2; C->rdx = 512; CALL(shim_sceAmprCommandBufferSetBuffer);
  uint64_t wflag = galloc(8);
  reset_regs(); C->rdi = cb2; C->rsi = wflag; C->rdx = 5; C->rcx = 0; C->r8 = 0; CALL(shim_sceAmprCommandBufferWaitOnAddress_04_00); CHECK(RAX32 == 0);
  reset_regs(); C->rdi = cb2; C->rsi = flag; C->rdx = 0xAB; C->rcx = 0; CALL(shim_sceAmprCommandBufferWriteAddress_04_00);
  uint64_t resp = galloc(8), idp = galloc(8);
  reset_regs(); C->rdi = cb2; C->rsi = 0; C->rdx = resp; C->rcx = idp; CALL(shim_sceKernelAprSubmitCommandBufferAndGetResult); CHECK(RAX32 == 0);
  uint32_t id2 = *(uint32_t *)H(idp);
  usleep(100000);
  CHECK(*(uint64_t *)H(flag) == 0);                 // still waiting for wflag == 5
  // a later, independent submission is not stuck behind the waiting one
  reset_regs(); C->rdi = cb; CALL(shim_sceAmprCommandBufferReset);
  reset_regs(); C->rdi = cb; C->rsi = 5; C->rdx = 1; C->rcx = 1; C->r8 = 0; C->r9 = 0; CALL(shim_sceAmprCommandBufferWriteCounter_04_00);
  apr_run(cb);
  *(volatile uint64_t *)H(wflag) = 5;
  reset_regs(); C->rdi = id2; CALL(shim_sceKernelAprWaitCommandBuffer); CHECK(RAX32 == 0);
  CHECK(*(uint64_t *)H(flag) == 0xAB);
  // WaitOnAddress/WaitOnCounter with a bad compare function are rejected
  reset_regs(); C->rdi = cb2; C->rsi = wflag; C->rdx = 5; C->rcx = 9; CALL(shim_sceAmprCommandBufferWaitOnAddress_04_00); CHECK(RAX32 == (int32_t)0x80020016);
  printf("ampr: markers, nops, map, counters, gather/scatter, prefix resolve ok\n");
  free(pat);
}


static void test_astro_stubs(void) {
  // AudioPropagation: query -> create -> source -> silent render
  uint64_t opt = galloc(64), mem = galloc(0x30), sys = galloc(8), src = galloc(8), cnt = galloc(4);
  uint32_t *o = (uint32_t *)H(opt); memset(o, 0, 64);
  o[0] = 0x010107d5; o[1] = 0; o[4] = 8; o[5] = 4; o[6] = 6; o[7] = 1; o[8] = 512;
  float spd = 340.f, ml = 100.f; memcpy(&o[9], &spd, 4); memcpy(&o[10], &ml, 4);
  memset(H(mem), 0, 0x30); *(uint32_t *)H(mem) = 0x010107d4;
  reset_regs(); C->rdi = opt; C->rsi = mem; CALL(shim_sceAudioPropagationSystemQueryMemory); CHECK(RAX32 == 0);
  CHECK(*(uint64_t *)H(mem + 0x18) > 100000);
  uint64_t blk = galloc(*(uint64_t *)H(mem + 0x18)); *(uint64_t *)H(mem + 0x10) = blk;
  reset_regs(); C->rdi = opt; C->rsi = mem; C->rdx = sys; CALL(shim_sceAudioPropagationSystemCreate); CHECK(RAX32 == 0 && *(uint64_t *)H(sys) == blk);
  reset_regs(); C->rdi = blk; C->rsi = src; CALL(shim_sceAudioPropagationSourceCreate); CHECK(RAX32 == 0 && *(uint64_t *)H(src) != 0);
  reset_regs(); C->rdi = *(uint64_t *)H(src); C->rsi = cnt; CALL(shim_sceAudioPropagationSourceGetAudioPathCount); CHECK(RAX32 == 0 && *(uint32_t *)H(cnt) == 36);
  uint64_t ri = galloc(0x30); memset(H(ri), 0xEE, 0x30);
  reset_regs(); C->rdi = blk; C->rsi = ri; C->rdx = 1; CALL(shim_sceAudioPropagationSourceRender); CHECK(RAX32 == 0 && *(uint32_t *)H(ri + 0x2c) == 0);
  // Json: an empty array iterates zero times
  uint64_t arr = galloc(8), it0 = galloc(8), it1 = galloc(8);
  memset(H(arr), 0xFF, 8);
  reset_regs(); C->rdi = arr; CALL(shim__ZN3sce4Json5ArrayC1Ev); CHECK(*(uint64_t *)H(arr) == 0);
  reset_regs(); C->rdi = it0; C->rsi = arr; CALL(shim__ZNK3sce4Json5Array5beginEv);
  reset_regs(); C->rdi = it1; C->rsi = arr; CALL(shim__ZNK3sce4Json5Array3endEv);
  reset_regs(); C->rdi = it0; C->rsi = it1; CALL(shim__ZNK3sce4Json5Array8iteratorneERKS2_); CHECK(C->rax == 0);
  reset_regs(); C->rdi = arr; CALL(shim__ZNK3sce4Json5Array5emptyEv); CHECK(C->rax == 1);
  // Value layout: payload at +16, type at +28 (UInteger = 3); nothing past 32 bytes is written
  uint64_t val = galloc(64); memset(H(val), 0xFF, 64);
  reset_regs(); C->rdi = val; C->rsi = 77; CALL(shim__ZN3sce4Json5ValueC1Em);
  CHECK(*(uint32_t *)H(val + 28) == 3 && *(uint64_t *)H(val + 16) == 77 && *(uint64_t *)H(val + 32) == ~0ull);
  // String is 8 bytes
  uint64_t js = galloc(16); memset(H(js), 0xFF, 16);
  reset_regs(); C->rdi = js; C->rsi = gstr("list"); CALL(shim__ZN3sce4Json6StringC1EPKc); CHECK(*(uint64_t *)H(js + 8) == ~0ull);
  reset_regs(); C->rdi = js; CALL(shim__ZNK3sce4Json6String5c_strEv); CHECK(!strcmp((char *)H(C->rax), "list"));
  reset_regs(); C->rdi = js; CALL(shim__ZNK3sce4Json6String6lengthEv); CHECK(C->rax == 4);
  reset_regs(); C->rdi = js; C->rsi = gstr("list"); CALL(shim__ZNK3sce4Json6StringeqEPKc); CHECK(C->rax == 1);
  // parse + read back
  const char *doc = " {\"name\":\"ps_\\u0041\\n\",\"n\":-3,\"u\":7,\"f\":1.5e1,\"ok\":true,\"list\":[1,\"two\",{\"k\":null}]} ";
  uint64_t root = galloc(32), gdoc = gstr(doc);
  reset_regs(); C->rdi = root; CALL(shim__ZN3sce4Json5ValueC1Ev);
  reset_regs(); C->rdi = root; C->rsi = gdoc; C->rdx = strlen(doc); CALL(shim__ZN3sce4Json6Parser5parseERNS0_5ValueEPKcm); CHECK(RAX32 == 0);
  reset_regs(); C->rdi = root; CALL(shim__ZNK3sce4Json5Value7getTypeEv); CHECK(C->rax == 7);
  reset_regs(); C->rdi = root; CALL(shim__ZNK3sce4Json5Value5countEv); CHECK(C->rax == 6);
#define JKEY(k) (reset_regs(), C->rdi = root, C->rsi = gstr(k), shim__ZNK3sce4Json5ValueixEPKc(C), C->rsp -= 8, C->rax)
#define JTYPE(v) (reset_regs(), C->rdi = (v), shim__ZNK3sce4Json5Value7getTypeEv(C), C->rsp -= 8, C->rax)
  uint64_t vn = JKEY("n"), vu = JKEY("u"), vf = JKEY("f"), vok = JKEY("ok"), vname = JKEY("name"), vlist = JKEY("list"), vmiss = JKEY("missing");
  CHECK(JTYPE(vn) == 2 && JTYPE(vu) == 3 && JTYPE(vf) == 4 && JTYPE(vok) == 1 && JTYPE(vname) == 5 && JTYPE(vlist) == 6 && JTYPE(vmiss) == 0);
  reset_regs(); C->rdi = vn; CALL(shim__ZNK3sce4Json5Value10getIntegerEv); CHECK(*(int64_t *)H(C->rax) == -3);
  reset_regs(); C->rdi = vu; CALL(shim__ZNK3sce4Json5Value11getUIntegerEv); CHECK(*(uint64_t *)H(C->rax) == 7);
  reset_regs(); C->rdi = vf; CALL(shim__ZNK3sce4Json5Value7getRealEv); CHECK(*(double *)H(C->rax) == 15.0);
  reset_regs(); C->rdi = vok; CALL(shim__ZNK3sce4Json5Value10getBooleanEv); CHECK(*(uint8_t *)H(C->rax) == 1);
  reset_regs(); C->rdi = vname; CALL(shim__ZNK3sce4Json5Value9getStringEv);
  reset_regs(); C->rdi = C->rax; CALL(shim__ZNK3sce4Json6String5c_strEv); CHECK(!strcmp((char *)H(C->rax), "ps_A\n"));
  reset_regs(); C->rdi = vmiss; CALL(shim__ZNK3sce4Json5Value7getRealEv); CHECK(*(double *)H(C->rax) == 0.0);
  // array: iterate, index
  reset_regs(); C->rdi = vlist; CALL(shim__ZNK3sce4Json5Value8getArrayEv); uint64_t la = C->rax;
  reset_regs(); C->rdi = it0; C->rsi = la; CALL(shim__ZNK3sce4Json5Array5beginEv);
  reset_regs(); C->rdi = it1; C->rsi = la; CALL(shim__ZNK3sce4Json5Array3endEv);
  int jn = 0; uint64_t jtypes = 0;
  for (;;) {
    reset_regs(); C->rdi = it0; C->rsi = it1; CALL(shim__ZNK3sce4Json5Array8iteratorneERKS2_); if (!C->rax || jn > 10) break;
    reset_regs(); C->rdi = it0; CALL(shim__ZNK3sce4Json5Array8iteratordeEv); jtypes = jtypes * 10 + JTYPE(C->rax);
    reset_regs(); C->rdi = it0; CALL(shim__ZN3sce4Json5Array8iteratorppEv); jn++;
  }
  CHECK(jn == 3 && jtypes == 357);
  reset_regs(); C->rdi = vlist; C->rsi = 1; CALL(shim__ZNK3sce4Json5ValueixEm); CHECK(JTYPE(C->rax) == 5);
  // object: iterate pairs (key String at +0, Value at +16), insert through operator[]
  reset_regs(); C->rdi = root; CALL(shim__ZNK3sce4Json5Value9getObjectEv); uint64_t ro = C->rax;
  reset_regs(); C->rdi = it0; C->rsi = ro; CALL(shim__ZNK3sce4Json6Object5beginEv);
  reset_regs(); C->rdi = it1; C->rsi = ro; CALL(shim__ZNK3sce4Json6Object3endEv);
  jn = 0; char keys[64] = "";
  for (;;) {
    reset_regs(); C->rdi = it0; C->rsi = it1; CALL(shim__ZNK3sce4Json6Object8iteratorneERKS2_); if (!C->rax || jn > 10) break;
    reset_regs(); C->rdi = it0; CALL(shim__ZNK3sce4Json6Object8iteratordeEv); uint64_t pr = C->rax;
    reset_regs(); C->rdi = pr; CALL(shim__ZNK3sce4Json6String5c_strEv); strcat(keys, (char *)H(C->rax)); strcat(keys, ",");
    if (jn == 1) CHECK(JTYPE(pr + 16) == 2);
    reset_regs(); C->rdi = it0; CALL(shim__ZN3sce4Json6Object8iteratorppEv); jn++;
  }
  CHECK(jn == 6 && !strcmp(keys, "name,n,u,f,ok,list,"));
  uint64_t obj2 = galloc(8), v2 = galloc(32), sout = galloc(8);
  reset_regs(); C->rdi = obj2; CALL(shim__ZN3sce4Json6ObjectC1Ev);
  reset_regs(); C->rdi = obj2; C->rsi = js; CALL(shim__ZN3sce4Json6ObjectixERKNS0_6StringE); uint64_t slot = C->rax;
  reset_regs(); C->rdi = slot; C->rsi = 1; CALL(shim__ZN3sce4Json5Value3setEb);
  reset_regs(); C->rdi = v2; C->rsi = obj2; CALL(shim__ZN3sce4Json5ValueC1ERKNS0_6ObjectE);
  reset_regs(); C->rdi = sout; CALL(shim__ZN3sce4Json6StringC1Ev);
  reset_regs(); C->rdi = v2; C->rsi = sout; CALL(shim__ZN3sce4Json5Value9serializeERNS0_6StringE); CHECK(RAX32 == 0);
  reset_regs(); C->rdi = sout; CALL(shim__ZNK3sce4Json6String5c_strEv); CHECK(!strcmp((char *)H(C->rax), "{\"list\":true}"));
  reset_regs(); C->rdi = root; C->rsi = gstr("{\"a\":"); C->rdx = 5; CALL(shim__ZN3sce4Json6Parser5parseERNS0_5ValueEPKcm); CHECK(RAX32 == (int32_t)0x80848101);
  // referValue(String) is a lookup: a pointer to the value of a present key, null for a missing key or a
  // non-object, and it never inserts (the object keeps its count and serialises as before) or retypes.
  {
    const char *d2 = "{\"name\":\"default\",\"isUsed\":true,\"n\":null}";
    uint64_t r2 = galloc(32), k1 = galloc(8), k2 = galloc(8), k3 = galloc(8), so2 = galloc(8), sc = galloc(32), nu = galloc(32);
    reset_regs(); C->rdi = r2; CALL(shim__ZN3sce4Json5ValueC1Ev);
    reset_regs(); C->rdi = r2; C->rsi = gstr(d2); C->rdx = strlen(d2); CALL(shim__ZN3sce4Json6Parser5parseERNS0_5ValueEPKcm); CHECK(RAX32 == 0);
    reset_regs(); C->rdi = k1; C->rsi = gstr("isUsed"); CALL(shim__ZN3sce4Json6StringC1EPKc);
    reset_regs(); C->rdi = k2; C->rsi = gstr("applyToAll"); CALL(shim__ZN3sce4Json6StringC1EPKc);
    reset_regs(); C->rdi = k3; C->rsi = gstr("n"); CALL(shim__ZN3sce4Json6StringC1EPKc);
    reset_regs(); C->rdi = r2; C->rsi = k1; CALL(shim__ZN3sce4Json5Value10referValueERKNS0_6StringE); uint64_t pv = C->rax;
    CHECK(pv != 0 && JTYPE(pv) == 1);
    reset_regs(); C->rdi = pv; CALL(shim__ZNK3sce4Json5Value10getBooleanEv); CHECK(*(uint8_t *)H(C->rax) == 1);
    for (int rep = 0; rep < 3; rep++) { // a missing key stays missing however often it is asked for
      reset_regs(); C->rdi = r2; C->rsi = k2; CALL(shim__ZN3sce4Json5Value10referValueERKNS0_6StringE); CHECK(C->rax == 0);
    }
    reset_regs(); C->rdi = r2; C->rsi = k3; CALL(shim__ZN3sce4Json5Value10referValueERKNS0_6StringE); CHECK(C->rax != 0 && JTYPE(C->rax) == 0); // a present null is a value
    reset_regs(); C->rdi = r2; CALL(shim__ZNK3sce4Json5Value5countEv); CHECK(C->rax == 3);
    reset_regs(); C->rdi = so2; CALL(shim__ZN3sce4Json6StringC1Ev);
    reset_regs(); C->rdi = r2; C->rsi = so2; CALL(shim__ZN3sce4Json5Value9serializeERNS0_6StringE);
    reset_regs(); C->rdi = so2; CALL(shim__ZNK3sce4Json6String5c_strEv); CHECK(!strcmp((char *)H(C->rax), "{\"name\":\"default\",\"isUsed\":true,\"n\":null}"));
    // not an object: null, and the value keeps its type (a scalar is not turned into an object)
    reset_regs(); C->rdi = sc; C->rsi = 77; CALL(shim__ZN3sce4Json5ValueC1Em);
    reset_regs(); C->rdi = sc; C->rsi = k1; CALL(shim__ZN3sce4Json5Value10referValueERKNS0_6StringE); CHECK(C->rax == 0 && JTYPE(sc) == 3);
    reset_regs(); C->rdi = nu; CALL(shim__ZN3sce4Json5ValueC1Ev);
    reset_regs(); C->rdi = nu; C->rsi = k1; CALL(shim__ZN3sce4Json5Value10referValueERKNS0_6StringE); CHECK(C->rax == 0 && JTYPE(nu) == 0);
    // empty text: parse error, destination untouched; bad text: parse error
    reset_regs(); C->rdi = sc; C->rsi = gstr(""); C->rdx = 0; CALL(shim__ZN3sce4Json6Parser5parseERNS0_5ValueEPKcm); CHECK(RAX32 == (int32_t)0x80848101 && JTYPE(sc) == 3 && *(uint64_t *)H(sc + 16) == 77);
    reset_regs(); C->rdi = nu; C->rsi = gstr("ui_title_03"); C->rdx = 11; CALL(shim__ZN3sce4Json6Parser5parseERNS0_5ValueEPKcm); CHECK(RAX32 == (int32_t)0x80848101 && JTYPE(nu) == 0);
  }
  // offline codes
  reset_regs(); CALL(shim_sceVoiceChatCreateRequest); CHECK(RAX32 == (int32_t)0x80550006);
  reset_regs(); CALL(shim_sceHttp2SendRequest); CHECK(RAX32 == (int32_t)0x80431063);
  uint64_t rid = galloc(4); *(uint32_t *)H(rid) = 5;
  reset_regs(); C->rsi = rid; CALL(shim_sceShareCaptureScreenshot); CHECK(RAX32 == (int32_t)0x81960007 && *(uint32_t *)H(rid) == 0xFFFFFFFFu);
  uint64_t pp = galloc(16), pm = galloc(64), ph = galloc(8); ((uint32_t *)H(pp))[0] = 16; ((uint32_t *)H(pp))[2] = 1920; ((uint32_t *)H(pp))[3] = 4;
  reset_regs(); C->rdi = pp; CALL(shim_scePngEncQueryMemorySize); int need = RAX32; CHECK(need > 285808);
  reset_regs(); C->rdi = pp; C->rsi = pm; C->rdx = need - 1; C->rcx = ph; CALL(shim_scePngEncCreate); CHECK(RAX32 == (int32_t)0x80690102);
  reset_regs(); C->rdi = pp; C->rsi = pm; C->rdx = need; C->rcx = ph; CALL(shim_scePngEncCreate); CHECK(RAX32 == 0 && *(uint64_t *)H(ph) == pm);
  uint64_t flag = galloc(4); *(uint32_t *)H(flag) = 0x01010101;
  reset_regs(); C->rdi = flag; CALL(shim_sceSystemServiceGetNoticeScreenSkipFlag); CHECK(RAX32 == 0 && H(flag)[0] == 0);
  uint64_t spk = galloc(80);
  reset_regs(); C->rdi = spk; CALL(shim_sceAudioOut2GetSpeakerInfo); CHECK(RAX32 == 0 && *(uint32_t *)H(spk + 4) == 3 && *(int16_t *)H(spk + 16) == 30);
  uint64_t ts = galloc(16); ((int64_t *)H(ts))[0] = 0; ((int64_t *)H(ts))[1] = 1000000;
  reset_regs(); C->rdi = ts; CALL(shim_sceKernelNanosleep); CHECK(RAX32 == 0);
  // timed rwlock: try-rdlock reports busy (stub), so the timed call must time out; wrlock succeeds at once
  reset_regs(); C->rsi = 2000; CALL(shim_scePthreadRwlockTimedrdlock); CHECK(RAX32 == (int32_t)0x8002003C);
  reset_regs(); C->rsi = 2000; CALL(shim_scePthreadRwlockTimedwrlock); CHECK(RAX32 == 0);
  printf("astro stubs ok\n");
}

// ------------------------------------------------------------------------------ AudioOut2 (child)

static int audio_child(const char *wav) {
  setenv("NPS5_AUDIO_NULL", "1", 1);
  setenv("PS4_AUDIO_DUMP", wav, 1);
  uint64_t cpar = galloc(64), qsz = galloc(8), hctx = galloc(8), hport = galloc(8), hport2 = galloc(8);
  reset_regs(); C->rdi = cpar; CALL(shim_sceAudioOut2ContextResetParam); if (RAX32) return 10;
  uint32_t *cp = (uint32_t *)H(cpar);
  if (cp[0] != 8 || cp[3] != 1 || cp[4] != 256) return 11;
  cp[3] = 2; /* queue depth */
  reset_regs(); C->rdi = cpar; C->rsi = qsz; CALL(shim_sceAudioOut2ContextQueryMemory);
  if (RAX32 || *(uint64_t *)H(qsz) != 61196u + 2912u * 8u) return 12;
  reset_regs(); CALL(shim_sceAudioOut2Initialize); if (RAX32) return 13;
  uint64_t huser = galloc(8);
  reset_regs(); C->rdi = 0x10000000; C->rsi = huser; CALL(shim_sceAudioOut2UserCreate); if (RAX32 || !*(uint64_t *)H(huser)) return 14;
  reset_regs(); C->rdi = cpar; C->rsi = 0; C->rdx = 0; C->rcx = hctx; CALL(shim_sceAudioOut2ContextCreate); if (RAX32) return 15;
  uint64_t ctxh = *(uint64_t *)H(hctx);

  // port A: stereo float sine; port B: 7.1 s16 with only front-left set
  uint64_t pa = galloc(64), pb = galloc(64);
  memset(H(pa), 0, 64); *(uint16_t *)H(pa) = 0; *(uint32_t *)H(pa + 4) = (2u << 8) | 0; *(uint32_t *)H(pa + 8) = 48000;
  *(uint64_t *)H(pa + 16) = *(uint64_t *)H(huser);
  memset(H(pb), 0, 64); *(uint32_t *)H(pb + 4) = (8u << 8) | 1; *(uint32_t *)H(pb + 8) = 48000;
  reset_regs(); C->rdi = ctxh; C->rsi = pa; C->rdx = hport; CALL(shim_sceAudioOut2PortCreate); if (RAX32) return 16;
  reset_regs(); C->rdi = ctxh; C->rsi = pb; C->rdx = hport2; CALL(shim_sceAudioOut2PortCreate); if (RAX32) return 17;
  uint64_t port = *(uint64_t *)H(hport), port2 = *(uint64_t *)H(hport2);
  uint64_t st = galloc(64);
  reset_regs(); C->rdi = port2; C->rsi = st; CALL(shim_sceAudioOut2PortGetState); if (RAX32 || H(st)[2] != 8) return 18;
  uint64_t bad = galloc(64); memset(H(bad), 0, 64); *(uint32_t *)H(bad + 4) = (6u << 8); // 6ch float on a main port
  uint64_t hbad = galloc(8);
  reset_regs(); C->rdi = ctxh; C->rsi = bad; C->rdx = hbad; CALL(shim_sceAudioOut2PortCreate);
  if (RAX32 != (int32_t)0x8026800E) return 19;

  const uint32_t grain = 256, grains = 120; // 120 * 5.33 ms = 0.64 s
  uint64_t bufA = galloc(grain * 2 * 4), bufB = galloc(grain * 8 * 2);
  uint64_t cellA = galloc(8), cellB = galloc(8), attrs = galloc(48);
  *(uint64_t *)H(cellA) = bufA; *(uint64_t *)H(cellB) = bufB;
  struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
  uint64_t phase = 0;
  for (uint32_t g = 0; g < grains; g++) {
    float *fa = (float *)H(bufA); int16_t *sb = (int16_t *)H(bufB);
    for (uint32_t i = 0; i < grain; i++, phase++) {
      float v = 0.8f * sinf(2.0f * (float)M_PI * 440.0f * (float)(phase % 48000) / 48000.0f);
      fa[i * 2] = v; fa[i * 2 + 1] = v;
      for (int c = 0; c < 8; c++) sb[i * 8 + c] = 0;
    }
    memset(H(attrs), 0, 48);
    *(uint32_t *)H(attrs) = 0; *(uint64_t *)H(attrs + 8) = cellA; *(uint64_t *)H(attrs + 16) = 8;
    reset_regs(); C->rdi = port; C->rsi = attrs; C->rdx = 1; CALL(shim_sceAudioOut2PortSetAttributes); if (RAX32) return 20;
    *(uint32_t *)H(attrs + 24) = 0; *(uint64_t *)H(attrs + 32) = cellB; *(uint64_t *)H(attrs + 40) = 8;
    reset_regs(); C->rdi = port2; C->rsi = attrs + 24; C->rdx = 1; CALL(shim_sceAudioOut2PortSetAttributes); if (RAX32) return 21;
    reset_regs(); C->rdi = ctxh; C->rsi = 1; CALL(shim_sceAudioOut2ContextPush); if (RAX32) return 22;
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  double el = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
  // blocking output is paced by the device: 120 grains cannot finish much faster than their duration
  // minus the queued slack (ring of 2 grains + 1024 frames ~ 30 ms)
  if (el < 0.64 - 0.08) { fprintf(stderr, "push did not block: %.3f s\n", el); return 30; }
  if (el > 3.0) { fprintf(stderr, "push too slow: %.3f s\n", el); return 31; }

  // non-blocking pushes fill the queue and are then refused
  int refused = 0;
  for (int i = 0; i < 64 && !refused; i++) {
    reset_regs(); C->rdi = ctxh; C->rsi = 0; CALL(shim_sceAudioOut2ContextPush);
    if (RAX32 == (int32_t)0x80260507) refused = 1;
  }
  if (!refused) return 32;
  reset_regs(); C->rdi = ctxh; C->rsi = 1; CALL(shim_sceAudioOut2ContextAdvance); if (RAX32) return 33;
  reset_regs(); C->rdi = ctxh; C->rsi = 1; CALL(shim_sceAudioOut2ContextPush); if (RAX32) return 34;
  reset_regs(); C->rdi = 9999; CALL(shim_sceAudioOut2ContextDestroy); if (RAX32 != (int32_t)0x80260503) return 35;
  reset_regs(); C->rdi = port; CALL(shim_sceAudioOut2PortDestroy); if (RAX32) return 36;
  reset_regs(); C->rdi = port2; CALL(shim_sceAudioOut2PortDestroy); if (RAX32) return 37;
  reset_regs(); C->rdi = ctxh; CALL(shim_sceAudioOut2ContextDestroy); if (RAX32) return 38;
  reset_regs(); C->rdi = *(uint64_t *)H(huser); CALL(shim_sceAudioOut2UserDestroy); if (RAX32) return 39;
  return 0; // exit() finalizes the WAV header
}

static int check_wav(const char *wav) {
  FILE *f = fopen(wav, "rb");
  if (!f) { printf("FAIL no wav %s\n", wav); return 1; }
  uint8_t h[44];
  if (fread(h, 1, 44, f) != 44 || memcmp(h, "RIFF", 4) || memcmp(h + 36, "data", 4)) { printf("FAIL wav header\n"); fclose(f); return 1; }
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 44, SEEK_SET);
  size_t frames = (size_t)(sz - 44) / 8;
  float *s = malloc(frames * 8);
  size_t got = fread(s, 8, frames, f); fclose(f);
  // find the loudest 4800-frame window; expect the 440 Hz tone at amplitude 0.8 * 0.5 (master gain)
  double bestrms = 0; size_t best = 0;
  for (size_t i = 0; i + 4800 <= got; i += 2400) {
    double e = 0; for (size_t k = 0; k < 4800; k++) e += (double)s[(i + k) * 2] * s[(i + k) * 2];
    if (e > bestrms) { bestrms = e; best = i; }
  }
  double rms = sqrt(bestrms / 4800.0);
  int cross = 0; float prev = s[best * 2]; double peak = 0;
  for (size_t k = 1; k < 4800; k++) {
    float v = s[(best + k) * 2];
    if (prev < 0 && v >= 0) cross++;
    prev = v; if (fabs(v) > peak) peak = fabs(v);
  }
  double freq = cross / (4800.0 / 48000.0);
  printf("audio: %zu frames dumped, tone rms %.3f peak %.3f, ~%.0f Hz\n", got, rms, peak, freq);
  CHECK(got > 48000 / 2);
  CHECK(peak > 0.35 && peak < 0.45);                 // 0.8 * 0.5 master gain
  CHECK(rms > 0.25 && rms < 0.31);                   // sine: peak / sqrt2
  CHECK(freq > 400 && freq < 480);
  // left == right for the stereo sine, and the quiet 7.1 s16 port adds nothing
  CHECK(fabsf(s[best * 2] - s[best * 2 + 1]) < 1e-4f);
  free(s);
  return 0;
}

// ---------------------------------------------------------------------------------- small calls

static void test_misc(void) {
  reset_regs(); CALL(shim_sceKernelIsTrinityMode); CHECK(RAX32 == 0);
  reset_regs(); C->rdi = 0x1000; C->rsi = 16; CALL(shim_pthread_attr_setsolosched_np); CHECK(RAX32 == 0);
  reset_regs(); C->rdi = 0x1000; C->rsi = 3; CALL(shim_pthread_attr_setsolosched_np); CHECK(RAX32 == 45);
  reset_regs(); C->rdi = 0; C->rsi = 0; CALL(shim_pthread_attr_setsolosched_np); CHECK(RAX32 == 22);
  reset_regs(); C->rdi = 2; C->rsi = 0x200000; C->rdx = 0x4000; CALL(shim_sceKernelSetPrtAperture); CHECK(RAX32 == 0);
  reset_regs(); C->rdi = 3; CALL(shim_sceKernelSetPrtAperture); CHECK(RAX32 == (int32_t)0x80020016);
  reset_regs(); C->rdi = 1; C->rsi = 0; CALL(shim_scePadSetVibrationMode); CHECK(RAX32 == (int32_t)0x80920001);
  reset_regs(); C->rdi = 1; C->rsi = 9; CALL(shim_scePadSetVibrationMode); CHECK(RAX32 == (int32_t)0x80920001);
  printf("misc: ok\n");
}

// External (AnyPS5) equeue events: delete frees the slot, and overflow is reported (-1), never silently dropped.
extern int nps5_ampr_note_external_event(int64_t eq64, uint64_t ident, uint64_t udata);
extern void nps5_ampr_remove_external_event(int64_t eq64, uint64_t ident);
static void test_ampr_external_events(void) {
  int ok = 0, refused = 0;
  for (int i = 0; i < 300; i++) { if (nps5_ampr_note_external_event(0x100000001LL, (uint64_t)i, 7) == 0) ok++; else refused++; }
  CHECK(ok > 64 && refused > 0 && ok + refused == 300); // more than the old 64 fit; the rest is reported, not dropped
  CHECK(nps5_ampr_note_external_event(0x100000001LL, 299, 8) != 0 || ok == 300);
  // delete frees a slot again
  nps5_ampr_remove_external_event(0x100000001LL, 0);
  CHECK(nps5_ampr_note_external_event(0x100000001LL, 1000, 1) == 0);
  CHECK(nps5_ampr_note_external_event(0x100000001LL, 1001, 1) != 0);
  // re-registering an existing (eq, ident) updates in place and never needs a slot
  CHECK(nps5_ampr_note_external_event(0x100000001LL, 1000, 2) == 0);
  for (int i = 0; i < 1100; i++) nps5_ampr_remove_external_event(0x100000001LL, (uint64_t)i);
  CHECK(nps5_ampr_note_external_event(0x100000001LL, 5, 1) == 0);
  nps5_ampr_remove_external_event(0x100000001LL, 5);
  printf("hlesys: external AMPR events ok (%d registered, %d refused)\n", ok, refused);
}


#include <pthread.h>
static void *lvl_thr(void *a) { (void)a; char b[128]; for (int i = 0; i < 2000; i++) { snprintf(b, sizeof b, "/app0/data/prein/Levels/Lv%d/x.bin", i % 20); ps4_vfs_note_level_path(b); } return NULL; }
#include "nps5_framerate_cfg.h"
static void test_framerate_cfg(void) {
  int al = -1;
  CHECK(nps5_framerate_cfg(NULL, NULL, &al) == 0 && al == 0);
  CHECK(nps5_framerate_cfg("93d4040", NULL, &al) == 0x93d4040ull && al == 0);
  CHECK(nps5_framerate_cfg("0x1000", "1", &al) == 0x1000ull && al == 0);        // generic wins over the alias
  CHECK(nps5_framerate_cfg("", "1", &al) == 0x93d4040ull && al == 1);           // alias =1 -> Astro's block
  CHECK(nps5_framerate_cfg(NULL, "2a0", &al) == 0x2a0ull && al == 1);           // alias with an address
  CHECK(nps5_framerate_cfg(NULL, "0", &al) == 0 && al == 0 && nps5_framerate_cfg(NULL, "x", &al) == 0);
  CHECK(nps5_framerate_cfg("zz", "1", &al) == 0x93d4040ull);                    // unparsable generic falls to the alias
  // the fps clamp: 1 <= lo <= hi <= 240 whatever the environment says (the rate is divided into)
  double lo = 0, hi = 0;
  CHECK(nps5_framerate_clamp(NULL, NULL, &lo, &hi) == 1 && lo == 20.0 && hi == 60.0);
  CHECK(nps5_framerate_clamp("30", "120", &lo, &hi) == 1 && lo == 30.0 && hi == 120.0);
  CHECK(nps5_framerate_clamp("1", "240", &lo, &hi) == 1 && lo == 1.0 && hi == 240.0);
  CHECK(nps5_framerate_clamp(NULL, "30", &lo, &hi) == 1 && lo == 20.0 && hi == 30.0);
  static const char *bad[][2] = {{"0", NULL}, {"-5", NULL}, {"abc", NULL}, {"", NULL}, {"nan", NULL}, {"inf", "inf"}, {"61", NULL},
                                 {NULL, "0"}, {NULL, "241"}, {NULL, "19"}, {"60", "30"}, {"30x", "60"}, {"0.5", "60"}, {"1e400", NULL}};
  for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
    lo = hi = -1;
    CHECK(nps5_framerate_clamp(bad[i][0], bad[i][1], &lo, &hi) == 0 && lo == 20.0 && hi == 60.0);
  }
}
// dirfix (podtrace8): the guest's dir reader keeps only DT_REG/DT_DIR; host symlinks (the app0_fast* farm) must report the TARGET type.
static uint8_t dirfix_type(const char *dir, const char *name, int *seen) {
    DIR *d = opendir(dir);
    uint8_t r = 0xEE;
    *seen = 0;
    if (!d) return r;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (!strcmp(e->d_name, name)) {
            *seen = 1;
            r = ps4_vfs_resolve_dirent_type(dirfd(d), e->d_name, e->d_type, NULL);
        }
    closedir(d);
    return r;
}
static void test_dirent_types(const char *tmp) {
    char d[600], p[700], q[700];
    snprintf(d, sizeof d, "%s/dirfix", tmp);
    mkdir(d, 0755);
    snprintf(p, sizeof p, "%s/real.anim", d);
    FILE *f = fopen(p, "w");
    if (f) {
        fputs("x", f);
        fclose(f);
    }
    snprintf(p, sizeof p, "%s/realdir", d);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/real.anim", d);
    snprintf(q, sizeof q, "%s/lnk_file.anim", d);
    CHECK(symlink(p, q) == 0);
    snprintf(p, sizeof p, "%s/realdir", d);
    snprintf(q, sizeof q, "%s/lnk_dir", d);
    CHECK(symlink(p, q) == 0);
    snprintf(q, sizeof q, "%s/lnk_dangling", d);
    CHECK(symlink("/nonexistent/dirfix_target", q) == 0);
    snprintf(q, sizeof q, "%s/lnk_chain", d);
    CHECK(symlink("lnk_file.anim", q) == 0); // relative link to a link
    int seen;
    CHECK(dirfix_type(d, "real.anim", &seen) == DT_REG && seen);
    CHECK(dirfix_type(d, "realdir", &seen) == DT_DIR && seen);
    CHECK(dirfix_type(d, "lnk_file.anim", &seen) == DT_REG && seen);
    CHECK(dirfix_type(d, "lnk_dir", &seen) == DT_DIR && seen);
    CHECK(dirfix_type(d, "lnk_chain", &seen) == DT_REG && seen);
    CHECK(dirfix_type(d, "lnk_dangling", &seen) == 0xFF && seen);
    CHECK(dirfix_type(d, "lnk_file.anim", &seen) == DT_REG); // second pass: served from the per-directory cache
    // DT_UNKNOWN fallback (file systems without d_type): real file / dir / link / missing
    int fd = open(d, O_RDONLY);
    CHECK(ps4_vfs_resolve_dirent_type(fd, "real.anim", DT_UNKNOWN, NULL) == DT_REG);
    CHECK(ps4_vfs_resolve_dirent_type(fd, "realdir", DT_UNKNOWN, NULL) == DT_DIR);
    CHECK(ps4_vfs_resolve_dirent_type(fd, "lnk_dir", DT_UNKNOWN, NULL) == DT_DIR);
    CHECK(ps4_vfs_resolve_dirent_type(fd, "lnk_dangling", DT_UNKNOWN, NULL) == DT_UNKNOWN); // stays unknown, not hidden
    CHECK(ps4_vfs_resolve_dirent_type(fd, "gone", DT_UNKNOWN, NULL) == DT_UNKNOWN);
    CHECK(ps4_vfs_resolve_dirent_type(fd, "real.anim", DT_REG, NULL) == DT_REG); // plain types pass through untouched
    close(fd);
    // cache invalidation: directory changes (new entry) must not serve stale answers
    snprintf(q, sizeof q, "%s/lnk_late", d);
    snprintf(p, sizeof p, "%s/realdir", d);
    CHECK(symlink(p, q) == 0);
    CHECK(dirfix_type(d, "lnk_late", &seen) == DT_DIR && seen);
    unlink(q);
    snprintf(p, sizeof p, "%s/real.anim", d);
    CHECK(symlink(p, q) == 0);
    CHECK(dirfix_type(d, "lnk_late", &seen) == DT_REG && seen);
    // sceKernelStat/stat on a symlink: the VFS stat follows it (target type)
    struct stat st;
    snprintf(q, sizeof q, "%s/lnk_dir", d);
    CHECK(stat(q, &st) == 0 && S_ISDIR(st.st_mode));
}

static void test_level_open(void) {
  test_framerate_cfg();
  CHECK(ps4_vfs_note_level_path("/app0/data/prein/levels/hoover_beach/a.bin") == 1);
  CHECK(ps4_vfs_note_level_path("/app0/data/prein/levels/hoover_beach/b/c.bin") == 0);
  CHECK(ps4_vfs_note_level_path("data/PREIN/LEVELS/Hoover_Beach/a") == 0);       // case-insensitive dedupe
  CHECK(ps4_vfs_note_level_path("/app0/levels/worldmap/x") == 1);
  CHECK(ps4_vfs_note_level_path("/app0/levels/worldmap") == 0);                   // no trailing component
  CHECK(ps4_vfs_note_level_path("/app0/levels/file.bin") == 0);                   // file, not a dir
  CHECK(ps4_vfs_note_level_path("/app0/mylevels/zzz/x") == 0);                    // must be a whole path component
  CHECK(ps4_vfs_note_level_path("/app0/data/other/x") == 0 && ps4_vfs_note_level_path("") == 0 && ps4_vfs_note_level_path(NULL) == 0);
  pthread_t th[4]; for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, lvl_thr, NULL);
  for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
  CHECK(ps4_vfs_note_level_path("/app0/levels/lv7/x") == 0 && ps4_vfs_note_level_path("/app0/levels/lv19/x") == 0);  // each of 20 logged exactly once by the race
  // generic scene key (NPS5_SCENE_OPEN_KEY)
  char k[64]; size_t kl = 0;
  CHECK(ps4_vfs_scene_key_parse(NULL, k, sizeof k, &kl) == 1 && !strcmp(k, "levels/") && kl == 7);
  CHECK(ps4_vfs_scene_key_parse("maps", k, sizeof k, &kl) == 1 && !strcmp(k, "maps/") && kl == 5);
  CHECK(ps4_vfs_scene_key_parse(" Content/Maps/ \n", k, sizeof k, &kl) == 1 && !strcmp(k, "Content/Maps/") && kl == 13);
  CHECK(ps4_vfs_scene_key_parse("off", k, sizeof k, &kl) == 0 && kl == 0);
  CHECK(ps4_vfs_scene_key_parse("", k, sizeof k, &kl) == 0);
  setenv("NPS5_SCENE_OPEN_KEY", "maps", 1); ps4_vfs_scene_reset();
  CHECK(ps4_vfs_note_scene_path("/app0/levels/worldmap/x") == 0);                 // old key no longer matches
  CHECK(ps4_vfs_note_scene_path("/app0/Game/MAPS/Arena01/a.umap") == 1);
  CHECK(ps4_vfs_note_scene_path("/app0/game/maps/arena01/b") == 0);               // deduped, case-insensitive
  CHECK(ps4_vfs_note_scene_path("/app0/mymaps/zzz/x") == 0);
  setenv("NPS5_SCENE_OPEN_KEY", "off", 1); ps4_vfs_scene_reset();
  CHECK(ps4_vfs_note_scene_path("/app0/maps/arena02/a") == 0 && ps4_vfs_note_scene_path("/app0/levels/x/a") == 0);
  unsetenv("NPS5_SCENE_OPEN_KEY"); ps4_vfs_scene_reset();                         // default again, names cleared
  CHECK(ps4_vfs_note_level_path("/app0/levels/lv7/x") == 1);
}

// ---- Apr hardening: record sizes, submit id race, resolve bounds, wait scheduling
static void cb_raw(uint64_t cb, uint64_t base, uint32_t offset, uint32_t size, uint16_t recording) {
  uint8_t *p = H(cb);
  memset(p, 0, 0x18);
  memcpy(p + 2, &recording, 2); memcpy(p + 4, &offset, 4); memcpy(p + 12, &size, 4); memcpy(p + 16, &base, 8);
}
static int submit_raw(uint64_t cb, uint32_t *id) {
  uint64_t idp = galloc(4);
  reset_regs(); C->rdi = cb; C->rdx = idp; CALL(shim_sceKernelAprSubmitCommandBufferAndGetId);
  if (id) *id = *(uint32_t *)H(idp);
  return RAX32;
}
static void wait_id(uint32_t id) { reset_regs(); C->rdi = id; CALL(shim_sceKernelAprWaitCommandBuffer); CHECK(RAX32 == 0); }
static void test_apr_hardening(void) {
  alarm(60); // a regression that blocks the worker must fail the run, not hang it
  uint64_t cb = galloc(0x18), mem = galloc(256), sentinel = galloc(8), stack = galloc(256);
  C->rsp = stack + 64;
  // a record shorter than its opcode reads is refused whole: nothing runs, nothing reaches the guest
  struct { uint32_t op, min; } ops[] = {{2, 24}, {3, 24}, {4, 38}, {5, 38}, {7, 16}, {8, 20}, {9, 24}, {13, 40}, {14, 40}, {15, 40}};
  for (unsigned i = 0; i < sizeof ops / sizeof *ops; i++) {
    memset(H(mem), 0x41, 256);
    *(uint64_t *)H(sentinel) = 0;
    uint32_t sz = ops[i].min - 4;
    memcpy(H(mem), &ops[i].op, 4); memcpy(H(mem) + 4, &sz, 4);
    *(uint64_t *)H(mem + 8) = sentinel; // where a handler reading past the record would store
    *(uint64_t *)H(mem + 16) = 0x1122334455667788ull;
    cb_raw(cb, mem, sz, 256, 0);
    CHECK(submit_raw(cb, NULL) == -1);
    usleep(2000);
    CHECK(*(uint64_t *)H(sentinel) == 0);
  }
  uint32_t fixed[] = {1, 6, 29}; // fixed-size records cut off by the end of the buffer
  for (unsigned i = 0; i < 3; i++) {
    memset(H(mem), 0, 256); memcpy(H(mem), &fixed[i], 4);
    cb_raw(cb, mem, fixed[i] == 29 ? 8 : 20, 256, 0);
    reset_regs(); C->rdi = cb; CALL(shim_sceKernelAprSubmitCommandBuffer); CHECK(RAX32 == -1);
  }
  { // a full-size WRITEADDR still runs
    memset(H(mem), 0, 256);
    uint32_t op = 2, sz = 32; memcpy(H(mem), &op, 4); memcpy(H(mem) + 4, &sz, 4);
    *(uint64_t *)H(mem + 8) = sentinel; *(uint64_t *)H(mem + 16) = 0x1122334455667788ull;
    cb_raw(cb, mem, 32, 256, 0);
    uint32_t id; CHECK(submit_raw(cb, &id) == 0); wait_id(id);
    CHECK(*(uint64_t *)H(sentinel) == 0x1122334455667788ull);
  }
  { // NOPs before, between and after records are skipped (a buffer may end in padding); the record still runs
    memset(H(mem), 0, 256);
    *(uint64_t *)H(sentinel) = 0;
    uint32_t nop3 = 0x5452000fu | 2u << 8, nop1 = 0x5452000fu, op = 2, sz = 32;
    memcpy(H(mem), &nop3, 4);                                 // 12 bytes
    memcpy(H(mem) + 12, &op, 4); memcpy(H(mem) + 16, &sz, 4); // WRITEADDR at 12
    *(uint64_t *)H(mem + 20) = sentinel; *(uint64_t *)H(mem + 28) = 0xa1a2a3a4a5a6a7a8ull;
    memcpy(H(mem) + 44, &nop1, 4);                            // trailing 4-byte NOP
    cb_raw(cb, mem, 48, 256, 0);
    uint32_t id; CHECK(submit_raw(cb, &id) == 0); wait_id(id);
    CHECK(*(uint64_t *)H(sentinel) == 0xa1a2a3a4a5a6a7a8ull);
    // only a NOP
    memcpy(H(mem), &nop1, 4);
    cb_raw(cb, mem, 4, 256, 0);
    CHECK(submit_raw(cb, &id) == 0); wait_id(id);
    // a NOP that says it is longer than what is left ends the buffer: nothing behind it is read
    *(uint64_t *)H(sentinel) = 0;
    uint32_t nop16 = 0x5452000fu | 15u << 8;
    memset(H(mem), 0, 256);
    memcpy(H(mem), &nop16, 4);
    memcpy(H(mem) + 64, &op, 4); memcpy(H(mem) + 68, &sz, 4); // would be the next record if the NOP were believed
    *(uint64_t *)H(mem + 72) = sentinel; *(uint64_t *)H(mem + 80) = 1;
    cb_raw(cb, mem, 8, 256, 0);
    CHECK(submit_raw(cb, &id) == 0); wait_id(id);
    CHECK(*(uint64_t *)H(sentinel) == 0);
  }
  // zero-byte jobs in a tight loop: the id the caller reads back belongs to its own job (the job is freed by the worker)
  cb_raw(cb, 0, 0, 0, 0);
  uint32_t last = 0;
  for (int k = 0; k < 20000; k++) {
    uint32_t id = 0;
    CHECK(submit_raw(cb, &id) == 0);
    CHECK(id == last + 1 || last == 0);
    last = id;
  }
  wait_id(last);
  // a wait with REC_HAS_WAIT clear must not block the FIFO worker
  uint64_t word = galloc(8);
  *(uint64_t *)H(word) = 0;
  memset(H(mem), 0, 256);
  uint32_t op = 4, sz = 48; memcpy(H(mem), &op, 4); memcpy(H(mem) + 4, &sz, 4);
  *(uint64_t *)H(mem + 8) = word; *(uint64_t *)H(mem + 16) = 1; *(uint64_t *)H(mem + 24) = ~0ull; // wait until *word == 1
  cb_raw(cb, mem, 48, 256, 0);
  uint32_t wid, zid;
  CHECK(submit_raw(cb, &wid) == 0);
  cb_raw(cb, 0, 0, 0, 0);
  CHECK(submit_raw(cb, &zid) == 0);
  wait_id(zid); // would hang behind the wait if it ran on the worker
  __atomic_store_n((uint64_t *)H(word), 1, __ATOMIC_RELEASE);
  wait_id(wid);
  // resolve: oversized counts and arrays outside guest memory are refused
  uint64_t paths = galloc(16), ids = galloc(16), szs = galloc(16), errp = galloc(4), res = galloc(16);
  *(uint64_t *)H(paths) = gstr("/apprt2/pre_a.bin"); *(uint64_t *)H(paths + 8) = *(uint64_t *)H(paths);
  uint32_t bad_n[] = {(1u << 20) + 1, 0x10000000u, 0xffffffffu};
  for (unsigned i = 0; i < 3; i++) {
    reset_regs(); C->rdi = paths; C->rsi = bad_n[i]; C->rdx = ids; C->rcx = errp; CALL(shim_sceKernelAprResolveFilepathsToIds); CHECK(RAX32 == -1);
    reset_regs(); C->rdi = paths; C->rsi = bad_n[i]; C->rdx = ids; C->rcx = szs; C->r8 = errp; CALL(shim_sceKernelAprResolveFilepathsToIdsAndFileSizes); CHECK(RAX32 == -1);
    reset_regs(); C->rdi = paths; C->rsi = bad_n[i]; C->rdx = ids; C->rcx = res; CALL(shim_sceKernelAprResolveFilepathsToIdsForEach); CHECK(RAX32 == -1);
  }
  reset_regs(); C->rdi = paths; C->rsi = 2; C->rdx = ARENA - 4; C->rcx = errp; CALL(shim_sceKernelAprResolveFilepathsToIds); CHECK(RAX32 == -1);
  reset_regs(); C->rdi = ARENA - 8; C->rsi = 2; C->rdx = ids; C->rcx = errp; CALL(shim_sceKernelAprResolveFilepathsToIds); CHECK(RAX32 == -1);
  reset_regs(); C->rdi = paths; C->rsi = 2; C->rdx = ids; C->rcx = errp; CALL(shim_sceKernelAprResolveFilepathsToIds); CHECK(RAX32 == 0);
  alarm(0);
  printf("apr: hardening ok\n");
}

int main(void) {
  char tmp[] = "/tmp/hlesys_unit.XXXXXX";
  if (!mkdtemp(tmp)) return 2;
  C = calloc(1, sizeof(GuestContext));
  C->mem_size = ARENA;
  C->mem_base = calloc(1, ARENA);
  C->fs_base = 0x2000;
  C->rsp = 0x6000;

  // AudioOut2 first, in a child, before this process has any threads (fork-safe): the child's
  // exit finalizes the WAV dump that the parent then inspects.
  char wav[700]; snprintf(wav, sizeof wav, "%s/out.wav", tmp);
  pid_t pid = fork();
  if (pid == 0) { int rc = audio_child(wav); fflush(NULL); exit(rc); }
  int status = 0; waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    printf("FAIL audio child status %d (exit %d)\n", status, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    fails++;
  } else {
    check_wav(wav);
  }

  test_userdata_roots(tmp);
  char appdir[700]; snprintf(appdir, sizeof appdir, "%s/app", tmp); mkdir(appdir, 0755); // the application root: the save/data roots below tmp must lie outside it (write gate)
  ps4_vfs_init(appdir);
  test_app0_readonly(appdir);
  test_savedata(tmp);
  test_savedata_modes();
  test_savedata_transfer(tmp);
  test_apr(tmp);
  test_apr_more(tmp);
  test_apr_hardening();
  test_astro_stubs();
  test_misc();
  test_level_open();
  test_dirent_types(tmp);

  char cmd[700]; snprintf(cmd, sizeof cmd, "rm -rf %s", tmp); system(cmd);
  test_ampr_external_events();
  printf(fails ? "hlesys_unit: %d FAILURES\n" : "hlesys_unit: all passed\n", fails);
  return fails ? 1 : 0;
}
