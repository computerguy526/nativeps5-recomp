// SPDX-License-Identifier: GPL-2.0-or-later
//
// SSE4.2 string compares: (V)PCMPISTRI and (V)PCMPESTRI, after the Intel SDM (vol. 2, 4.1
// "Imm8 control byte operation for PCMPESTRI/PCMPESTRM/PCMPISTRI/PCMPISTRM").
//
// Operands, in the SDM's words: "xmm1" is the first (register) operand, "xmm2/m128" the second.
// BoolRes[j][i] compares element j of xmm2/m128 with element i of xmm1, and every bit j of
// IntRes1/IntRes2 (hence the index returned in ECX) is a position in xmm2/m128:
//   equal any     (imm8[3:2]=0)  xmm1 is a character set, bit j: xmm2/m128[j] is in the set
//   ranges        (1)            xmm1 holds (lo, hi) pairs, bit j: xmm2/m128[j] is in a range
//   equal each    (2)            bit j: xmm2/m128[j] == xmm1[j]
//   equal ordered (3)            bit j: xmm1 (the needle) matches xmm2/m128 from position j on
// Elements past the end of a string are invalid and override the comparison (table 4-7).
// Flags: CF = IntRes2 != 0, ZF = xmm2/m128 is shorter than the register, SF = xmm1 is shorter,
// OF = IntRes2[0], AF = PF = 0.
//
// The file depends on recomp_context.h only, so the FEX oracle harness (cmd/fexasm) links it as is.

#include "recomp_context.h"

#include <stdint.h>
#include <string.h>

// Element i of a 16-byte operand, zero- or sign-extended as imm8[1:0] says.
static int32_t pcmpstr_elem(const uint8_t *p, int i, int is_word, int is_signed) {
  if (is_word) {
    uint16_t v;
    memcpy(&v, p + 2 * i, 2);
    return is_signed ? (int32_t)(int16_t)v : (int32_t)v;
  }
  return is_signed ? (int32_t)(int8_t)p[i] : (int32_t)p[i];
}

// Implicit length: the index of the first zero element, or the element count.
static int pcmpstr_implicit_len(const uint8_t *p, int is_word, int n) {
  for (int i = 0; i < n; i++) {
    if (pcmpstr_elem(p, i, is_word, 0) == 0) return i;
  }
  return n;
}

// Explicit length: |EAX| / |EDX| saturated to the element count.
static int pcmpstr_explicit_len(uint64_t reg, int n) {
  int64_t v = (int64_t)(int32_t)reg;
  if (v < 0) v = -v;
  return v > n ? n : (int)v;
}

// op2 = xmm2/m128 (len2 valid elements), op1 = xmm1 (len1 valid elements).
static void pcmpstr_core(GuestContext *ctx, const uint8_t *op2, const uint8_t *op1, int len2, int len1,
                         uint8_t imm8) {
  const int is_word = imm8 & 1;
  const int is_signed = (imm8 >> 1) & 1;
  const int agg = (imm8 >> 2) & 3;
  const int pol = (imm8 >> 4) & 3;
  const int n = is_word ? 8 : 16;
  const uint32_t all = (1u << n) - 1;

  int32_t a[16], b[16]; // a = xmm1, b = xmm2/m128
  for (int i = 0; i < n; i++) {
    a[i] = pcmpstr_elem(op1, i, is_word, is_signed);
    b[i] = pcmpstr_elem(op2, i, is_word, is_signed);
  }

  uint32_t res1 = 0;
  for (int j = 0; j < n; j++) {
    int bit = 0;
    switch (agg) {
    case 0: // equal any: either side invalid -> 0
      if (j < len2) {
        for (int i = 0; i < len1; i++) {
          if (b[j] == a[i]) { bit = 1; break; }
        }
      }
      break;
    case 1: // ranges: either side invalid -> 0; a pair needs both of its bounds
      if (j < len2) {
        for (int i = 0; i + 1 < len1; i += 2) {
          if (b[j] >= a[i] && b[j] <= a[i + 1]) { bit = 1; break; }
        }
      }
      break;
    case 2: // equal each: both invalid -> 1, one invalid -> 0
      if (j < len2 && j < len1) bit = b[j] == a[j];
      else bit = j >= len2 && j >= len1;
      break;
    case 3: // equal ordered: xmm1 invalid -> 1, xmm1 valid and xmm2/m128 invalid -> 0
      bit = 1;
      for (int i = 0, k = j; k < n; i++, k++) {
        if (i >= len1) break;                         // the needle is exhausted: all later terms are 1
        if (k >= len2 || b[k] != a[i]) { bit = 0; break; }
      }
      break;
    }
    res1 |= (uint32_t)bit << j;
  }

  uint32_t res2 = res1;
  if (pol == 1) {
    res2 = ~res1 & all;
  } else if (pol == 3) { // negate only the positions that are valid in xmm2/m128
    uint32_t valid = len2 >= n ? all : (1u << len2) - 1;
    res2 = res1 ^ valid;
  }

  int index = n;
  if (res2) index = (imm8 & 0x40) ? 31 - __builtin_clz(res2) : __builtin_ctz(res2);
  ctx->rcx = (uint64_t)index;

  ctx->cf = res2 != 0;
  ctx->zf = len2 < n;
  ctx->sf = len1 < n;
  ctx->of = res2 & 1;
  ctx->af = 0;
  ctx->pf = 1; // lazy PF byte: odd parity = PF clear (recomp_context.h)
}

// (V)PCMPISTRI xmm1, xmm2/m128, imm8: src2_ptr = xmm2/m128, src1_ptr = xmm1.
void recomp_vpcmpistri_impl(GuestContext *ctx, const void *src2_ptr, const void *src1_ptr, uint8_t imm8) {
  uint8_t op2[16], op1[16];
  memcpy(op2, src2_ptr, 16);
  memcpy(op1, src1_ptr, 16);
  const int is_word = imm8 & 1, n = is_word ? 8 : 16;
  pcmpstr_core(ctx, op2, op1, pcmpstr_implicit_len(op2, is_word, n), pcmpstr_implicit_len(op1, is_word, n), imm8);
}

// (V)PCMPESTRI: the length of xmm1 is |EAX|, of xmm2/m128 |EDX| (ctx holds the guest registers).
void recomp_vpcmpe_stri_impl(GuestContext *ctx, const void *src2_ptr, const void *src1_ptr, uint8_t imm8) {
  uint8_t op2[16], op1[16];
  memcpy(op2, src2_ptr, 16);
  memcpy(op1, src1_ptr, 16);
  const int n = (imm8 & 1) ? 8 : 16;
  pcmpstr_core(ctx, op2, op1, pcmpstr_explicit_len(ctx->rdx, n), pcmpstr_explicit_len(ctx->rax, n), imm8);
}
