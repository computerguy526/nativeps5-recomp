/* Jump tables: dense switches of several widths, signed/unsigned index, a switch
 * on a 16-bit value, and a computed-goto bytecode interpreter (label-as-value tables). */
#include "ps5test.h"

static T_NOINLINE uint64_t sw48(unsigned x, uint64_t acc) {
  switch (x) {
  case 0: return acc * 3 + 1;       case 1: return acc ^ 0x1111;      case 2: return acc - 77;
  case 3: return acc << 3;          case 4: return acc >> 2;          case 5: return acc * acc;
  case 6: return ~acc;              case 7: return acc + 0x70707;     case 8: return (acc << 9) | (acc >> 55);
  case 9: return acc / 7;           case 10: return acc % 1009;       case 11: return acc ^ (acc >> 13);
  case 12: return acc * 0x9e3779b97f4a7c15ULL; case 13: return acc + x; case 14: return acc - x * 3;
  case 15: return __builtin_bswap64(acc); case 16: return acc & 0xf0f0f0f0f0f0f0f0ULL; case 17: return acc | 0x0f0f;
  case 18: return acc + 18;         case 19: return acc * 19;         case 20: return acc ^ 20;
  case 21: return (acc >> 21) + 21; case 22: return acc - 22;         case 23: return acc * 23 + 23;
  case 24: return acc ^ 0x2424;     case 25: return __builtin_popcountll(acc) + acc; case 26: return acc >> 26;
  case 27: return acc + 27 * 27;    case 28: return acc * 28;         case 29: return ~(acc ^ 29);
  case 30: return acc - 0x3030;     case 31: return acc << 31;        case 32: return acc * 32 + 1;
  case 33: return acc ^ 0x3333;     case 34: return acc + 34;         case 35: return acc - 35;
  case 36: return acc * 36;         case 37: return acc >> 37;        case 38: return acc ^ 38;
  case 39: return acc + 0x39393939; case 40: return acc * 40 - 1;     case 41: return acc ^ 41;
  case 42: return acc + 42;         case 43: return ~acc + 43;        case 44: return acc / 44;
  case 45: return acc * 45;         case 46: return acc - 46;         case 47: return acc ^ 0x4747;
  default: return acc + 0xdead;
  }
}
static T_NOINLINE int sw_signed(int x) {
  switch (x) {
  case -5: return 11; case -4: return 22; case -3: return 33; case -2: return 44; case -1: return 55;
  case 0: return 66; case 1: return 77; case 2: return 88; case 3: return 99; case 4: return 110; case 6: return 121;
  case 7: return 132; case 8: return 143;
  default: return -1;
  }
}
static T_NOINLINE uint32_t sw_u16(uint16_t x) {
  switch (x) {
  case 1000: return 1; case 1001: return 10; case 1002: return 100; case 1003: return 1000; case 1005: return 10000;
  case 1006: return 3; case 1007: return 33; case 1008: return 333; case 1010: return 3333;
  default: return 0;
  }
}
static T_NOINLINE int sw_chars(char c) {
  switch (c) {
  case 'a': case 'e': case 'i': case 'o': case 'u': return 1;
  case 'b': case 'c': case 'd': return 2;
  case 'x': case 'y': case 'z': return 3;
  case ' ': return 4; case '0'...'9': return 5;
  default: return 0;
  }
}

enum { OP_PUSH, OP_ADD, OP_MUL, OP_DUP, OP_SWAP, OP_DEC, OP_JNZ, OP_HALT, OP_XOR };
static T_NOINLINE uint64_t run_vm(const uint8_t *code) {
  static const void *const tbl[] = {&&l_push, &&l_add, &&l_mul, &&l_dup, &&l_swap, &&l_dec, &&l_jnz, &&l_halt, &&l_xor};
  uint64_t st[32]; int sp = 0; const uint8_t *pc = code;
#define NEXT goto *tbl[*pc++]
  NEXT;
l_push: st[sp++] = *pc++; NEXT;
l_add: sp--; st[sp - 1] += st[sp]; NEXT;
l_mul: sp--; st[sp - 1] *= st[sp]; NEXT;
l_dup: st[sp] = st[sp - 1]; sp++; NEXT;
l_swap: { uint64_t t = st[sp - 1]; st[sp - 1] = st[sp - 2]; st[sp - 2] = t; } NEXT;
l_dec: st[sp - 1]--; NEXT;
l_xor: sp--; st[sp - 1] ^= st[sp] << 7; NEXT;
l_jnz: { int off = *pc++; if (st[sp - 1]) pc = code + off; } NEXT;
l_halt: return st[0] ^ st[1];
}

int main(void) {
  uint64_t acc = 12345, h = T_FNV0;
  for (unsigned i = 0; i < 4000; i++) { acc = sw48((i * 7 + (acc & 3)) % 52, acc + i); }
  t_hex("sw48", acc);
  for (int x = -8; x < 14; x++) { int r = sw_signed(x); h = t_fnv(h, &r, 4); }
  t_hex("sw_signed", h); h = T_FNV0;
  for (unsigned x = 995; x < 1015; x++) { uint32_t r = sw_u16((uint16_t)x); h = t_fnv(h, &r, 4); }
  t_hex("sw_u16", h); h = T_FNV0;
  const char *txt = "the quick brown fox 0123456789 jumps over lazy dogs";
  for (const char *p = txt; *p; p++) { int r = sw_chars(*p); h = t_fnv(h, &r, 4); }
  t_hex("sw_chars", h);
  /* acc=1,counter=5000: loop { acc = acc*3 + ... } */
  static const uint8_t prog[] = {
    OP_PUSH, 1, OP_PUSH, 100,            /* acc counter */
    /*4*/ OP_SWAP, OP_PUSH, 3, OP_MUL, OP_PUSH, 7, OP_ADD, OP_SWAP, OP_DEC, OP_JNZ, 4,
    OP_PUSH, 5, OP_XOR, OP_HALT};
  t_hex("vm", run_vm(prog));
  return 0;
}
