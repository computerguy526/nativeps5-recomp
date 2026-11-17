/* Recursion depth: deep linear recursion (stack growth), mutual recursion, Ackermann,
 * fib (call/return heavy), recursion through function pointers, and alloca-style frames. */
#include "ps5test.h"

static T_NOINLINE uint64_t depth_sum(uint32_t n, volatile uint64_t *probe) {
  volatile uint64_t pad[2]; pad[0] = n; pad[1] = n * 3;
  if (n == 0) return 0;
  uint64_t r = depth_sum(n - 1, probe);
  *probe += pad[0] ^ pad[1];
  return r + pad[0];
}
static T_NOINLINE uint32_t fib(uint32_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static T_NOINLINE uint32_t ack(uint32_t m, uint32_t n) { return m == 0 ? n + 1 : n == 0 ? ack(m - 1, 1) : ack(m - 1, ack(m, n - 1)); }
static T_NOINLINE int is_even(unsigned n);
static T_NOINLINE int is_odd(unsigned n) { return n == 0 ? 0 : is_even(n - 1); }
static T_NOINLINE int is_even(unsigned n) { return n == 0 ? 1 : is_odd(n - 1); }
typedef uint64_t (*Rec)(uint64_t, Rec);
static uint64_t rec_fp(uint64_t n, Rec self) { return n ? n + self(n - 1, self) : 0; }
static T_NOINLINE uint64_t varframe(unsigned n) {
  if (n == 0) return 1;
  char buf[(n & 15) * 8 + 8]; /* VLA: dynamic frame sizes */
  for (unsigned i = 0; i < sizeof buf; i++) buf[i] = (char)(i * n);
  return varframe(n - 1) * 31 + t_fnv(T_FNV0, buf, sizeof buf);
}
int main(void) {
  volatile uint64_t probe = 0;
  t_dec("depth_sum_20000", (long long)depth_sum(20000, &probe)); t_hex("probe", probe);
  t_dec("fib_27", fib(27));
  t_dec("ack_2_300", ack(2, 300)); t_dec("ack_3_5", ack(3, 5));
  t_dec("even_5000", is_even(5000)); t_dec("odd_4999", is_odd(4999));
  t_dec("rec_fp_3000", (long long)rec_fp(3000, rec_fp));
  t_hex("varframe_300", varframe(300));
  return 0;
}
