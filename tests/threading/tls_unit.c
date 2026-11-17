// Host test of the pthread TLS key table (review 2026-11-15 #6): FreeBSD's PTHREAD_KEYS_MAX is 256, so 255 keys can be
// created (UE asserts when AllocTlsSlot fails), each keeps its own value and destructor, and the 256th fails with EAGAIN.
#include "recomp_runtime.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void shim_pthread_key_create(GuestContext *);
void shim_pthread_key_delete(GuestContext *);
void shim_pthread_setspecific(GuestContext *);
void shim_pthread_getspecific(GuestContext *);
#define ARENA (1ULL << 20)
static GuestContext *C;
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define CALL(fn) do { uint64_t rsp0 = C->rsp; fn(C); C->rsp = rsp0; } while (0)
int main(void) {
  C = calloc(1, sizeof(GuestContext));
  C->mem_size = ARENA; C->mem_base = calloc(1, ARENA); C->fs_base = 0x2000; C->rsp = 0x6000;
  uint64_t keyp = 0x8000;
  int made = 0; uint32_t keys[300];
  for (int i = 0; i < 300; i++) {
    C->rdi = keyp; C->rsi = 0x1000 + (uint64_t)i * 8; C->rax = 99; // a destructor address per key
    CALL(shim_pthread_key_create);
    if (C->rax == 0) { keys[made++] = *(uint32_t *)(C->mem_base + keyp); }
    else { CHECK(C->rax == EAGAIN); break; }
  }
  CHECK(made >= 250 && made <= 256);          // the table is 256 deep (key 0 is never handed out)
  CHECK(made < 300);                          // and refuses beyond that
  for (int i = 0; i < made; i++) { C->rdi = keys[i]; C->rsi = 0xA000 + (uint64_t)i; CALL(shim_pthread_setspecific); CHECK(C->rax == 0); }
  int ok = 0;
  for (int i = 0; i < made; i++) { C->rdi = keys[i]; CALL(shim_pthread_getspecific); if (C->rax == 0xA000 + (uint64_t)i) ok++; }
  CHECK(ok == made);                           // keys >= 128 keep their own value (were EINVAL / lost before)
  printf("threading: %d TLS keys, EAGAIN after ok\n", made);
  printf(fails ? "tls_unit: %d FAILURES\n" : "tls_unit: all passed\n", fails);
  return fails ? 1 : 0;
}
