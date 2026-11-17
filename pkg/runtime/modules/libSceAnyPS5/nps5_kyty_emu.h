// SPDX-License-Identifier: GPL-2.0-or-later
//
// Emulation of one trapped arm64 load or store (Kyty GPU seam, docs/m3/kyty-seam.md "False sharing on 16 KiB pages").
//
// A 16 KiB host page stays inaccessible while one of its four 4 KiB tracker pages is access-watched (the GPU owns its
// bytes). A CPU access to one of the OTHER tracker pages of that host page is legal and must not disturb the watched
// one: reporting it to the library as a write to the watched page costs a GPU drain (milliseconds) and repeats every
// frame. Instead the trapped instruction is carried out here against the always-accessible alias of the same bytes and
// the thread continues behind it. One signal plus this is 2-3 us.
//
// Covered: LDR/STR and their byte/half/word/sign-extending forms with immediate (scaled, unscaled, pre/post-index)
// and register offsets, LDP/STP/LDPSW (all index forms), the same for SIMD&FP registers (B/H/S/D/Q), DC ZVA, and the
// ordered forms the recompiler emits for x86 TSO accesses (LDAR/STLR, LDAPR, LDAPUR/STLUR and their sign-extending
// loads; a full barrier follows), and the Advanced SIMD structure forms: LD1-LD4/ST1-ST4 of multiple structures, of
// one lane, and LD1R-LD4R, without and with post-index (the recompiler lowers x86 broadcast loads to LD1R).
// Not covered (the caller falls back): exclusives and atomic read-modify-writes (LDXR/STXR, CAS, LSE LDADD/SWP...),
// loads with pointer authentication, anything else.
//
// Self-contained: no runtime headers, so it is unit-tested alone against the CPU (tests/kyty_seam/emu_test.c).

#ifndef NPS5_KYTY_EMU_H
#define NPS5_KYTY_EMU_H

#include <stdbool.h>
#include <stdint.h>

typedef struct Nps5A64Access {
  uint64_t addr;   // first byte accessed (host address, as the instruction computes it)
  uint32_t size;   // bytes accessed (1..64)
  bool is_store;
} Nps5A64Access;

// Where the bytes of host [addr, addr+size) can be accessed instead (the alias), or NULL to refuse the emulation (the
// range is not wholly usable). Called once per emulated instruction, in the signal handler.
typedef void *(*Nps5A64Redirect)(void *user, const Nps5A64Access *access);

// Decodes the instruction at the faulting pc of `ucontext` (a ucontext_t of a data abort). When it is a covered load
// or store whose accessed range contains `fault_addr` and `redirect` accepts it: performs it on the redirected bytes,
// updates the registers (also base writeback) and advances pc. Returns true when the instruction was carried out.
// Returns false and changes nothing otherwise. Async-signal-safe.
bool nps5_a64_emulate_access(void *ucontext, uint64_t fault_addr, Nps5A64Redirect redirect, void *user);

// What the instruction at the faulting pc of `ucontext` accesses, without carrying it out. False when not covered.
bool nps5_a64_fault_access(void *ucontext, Nps5A64Access *out);

// The decoder alone (tests): fills what `insn` would access given the general registers x[0..30] and sp. False for an
// instruction that is not covered.
bool nps5_a64_decode_access(uint32_t insn, const uint64_t x[31], uint64_t sp, Nps5A64Access *out);

#endif
