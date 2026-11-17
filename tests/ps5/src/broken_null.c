/* §8.2 broken #3: null dereference in a named function.
 * Expected diagnosis: SIGSEGV at null_deref_site, fault address 0 classified as unmapped. */
#include "ps5test.h"
static T_NOINLINE int null_deref_site(volatile int *p) { *p = 0x1234; return *p; }
int main(void) {
  t_str("broken_null: before\n");
  volatile int *volatile p = (int *)0;
  int r = null_deref_site(p);
  t_dec("broken_null: after (unreachable)", r);
  return 0;
}
