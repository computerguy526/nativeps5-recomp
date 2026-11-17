/* Extra broken program (beyond the §8.2 four): longjmp to a setjmp whose function already returned.
 * Expected diagnosis: the runtime refuses the jump (target frame is dead) and names the setjmp site;
 * real libc would resume garbage. */
#include "ps5test.h"
#include <setjmp.h>
static jmp_buf g_jb;
static T_NOINLINE int stale_setjmp_site(void) { return setjmp(g_jb); }
int main(void) {
  t_str("broken_longjmp: before\n");
  int r = stale_setjmp_site();
  if (r == 0) longjmp(g_jb, 1);
  t_dec("broken_longjmp: after (unreachable)", r);
  return 0;
}
