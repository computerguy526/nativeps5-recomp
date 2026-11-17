/* SSE/AVX2/FMA forms taken from the Syobon census (work/s2/out/census_syobon.txt):
 * VPBROADCAST{B,W,D,Q} (xmm/ymm, mem/reg), VBROADCASTI128, VPERMQ/VPERMPD/VPERM2I128/
 * VPERMD/VPERMPS, VPMULHUW, VPS{LL,RL,RA}V{D,Q}, VPMASKMOVD, VFMADD/VFMSUB/VFNMADD/
 * VFNMSUB {SS,SD,PS,PD} in 132/213/231 shapes, PSHUFW.  Each case has a hardware
 * implementation (x86-64 intrinsics) and a scalar model; the hardware result is
 * printed (the model on the host build) and the two are compared on x86. */
#include "ps5test.h"
#include <math.h>
#include <string.h>
#ifdef __x86_64__
#include <immintrin.h>
#endif

typedef struct { uint8_t b[32]; } V;
static uint32_t g32(const V *v, int i) { uint32_t x; memcpy(&x, v->b + 4 * i, 4); return x; }
static void s32(V *v, int i, uint32_t x) { memcpy(v->b + 4 * i, &x, 4); }
static uint64_t g64(const V *v, int i) { uint64_t x; memcpy(&x, v->b + 8 * i, 8); return x; }
static void s64(V *v, int i, uint64_t x) { memcpy(v->b + 8 * i, &x, 8); }
static uint16_t g16(const V *v, int i) { uint16_t x; memcpy(&x, v->b + 2 * i, 2); return x; }
static void s16(V *v, int i, uint16_t x) { memcpy(v->b + 2 * i, &x, 2); }
static float gf(const V *v, int i) { float x; memcpy(&x, v->b + 4 * i, 4); return x; }
static void sf(V *v, int i, float x) { memcpy(v->b + 4 * i, &x, 4); }
static double gd(const V *v, int i) { double x; memcpy(&x, v->b + 8 * i, 8); return x; }
static void sd(V *v, int i, double x) { memcpy(v->b + 8 * i, &x, 8); }

typedef void (*Fn)(V *out, const V *a, const V *b, const V *c);
typedef struct { const char *name; Fn hw, md; } Case;

#define CASE(NAME, HWBODY, MDBODY)                                                                   \
  static T_NOINLINE void md_##NAME(V *o, const V *a, const V *b, const V *c) { (void)a; (void)b; (void)c; memset(o, 0, 32); MDBODY } \
  CASEHW(NAME, HWBODY)
#ifdef __x86_64__
#define CASEHW(NAME, HWBODY) \
  static T_NOINLINE void hw_##NAME(V *o, const V *a, const V *b, const V *c) { (void)a; (void)b; (void)c; HWBODY }
#define LD128(p) _mm_loadu_si128((const __m128i *)(p))
#define LD256(p) _mm256_loadu_si256((const __m256i *)(p))
#define ST128(p, v) _mm_storeu_si128((__m128i *)(p), v)
#define ST256(p, v) _mm256_storeu_si256((__m256i *)(p), v)
#else
#define CASEHW(NAME, HWBODY) static void hw_##NAME(V *o, const V *a, const V *b, const V *c) { md_##NAME(o, a, b, c); }
#define LD128(p) 0
#define LD256(p) 0
#define ST128(p, v) (void)0
#define ST256(p, v) (void)0
#define _mm_empty() (void)0
#endif

/* broadcasts */
CASE(bcast_b_xx, ST128(o, _mm_broadcastb_epi8(LD128(a)));, for (int i = 0; i < 16; i++) o->b[i] = a->b[0];)
CASE(bcast_b_xm, ST128(o, _mm_set1_epi8(*(const char *)(a->b + 3)));, for (int i = 0; i < 16; i++) o->b[i] = a->b[3];)
CASE(bcast_b_ym, ST256(o, _mm256_set1_epi8(*(const char *)(a->b + 5)));, for (int i = 0; i < 32; i++) o->b[i] = a->b[5];)
CASE(bcast_b_yx, ST256(o, _mm256_broadcastb_epi8(LD128(a)));, for (int i = 0; i < 32; i++) o->b[i] = a->b[0];)
CASE(bcast_w_xm, ST128(o, _mm_set1_epi16(*(const short *)(a->b + 6)));, for (int i = 0; i < 8; i++) s16(o, i, g16(a, 3));)
CASE(bcast_w_xx, ST128(o, _mm_broadcastw_epi16(LD128(a)));, for (int i = 0; i < 8; i++) s16(o, i, g16(a, 0));)
CASE(bcast_w_ym, ST256(o, _mm256_set1_epi16(*(const short *)(a->b + 2)));, for (int i = 0; i < 16; i++) s16(o, i, g16(a, 1));)
CASE(bcast_w_yx, ST256(o, _mm256_broadcastw_epi16(LD128(a)));, for (int i = 0; i < 16; i++) s16(o, i, g16(a, 0));)
CASE(bcast_d_xm, ST128(o, _mm_set1_epi32(*(const int *)(a->b + 8)));, for (int i = 0; i < 4; i++) s32(o, i, g32(a, 2));)
CASE(bcast_d_xx, ST128(o, _mm_broadcastd_epi32(LD128(a)));, for (int i = 0; i < 4; i++) s32(o, i, g32(a, 0));)
CASE(bcast_d_ym, ST256(o, _mm256_set1_epi32(*(const int *)(a->b + 4)));, for (int i = 0; i < 8; i++) s32(o, i, g32(a, 1));)
CASE(bcast_d_yx, ST256(o, _mm256_broadcastd_epi32(LD128(a)));, for (int i = 0; i < 8; i++) s32(o, i, g32(a, 0));)
CASE(bcast_q_xm, ST128(o, _mm_set1_epi64x(*(const long long *)(a->b + 8)));, for (int i = 0; i < 2; i++) s64(o, i, g64(a, 1));)
CASE(bcast_q_ym, ST256(o, _mm256_set1_epi64x(*(const long long *)(a->b + 16)));, for (int i = 0; i < 4; i++) s64(o, i, g64(a, 2));)
CASE(bcast_q_yx, ST256(o, _mm256_broadcastq_epi64(LD128(a)));, for (int i = 0; i < 4; i++) s64(o, i, g64(a, 0));)
CASE(bcast_i128, ST256(o, _mm256_broadcastsi128_si256(LD128(a + 0)));, for (int i = 0; i < 16; i++) { o->b[i] = a->b[i]; o->b[16 + i] = a->b[i]; })
/* permutes */
CASE(permq_1b, ST256(o, _mm256_permute4x64_epi64(LD256(a), 0x1B));, for (int i = 0; i < 4; i++) s64(o, i, g64(a, (0x1B >> (2 * i)) & 3));)
CASE(permq_c6, ST256(o, _mm256_permute4x64_epi64(LD256(a), 0xC6));, for (int i = 0; i < 4; i++) s64(o, i, g64(a, (0xC6 >> (2 * i)) & 3));)
CASE(permpd_4e, ST256(o, _mm256_castpd_si256(_mm256_permute4x64_pd(_mm256_castsi256_pd(LD256(a)), 0x4E)));, for (int i = 0; i < 4; i++) s64(o, i, g64(a, (0x4E >> (2 * i)) & 3));)
#define P2I(IMM) \
CASE(perm2i_##IMM, ST256(o, _mm256_permute2x128_si256(LD256(a), LD256(b), 0x##IMM));, \
  for (int h = 0; h < 2; h++) { int ctl = (0x##IMM >> (4 * h)) & 0xf; \
    for (int i = 0; i < 16; i++) o->b[16 * h + i] = (ctl & 8) ? 0 : ((ctl & 3) < 2 ? a : b)->b[16 * (ctl & 1) + i]; })
P2I(20) P2I(31) P2I(83) P2I(08) P2I(12)
CASE(permd, ST256(o, _mm256_permutevar8x32_epi32(LD256(a), LD256(b)));, for (int i = 0; i < 8; i++) s32(o, i, g32(a, g32(b, i) & 7));)
CASE(permps, ST256(o, _mm256_castps_si256(_mm256_permutevar8x32_ps(_mm256_castsi256_ps(LD256(a)), LD256(b))));, for (int i = 0; i < 8; i++) s32(o, i, g32(a, g32(b, i) & 7));)
/* integer arithmetic / variable shifts (counts >= width give 0 / sign) */
CASE(mulhuw_x, ST128(o, _mm_mulhi_epu16(LD128(a), LD128(b)));, for (int i = 0; i < 8; i++) s16(o, i, (uint16_t)(((uint32_t)g16(a, i) * g16(b, i)) >> 16));)
CASE(mulhuw_y, ST256(o, _mm256_mulhi_epu16(LD256(a), LD256(b)));, for (int i = 0; i < 16; i++) s16(o, i, (uint16_t)(((uint32_t)g16(a, i) * g16(b, i)) >> 16));)
CASE(sllvd_x, ST128(o, _mm_sllv_epi32(LD128(a), LD128(b)));, for (int i = 0; i < 4; i++) s32(o, i, g32(b, i) > 31 ? 0 : g32(a, i) << g32(b, i));)
CASE(sllvd_y, ST256(o, _mm256_sllv_epi32(LD256(a), LD256(b)));, for (int i = 0; i < 8; i++) s32(o, i, g32(b, i) > 31 ? 0 : g32(a, i) << g32(b, i));)
CASE(srlvd_x, ST128(o, _mm_srlv_epi32(LD128(a), LD128(b)));, for (int i = 0; i < 4; i++) s32(o, i, g32(b, i) > 31 ? 0 : g32(a, i) >> g32(b, i));)
CASE(srlvd_y, ST256(o, _mm256_srlv_epi32(LD256(a), LD256(b)));, for (int i = 0; i < 8; i++) s32(o, i, g32(b, i) > 31 ? 0 : g32(a, i) >> g32(b, i));)
CASE(sravd_x, ST128(o, _mm_srav_epi32(LD128(a), LD128(b)));, for (int i = 0; i < 4; i++) s32(o, i, (uint32_t)((int32_t)g32(a, i) >> (g32(b, i) > 31 ? 31 : g32(b, i))));)
CASE(sllvq_y, ST256(o, _mm256_sllv_epi64(LD256(a), LD256(b)));, for (int i = 0; i < 4; i++) s64(o, i, g64(b, i) > 63 ? 0 : g64(a, i) << g64(b, i));)
CASE(srlvq_x, ST128(o, _mm_srlv_epi64(LD128(a), LD128(b)));, for (int i = 0; i < 2; i++) s64(o, i, g64(b, i) > 63 ? 0 : g64(a, i) >> g64(b, i));)
CASE(maskload_d, ST256(o, _mm256_maskload_epi32((const int *)a, LD256(c)));, for (int i = 0; i < 8; i++) s32(o, i, (g32(c, i) >> 31) ? g32(a, i) : 0);)
CASE(maskstore_d, { _mm256_storeu_si256((__m256i *)o, LD256(b)); _mm256_maskstore_epi32((int *)o, LD256(c), LD256(a)); }, memcpy(o, b, 32); for (int i = 0; i < 8; i++) if (g32(c, i) >> 31) s32(o, i, g32(a, i));)
CASE(pshufw, { __m64 t = *(const __m64 *)a; t = _mm_shuffle_pi16(t, 0x1B); *(__m64 *)o = t; _mm_empty(); }, for (int i = 0; i < 4; i++) s16(o, i, g16(a, 3 - i));)

/* FMA (inputs are small finite floats, see mkfloat) */
#ifdef __x86_64__
#define LDPS(p) _mm_loadu_ps((const float *)(p))
#define LDPD(p) _mm_loadu_pd((const double *)(p))
#define LDYPS(p) _mm256_loadu_ps((const float *)(p))
#define LDYPD(p) _mm256_loadu_pd((const double *)(p))
#else
#define LDPS(p) 0
#define LDPD(p) 0
#define LDYPS(p) 0
#define LDYPD(p) 0
#define _mm_storeu_ps(p, v) (void)0
#define _mm_storeu_pd(p, v) (void)0
#define _mm256_storeu_ps(p, v) (void)0
#define _mm256_storeu_pd(p, v) (void)0
#define _mm_fmadd_ss(a, b, c) 0
#define _mm_fmadd_sd(a, b, c) 0
#define _mm_fmsub_ss(a, b, c) 0
#define _mm_fnmadd_ss(a, b, c) 0
#define _mm_fnmsub_ss(a, b, c) 0
#define _mm_fnmadd_sd(a, b, c) 0
#define _mm_fmsub_sd(a, b, c) 0
#define _mm_fmadd_ps(a, b, c) 0
#define _mm_fmsub_ps(a, b, c) 0
#define _mm256_fmadd_ps(a, b, c) 0
#define _mm256_fnmadd_ps(a, b, c) 0
#define _mm256_fmadd_pd(a, b, c) 0
#define _mm256_fmsub_pd(a, b, c) 0
#define _mm256_fnmsub_pd(a, b, c) 0
#endif
#define FSS(NAME, INTR, SIGN_P, SIGN_C)                                                              \
  CASE(NAME##_abc, _mm_storeu_ps((float *)o, INTR(LDPS(a), LDPS(b), LDPS(c)));, sf(o, 0, fmaf(SIGN_P gf(a, 0), gf(b, 0), SIGN_C gf(c, 0))); for (int i = 1; i < 4; i++) sf(o, i, gf(a, i));) \
  CASE(NAME##_bca, _mm_storeu_ps((float *)o, INTR(LDPS(b), LDPS(c), LDPS(a)));, sf(o, 0, fmaf(SIGN_P gf(b, 0), gf(c, 0), SIGN_C gf(a, 0))); for (int i = 1; i < 4; i++) sf(o, i, gf(b, i));) \
  CASE(NAME##_cab, _mm_storeu_ps((float *)o, INTR(LDPS(c), LDPS(a), LDPS(b)));, sf(o, 0, fmaf(SIGN_P gf(c, 0), gf(a, 0), SIGN_C gf(b, 0))); for (int i = 1; i < 4; i++) sf(o, i, gf(c, i));) \
  CASE(NAME##_mem, { __m128 x = LDPS(a); __m128 y = LDPS(b); _mm_storeu_ps((float *)o, INTR(x, y, _mm_load_ss((const float *)(c->b + 4)))); }, \
       sf(o, 0, fmaf(SIGN_P gf(a, 0), gf(b, 0), SIGN_C gf(c, 1))); for (int i = 1; i < 4; i++) sf(o, i, gf(a, i));)
FSS(fmadd_ss, _mm_fmadd_ss, +, +)
FSS(fmsub_ss, _mm_fmsub_ss, +, -)
FSS(fnmadd_ss, _mm_fnmadd_ss, -, +)
FSS(fnmsub_ss, _mm_fnmsub_ss, -, -)
CASE(fmadd_sd_abc, _mm_storeu_pd((double *)o, _mm_fmadd_sd(LDPD(a), LDPD(b), LDPD(c)));, sd(o, 0, fma(gd(a, 0), gd(b, 0), gd(c, 0))); sd(o, 1, gd(a, 1));)
CASE(fmadd_sd_cba, _mm_storeu_pd((double *)o, _mm_fmadd_sd(LDPD(c), LDPD(b), LDPD(a)));, sd(o, 0, fma(gd(c, 0), gd(b, 0), gd(a, 0))); sd(o, 1, gd(c, 1));)
CASE(fnmadd_sd, _mm_storeu_pd((double *)o, _mm_fnmadd_sd(LDPD(a), LDPD(b), LDPD(c)));, sd(o, 0, fma(-gd(a, 0), gd(b, 0), gd(c, 0))); sd(o, 1, gd(a, 1));)
CASE(fmsub_sd, _mm_storeu_pd((double *)o, _mm_fmsub_sd(LDPD(a), LDPD(b), LDPD(c)));, sd(o, 0, fma(gd(a, 0), gd(b, 0), -gd(c, 0))); sd(o, 1, gd(a, 1));)
CASE(fmadd_ps_x, _mm_storeu_ps((float *)o, _mm_fmadd_ps(LDPS(a), LDPS(b), LDPS(c)));, for (int i = 0; i < 4; i++) sf(o, i, fmaf(gf(a, i), gf(b, i), gf(c, i)));)
CASE(fmsub_ps_x, _mm_storeu_ps((float *)o, _mm_fmsub_ps(LDPS(a), LDPS(b), LDPS(c)));, for (int i = 0; i < 4; i++) sf(o, i, fmaf(gf(a, i), gf(b, i), -gf(c, i)));)
CASE(fmadd_ps_y, _mm256_storeu_ps((float *)o, _mm256_fmadd_ps(LDYPS(a), LDYPS(b), LDYPS(c)));, for (int i = 0; i < 8; i++) sf(o, i, fmaf(gf(a, i), gf(b, i), gf(c, i)));)
CASE(fnmadd_ps_y, _mm256_storeu_ps((float *)o, _mm256_fnmadd_ps(LDYPS(a), LDYPS(b), LDYPS(c)));, for (int i = 0; i < 8; i++) sf(o, i, fmaf(-gf(a, i), gf(b, i), gf(c, i)));)
CASE(fmadd_pd_y, _mm256_storeu_pd((double *)o, _mm256_fmadd_pd(LDYPD(a), LDYPD(b), LDYPD(c)));, for (int i = 0; i < 4; i++) sd(o, i, fma(gd(a, i), gd(b, i), gd(c, i)));)
CASE(fmsub_pd_y, _mm256_storeu_pd((double *)o, _mm256_fmsub_pd(LDYPD(a), LDYPD(b), LDYPD(c)));, for (int i = 0; i < 4; i++) sd(o, i, fma(gd(a, i), gd(b, i), -gd(c, i)));)
CASE(fnmsub_pd_y, _mm256_storeu_pd((double *)o, _mm256_fnmsub_pd(LDYPD(a), LDYPD(b), LDYPD(c)));, for (int i = 0; i < 4; i++) sd(o, i, fma(-gd(a, i), gd(b, i), -gd(c, i)));)

/* Exact census forms via inline asm (clang canonicalises the intrinsics of these to FP-domain
 * or 213/132 shapes, so the intrinsic cases above do not reach VPERMQ/VPERM2I128/VBROADCASTI128/
 * VPBROADCASTQ/VF*231 encodings). */
#define XCASE(NAME, HW, MD) \
  static T_NOINLINE void md_##NAME(V *o, const V *a, const V *b, const V *c) { (void)a; (void)b; (void)c; memset(o, 0, 32); MD } \
  CASEHW(NAME, HW)
XCASE(asm_vpermq_1b, { __m256i r; __asm__("vpermq $0x1b,%1,%0" : "=x"(r) : "x"(LD256(a))); ST256(o, r); }, for (int i = 0; i < 4; i++) s64(o, i, g64(a, 3 - i));)
XCASE(asm_vpermq_93, { __m256i r; __asm__("vpermq $0x93,%1,%0" : "=x"(r) : "x"(LD256(a))); ST256(o, r); }, for (int i = 0; i < 4; i++) s64(o, i, g64(a, (0x93 >> (2 * i)) & 3));)
XCASE(asm_vpermpd_b1, { __m256i r; __asm__("vpermpd $0xb1,%1,%0" : "=x"(r) : "x"(LD256(a))); ST256(o, r); }, for (int i = 0; i < 4; i++) s64(o, i, g64(a, (0xb1 >> (2 * i)) & 3));)
XCASE(asm_vperm2i128_31, { __m256i r; __asm__("vperm2i128 $0x31,%2,%1,%0" : "=x"(r) : "x"(LD256(a)), "x"(LD256(b))); ST256(o, r); },
  for (int h = 0; h < 2; h++) { int ctl = (0x31 >> (4 * h)) & 0xf; for (int i = 0; i < 16; i++) o->b[16 * h + i] = ((ctl & 3) < 2 ? a : b)->b[16 * (ctl & 1) + i]; })
XCASE(asm_vperm2i128_28, { __m256i r; __asm__("vperm2i128 $0x28,%2,%1,%0" : "=x"(r) : "x"(LD256(a)), "x"(LD256(b))); ST256(o, r); },
  for (int i = 0; i < 16; i++) { o->b[i] = 0; o->b[16 + i] = b->b[i]; })
XCASE(asm_vbroadcasti128_m, { __m256i r; __asm__("vbroadcasti128 %1,%0" : "=x"(r) : "m"(*(const __m128i *)(a->b))); ST256(o, r); }, for (int i = 0; i < 16; i++) { o->b[i] = a->b[i]; o->b[16 + i] = a->b[i]; })
XCASE(asm_vpbroadcastq_xm, { __m128i r; __asm__("vpbroadcastq %1,%0" : "=x"(r) : "m"(*(const long long *)(a->b + 8))); ST128(o, r); }, for (int i = 0; i < 2; i++) s64(o, i, g64(a, 1));)
XCASE(asm_vpbroadcastq_ym, { __m256i r; __asm__("vpbroadcastq %1,%0" : "=x"(r) : "m"(*(const long long *)(a->b + 16))); ST256(o, r); }, for (int i = 0; i < 4; i++) s64(o, i, g64(a, 2));)
XCASE(asm_vpbroadcastq_yx, { __m256i r; __asm__("vpbroadcastq %1,%0" : "=x"(r) : "x"(LD128(a))); ST256(o, r); }, for (int i = 0; i < 4; i++) s64(o, i, g64(a, 0));)
XCASE(asm_vpbroadcastd_xm, { __m128i r; __asm__("vpbroadcastd %1,%0" : "=x"(r) : "m"(*(const int *)(a->b + 4))); ST128(o, r); }, for (int i = 0; i < 4; i++) s32(o, i, g32(a, 1));)
XCASE(asm_vpbroadcastd_ym, { __m256i r; __asm__("vpbroadcastd %1,%0" : "=x"(r) : "m"(*(const int *)(a->b + 8))); ST256(o, r); }, for (int i = 0; i < 8; i++) s32(o, i, g32(a, 2));)
XCASE(asm_vpbroadcastw_xm, { __m128i r; __asm__("vpbroadcastw %1,%0" : "=x"(r) : "m"(*(const short *)(a->b + 2))); ST128(o, r); }, for (int i = 0; i < 8; i++) s16(o, i, g16(a, 1));)
XCASE(asm_vpbroadcastb_xm, { __m128i r; __asm__("vpbroadcastb %1,%0" : "=x"(r) : "m"(*(const char *)(a->b + 1))); ST128(o, r); }, for (int i = 0; i < 16; i++) o->b[i] = a->b[1];)
XCASE(asm_vfmadd231ss_rr, { __m128 acc = LDPS(a); __asm__("vfmadd231ss %2,%1,%0" : "+x"(acc) : "x"(LDPS(b)), "x"(LDPS(c))); _mm_storeu_ps((float *)o, acc); },
  sf(o, 0, fmaf(gf(b, 0), gf(c, 0), gf(a, 0))); for (int i = 1; i < 4; i++) sf(o, i, gf(a, i));)
XCASE(asm_vfmadd231ss_rm, { __m128 acc = LDPS(a); __asm__("vfmadd231ss %2,%1,%0" : "+x"(acc) : "x"(LDPS(b)), "m"(*(const float *)(c->b + 4))); _mm_storeu_ps((float *)o, acc); },
  sf(o, 0, fmaf(gf(b, 0), gf(c, 1), gf(a, 0))); for (int i = 1; i < 4; i++) sf(o, i, gf(a, i));)
XCASE(asm_vfmsub231ss, { __m128 acc = LDPS(a); __asm__("vfmsub231ss %2,%1,%0" : "+x"(acc) : "x"(LDPS(b)), "x"(LDPS(c))); _mm_storeu_ps((float *)o, acc); },
  sf(o, 0, fmaf(gf(b, 0), gf(c, 0), -gf(a, 0))); for (int i = 1; i < 4; i++) sf(o, i, gf(a, i));)
XCASE(asm_vfnmadd231ss, { __m128 acc = LDPS(a); __asm__("vfnmadd231ss %2,%1,%0" : "+x"(acc) : "x"(LDPS(b)), "x"(LDPS(c))); _mm_storeu_ps((float *)o, acc); },
  sf(o, 0, fmaf(-gf(b, 0), gf(c, 0), gf(a, 0))); for (int i = 1; i < 4; i++) sf(o, i, gf(a, i));)
XCASE(asm_vfmadd231sd, { __m128d acc = LDPD(a); __asm__("vfmadd231sd %2,%1,%0" : "+x"(acc) : "x"(LDPD(b)), "x"(LDPD(c))); _mm_storeu_pd((double *)o, acc); },
  sd(o, 0, fma(gd(b, 0), gd(c, 0), gd(a, 0))); sd(o, 1, gd(a, 1));)
XCASE(asm_vfmadd231ps_y, { __m256 acc = LDYPS(a); __asm__("vfmadd231ps %2,%1,%0" : "+x"(acc) : "x"(LDYPS(b)), "x"(LDYPS(c))); _mm256_storeu_ps((float *)o, acc); },
  for (int i = 0; i < 8; i++) sf(o, i, fmaf(gf(b, i), gf(c, i), gf(a, i)));)
XCASE(asm_vfmadd231ps_ym, { __m256 acc = LDYPS(a); __asm__("vfmadd231ps %2,%1,%0" : "+x"(acc) : "x"(LDYPS(b)), "m"(*(const __m256 *)(c->b))); _mm256_storeu_ps((float *)o, acc); },
  for (int i = 0; i < 8; i++) sf(o, i, fmaf(gf(b, i), gf(c, i), gf(a, i)));)

#define C(N) {#N, hw_##N, md_##N}
static const Case cases[] = {
  C(bcast_b_xx), C(bcast_b_xm), C(bcast_b_ym), C(bcast_b_yx), C(bcast_w_xm), C(bcast_w_xx), C(bcast_w_ym), C(bcast_w_yx),
  C(bcast_d_xm), C(bcast_d_xx), C(bcast_d_ym), C(bcast_d_yx), C(bcast_q_xm), C(bcast_q_ym), C(bcast_q_yx), C(bcast_i128),
  C(permq_1b), C(permq_c6), C(permpd_4e), C(perm2i_20), C(perm2i_31), C(perm2i_83), C(perm2i_08), C(perm2i_12), C(permd), C(permps),
  C(mulhuw_x), C(mulhuw_y), C(sllvd_x), C(sllvd_y), C(srlvd_x), C(srlvd_y), C(sravd_x), C(sllvq_y), C(srlvq_x),
  C(maskload_d), C(maskstore_d), C(pshufw),
  C(fmadd_ss_abc), C(fmadd_ss_bca), C(fmadd_ss_cab), C(fmadd_ss_mem), C(fmsub_ss_abc), C(fmsub_ss_bca), C(fmsub_ss_cab), C(fmsub_ss_mem),
  C(fnmadd_ss_abc), C(fnmadd_ss_bca), C(fnmadd_ss_cab), C(fnmadd_ss_mem), C(fnmsub_ss_abc), C(fnmsub_ss_bca), C(fnmsub_ss_cab), C(fnmsub_ss_mem),
  C(fmadd_sd_abc), C(fmadd_sd_cba), C(fnmadd_sd), C(fmsub_sd),
  C(asm_vpermq_1b), C(asm_vpermq_93), C(asm_vpermpd_b1), C(asm_vperm2i128_31), C(asm_vperm2i128_28), C(asm_vbroadcasti128_m),
  C(asm_vpbroadcastq_xm), C(asm_vpbroadcastq_ym), C(asm_vpbroadcastq_yx), C(asm_vpbroadcastd_xm), C(asm_vpbroadcastd_ym), C(asm_vpbroadcastw_xm), C(asm_vpbroadcastb_xm),
  C(asm_vfmadd231ss_rr), C(asm_vfmadd231ss_rm), C(asm_vfmsub231ss), C(asm_vfnmadd231ss), C(asm_vfmadd231sd), C(asm_vfmadd231ps_y), C(asm_vfmadd231ps_ym),
  C(fmadd_ps_x), C(fmsub_ps_x), C(fmadd_ps_y), C(fnmadd_ps_y), C(fmadd_pd_y), C(fmsub_pd_y), C(fnmsub_pd_y),
};

static void mkint(V *v, uint64_t *s) { for (int i = 0; i < 4; i++) s64(v, i, t_lcg(s)); }
/* small finite floats/doubles: k/64 with k in [-512,511]; shifts counts: mix in/out of range */
static void mkfloat(V *v, uint64_t *s) {
  for (int i = 0; i < 8; i++) sf(v, i, (float)((int)(t_lcg(s) & 1023) - 512) / 64.0f);
}
static void mkdouble(V *v, uint64_t *s) {
  for (int i = 0; i < 4; i++) sd(v, i, (double)((int)(t_lcg(s) & 0xffff) - 32768) / 1024.0);
}

int main(void) {
  uint64_t s = 0x5eed, total = T_FNV0; int bad = 0;
  static _Alignas(32) V in[3], out_hw, out_md;
  for (unsigned ci = 0; ci < sizeof cases / sizeof cases[0]; ci++) {
    const Case *k = &cases[ci];
    uint64_t h = T_FNV0;
    int isf = k->name[0] == 'f' || strstr(k->name, "asm_vf"), isd = isf && (strstr(k->name, "_sd") || strstr(k->name, "_pd"));
    for (int it = 0; it < 24; it++) {
      for (int j = 0; j < 3; j++) {
        if (isd) mkdouble(&in[j], &s); else if (isf) mkfloat(&in[j], &s); else mkint(&in[j], &s);
      }
      if (!isf && it % 3 == 1) for (int i = 0; i < 8; i++) s32(&in[1], i, g32(&in[1], i) & 63); /* shift counts / permute idx / small */
      if (!isf && it % 3 == 2) for (int i = 0; i < 8; i++) s32(&in[1], i, g32(&in[1], i) & 7);
      memset(&out_hw, 0, 32); memset(&out_md, 0, 32);
      k->hw(&out_hw, &in[0], &in[1], &in[2]);
      k->md(&out_md, &in[0], &in[1], &in[2]);
      if (memcmp(&out_hw, &out_md, 32)) {
        if (bad++ < 12) { t_str("MISMATCH "); t_str(k->name); t_dec(" iter", it); for (int w = 0; w < 4; w++) { t_hex(" hw", g64(&out_hw, w)); t_hex(" md", g64(&out_md, w)); } }
      }
      h = t_fnv(h, &out_hw, 32);
    }
    t_str(k->name); t_hex("  fnv", h);
    total = t_fnv(total, &h, 8);
  }
  /* one fully printed sample per family so a diff is readable */
  for (int j = 0; j < 3; j++) mkint(&in[j], &s);
  static const char *dn[] = {"permq_1b", "permd", "sllvd_y", "mulhuw_y"};
  for (unsigned d = 0; d < 4; d++) for (unsigned ci = 0; ci < sizeof cases / sizeof cases[0]; ci++) if (!strcmp(cases[ci].name, dn[d])) {
    for (int i = 0; i < 8; i++) s32(&in[1], i, g32(&in[1], i) & 7);
    cases[ci].hw(&out_hw, &in[0], &in[1], &in[2]);
    t_str(dn[d]); for (int w = 0; w < 4; w++) t_hex(" q", g64(&out_hw, w));
  }
  t_dec("model_mismatches", bad);
  t_hex("total", total);
  return bad != 0;
}
