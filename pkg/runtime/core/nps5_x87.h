// SPDX-License-Identifier: GPL-2.0-or-later
//
// x87 80-bit extended precision (translate option x87=f80, the default; docs/m2/x87-f80.md).
//
// The lifter (pkg/lifter/arm64_lifter_x87f80.go) lowers each x87 instruction to one call of
// _recomp_x87 (the bridge in pkg/emitter/asm_x87.go), which saves the live host state and calls
// recomp_x87_impl below. The instruction is passed as one 32-bit op word and one argument (a host
// pointer to the memory operand, or a value). The numbers below are mirrored in the lifter
// (pkg/lifter/arm64_lifter_x87f80.go, x87K*); a Go test parses this header to keep them equal.
//
// State (GuestContext): fpu80[8] hold the 80-bit registers by physical number, fpu_top the TOP
// field, fpu_cw the control word, fpu_sw the status word without TOP (the helpers keep the TOP
// bits of fpu_sw zero and merge fpu_top when the word is read), fpu_valid bit n = register n is
// not empty (the tag word is derived from it and from the value).

#ifndef NPS5_X87_H
#define NPS5_X87_H

#include <stdint.h>

// Op word: kind | a<<8 | b<<11 | pop<<14 | pop2<<15 | sz<<16 | aux<<18.
#define X87_KIND(op) ((op) & 0xffu)
#define X87_A(op) (((op) >> 8) & 7u)
#define X87_B(op) (((op) >> 11) & 7u)
#define X87_POP(op) (((op) >> 14) & 1u)
#define X87_POP2(op) (((op) >> 15) & 1u)
#define X87_SZ(op) (((op) >> 16) & 3u)
#define X87_AUX(op) (((op) >> 18) & 15u)

#define X87_K_FLD_M 1     // sz 0 m32, 1 m64, 2 m80; arg = host pointer
#define X87_K_FLD_ST 2    // a = i
#define X87_K_FLD_CONST 3 // a: 0 1.0, 1 log2(10), 2 log2(e), 3 pi, 4 log10(2), 5 ln(2), 6 +0
#define X87_K_FILD 4      // sz 0 m16, 1 m32, 2 m64
#define X87_K_FST_M 5     // sz as FLD_M, pop
#define X87_K_FST_ST 6    // a = i, pop
#define X87_K_FIST 7      // sz as FILD, pop, aux 1 = truncate (FISTTP)
#define X87_K_ARITH_ST 8  // aux = operation, a = destination, b = source, pop
#define X87_K_ARITH_M 9   // aux = operation, sz 0 m32, 1 m64, 2 i16, 3 i32
#define X87_K_CHS 10
#define X87_K_ABS 11
#define X87_K_SQRT 12
#define X87_K_RNDINT 13
#define X87_K_SCALE 14
#define X87_K_XTRACT 15
#define X87_K_PREM 16     // aux 1 = FPREM1
#define X87_K_COM_ST 17   // a = i, aux 1 = unordered (FUCOM), pop, pop2 (FCOMPP / FUCOMPP)
#define X87_K_COM_M 18    // sz 0 m32, 1 m64, 2 i16, 3 i32, pop
#define X87_K_COMI 19     // a = i, aux 1 = unordered, pop; returns NZCV as after an arm64 fcmp
#define X87_K_TST 20
#define X87_K_XAM 21
#define X87_K_XCH 22      // a = i
#define X87_K_LDCW 23     // arg = control word
#define X87_K_STCW 24     // returns the control word
#define X87_K_STSW 25     // returns the status word with TOP
#define X87_K_NINIT 26
#define X87_K_NCLEX 27
#define X87_K_CMOV 28     // a = i; the lifter calls it only when the condition holds
#define X87_K_FREE 29     // a = i, pop (FFREEP)
#define X87_K_DECSTP 30
#define X87_K_INCSTP 31
#define X87_K_STENV 32    // arg = host pointer to the environment; sz 1 = 16-bit operand size (14 bytes)
#define X87_K_LDENV 33
#define X87_K_SAVE 34     // arg = host pointer to the 108-byte state (FNSAVE: environment + ST0-7); sz as STENV
#define X87_K_RSTOR 35
#define X87_K_FXSAVE 36   // x87 part of the 512-byte FXSAVE image (FCW, FSW, FTW, ST0-7)
#define X87_K_FXRSTOR 37
#define X87_K_BLD 38      // arg = host pointer to the 10-byte packed BCD
#define X87_K_BSTP 39

#define X87_ARITH_ADD 0
#define X87_ARITH_SUB 1
#define X87_ARITH_SUBR 2
#define X87_ARITH_MUL 3
#define X87_ARITH_DIV 4
#define X87_ARITH_DIVR 5

#ifdef __cplusplus
extern "C" {
#endif

struct GuestContext;
uint64_t recomp_x87_impl(struct GuestContext *ctx, uint32_t op, uint64_t arg);
// Batch entry: n (op word, argument) pairs of 16 bytes, run in order; returns the last result (the bridge's _recomp_x87_run).
uint64_t recomp_x87_run_impl(struct GuestContext *ctx, const uint64_t *pairs, uint64_t n);

// Lean entries (preserve_most, integer-only): execute the leading operations that need no SoftFloat; return
// {result of the last one, number executed}. The bridge falls back to the full helpers for the rest.
typedef struct {
  uint64_t ret, done;
} x87_fast_ret;
__attribute__((preserve_most)) x87_fast_ret recomp_x87_fast(struct GuestContext *ctx, uint32_t op, uint64_t arg);
__attribute__((preserve_most)) x87_fast_ret recomp_x87_run_fast(struct GuestContext *ctx, const uint64_t *pairs, uint64_t n);

#ifdef __cplusplus
}
#endif

#endif
