/* Tier-1 failure paths: the guest transfers control to an address no translation can serve.
 *   MODE 1: a data address (writable segment)      -> outside_guest_code
 *   MODE 2: executable bytes that do not decode    -> undecodable
 *   MODE 3: the null pointer                       -> outside_guest_code
 * Each must end in crash.txt with the tier1_* fields, never silently. */
#include "../src/ps5test.h"
#ifndef MODE
#define MODE 1
#endif
volatile char data_buf[64] = {1};
__asm__(".pushsection bad_text,\"ax\",@progbits\n.balign 16\nbad_fn:\n.byte 0x06,0x06,0x06,0x06\n.popsection\n"
        ".pushsection .rodata.bad_offs,\"a\",@progbits\n.balign 4\nbad_offs:\n.long bad_fn - bad_offs\n.popsection\n");
extern const int32_t bad_offs[1] __attribute__((visibility("hidden")));
int main(void) {
  t_str("before\n");
  volatile int sel = 0;
  void (*volatile f)(void) = 0; /* volatile: calling a constant null is UB the compiler turns into a trap */
#if MODE == 1
  f = (void (*)(void))(uintptr_t)(data_buf + sel);
#elif MODE == 2
  f = (void (*)(void))((uintptr_t)bad_offs + (intptr_t)bad_offs[sel]);
#endif
  f();
  t_str("after\n");
  return 0;
}
