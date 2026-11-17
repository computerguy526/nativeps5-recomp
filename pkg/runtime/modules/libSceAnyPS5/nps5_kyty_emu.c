// SPDX-License-Identifier: GPL-2.0-or-later
//
// Emulation of one trapped arm64 load or store. See nps5_kyty_emu.h.

#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#include "nps5_kyty_emu.h"

#include <string.h>

#if defined(__aarch64__) && defined(__APPLE__)
#include <mach/mach.h>
#include <signal.h>
#include <sys/ucontext.h>
#endif

typedef enum { K_SINGLE, K_PAIR, K_ZVA, K_VMULTI, K_VLANE, K_VREP } Kind;

typedef struct {
  Kind kind;
  bool is_store, vreg;
  uint8_t rt, rt2, rn;
  uint8_t bytes;    // per register
  uint8_t sext;     // 0 none, 32 sign-extend to 32 bits (upper half of X zero), 64 sign-extend to 64 bits
  bool writeback;
  bool ordered;     // an acquire/release form: a full barrier follows the emulated access
  // Advanced SIMD structure forms (K_VMULTI: LD1-4/ST1-4 multiple structures; K_VLANE: one lane per register;
  // K_VREP: LD1R-LD4R). Registers rt, rt+1, ... (mod 32).
  uint8_t rpt, selem; // K_VMULTI: repetitions x registers per structure; lane/replicate forms: selem registers
  uint8_t ebytes;     // bytes per element
  uint8_t elems;      // K_VMULTI: elements per register; K_VREP: lanes filled per register
  uint8_t index;      // K_VLANE: the lane
  uint8_t total;      // bytes accessed by a structure form
  uint64_t ea;      // first byte accessed
  uint64_t new_base; // base register after the instruction (writeback forms)
} Decoded;

static inline int64_t sext(uint64_t v, unsigned bits) { return (int64_t)(v << (64 - bits)) >> (64 - bits); }
static inline uint64_t base_reg(const uint64_t x[31], uint64_t sp, unsigned n) { return n == 31 ? sp : x[n]; }

static bool decode(uint32_t insn, const uint64_t x[31], uint64_t sp, Decoded *d) {
  memset(d, 0, sizeof *d);
  if ((insn & 0xFFFFFFE0u) == 0xD50B7420u) { // DC ZVA, Xt: zero the 64-byte block holding Xt
    d->kind = K_ZVA;
    d->is_store = true;
    d->rt = insn & 31;
    d->bytes = 64;
    d->ea = (d->rt == 31 ? 0 : x[d->rt]) & ~63ull;
    return true;
  }
  if ((insn & 0x0A000000u) != 0x08000000u) return false; // not the loads-and-stores class
  // Ordered loads and stores without exclusivity. The recompiler emits them for x86 loads and stores that need TSO
  // order (LDAPR/LDAPUR, STLR/STLUR); they access memory like LDR/STR. The barrier they imply is issued after the
  // emulated access (d->ordered).
  if ((insn & 0x3FA00000u) == 0x08800000u) { // LDAR / STLR / LDLAR / STLLR: [Xn]
    d->kind = K_SINGLE;
    d->ordered = true;
    d->rn = (insn >> 5) & 31;
    d->rt = insn & 31;
    d->bytes = (uint8_t)(1u << (insn >> 30));
    d->is_store = !((insn >> 22) & 1);
    d->ea = base_reg(x, sp, d->rn);
    return true;
  }
  if ((insn & 0x3FE0FC00u) == 0x38A0C000u) { // LDAPR: [Xn]
    d->kind = K_SINGLE;
    d->ordered = true;
    d->rn = (insn >> 5) & 31;
    d->rt = insn & 31;
    d->bytes = (uint8_t)(1u << (insn >> 30));
    d->ea = base_reg(x, sp, d->rn);
    return true;
  }
  if ((insn & 0x3F200C00u) == 0x19000000u) { // LDAPUR / STLUR and the sign-extending loads: [Xn, #imm9]
    const unsigned size = insn >> 30, opc = (insn >> 22) & 3;
    d->kind = K_SINGLE;
    d->ordered = true;
    d->rn = (insn >> 5) & 31;
    d->rt = insn & 31;
    d->bytes = (uint8_t)(1u << size);
    if (opc == 0) d->is_store = true;
    else if (opc == 2) { if (size == 3) return false; d->sext = 64; }
    else if (opc == 3) { if (size >= 2) return false; d->sext = 32; }
    d->ea = base_reg(x, sp, d->rn) + (uint64_t)sext((insn >> 12) & 0x1ff, 9);
    return true;
  }
  const unsigned cls = (insn >> 27) & 7; // bits 29:27
  const bool v = (insn >> 26) & 1;
  d->vreg = v;
  d->rn = (insn >> 5) & 31;
  d->rt = insn & 31;
  const uint64_t base = base_reg(x, sp, d->rn);
  if (cls == 1 && v) { // Advanced SIMD load/store structures (the recompiler's LD1R = x86 broadcast loads, LD1/ST1)
    if (insn >> 31) return false;
    const bool q = (insn >> 30) & 1, post = (insn >> 23) & 1, l = (insn >> 22) & 1;
    const unsigned rm = (insn >> 16) & 31, opcode = (insn >> 12) & 15, size = (insn >> 10) & 3;
    d->is_store = !l;
    d->ea = base;
    if (!((insn >> 24) & 1)) { // multiple structures
      if ((insn >> 21) & 1) return false;
      if (!post && rm != 0) return false;
      switch (opcode) {
      case 0: d->rpt = 1; d->selem = 4; break;  // LD4 / ST4
      case 2: d->rpt = 4; d->selem = 1; break;  // LD1 / ST1, four registers
      case 4: d->rpt = 1; d->selem = 3; break;  // LD3 / ST3
      case 6: d->rpt = 3; d->selem = 1; break;  // LD1 / ST1, three registers
      case 7: d->rpt = 1; d->selem = 1; break;  // LD1 / ST1, one register
      case 8: d->rpt = 1; d->selem = 2; break;  // LD2 / ST2
      case 10: d->rpt = 2; d->selem = 1; break; // LD1 / ST1, two registers
      default: return false;
      }
      if (size == 3 && !q && d->selem != 1) return false; // reserved
      d->kind = K_VMULTI;
      d->ebytes = (uint8_t)(1u << size);
      d->elems = (uint8_t)((q ? 16u : 8u) >> size);
      d->total = (uint8_t)(d->rpt * d->selem * (q ? 16u : 8u));
    } else { // single structure: one lane, or load and replicate
      const unsigned r = (insn >> 21) & 1, s = (insn >> 12) & 1;
      if (!post && rm != 0) return false;
      unsigned scale = opcode >> 2, index = 0;
      d->selem = (uint8_t)((((opcode >> 1) & 1) << 1 | r) + 1);
      d->kind = K_VLANE;
      switch (scale) {
      case 0: index = (unsigned)q << 3 | s << 2 | size; break;
      case 1:
        if (size & 1) return false;
        index = (unsigned)q << 2 | s << 1 | size >> 1;
        break;
      case 2:
        if (size & 2) return false;
        if (!(size & 1)) index = (unsigned)q << 1 | s;
        else {
          if (s) return false;
          index = q;
          scale = 3;
        }
        break;
      default: // LD1R..LD4R
        if (!l || s) return false;
        scale = size;
        d->kind = K_VREP;
        d->elems = (uint8_t)((q ? 16u : 8u) >> scale);
        break;
      }
      d->ebytes = (uint8_t)(1u << scale);
      d->index = (uint8_t)index;
      d->total = (uint8_t)(d->selem * d->ebytes);
    }
    if (post) {
      d->writeback = true;
      d->new_base = base + (rm == 31 ? d->total : x[rm]);
    }
    return true;
  }
  if (cls == 7) { // load/store register
    const unsigned size = insn >> 30, opc = (insn >> 22) & 3;
    d->kind = K_SINGLE;
    if (!v) {
      d->bytes = (uint8_t)(1u << size);
      switch (opc) {
      case 0: d->is_store = true; break;
      case 1: break;
      case 2:
        if (size == 3) return false; // PRFM
        d->sext = 64;
        break;
      default:
        if (size >= 2) return false;
        d->sext = 32;
        break;
      }
    } else if (opc < 2) {
      d->bytes = (uint8_t)(1u << size);
      d->is_store = opc == 0;
    } else {
      if (size != 0) return false;
      d->bytes = 16;
      d->is_store = opc == 2;
    }
    unsigned scale = 0;
    while ((1u << scale) < d->bytes) scale++;
    if ((insn >> 24) & 1) { // unsigned scaled immediate
      d->ea = base + ((uint64_t)((insn >> 10) & 0xfff) << scale);
      return true;
    }
    if (!((insn >> 21) & 1)) { // 9-bit signed immediate: unscaled, post-index, pre-index
      const int64_t imm = sext((insn >> 12) & 0x1ff, 9);
      switch ((insn >> 10) & 3) {
      case 0: d->ea = base + (uint64_t)imm; return true;
      case 1: d->ea = base; d->writeback = true; d->new_base = base + (uint64_t)imm; break;
      case 3: d->ea = base + (uint64_t)imm; d->writeback = true; d->new_base = d->ea; break;
      default: return false; // unprivileged forms
      }
      // Writeback with the base as the transferred general register is unpredictable: not emulated.
      return v || d->rt != d->rn || d->rn == 31;
    }
    if (((insn >> 10) & 3) != 2) return false; // atomic memory operations, pointer-authenticated loads
    const unsigned rm = (insn >> 16) & 31, option = (insn >> 13) & 7, s = (insn >> 12) & 1;
    uint64_t off = rm == 31 ? 0 : x[rm];
    switch (option) {
    case 2: off = (uint32_t)off; break;                 // UXTW
    case 3: break;                                      // LSL
    case 6: off = (uint64_t)(int64_t)(int32_t)off; break; // SXTW
    case 7: break;                                      // SXTX
    default: return false;
    }
    d->ea = base + (off << (s ? scale : 0));
    return true;
  }
  if (cls == 5) { // load/store pair
    const unsigned opc = insn >> 30, mode = (insn >> 23) & 3;
    const bool l = (insn >> 22) & 1;
    d->kind = K_PAIR;
    d->rt2 = (insn >> 10) & 31;
    d->is_store = !l;
    if (!v) {
      if (opc == 0) d->bytes = 4;
      else if (opc == 2) d->bytes = 8;
      else if (opc == 1 && l) { d->bytes = 4; d->sext = 64; } // LDPSW
      else return false;
    } else {
      if (opc > 2) return false;
      d->bytes = (uint8_t)(4u << opc);
    }
    const int64_t off = sext((insn >> 15) & 0x7f, 7) * (int64_t)d->bytes;
    switch (mode) {
    case 1: d->ea = base; d->writeback = true; d->new_base = base + (uint64_t)off; break;
    case 3: d->ea = base + (uint64_t)off; d->writeback = true; d->new_base = d->ea; break;
    default: d->ea = base + (uint64_t)off; break; // offset, no-allocate
    }
    if (d->writeback && !v && d->rn != 31 && (d->rt == d->rn || d->rt2 == d->rn)) return false; // unpredictable
    if (l && !v && d->rt == d->rt2) return false;                                                // unpredictable
    return true;
  }
  return false;
}

static uint32_t access_size(const Decoded *d) {
  if (d->kind >= K_VMULTI) return d->total;
  return d->kind == K_PAIR ? 2u * d->bytes : d->bytes;
}

bool nps5_a64_decode_access(uint32_t insn, const uint64_t x[31], uint64_t sp, Nps5A64Access *out) {
  Decoded d;
  if (!decode(insn, x, sp, &d)) return false;
  out->addr = d.ea;
  out->size = access_size(&d);
  out->is_store = d.is_store;
  return true;
}

#if defined(__aarch64__) && defined(__APPLE__)

static void load_gpr(uint64_t x[31], unsigned rt, const uint8_t *p, unsigned bytes, unsigned sx) {
  uint64_t val = 0;
  memcpy(&val, p, bytes);
  if (sx) {
    val = (uint64_t)sext(val, bytes * 8);
    if (sx == 32) val = (uint32_t)val;
  }
  if (rt != 31) x[rt] = val;
}

bool nps5_a64_fault_access(void *ucontext, Nps5A64Access *out) {
  ucontext_t *uc = (ucontext_t *)ucontext;
  if (!uc || !uc->uc_mcontext || !out) return false;
  _STRUCT_ARM_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
  uint64_t x[31];
  for (int i = 0; i < 29; i++) x[i] = ss->__x[i];
  x[29] = arm_thread_state64_get_fp(*ss);
  x[30] = (uint64_t)arm_thread_state64_get_lr(*ss);
  uint32_t insn;
  memcpy(&insn, (const void *)(uintptr_t)arm_thread_state64_get_pc(*ss), 4);
  return nps5_a64_decode_access(insn, x, arm_thread_state64_get_sp(*ss), out);
}

bool nps5_a64_emulate_access(void *ucontext, uint64_t fault_addr, Nps5A64Redirect redirect, void *user) {
  ucontext_t *uc = (ucontext_t *)ucontext;
  if (!uc || !uc->uc_mcontext || !redirect) return false;
  _STRUCT_ARM_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
  _STRUCT_ARM_NEON_STATE64 *ns = &uc->uc_mcontext->__ns;
  const uint64_t pc = arm_thread_state64_get_pc(*ss);
  uint64_t x[31];
  for (int i = 0; i < 29; i++) x[i] = ss->__x[i];
  x[29] = arm_thread_state64_get_fp(*ss);
  x[30] = (uint64_t)arm_thread_state64_get_lr(*ss);
  const uint64_t sp = arm_thread_state64_get_sp(*ss);
  uint32_t insn;
  memcpy(&insn, (const void *)(uintptr_t)pc, 4); // the instruction that trapped is mapped and readable
  Decoded d;
  if (!decode(insn, x, sp, &d)) return false;
  const uint32_t size = access_size(&d);
  if (fault_addr < d.ea || fault_addr - d.ea >= size) return false; // not what trapped (decoder and CPU disagree): hands off
  const Nps5A64Access acc = {d.ea, size, d.is_store};
  uint8_t *p = (uint8_t *)redirect(user, &acc);
  if (!p) return false;

  // A release store (STLR/STLUR) is ordered after everything this thread did before it: the plain store below
  // needs the barrier in front as well as the one after it.
  if (d.ordered && d.is_store) __sync_synchronize();
  switch (d.kind) {
  case K_ZVA: memset(p, 0, 64); break;
  case K_VMULTI: { // element e of each register in turn; interleaved over selem registers
    uint8_t reg[4][16];
    const unsigned regs = (unsigned)d.rpt * d.selem;
    for (unsigned i = 0; i < regs; i++) {
      if (d.is_store) memcpy(reg[i], &ns->__v[(d.rt + i) & 31], 16);
      else memset(reg[i], 0, 16);
    }
    uint8_t *q = p;
    for (unsigned r = 0; r < d.rpt; r++)
      for (unsigned e = 0; e < d.elems; e++)
        for (unsigned s = 0; s < d.selem; s++, q += d.ebytes) {
          if (d.is_store) memcpy(q, reg[r + s] + e * d.ebytes, d.ebytes);
          else memcpy(reg[r + s] + e * d.ebytes, q, d.ebytes);
        }
    if (!d.is_store)
      for (unsigned i = 0; i < regs; i++) memcpy(&ns->__v[(d.rt + i) & 31], reg[i], 16);
    break;
  }
  case K_VLANE:
    for (unsigned s = 0; s < d.selem; s++) {
      uint8_t reg[16];
      memcpy(reg, &ns->__v[(d.rt + s) & 31], 16);
      if (d.is_store) memcpy(p + s * d.ebytes, reg + d.index * d.ebytes, d.ebytes);
      else {
        memcpy(reg + d.index * d.ebytes, p + s * d.ebytes, d.ebytes); // the other lanes keep their bits
        memcpy(&ns->__v[(d.rt + s) & 31], reg, 16);
      }
    }
    break;
  case K_VREP:
    for (unsigned s = 0; s < d.selem; s++) {
      uint8_t reg[16] = {0};
      for (unsigned e = 0; e < d.elems; e++) memcpy(reg + e * d.ebytes, p + s * d.ebytes, d.ebytes);
      memcpy(&ns->__v[(d.rt + s) & 31], reg, 16);
    }
    break;
  case K_SINGLE:
  case K_PAIR:
    for (unsigned k = 0; k < (d.kind == K_PAIR ? 2u : 1u); k++) {
      const unsigned r = k ? d.rt2 : d.rt;
      uint8_t *q = p + (size_t)k * d.bytes;
      if (d.vreg) {
        if (d.is_store) memcpy(q, &ns->__v[r], d.bytes);
        else {
          __uint128_t val = 0;
          memcpy(&val, q, d.bytes);
          ns->__v[r] = val;
        }
      } else if (d.is_store) {
        const uint64_t val = r == 31 ? 0 : x[r];
        memcpy(q, &val, d.bytes);
      } else {
        load_gpr(x, r, q, d.bytes, d.sext);
      }
    }
    break;
  }
  if (d.ordered) __sync_synchronize();
  for (int i = 0; i < 29; i++) ss->__x[i] = x[i];
  arm_thread_state64_set_fp(*ss, x[29]);
  arm_thread_state64_set_lr_fptr(*ss, (void *)(uintptr_t)x[30]);
  if (d.writeback) {
    if (d.rn == 31) arm_thread_state64_set_sp(*ss, d.new_base);
    else if (d.rn < 29) ss->__x[d.rn] = d.new_base;
    else if (d.rn == 29) arm_thread_state64_set_fp(*ss, d.new_base);
    else arm_thread_state64_set_lr_fptr(*ss, (void *)(uintptr_t)d.new_base);
  }
  arm_thread_state64_set_pc_fptr(*ss, (void *)(uintptr_t)(pc + 4));
  return true;
}

#else

bool nps5_a64_emulate_access(void *ucontext, uint64_t fault_addr, Nps5A64Redirect redirect, void *user) {
  (void)ucontext; (void)fault_addr; (void)redirect; (void)user;
  return false;
}
bool nps5_a64_fault_access(void *ucontext, Nps5A64Access *out) {
  (void)ucontext; (void)out;
  return false;
}

#endif
