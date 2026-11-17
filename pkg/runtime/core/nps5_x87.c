// SPDX-License-Identifier: GPL-2.0-or-later
//
// x87 80-bit extended precision on Berkeley SoftFloat 3e (8086-SSE specialization): the helpers
// behind the lifter's x87=f80 mode. See nps5_x87.h for the calling convention and the state in
// GuestContext, and docs/m2/x87-f80.md for what is and is not modelled.
//
// Responses follow the masked-exception behaviour of the x87 (the default control word 0x037f
// masks everything; unmasked exceptions are not delivered, they only set the status word's flags):
// an invalid operation gives the real indefinite (negative quiet NaN, significand 0xC000...) or the
// integer indefinite; a stack overflow or underflow sets IE and SF (C1 distinguishes them) and
// yields the indefinite.

#include "platform.h"
#include "softfloat.h"
#include "internals.h"
#include "specialize.h"

#include "nps5_x87.h"
#include "recomp_context.h"

#include <string.h>

_Static_assert(sizeof(x87_reg_t) == 16, "x87_reg_t is the 16-byte slot");
_Static_assert(sizeof(extFloat80_t) == 16 && offsetof(extFloat80_t, signExp) == 8, "extFloat80_t layout");

typedef extFloat80_t f80;

#define SW_IE 0x0001u
#define SW_ZE 0x0004u
#define SW_SF 0x0040u
#define SW_ES 0x0080u
#define SW_C0 0x0100u
#define SW_C1 0x0200u
#define SW_C2 0x0400u
#define SW_C3 0x4000u
#define SW_CC (SW_C0 | SW_C1 | SW_C2 | SW_C3)

static inline f80 mk(uint64_t m, uint16_t se) {
  f80 r;
  r.signif = m;
  r.signExp = se;
  return r;
}
static const f80 INDEF = {UINT64_C(0xC000000000000000), 0xFFFF};

static inline f80 rd(const x87_reg_t *r) { return mk(r->mant, r->se); }
static inline void wr(x87_reg_t *r, f80 v) {
  r->mant = v.signif;
  r->se = v.signExp;
}

static inline bool is_nan(f80 v) { return (v.signExp & 0x7fff) == 0x7fff && (v.signif << 1) != 0; }
static inline bool is_inf(f80 v) { return (v.signExp & 0x7fff) == 0x7fff && (v.signif << 1) == 0; }
static inline bool is_zero(f80 v) { return (v.signExp & 0x7fff) == 0 && v.signif == 0; }

// ---- control word, exception flags ----

// Sets SoftFloat's rounding mode and the extF80 rounding precision from the control word.
// RC (FCW 11:10) 0 nearest even, 1 down, 2 up, 3 toward zero; PC (FCW 9:8) 0 = 24-bit, 2 = 53-bit,
// 3 = 64-bit significand (1 is reserved and treated as 64).
static inline void env_begin(const GuestContext *c) {
  static const uint8_t rm[4] = {softfloat_round_near_even, softfloat_round_min, softfloat_round_max,
                                softfloat_round_minMag};
  static const uint8_t pc[4] = {32, 80, 64, 80};
  softfloat_roundingMode = rm[(c->fpu_cw >> 10) & 3];
  extF80_roundingPrecision = pc[(c->fpu_cw >> 8) & 3];
  softfloat_exceptionFlags = 0;
}

// SoftFloat flags (inexact 1, underflow 2, overflow 4, infinite = divide by zero 8, invalid 16) to
// the status word (IE 1, DE 2, ZE 4, OE 8, UE 16, PE 32).
static inline unsigned sf2sw(unsigned f) {
  return ((f & softfloat_flag_invalid) ? 0x01u : 0) | ((f & softfloat_flag_infinite) ? 0x04u : 0) |
         ((f & softfloat_flag_overflow) ? 0x08u : 0) | ((f & softfloat_flag_underflow) ? 0x10u : 0) |
         ((f & softfloat_flag_inexact) ? 0x20u : 0);
}
static inline void env_end(GuestContext *c) {
  unsigned f = softfloat_exceptionFlags;
  if (f) {
    c->fpu_sw |= (uint16_t)sf2sw(f);
    softfloat_exceptionFlags = 0;
  }
}

// ---- register stack ----

static inline unsigned phys(const GuestContext *c, unsigned i) { return ((unsigned)c->fpu_top + i) & 7; }
static inline void set_c1(GuestContext *c, int v) {
  c->fpu_sw = (uint16_t)(v ? (c->fpu_sw | SW_C1) : (c->fpu_sw & ~SW_C1));
}

// Reads ST(i). An empty register is a stack underflow: IE and SF, C1 = 0, the indefinite.
static f80 st_get(GuestContext *c, unsigned i) {
  unsigned p = phys(c, i);
  if (!((c->fpu_valid >> p) & 1)) {
    c->fpu_sw = (uint16_t)((c->fpu_sw | SW_IE | SW_SF) & ~SW_C1);
    return INDEF;
  }
  return rd(&c->fpu80[p]);
}
static void st_put(GuestContext *c, unsigned i, f80 v) {
  unsigned p = phys(c, i);
  wr(&c->fpu80[p], v);
  c->fpu_valid |= (uint8_t)(1u << p);
}
static void st_push(GuestContext *c, f80 v) {
  unsigned p = ((unsigned)c->fpu_top - 1) & 7;
  c->fpu_top = (int)p;
  if ((c->fpu_valid >> p) & 1) { // stack overflow: IE, SF, C1 = 1, the indefinite
    c->fpu_sw |= SW_IE | SW_SF | SW_C1;
    v = INDEF;
  }
  wr(&c->fpu80[p], v);
  c->fpu_valid |= (uint8_t)(1u << p);
}
static void st_pop(GuestContext *c) {
  unsigned p = (unsigned)c->fpu_top & 7;
  c->fpu_valid &= (uint8_t)~(1u << p);
  c->fpu_top = (int)((p + 1) & 7);
}

// ---- memory operands (host pointers; may be unaligned) ----

// Integer to extended: exact, so no SoftFloat call (and no state) is needed.
static f80 int_to_f80(int64_t i) {
  if (i == 0) return mk(0, 0);
  uint64_t m = i < 0 ? 0 - (uint64_t)i : (uint64_t)i;
  unsigned z = (unsigned)__builtin_clzll(m);
  return mk(m << z, (uint16_t)((i < 0 ? 0x8000u : 0u) | (0x403eu - z)));
}
// float32/float64 to extended: exact for normal numbers, done inline; zero, denormals, infinities and
// NaNs (the NaN is quieted and IE raised for a signaling one) go through SoftFloat.
static f80 load_f32(uint64_t a) {
  float32_t f;
  memcpy(&f.v, (const void *)a, 4);
  unsigned e = (f.v >> 23) & 0xff;
  if (e != 0 && e != 0xff)
    return mk(UINT64_C(0x8000000000000000) | (uint64_t)(f.v & 0x7fffff) << 40, (uint16_t)((f.v >> 31 << 15) | (e + 0x3f80)));
  return f32_to_extF80(f);
}
static f80 load_f64(uint64_t a) {
  float64_t f;
  memcpy(&f.v, (const void *)a, 8);
  unsigned e = (unsigned)(f.v >> 52) & 0x7ff;
  if (e != 0 && e != 0x7ff)
    return mk(UINT64_C(0x8000000000000000) | f.v << 11, (uint16_t)((f.v >> 63 << 15) | (e + 0x3c00)));
  return f64_to_extF80(f);
}
static f80 load_m80(uint64_t a) {
  f80 r;
  memcpy(&r.signif, (const void *)a, 8);
  memcpy(&r.signExp, (const uint8_t *)a + 8, 2);
  return r;
}
static void store_m80(uint64_t a, f80 v) {
  memcpy((void *)a, &v.signif, 8);
  memcpy((uint8_t *)a + 8, &v.signExp, 2);
}
static inline __attribute__((always_inline)) f80 load_int(unsigned sz, uint64_t a) { // sz 2 = int16, 3 = int32 (arith/compare memory forms)
  if (sz == 2) {
    int16_t v;
    memcpy(&v, (const void *)a, 2);
    return int_to_f80(v);
  }
  int32_t v;
  memcpy(&v, (const void *)a, 4);
  return int_to_f80(v);
}
// The operand of an arithmetic or compare memory form: sz 0 float32, 1 float64, 2 int16, 3 int32.
static f80 load_operand(unsigned sz, uint64_t a) {
  switch (sz) {
  case 0:
    return load_f32(a);
  case 1:
    return load_f64(a);
  default:
    return load_int(sz, a);
  }
}

// ---- arithmetic ----

static f80 arith(unsigned aop, f80 a, f80 b) { // a = destination, b = source
  switch (aop) {
  case X87_ARITH_ADD:
    return extF80_add(a, b);
  case X87_ARITH_SUB:
    return extF80_sub(a, b);
  case X87_ARITH_SUBR:
    return extF80_sub(b, a);
  case X87_ARITH_MUL:
    return extF80_mul(a, b);
  case X87_ARITH_DIV:
    return extF80_div(a, b);
  default:
    return extF80_div(b, a);
  }
}

// Comparison as the status word's condition bits: > 0, < C0, = C3, unordered C3|C2|C0. A NaN
// operand is invalid for the ordered compares (FCOM, FCOMI, FTST) and, for the unordered ones
// (FUCOM, FUCOMI), only when it is signaling.
static unsigned compare(f80 x, f80 y, bool unordered_form) {
  if (is_nan(x) || is_nan(y)) {
    if (!unordered_form || extF80_isSignalingNaN(x) || extF80_isSignalingNaN(y))
      softfloat_raiseFlags(softfloat_flag_invalid);
    return SW_C3 | SW_C2 | SW_C0;
  }
  if (extF80_eq(x, y)) return SW_C3;
  if (extF80_lt(x, y)) return SW_C0;
  return 0;
}

// ---- constants ----

// The five FLD constants: significand rounded down, exponent field, and whether the exact value is
// closer to the next significand up (round to nearest). The hardware rounds them by RC: nearest as
// stated, down and toward zero take the lower, up takes the higher.
static f80 fld_const(unsigned id, unsigned rc) {
  static const struct {
    uint64_t lo;
    uint16_t exp;
    bool near_up;
  } t[] = {
      {UINT64_C(0xD49A784BCD1B8AFE), 0x4000, false}, // log2(10)
      {UINT64_C(0xB8AA3B295C17F0BB), 0x3FFF, true},  // log2(e)
      {UINT64_C(0xC90FDAA22168C234), 0x4000, true},  // pi
      {UINT64_C(0x9A209A84FBCFF798), 0x3FFD, true},  // log10(2)
      {UINT64_C(0xB17217F7D1CF79AB), 0x3FFE, true},  // ln(2)
  };
  if (id == 0) return mk(UINT64_C(0x8000000000000000), 0x3FFF);
  if (id == 6) return mk(0, 0);
  unsigned i = id - 1;
  bool up = rc == 0 ? t[i].near_up : rc == 2;
  return mk(t[i].lo + (up ? 1 : 0), t[i].exp);
}

// ---- FSCALE, FXTRACT, FPREM ----

// ST(0) * 2^trunc(ST(1)).
static f80 do_scale(f80 a, f80 b) {
  if (is_nan(a) || is_nan(b)) return extF80_mul(a, b); // propagates (and raises IE for a signaling NaN)
  uint16_t sign = a.signExp & 0x8000;
  if (is_inf(b)) {
    if (b.signExp & 0x8000) { // a * 2^-inf
      if (is_inf(a)) {
        softfloat_raiseFlags(softfloat_flag_invalid);
        return INDEF;
      }
      return mk(0, sign);
    }
    return extF80_mul(a, b); // 0 * inf is invalid (indefinite), anything else is an infinity
  }
  if (is_inf(a) || is_zero(a)) return a;
  uint_fast8_t keep = softfloat_exceptionFlags;
  int64_t n = extF80_to_i64_r_minMag(b, false);
  softfloat_exceptionFlags = keep; // an out-of-range count only needs clamping
  if (n > 100000 || n == INT64_MIN) n = (b.signExp & 0x8000) ? -100000 : 100000;
  if (n < -100000) n = -100000;
  int32_t ea = a.signExp & 0x7fff;
  if (ea == 0) ea = 1;
  uint_fast8_t pc = extF80_roundingPrecision;
  extF80_roundingPrecision = 80;
  f80 r = softfloat_normRoundPackToExtF80(sign != 0, ea + (int32_t)n, a.signif, 0, 80);
  extF80_roundingPrecision = pc;
  return r;
}

// value r * 2^e2 (exact), r != 0 -> normalized when the exponent allows, denormal otherwise.
static f80 pack_exact(uint16_t sign, uint64_t r, int e2) {
  if (r == 0) return mk(0, sign);
  int sh = __builtin_clzll(r);
  uint64_t m = r << sh;
  int biased = e2 + 63 - sh + 16383;
  if (biased >= 1) return mk(m, (uint16_t)(sign | biased));
  int s = 1 - biased;
  return mk(s >= 64 ? 0 : m >> s, sign);
}

// FPREM (truncating) and FPREM1 (nearest, ties to even). Returns the remainder; *cc gets the
// status word condition bits: C2 for an incomplete reduction, otherwise C0 = Q2, C3 = Q1, C1 = Q0.
static f80 do_prem(f80 a, f80 b, bool nearest, unsigned *cc) {
  *cc = 0;
  if (is_nan(a) || is_nan(b)) return extF80_mul(a, b);
  if (is_inf(a) || is_zero(b)) {
    softfloat_raiseFlags(softfloat_flag_invalid);
    return INDEF;
  }
  if (is_inf(b) || is_zero(a)) return a;
  uint16_t sign = a.signExp & 0x8000;
  int ea = a.signExp & 0x7fff, eb = b.signExp & 0x7fff;
  if (ea == 0) ea = 1;
  if (eb == 0) eb = 1;
  uint64_t ma = a.signif, mb = b.signif;
  int d = ea - eb;
  if (d < 0) {
    // |a| < |b| by exponent: the quotient is 0, except that FPREM1 rounds up past one half.
    if (nearest && d == -1 && ma > mb) {
      uint64_t r = (uint64_t)(((unsigned __int128)mb << 1) - ma); // 2*mb - ma < 2^64
      *cc = SW_C1; // Q0
      return pack_exact(sign ^ 0x8000, r, eb - 16383 - 63 - 1);
    }
    return a;
  }
  int k = d < 64 ? d : 63;
  unsigned __int128 r = ma;
  uint64_t q = 0;
  for (int i = 0;; i++) {
    if (r >= mb) {
      r -= mb;
      q |= 1;
    }
    if (i == k) break;
    r <<= 1;
    q <<= 1;
  }
  if (d >= 64) { // partial remainder: the reduction is not complete, C2 = 1
    *cc = SW_C2;
    return pack_exact(sign, (uint64_t)r, ea - 16383 - 63 - k);
  }
  if (nearest) {
    unsigned __int128 r2 = r << 1;
    if (r2 > mb || (r2 == mb && (q & 1))) {
      r = mb - r;
      q++;
      sign ^= 0x8000;
    }
  }
  *cc = ((q & 4) ? SW_C0 : 0) | ((q & 2) ? SW_C3 : 0) | ((q & 1) ? SW_C1 : 0);
  return pack_exact(sign, (uint64_t)r, eb - 16383 - 63);
}

// FXTRACT: the unbiased exponent and the significand (exponent 0, sign kept).
static void do_xtract(f80 v, f80 *expo, f80 *sig) {
  if (is_nan(v)) {
    *expo = *sig = extF80_add(v, v);
    return;
  }
  if (is_inf(v)) {
    *sig = v;
    *expo = mk(UINT64_C(0x8000000000000000), 0x7FFF);
    return;
  }
  if (is_zero(v)) {
    softfloat_raiseFlags(softfloat_flag_infinite);
    *sig = v;
    *expo = mk(UINT64_C(0x8000000000000000), 0xFFFF);
    return;
  }
  int e = v.signExp & 0x7fff;
  uint64_t m = v.signif;
  if (e == 0) e = 1;
  int sh = __builtin_clzll(m);
  m <<= sh;
  e -= sh;
  *expo = i32_to_extF80(e - 16383);
  *sig = mk(m, (uint16_t)((v.signExp & 0x8000) | 0x3FFF));
}

// ---- integer conversion ----

static void do_fist(GuestContext *c, unsigned sz, bool trunc, uint64_t dst) {
  f80 v = st_get(c, 0);
  uint_fast8_t rm = softfloat_roundingMode;
  switch (sz) {
  case 2: {
    int64_t r = trunc ? extF80_to_i64_r_minMag(v, true) : extF80_to_i64(v, rm, true);
    memcpy((void *)dst, &r, 8);
    break;
  }
  case 1: {
    int32_t r = trunc ? extF80_to_i32_r_minMag(v, true) : extF80_to_i32(v, rm, true);
    memcpy((void *)dst, &r, 4);
    break;
  }
  default: {
    int32_t r = trunc ? extF80_to_i32_r_minMag(v, true) : extF80_to_i32(v, rm, true);
    if (softfloat_exceptionFlags & softfloat_flag_invalid) {
      r = -0x8000;
    } else if (r < -0x8000 || r > 0x7fff) {
      softfloat_exceptionFlags = (softfloat_exceptionFlags & ~softfloat_flag_inexact) | softfloat_flag_invalid;
      r = -0x8000;
    }
    int16_t h = (int16_t)r;
    memcpy((void *)dst, &h, 2);
    break;
  }
  }
}

// ---- tag word, environment, FNSAVE ----

static uint16_t tag_word(const GuestContext *c) {
  uint16_t tw = 0;
  for (unsigned n = 0; n < 8; n++) {
    unsigned t;
    if (!((c->fpu_valid >> n) & 1)) {
      t = 3;
    } else {
      f80 v = rd(&c->fpu80[n]);
      unsigned e = v.signExp & 0x7fff;
      if (e == 0x7fff) t = 2;
      else if (e == 0) t = v.signif == 0 ? 1 : 2;
      else t = (v.signif >> 63) ? 0 : 2;
    }
    tw |= (uint16_t)(t << (2 * n));
  }
  return tw;
}

static uint16_t sw_with_top(const GuestContext *c) {
  return (uint16_t)((c->fpu_sw & ~0x3800u) | (((unsigned)c->fpu_top & 7) << 11));
}

static void put16(uint8_t *p, unsigned v) { memcpy(p, &(uint16_t){(uint16_t)v}, 2); }
static unsigned get16(const uint8_t *p) {
  uint16_t v;
  memcpy(&v, p, 2);
  return v;
}

// The environment image: 28 bytes (32-bit operand size: 4-byte fields CW, SW, TW at 0, 4, 8) or,
// with an operand-size prefix (sz 1), 14 bytes (2-byte fields at 0, 2, 4). The registers of
// FNSAVE/FRSTOR follow it.
static void env_store(const GuestContext *c, uint8_t *p, bool o16) {
  memset(p, 0, o16 ? 14 : 28);
  put16(p + 0, c->fpu_cw);
  put16(p + (o16 ? 2 : 4), sw_with_top(c));
  put16(p + (o16 ? 4 : 8), tag_word(c));
}
static void env_load(GuestContext *c, const uint8_t *p, bool o16) {
  c->fpu_cw = (uint16_t)get16(p + 0);
  unsigned sw = get16(p + (o16 ? 2 : 4));
  c->fpu_top = (int)((sw >> 11) & 7);
  c->fpu_sw = (uint16_t)(sw & ~0x3800u);
  unsigned tw = get16(p + (o16 ? 4 : 8));
  uint8_t valid = 0;
  for (unsigned n = 0; n < 8; n++)
    if (((tw >> (2 * n)) & 3) != 3) valid |= (uint8_t)(1u << n);
  c->fpu_valid = valid;
}

static void fninit(GuestContext *c) {
  c->fpu_cw = 0x037f;
  c->fpu_sw = 0;
  c->fpu_top = 0;
  c->fpu_valid = 0;
}

// ---- packed BCD ----

static f80 bcd_load(const uint8_t *p) {
  int64_t v = 0;
  for (int i = 8; i >= 0; i--) v = v * 100 + (p[i] >> 4) * 10 + (p[i] & 15);
  f80 r = i64_to_extF80(v);
  if (p[9] & 0x80) r.signExp ^= 0x8000;
  return r;
}
static void bcd_store(f80 v, uint8_t *p, uint_fast8_t rm) {
  int64_t n = extF80_to_i64(v, rm, true);
  bool neg = (v.signExp & 0x8000) != 0;
  if ((softfloat_exceptionFlags & softfloat_flag_invalid) || n >= INT64_C(1000000000000000000) ||
      n <= -INT64_C(1000000000000000000)) {
    softfloat_exceptionFlags = (softfloat_exceptionFlags & ~softfloat_flag_inexact) | softfloat_flag_invalid;
    memset(p, 0, 7);
    p[7] = 0xC0;
    p[8] = p[9] = 0xFF;
    return;
  }
  uint64_t u = n < 0 ? (uint64_t)-n : (uint64_t)n;
  for (int i = 0; i < 9; i++) {
    unsigned lo = u % 10, hi = (u / 10) % 10;
    u /= 100;
    p[i] = (uint8_t)(hi << 4 | lo);
  }
  p[9] = neg ? 0x80 : 0;
}

// ---- classification (FXAM) ----

static unsigned xam(const GuestContext *c) {
  const x87_reg_t *r = &c->fpu80[phys(c, 0)];
  unsigned code = (r->se & 0x8000) ? SW_C1 : 0;
  if (!((c->fpu_valid >> phys(c, 0)) & 1)) return code | SW_C3 | SW_C0;
  unsigned e = r->se & 0x7fff;
  bool integer = (r->mant >> 63) != 0;
  if (e == 0x7fff) {
    if (!integer) return code;                  // unsupported
    return code | ((r->mant << 1) ? SW_C0 : SW_C2 | SW_C0);
  }
  if (e == 0) return code | (r->mant == 0 ? SW_C3 : SW_C3 | SW_C2);
  return integer ? code | SW_C2 : code; // unsupported if the integer bit is clear
}


// ---- exact integer fast paths (no SoftFloat, no thread-local state) ----
//
// FADD/FSUB/FSUBR/FMUL and the compares on operands that are zero or normal numbers (exponent field
// 1..0x7ffe, integer bit set) are computed here with 128-bit integer arithmetic and rounded exactly as
// the hardware does it for the control word's PC (24/53/64-bit significand) and RC. Everything else
// (denormal and unnormal operands, infinities, NaNs, empty registers, results whose exponent leaves
// 1..0x7ffe, i.e. overflow and underflow, a zero result of a subtraction of equal values, FDIV, FSQRT)
// returns false before anything is changed and takes the SoftFloat path. Only PE can be raised here.
// tests: TestX87FastArith (>= 1e6 random operand pairs per operation, PC and RC, against SoftFloat).

typedef unsigned __int128 u128;

// 0 = ordinary (normal, integer bit set), 1 = zero, 2 = anything else
static inline __attribute__((always_inline)) int fclass(f80 v) {
  unsigned e = v.signExp & 0x7fff;
  if (e == 0) return v.signif == 0 ? 1 : 2;
  if (e == 0x7fff || !(v.signif >> 63)) return 2;
  return 0;
}

// Rounds the normalized significand m (bit 63 set) with the discarded bits rem (a fraction of the unit
// in m's last place, rem >> 63 = the half bit) to `shift` = 64 - PC bits.
static inline __attribute__((always_inline)) bool fround(uint64_t *m, int *e, uint64_t rem, unsigned shift, unsigned rc, bool neg, bool *inexact) {
  uint64_t lowmask = shift ? (UINT64_C(1) << shift) - 1 : 0;
  bool guard, sticky;
  if (shift == 0) {
    guard = (rem >> 63) != 0;
    sticky = (rem << 1) != 0;
  } else {
    guard = ((*m >> (shift - 1)) & 1) != 0;
    sticky = ((*m & (lowmask >> 1)) | rem) != 0;
  }
  uint64_t v = *m & ~lowmask;
  bool inex = guard || sticky, up;
  switch (rc) {
  case 0:
    up = guard && (sticky || ((*m >> shift) & 1));
    break;
  case 1:
    up = neg && inex;
    break;
  case 2:
    up = !neg && inex;
    break;
  default:
    up = false;
  }
  if (up) {
    v += UINT64_C(1) << shift;
    if (v == 0) {
      v = UINT64_C(0x8000000000000000);
      (*e)++;
    }
  }
  *m = v;
  *inexact = inex;
  return *e >= 1 && *e <= 0x7ffe;
}

static inline __attribute__((always_inline)) unsigned pc_shift(unsigned cw) {
  unsigned pc = (cw >> 8) & 3;
  return pc == 0 ? 40 : pc == 2 ? 11 : 0;
}

// a + b (sub: a - b); false = take the slow path.
static inline __attribute__((always_inline)) bool fast_addsub(f80 a, f80 b, bool sub, unsigned cw, f80 *r, bool *inexact) {
  int ca = fclass(a), cb = fclass(b);
  if (ca == 2 || cb == 2 || (ca == 1 && cb == 1)) return false;
  unsigned rc = (cw >> 10) & 3, shift = pc_shift(cw);
  unsigned sa = a.signExp >> 15, sb = (b.signExp >> 15) ^ (sub ? 1u : 0u);
  int ea = a.signExp & 0x7fff, eb = b.signExp & 0x7fff;
  uint64_t ma = a.signif, mb = b.signif;
  // X = larger magnitude, Y = the other (or zero)
  unsigned sx, sy;
  int ex, ey;
  uint64_t mx, my;
  if (ca == 1 || (cb != 1 && (eb > ea || (eb == ea && mb > ma)))) {
    sx = sb, ex = eb, mx = mb, sy = sa, ey = ea, my = ma;
    if (ca == 1) my = 0, ey = ex;
  } else {
    sx = sa, ex = ea, mx = ma, sy = sb, ey = eb, my = mb;
    if (cb == 1) my = 0, ey = ex;
  }
  bool same = sx == sy;
  if (!same && ex == ey && mx == my) { // exact cancellation: +0, or -0 rounding down
    r->signif = 0;
    r->signExp = (uint16_t)(rc == 1 ? 0x8000 : 0);
    *inexact = false;
    return true;
  }
  u128 A = (u128)mx << 63, B = (u128)my << 63;
  unsigned d = (unsigned)(ex - ey);
  if (d) {
    if (d >= 127) {
      B = B != 0;
    } else {
      bool lost = (B & (((u128)1 << d) - 1)) != 0;
      B = (B >> d) | (lost ? 1 : 0);
    }
  }
  u128 S = same ? A + B : A - B;
  uint64_t hi = (uint64_t)(S >> 64);
  unsigned lz = hi ? (unsigned)__builtin_clzll(hi) : 64 + (unsigned)__builtin_clzll((uint64_t)S);
  S <<= lz;
  int e = ex + 1 - (int)lz;
  if (e < 1) return false;
  uint64_t m = (uint64_t)(S >> 64);
  if (!fround(&m, &e, (uint64_t)S, shift, rc, sx != 0, inexact)) return false;
  r->signif = m;
  r->signExp = (uint16_t)((sx << 15) | (unsigned)e);
  return true;
}

static inline __attribute__((always_inline)) bool fast_mul(f80 a, f80 b, unsigned cw, f80 *r, bool *inexact) {
  int ca = fclass(a), cb = fclass(b);
  if (ca == 2 || cb == 2) return false;
  unsigned sign = (a.signExp ^ b.signExp) & 0x8000;
  *inexact = false;
  if (ca == 1 || cb == 1) {
    r->signif = 0;
    r->signExp = (uint16_t)sign;
    return true;
  }
  u128 P = (u128)a.signif * b.signif;
  int e = (a.signExp & 0x7fff) + (b.signExp & 0x7fff) - 0x3fff;
  if (P >> 127) e++;
  else P <<= 1;
  if (e < 1 || e > 0x7ffe) return false;
  uint64_t m = (uint64_t)(P >> 64);
  if (!fround(&m, &e, (uint64_t)P, pc_shift(cw), (cw >> 10) & 3, sign != 0, inexact)) return false;
  r->signif = m;
  r->signExp = (uint16_t)(sign | (unsigned)e);
  return true;
}

static inline __attribute__((always_inline)) bool fast_arith(unsigned aop, f80 a, f80 b, unsigned cw, f80 *r, bool *inexact) {
  if (aop == X87_ARITH_MUL) return fast_mul(a, b, cw, r, inexact);
  if (aop > X87_ARITH_SUBR) return false;
  bool swap = aop == X87_ARITH_SUBR; // b - a
  return fast_addsub(swap ? b : a, swap ? a : b, aop != X87_ARITH_ADD, cw, r, inexact);
}

// Condition bits of x against y for zero/normal operands: C3 equal, C0 less, 0 greater; false = slow path.
static inline __attribute__((always_inline)) bool fast_cmp(f80 x, f80 y, unsigned *cc) {
  int cx = fclass(x), cy = fclass(y);
  if (cx == 2 || cy == 2) return false;
  if (cx == 1 && cy == 1) {
    *cc = SW_C3;
    return true;
  }
  unsigned sx = x.signExp >> 15, sy = y.signExp >> 15;
  if (cx == 1) { // zero is below a positive number and above a negative one
    *cc = sy ? 0 : SW_C0;
    return true;
  }
  if (cy == 1) {
    *cc = sx ? SW_C0 : 0;
    return true;
  }
  if (sx != sy) {
    *cc = sx ? SW_C0 : 0;
    return true;
  }
  unsigned ex = x.signExp & 0x7fff, ey = y.signExp & 0x7fff;
  int lt = ex != ey ? (ex < ey ? -1 : 1) : (x.signif == y.signif ? 0 : x.signif < y.signif ? -1 : 1);
  if (lt == 0) *cc = SW_C3;
  else *cc = ((lt < 0) != (sx != 0)) ? SW_C0 : 0;
  return true;
}

// The memory operand without SoftFloat: false for a float32/float64 that is not a normal number.
static inline __attribute__((always_inline)) bool load_operand_fast(unsigned sz, uint64_t a, f80 *v) {
  switch (sz) {
  case 0: {
    uint32_t w;
    memcpy(&w, (const void *)a, 4);
    unsigned e = (w >> 23) & 0xff;
    if (e == 0 || e == 0xff) return false;
    *v = mk(UINT64_C(0x8000000000000000) | (uint64_t)(w & 0x7fffff) << 40, (uint16_t)((w >> 31 << 15) | (e + 0x3f80)));
    return true;
  }
  case 1: {
    uint64_t w;
    memcpy(&w, (const void *)a, 8);
    unsigned e = (unsigned)(w >> 52) & 0x7ff;
    if (e == 0 || e == 0x7ff) return false;
    *v = mk(UINT64_C(0x8000000000000000) | w << 11, (uint16_t)((w >> 63 << 15) | (e + 0x3c00)));
    return true;
  }
  default:
    *v = load_int(sz, a);
    return true;
  }
}

// FADD & co and the compares: true if done (state updated), false = nothing changed.
static inline __attribute__((always_inline)) bool fast_try(GuestContext *c, uint32_t op, uint64_t arg, uint64_t *ret) {
#ifdef NPS5_X87_NOFAST // the all-SoftFloat reference build of the tests
  (void)c, (void)op, (void)arg, (void)ret;
  return false;
#endif
  unsigned k = X87_KIND(op), a = X87_A(op), b = X87_B(op), sz = X87_SZ(op), aux = X87_AUX(op);
  bool pop = X87_POP(op) != 0;
  unsigned top = (unsigned)c->fpu_top;
  unsigned v = c->fpu_valid;
  f80 x, y, r;
  bool inex;
  unsigned cc;
  switch (k) {
  case X87_K_ARITH_ST:
  case X87_K_ARITH_M: {
    unsigned dst = k == X87_K_ARITH_ST ? (top + a) & 7 : top;
    if (!((v >> dst) & 1)) return false;
    if (k == X87_K_ARITH_ST) {
      if (!((v >> ((top + b) & 7)) & 1)) return false;
      y = rd(&c->fpu80[(top + b) & 7]);
    } else if (!load_operand_fast(sz, arg, &y)) {
      return false;
    }
    x = rd(&c->fpu80[dst]);
    if (!fast_arith(aux, x, y, c->fpu_cw, &r, &inex)) return false;
    c->fpu_sw = (uint16_t)((c->fpu_sw & ~SW_C1) | (inex ? 0x20u : 0));
    wr(&c->fpu80[dst], r);
    if (pop && k == X87_K_ARITH_ST) st_pop(c);
    return true;
  }
  case X87_K_COM_ST:
  case X87_K_COM_M:
  case X87_K_TST:
  case X87_K_COMI:
    if (!((v >> top) & 1)) return false;
    x = rd(&c->fpu80[top]);
    if (k == X87_K_COM_ST || k == X87_K_COMI) {
      if (!((v >> ((top + a) & 7)) & 1)) return false;
      y = rd(&c->fpu80[(top + a) & 7]);
    } else if (k == X87_K_COM_M) {
      if (!load_operand_fast(sz, arg, &y)) return false;
    } else {
      y = mk(0, 0);
    }
    if (!fast_cmp(x, y, &cc)) return false;
    if (k == X87_K_COMI) {
      *ret = cc == SW_C3 ? UINT64_C(0x60000000) : cc == SW_C0 ? UINT64_C(0x80000000) : UINT64_C(0x20000000);
    } else {
      c->fpu_sw = (uint16_t)((c->fpu_sw & ~SW_CC) | cc);
    }
    if (pop) st_pop(c);
    if (k != X87_K_COMI && X87_POP2(op)) st_pop(c);
    return true;
  default:
    return false;
  }
}

// ---- dispatcher ----

static bool needs_env(unsigned k) {
  switch (k) {
  case X87_K_FLD_ST:
  case X87_K_CHS:
  case X87_K_ABS:
  case X87_K_XCH:
  case X87_K_LDCW:
  case X87_K_STCW:
  case X87_K_STSW:
  case X87_K_NINIT:
  case X87_K_NCLEX:
  case X87_K_CMOV:
  case X87_K_FREE:
  case X87_K_DECSTP:
  case X87_K_INCSTP:
  case X87_K_STENV:
  case X87_K_LDENV:
  case X87_K_SAVE:
  case X87_K_RSTOR:
  case X87_K_FXSAVE:
  case X87_K_FXRSTOR:
  case X87_K_XAM:
    return false;
  }
  return true;
}

// envset: SoftFloat's rounding mode and precision already match the control word (a batch sets them
// once, see recomp_x87_run_impl); the exception flags are zero on entry in that case.
static inline uint64_t x87_exec(GuestContext *c, uint32_t op, uint64_t arg, bool *envset) {
  unsigned k = X87_KIND(op), a = X87_A(op), b = X87_B(op), sz = X87_SZ(op), aux = X87_AUX(op);
  bool pop = X87_POP(op) != 0;
  uint64_t ret = 0;
  bool env = needs_env(k);
  if ((k == X87_K_ARITH_ST || k == X87_K_ARITH_M || k == X87_K_COM_ST || k == X87_K_COM_M || k == X87_K_COMI ||
       k == X87_K_TST) &&
      fast_try(c, op, arg, &ret))
    return ret;
  if (env && !*envset) {
    env_begin(c);
    *envset = true;
  }

  switch (k) {
  case X87_K_FLD_M: {
    f80 v = sz == 0 ? load_f32(arg) : sz == 1 ? load_f64(arg) : load_m80(arg);
    set_c1(c, 0);
    st_push(c, v);
    break;
  }
  case X87_K_FLD_ST: {
    f80 v = st_get(c, a);
    set_c1(c, 0);
    st_push(c, v);
    break;
  }
  case X87_K_FLD_CONST:
    set_c1(c, 0);
    st_push(c, fld_const(a, (c->fpu_cw >> 10) & 3));
    break;
  case X87_K_FILD: {
    f80 v;
    if (sz == 2) {
      int64_t i;
      memcpy(&i, (const void *)arg, 8);
      v = int_to_f80(i);
    } else {
      v = load_int(sz == 0 ? 2 : 3, arg);
    }
    set_c1(c, 0);
    st_push(c, v);
    break;
  }
  case X87_K_FST_M: {
    f80 v = st_get(c, 0);
    set_c1(c, 0);
    if (sz == 0) {
      float32_t f = extF80_to_f32(v);
      memcpy((void *)arg, &f.v, 4);
    } else if (sz == 1) {
      float64_t f = extF80_to_f64(v);
      memcpy((void *)arg, &f.v, 8);
    } else {
      store_m80(arg, v);
    }
    if (pop) st_pop(c);
    break;
  }
  case X87_K_FST_ST: {
    f80 v = st_get(c, 0);
    set_c1(c, 0);
    if (a != 0 || !pop) st_put(c, a, v);
    if (pop) st_pop(c);
    break;
  }
  case X87_K_FIST:
    set_c1(c, 0);
    do_fist(c, sz, aux == 1, arg);
    if (pop) st_pop(c);
    break;
  case X87_K_ARITH_ST: {
    f80 x = st_get(c, a), y = st_get(c, b);
    set_c1(c, 0);
    st_put(c, a, arith(aux, x, y));
    if (pop) st_pop(c);
    break;
  }
  case X87_K_ARITH_M: {
    f80 y = load_operand(sz, arg);
    f80 x = st_get(c, 0);
    set_c1(c, 0);
    st_put(c, 0, arith(aux, x, y));
    break;
  }
  case X87_K_CHS:
  case X87_K_ABS: {
    f80 v = st_get(c, 0);
    set_c1(c, 0);
    v.signExp = k == X87_K_CHS ? (uint16_t)(v.signExp ^ 0x8000) : (uint16_t)(v.signExp & 0x7fff);
    st_put(c, 0, v);
    break;
  }
  case X87_K_SQRT: {
    f80 v = st_get(c, 0);
    set_c1(c, 0);
    st_put(c, 0, extF80_sqrt(v));
    break;
  }
  case X87_K_RNDINT: {
    f80 v = st_get(c, 0);
    set_c1(c, 0);
    st_put(c, 0, extF80_roundToInt(v, softfloat_roundingMode, true));
    break;
  }
  case X87_K_SCALE: {
    f80 x = st_get(c, 0), y = st_get(c, 1);
    set_c1(c, 0);
    st_put(c, 0, do_scale(x, y));
    break;
  }
  case X87_K_XTRACT: {
    f80 v = st_get(c, 0), e, s;
    set_c1(c, 0);
    do_xtract(v, &e, &s);
    st_put(c, 0, e);
    st_push(c, s);
    break;
  }
  case X87_K_PREM: {
    f80 x = st_get(c, 0), y = st_get(c, 1);
    unsigned cc;
    f80 r = do_prem(x, y, aux == 1, &cc);
    c->fpu_sw = (uint16_t)((c->fpu_sw & ~SW_CC) | cc);
    st_put(c, 0, r);
    break;
  }
  case X87_K_COM_ST:
  case X87_K_COM_M:
  case X87_K_TST: {
    f80 x = st_get(c, 0), y;
    bool unordered = aux == 1;
    if (k == X87_K_COM_ST) y = st_get(c, a);
    else if (k == X87_K_COM_M) y = load_operand(sz, arg);
    else y = mk(0, 0);
    c->fpu_sw = (uint16_t)((c->fpu_sw & ~SW_CC) | compare(x, y, unordered));
    if (pop) st_pop(c);
    if (X87_POP2(op)) st_pop(c);
    break;
  }
  case X87_K_COMI: {
    f80 x = st_get(c, 0), y = st_get(c, a);
    unsigned cc = compare(x, y, aux == 1);
    ret = cc == SW_C3 ? UINT64_C(0x60000000) : cc == SW_C0 ? UINT64_C(0x80000000)
                                              : cc == 0    ? UINT64_C(0x20000000)
                                                           : UINT64_C(0x30000000);
    if (pop) st_pop(c);
    break;
  }
  case X87_K_XAM:
    c->fpu_sw = (uint16_t)((c->fpu_sw & ~SW_CC) | xam(c));
    break;
  case X87_K_XCH: {
    f80 x = st_get(c, 0), y = st_get(c, a);
    set_c1(c, 0);
    st_put(c, 0, y);
    st_put(c, a, x);
    break;
  }
  case X87_K_LDCW:
    c->fpu_cw = (uint16_t)arg;
    break;
  case X87_K_STCW:
    ret = c->fpu_cw;
    break;
  case X87_K_STSW:
    ret = sw_with_top(c);
    break;
  case X87_K_NINIT:
    fninit(c);
    break;
  case X87_K_NCLEX:
    c->fpu_sw &= (uint16_t)~0x80ffu;
    break;
  case X87_K_CMOV: {
    f80 v = st_get(c, a);
    st_put(c, 0, v);
    break;
  }
  case X87_K_FREE:
    c->fpu_valid &= (uint8_t)~(1u << phys(c, a));
    if (pop) st_pop(c);
    break;
  case X87_K_DECSTP:
    c->fpu_top = (c->fpu_top - 1) & 7;
    set_c1(c, 0);
    break;
  case X87_K_INCSTP:
    c->fpu_top = (c->fpu_top + 1) & 7;
    set_c1(c, 0);
    break;
  case X87_K_STENV:
    env_store(c, (uint8_t *)arg, sz == 1);
    c->fpu_cw |= 0x3f; // FNSTENV masks all exceptions
    break;
  case X87_K_LDENV:
    env_load(c, (const uint8_t *)arg, sz == 1);
    break;
  case X87_K_SAVE: {
    uint8_t *p = (uint8_t *)arg;
    unsigned off = sz == 1 ? 14 : 28;
    env_store(c, p, sz == 1);
    for (unsigned i = 0; i < 8; i++) {
      f80 v = rd(&c->fpu80[phys(c, i)]);
      store_m80((uint64_t)(p + off + 10 * i), v);
    }
    fninit(c);
    break;
  }
  case X87_K_RSTOR: {
    const uint8_t *p = (const uint8_t *)arg;
    unsigned off = sz == 1 ? 14 : 28;
    env_load(c, p, sz == 1);
    for (unsigned i = 0; i < 8; i++) wr(&c->fpu80[phys(c, i)], load_m80((uint64_t)(p + off + 10 * i)));
    break;
  }
  case X87_K_FXSAVE: {
    uint8_t *p = (uint8_t *)arg;
    put16(p + 0, c->fpu_cw);
    put16(p + 2, sw_with_top(c));
    p[4] = c->fpu_valid;
    for (unsigned i = 0; i < 8; i++) {
      memset(p + 32 + 16 * i, 0, 16);
      store_m80((uint64_t)(p + 32 + 16 * i), rd(&c->fpu80[phys(c, i)]));
    }
    break;
  }
  case X87_K_FXRSTOR: {
    const uint8_t *p = (const uint8_t *)arg;
    c->fpu_cw = (uint16_t)get16(p + 0);
    unsigned sw = get16(p + 2);
    c->fpu_top = (int)((sw >> 11) & 7);
    c->fpu_sw = (uint16_t)(sw & ~0x3800u);
    c->fpu_valid = p[4];
    for (unsigned i = 0; i < 8; i++) wr(&c->fpu80[phys(c, i)], load_m80((uint64_t)(p + 32 + 16 * i)));
    break;
  }
  case X87_K_BLD: {
    f80 v = bcd_load((const uint8_t *)arg);
    set_c1(c, 0);
    st_push(c, v);
    break;
  }
  case X87_K_BSTP: {
    f80 v = st_get(c, 0);
    set_c1(c, 0);
    bcd_store(v, (uint8_t *)arg, softfloat_roundingMode);
    st_pop(c);
    break;
  }
  default:
    break;
  }
  if (env) env_end(c);
  return ret;
}

uint64_t recomp_x87_impl(GuestContext *c, uint32_t op, uint64_t arg) {
  bool envset = false;
  return x87_exec(c, op, arg, &envset);
}

// A batch of consecutive x87 instructions (the bridge's _recomp_x87_run): n pairs of (op word,
// argument) of 16 bytes each, run in order. The result is that of the last one (the NZCV bits of a
// closing FCOMI); instructions with a register result in the middle of a batch are never emitted.
// SoftFloat's state is thread-local, and every access to it is a call on this platform: the rounding
// mode and the precision are set once for the batch and again after an instruction that can change
// the control word; env_end leaves the exception flags zero, as env_begin would.
uint64_t recomp_x87_run_impl(GuestContext *c, const uint64_t *pairs, uint64_t n) {
  uint64_t ret = 0;
  bool envset = false;
  for (uint64_t i = 0; i < n; i++) {
    uint32_t op = (uint32_t)pairs[2 * i];
    unsigned k = X87_KIND(op);
    ret = x87_exec(c, op, pairs[2 * i + 1], &envset);
    switch (k) { // these set the control word
    case X87_K_NINIT:
    case X87_K_LDENV:
    case X87_K_RSTOR:
    case X87_K_FXRSTOR:
    case X87_K_LDCW:
      envset = false;
    }
  }
  return ret;
}

// ---- lean entries (preserve_most) ----
//
// The bridge's _recomp_x87_a / _recomp_x87_run first try these: they run what fast_try can do (the
// integer FADD/FSUB/FSUBR/FMUL and compares) without saving the host state: preserve_most makes the
// compiler itself save the few x9-x15 it uses, and the code is integer-only (TestX87RuntimeVectorRegs
// checks that no vector register appears, so v0-v7 survive). `done` is the number of operations
// executed (0 or 1 for the single entry); the bridge hands the rest to the full helper.
__attribute__((preserve_most, flatten)) x87_fast_ret recomp_x87_fast(GuestContext *c, uint32_t op, uint64_t arg) {
  x87_fast_ret r = {0, 0};
  r.done = fast_try(c, op, arg, &r.ret);
  return r;
}

__attribute__((preserve_most, flatten)) x87_fast_ret recomp_x87_run_fast(GuestContext *c, const uint64_t *pairs, uint64_t n) {
  x87_fast_ret r = {0, 0};
  while (r.done < n && fast_try(c, (uint32_t)pairs[2 * r.done], pairs[2 * r.done + 1], &r.ret)) r.done++;
  return r;
}
