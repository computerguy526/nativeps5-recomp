/* §8.2 broken #1: bad import.  sceNps5NoSuchFunction is not exported by any library;
 * the link uses --unresolved-symbols=ignore-all so it becomes an unresolved dynamic import.
 * Expected diagnosis: imports.json marks it missing; crash report names bad_import_caller. */
#include "ps5test.h"
extern int sceNps5NoSuchFunction(int, int);
static T_NOINLINE int bad_import_caller(int a) { return sceNps5NoSuchFunction(a, a + 1) + 1; }
int main(void) {
  t_str("broken_import: before\n");
  volatile int a = 7;
  int r = bad_import_caller(a);
  t_dec("broken_import: after (unreachable)", r);
  return 0;
}
