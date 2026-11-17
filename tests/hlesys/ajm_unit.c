// Host test of the libSceAjm batch builder and worker (m2-hlesys) over the real minimp3 decoder:
// builds a batch with sceAjmBatchJobControlBufferRa + sceAjmBatchJobRunBufferRa (the PS5 buffer
// builders), starts it, waits, and checks the decoded frame through ps4_ajm_instance_stats.
// Built and run by tools/ajm_unit.sh. No guest code runs.
#include "recomp_runtime.h"
#include "nps5_diag.h"
#include "ps4_ajm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t g_nps5_level[NPS5_S_COUNT];
void nps5_log(int s, int l, const char *fmt, ...) { (void)s; (void)l; (void)fmt; }
void nps5_log_compat(int s, int l, const char *tag, const char *fmt, ...) { (void)s; (void)l; (void)tag; (void)fmt; }

#define ARENA (16ULL << 20)
static GuestContext *C;
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define H(a) ((uint8_t *)C->mem_base + (a))
#define CALL(fn) do { uint64_t rsp0 = C->rsp; fn(C); C->rsp = rsp0; } while (0)
static uint64_t bump = 0x100000;
static uint64_t galloc(size_t n) { uint64_t r = bump; bump += (n + 63) & ~63ULL; return r; }
static void regs0(void) { C->rdi = C->rsi = C->rdx = C->rcx = C->r8 = C->r9 = 0; }
// Stack args of a SysV call: [rsp] = return address, [rsp+8..] = 7th argument on.
static void stk(int i, uint64_t v) { *(uint64_t *)H(C->rsp + 8 + 8 * (uint64_t)i) = v; }

int main(void) {
  C = calloc(1, sizeof(GuestContext));
  C->mem_size = ARENA;
  C->mem_base = calloc(1, ARENA);
  C->fs_base = 0x2000;
  C->rsp = 0x6000;

  uint64_t pctx = galloc(8), pinst = galloc(8), pbid = galloc(8), perr = galloc(64);
  regs0(); C->rsi = pctx; CALL(shim_sceAjmInitialize); CHECK((int32_t)C->rax == 0);
  uint32_t cid = *(uint32_t *)H(pctx);
  regs0(); C->rdi = cid; C->rsi = AJM_CODEC_MP3_DEC; CALL(shim_sceAjmModuleRegister); CHECK((int32_t)C->rax == 0);
  regs0(); C->rdi = cid; C->rsi = AJM_CODEC_MP3_DEC; C->rdx = 2ULL << 3 /* stereo, S16 */; C->rcx = pinst;
  CALL(shim_sceAjmInstanceCreate); CHECK((int32_t)C->rax == 0);
  uint32_t inst = *(uint32_t *)H(pinst);

  // One MPEG-1 Layer III frame of silence: 128 kbit/s, 44.1 kHz, no padding -> 417 bytes.
  uint64_t in = galloc(512), out = galloc(8192), sb = galloc(64), csb = galloc(64), batch = galloc(1024);
  memset(H(in), 0, 512);
  H(in)[0] = 0xFF; H(in)[1] = 0xFB; H(in)[2] = 0x90; H(in)[3] = 0x00;

  regs0(); C->rdi = batch; C->rsi = inst; C->rdx = (uint64_t)AJM_CONTROL_INITIALIZE << 13;
  C->rcx = 0; C->r8 = 0; C->r9 = csb; stk(0, 64); stk(1, 0);
  CALL(shim_sceAjmBatchJobControlBufferRa);
  uint64_t cur = C->rax; CHECK(cur > batch);

  regs0(); C->rdi = cur; C->rsi = inst; C->rdx = 0; C->rcx = in; C->r8 = 417; C->r9 = out;
  stk(0, 4608); stk(1, sb); stk(2, 64); stk(3, 0);
  CALL(shim_sceAjmBatchJobRunBufferRa);
  uint64_t end = C->rax; CHECK(end > cur);

  regs0(); C->rdi = cid; C->rsi = batch; C->rdx = end - batch; C->r8 = perr; C->r9 = pbid;
  CALL(shim_sceAjmBatchStartBuffer); CHECK((int32_t)C->rax == 0);
  regs0(); C->rdi = cid; C->rsi = *(uint32_t *)H(pbid); C->rdx = 5000; C->rcx = perr;
  CALL(shim_sceAjmBatchWait); CHECK((int32_t)C->rax == 0);
  CHECK(*(int32_t *)H(perr) == 0);

  PS4AjmInstanceStats st;
  CHECK(ps4_ajm_instance_stats(cid, inst, &st) == 0);
  CHECK(st.jobs == 2);
  CHECK(st.errors == 0);
  CHECK(st.frames_decoded == 1);
  CHECK(st.samples_out == 1152);
  int nz = 0;
  for (int i = 0; i < 4608; i++) nz |= H(out)[i];
  CHECK(nz == 0); // silence in, silence out

  // A batch naming an instance that does not exist reports the failure, not a crash.
  regs0(); C->rdi = cid; C->rsi = 0x7fff; CALL(shim_sceAjmInstanceDestroy);
  ps4_ajm_destroy();
  printf(fails ? "AJM FAIL (%d)\n" : "AJM OK\n", fails);
  return fails != 0;
}
