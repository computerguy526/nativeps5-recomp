/* Integer arithmetic and flag consumers: add/sub/adc/sbb/logic/neg/inc/dec at
 * 8/32/64 bit, every flag (CF PF AF ZF SF OF) read through pushfq; lahf; jp/jnp;
 * setcc/cmovcc; DF through std/cld+pushfq.  On x86-64 the hardware result is
 * printed and checked against a portable model; on the host build the model is
 * printed, so one expected file serves both. */
#include "ps5test.h"

#define FMASK 0x8D5u /* CF PF AF ZF SF OF */
typedef struct { uint64_t r; unsigned fl; } Res;
enum { ADD, SUB, ADC, SBB, AND, OR, XOR, NEG, INC, DEC, NOPS };
static const char *opname[NOPS] = {"add","sub","adc","sbb","and","or","xor","neg","inc","dec"};

static unsigned pf(uint64_t r) { return (__builtin_popcount((unsigned)(r & 0xff)) & 1) ? 0 : 0x4; }

static Res model(int op, int bits, uint64_t a, uint64_t b, int cin) {
  uint64_t m = bits == 64 ? ~0ULL : ((1ULL << bits) - 1), sb = 1ULL << (bits - 1);
  a &= m; b &= m;
  uint64_t r = 0; int cf = 0, of = 0, af = 0;
  switch (op) {
  case ADD: r = (a + b) & m; cf = r < a; of = ((a ^ r) & (b ^ r) & sb) != 0; af = ((a ^ b ^ r) & 0x10) != 0; break;
  case ADC: r = (a + b + cin) & m; cf = cin ? r <= a : r < a; of = ((a ^ r) & (b ^ r) & sb) != 0; af = ((a ^ b ^ r) & 0x10) != 0; break;
  case SUB: r = (a - b) & m; cf = a < b; of = ((a ^ b) & (a ^ r) & sb) != 0; af = ((a ^ b ^ r) & 0x10) != 0; break;
  case SBB: r = (a - b - cin) & m; cf = cin ? a <= b : a < b; of = ((a ^ b) & (a ^ r) & sb) != 0; af = ((a ^ b ^ r) & 0x10) != 0; break;
  case AND: r = a & b; break;
  case OR: r = a | b; break;
  case XOR: r = a ^ b; break;
  case NEG: r = (0 - a) & m; cf = a != 0; of = a == sb; af = ((a ^ r) & 0x10) != 0; break;
  case INC: r = (a + 1) & m; cf = cin; of = r == sb; af = ((a ^ r) & 0x10) != 0; break;
  case DEC: r = (a - 1) & m; cf = cin; of = a == sb; af = ((a ^ r) & 0x10) != 0; break;
  }
  unsigned fl = (cf ? 1 : 0) | pf(r) | (af ? 0x10 : 0) | ((r & m) == 0 ? 0x40 : 0) | ((r & sb) ? 0x80 : 0) | (of ? 0x800 : 0);
  if (op == AND || op == OR || op == XOR) fl &= ~0x10u; /* AF undefined for logic ops */
  return (Res){r, fl};
}

#ifdef __x86_64__
#define HW(NAME, T, INSN2, INSN1)                                                              \
  static T_NOINLINE Res NAME(int op, uint64_t a64, uint64_t b64, int cin) {                      \
    T a = (T)a64, b = (T)b64; unsigned long f;                                                 \
    switch (op) {                                                                              \
    case ADD: __asm__ volatile("lea -128(%%rsp),%%rsp\n bt $0,%k[c]\n add %[b],%[a]\n pushfq\n pop %[f]\n lea 128(%%rsp),%%rsp" : [a]"+r"(a), [f]"=r"(f) : [b]"r"(b), [c]"r"(cin) : "cc"); break; \
    case ADC: __asm__ volatile("lea -128(%%rsp),%%rsp\n bt $0,%k[c]\n adc %[b],%[a]\n pushfq\n pop %[f]\n lea 128(%%rsp),%%rsp" : [a]"+r"(a), [f]"=r"(f) : [b]"r"(b), [c]"r"(cin) : "cc"); break; \
    case SUB: __asm__ volatile("lea -128(%%rsp),%%rsp\n bt $0,%k[c]\n sub %[b],%[a]\n pushfq\n pop %[f]\n lea 128(%%rsp),%%rsp" : [a]"+r"(a), [f]"=r"(f) : [b]"r"(b), [c]"r"(cin) : "cc"); break; \
    case SBB: __asm__ volatile("lea -128(%%rsp),%%rsp\n bt $0,%k[c]\n sbb %[b],%[a]\n pushfq\n pop %[f]\n lea 128(%%rsp),%%rsp" : [a]"+r"(a), [f]"=r"(f) : [b]"r"(b), [c]"r"(cin) : "cc"); break; \
    case AND: __asm__ volatile("lea -128(%%rsp),%%rsp\n bt $0,%k[c]\n and %[b],%[a]\n pushfq\n pop %[f]\n lea 128(%%rsp),%%rsp" : [a]"+r"(a), [f]"=r"(f) : [b]"r"(b), [c]"r"(cin) : "cc"); break; \
    case OR:  __asm__ volatile("lea -128(%%rsp),%%rsp\n bt $0,%k[c]\n or %[b],%[a]\n pushfq\n pop %[f]\n lea 128(%%rsp),%%rsp" : [a]"+r"(a), [f]"=r"(f) : [b]"r"(b), [c]"r"(cin) : "cc"); break; \
    case XOR: __asm__ volatile("lea -128(%%rsp),%%rsp\n bt $0,%k[c]\n xor %[b],%[a]\n pushfq\n pop %[f]\n lea 128(%%rsp),%%rsp" : [a]"+r"(a), [f]"=r"(f) : [b]"r"(b), [c]"r"(cin) : "cc"); break; \
    case NEG: __asm__ volatile("lea -128(%%rsp),%%rsp\n bt $0,%k[c]\n neg %[a]\n pushfq\n pop %[f]\n lea 128(%%rsp),%%rsp" : [a]"+r"(a), [f]"=r"(f) : [c]"r"(cin) : "cc"); break; \
    case INC: __asm__ volatile("lea -128(%%rsp),%%rsp\n bt $0,%k[c]\n inc %[a]\n pushfq\n pop %[f]\n lea 128(%%rsp),%%rsp" : [a]"+r"(a), [f]"=r"(f) : [c]"r"(cin) : "cc"); break; \
    default:  __asm__ volatile("lea -128(%%rsp),%%rsp\n bt $0,%k[c]\n dec %[a]\n pushfq\n pop %[f]\n lea 128(%%rsp),%%rsp" : [a]"+r"(a), [f]"=r"(f) : [c]"r"(cin) : "cc"); break; \
    }                                                                                          \
    return (Res){(uint64_t)a, (unsigned)f & FMASK};                                            \
  }
HW(hw8, uint8_t, 0, 0)
HW(hw32, uint32_t, 0, 0)
HW(hw64, uint64_t, 0, 0)
static Res hw(int op, int bits, uint64_t a, uint64_t b, int cin) {
  return bits == 8 ? hw8(op, a, b, cin) : bits == 32 ? hw32(op, a, b, cin) : hw64(op, a, b, cin);
}
#else
static Res hw(int op, int bits, uint64_t a, uint64_t b, int cin) { return model(op, bits, a, b, cin); }
#endif

static const uint64_t vecs[] = {0, 1, 2, 0x0f, 0x10, 0x7f, 0x80, 0x81, 0xff, 0x100, 0x7fffffff, 0x80000000ULL,
  0xffffffffULL, 0x7fffffffffffffffULL, 0x8000000000000000ULL, 0xffffffffffffffffULL, 0x123456789abcdef0ULL, 0x0f0f0f0f0f0f0f0fULL};
#define NV (int)(sizeof vecs / sizeof vecs[0])

static T_NOINLINE int parity_branch(uint32_t a, uint32_t b) {
  int r;
#ifdef __x86_64__
  __asm__ volatile("add %[b],%[a]\n jp 1f\n movl $0,%[r]\n jmp 2f\n1: movl $1,%[r]\n2:" : [a]"+r"(a), [r]"=&r"(r) : [b]"r"(b) : "cc");
#else
  r = pf((uint64_t)(a + b)) ? 1 : 0;
#endif
  return r;
}
static T_NOINLINE int nparity_branch(uint32_t a, uint32_t b) {
  int r;
#ifdef __x86_64__
  __asm__ volatile("sub %[b],%[a]\n jnp 1f\n movl $0,%[r]\n jmp 2f\n1: movl $1,%[r]\n2:" : [a]"+r"(a), [r]"=&r"(r) : [b]"r"(b) : "cc");
#else
  r = pf((uint64_t)(a - b)) ? 0 : 1;
#endif
  return r;
}
static T_NOINLINE unsigned lahf_after_add(uint32_t a, uint32_t b) {
  unsigned ah;
#ifdef __x86_64__
  __asm__ volatile("add %[b],%[a]\n lahf\n movzbl %%ah,%[r]" : [a]"+r"(a), [r]"=r"(ah) : [b]"r"(b) : "cc", "ah");
#else
  Res m = model(ADD, 32, a, b, 0);
  ah = (m.fl & 0xD5) | 2; /* SF ZF AF PF CF, bit1 always 1 */
#endif
  return ah;
}
static T_NOINLINE unsigned df_flags(int set) {
  unsigned long f;
#ifdef __x86_64__
  if (set) __asm__ volatile("std\n lea -128(%%rsp),%%rsp\n pushfq\n pop %[f]\n lea 128(%%rsp),%%rsp\n cld" : [f]"=r"(f));
  else __asm__ volatile("cld\n lea -128(%%rsp),%%rsp\n pushfq\n pop %[f]\n lea 128(%%rsp),%%rsp" : [f]"=r"(f));
  return (unsigned)f & 0x400;
#else
  (void)f; return set ? 0x400 : 0;
#endif
}
static T_NOINLINE uint64_t cmov_setcc(uint64_t a, uint64_t b) {
  uint64_t r = (a < b) ? a : b;                 /* cmovb/cmova shapes */
  r += (int64_t)a < (int64_t)b ? 3 : 5;         /* setl/cmovl */
  r ^= (uint64_t)((a & 0xff) == (b & 0xff));    /* sete */
  { int64_t t; r += __builtin_add_overflow((int64_t)a, (int64_t)b, &t) ? 100 : 0; } /* seto */
  return r;
}

int main(void) {
  static const int widths[3] = {8, 32, 64};
  uint64_t h = T_FNV0; int bad = 0;
  for (int wi = 0; wi < 3; wi++) {
    int bits = widths[wi];
    for (int op = 0; op < NOPS; op++) {
      uint64_t oh = T_FNV0;
      for (int i = 0; i < NV; i++) for (int j = 0; j < NV; j++) for (int c = 0; c < 2; c++) {
        Res x = hw(op, bits, vecs[i], vecs[j], c);
        Res m = model(op, bits, vecs[i], vecs[j], c);
        if (x.r != m.r || x.fl != m.fl) {
          if (bad++ < 8) {
            t_str("MISMATCH "); t_str(opname[op]); t_dec(" bits", bits); t_hex(" a", vecs[i]); t_hex(" b", vecs[j]);
            t_dec(" cin", c); t_hex(" hw_r", x.r); t_hex(" hw_fl", x.fl); t_hex(" md_r", m.r); t_hex(" md_fl", m.fl);
          }
        }
        oh = t_fnv(oh, &x, sizeof x);
      }
      t_str(opname[op]); t_dec("/bits", bits); t_hex("  fnv", oh);
      h = t_fnv(h, &oh, 8);
    }
  }
  t_dec("model_mismatches", bad);
  uint64_t ph = T_FNV0;
  for (uint32_t a = 0; a < 512; a++) {
    uint32_t b = a * 2654435761u >> 7;
    ph = t_fnv(ph, &(int){parity_branch(a, b)}, 4);
    ph = t_fnv(ph, &(int){nparity_branch(a, b)}, 4);
    ph = t_fnv(ph, &(unsigned){lahf_after_add(a, b)}, 4);
  }
  t_hex("parity_lahf_fnv", ph);
  t_dec("parity_0x03_0x00", parity_branch(3, 0));
  t_dec("parity_0x01_0x00", parity_branch(1, 0));
  t_hex("lahf_0xff_1", lahf_after_add(0xff, 1));
  t_hex("lahf_0x7fffffff_1", lahf_after_add(0x7fffffff, 1));
  t_hex("df_set", df_flags(1));
  t_hex("df_clr", df_flags(0));
  uint64_t cs = 0;
  for (int i = 0; i < NV; i++) for (int j = 0; j < NV; j++) cs = cs * 31 + cmov_setcc(vecs[i], vecs[j]);
  t_hex("cmov_setcc", cs);
  /* shifts/rotates/mul/div (portable) */
  uint64_t sh = T_FNV0;
  for (int i = 0; i < NV; i++) for (int k = 0; k < 70; k += 3) {
    uint64_t v = vecs[i]; unsigned n = k & 63;
    uint64_t r[7] = {v << n, v >> n, (uint64_t)((int64_t)v >> n), n ? (v << n | v >> (64 - n)) : v, n ? (v >> n | v << (64 - n)) : v,
      (uint64_t)__builtin_clzll(v | 1) + (uint64_t)__builtin_ctzll(v | 1ULL << 63), (uint64_t)__builtin_bswap64(v)};
    sh = t_fnv(sh, r, sizeof r);
  }
  t_hex("shift_rot_fnv", sh);
  uint64_t mh = T_FNV0;
  for (int i = 0; i < NV; i++) for (int j = 1; j < NV; j++) {
    __uint128_t p = (__uint128_t)vecs[i] * vecs[j];
    __int128 sp = (__int128)(int64_t)vecs[i] * (int64_t)vecs[j];
    uint64_t q = vecs[i] / vecs[j], rr = vecs[i] % vecs[j];
    int64_t sq = (int64_t)vecs[j] == -1 ? -(int64_t)vecs[i] : (int64_t)vecs[i] / (int64_t)vecs[j];
    uint64_t r[6] = {(uint64_t)p, (uint64_t)(p >> 64), (uint64_t)sp, (uint64_t)(sp >> 64), q ^ rr, (uint64_t)sq};
    mh = t_fnv(mh, r, sizeof r);
  }
  t_hex("mul_div_fnv", mh);
  t_hex("total", h);
  return bad != 0;
}
