/* §8.2 broken #2: unsupported instruction.  A rare instruction is emitted with .byte so
 * no assembler support is involved: C4 E2 F3 F5 C2 = PDEP rax, rcx, rdx (BMI2).
 * (If the lifter ever gains PDEP, swap the bytes for another rare form, e.g. XTEST 0F 01 D6.)
 * Expected diagnosis: census.json lists the stub at rare_insn_site; running it traps there. */
#include "ps5test.h"
static T_NOINLINE uint64_t rare_insn_site(uint64_t src, uint64_t mask) {
  uint64_t r = 0;
#ifdef __x86_64__
  __asm__ volatile("mov %[s],%%rcx\n mov %[m],%%rdx\n .byte 0xc4,0xe2,0xf3,0xf5,0xc2\n mov %%rax,%[r]"
                   : [r]"=r"(r) : [s]"r"(src), [m]"r"(mask) : "rax", "rcx", "rdx");
#else
  (void)src; (void)mask;
#endif
  return r;
}
int main(void) {
  t_str("broken_unsupported: before\n");
  volatile uint64_t s = 0x5, m = 0xf0f0;
  uint64_t r = rare_insn_site(s, m);
  t_hex("broken_unsupported: pdep", r);
  return 0;
}
