// §8.2 broken #4: uncaught C++ exception.
// Expected diagnosis: std::terminate/abort, crash report names uncaught_thrower in the unwound chain.
#include "ps5test.h"
#include <stdexcept>
static T_NOINLINE int uncaught_thrower(int v) {
  if (v > 0) throw std::runtime_error("uncaught-m1");
  return v;
}
int main() {
  t_str("broken_exception: before\n");
  volatile int v = 3;
  int r = uncaught_thrower(v);
  t_dec("broken_exception: after (unreachable)", r);
  return 0;
}
