#!/usr/bin/env python3
"""Generates pkg/lifter/arm64_fex_cases_test.go.

Each case is a FEX-Emu unit test (unittests/ASM/<src>, MIT, see NOTICE) rewritten for the
lifter's native exec harness: the 0xe0000000 data base becomes the exec window base, data that
the FEX test keeps after its code is loaded from the window, the final HLT is dropped, and the
RegData of the test becomes wantRegs. Instructions are assembled with llvm-mc and listed as hex;
nothing is executed.

    python3 tools/gen_fex_cases.py > pkg/lifter/arm64_fex_cases_test.go && gofmt -w pkg/lifter/arm64_fex_cases_test.go

Needs llvm-mc and llvm-objdump (LLVM_BIN, default /opt/homebrew/opt/llvm/bin).
"""
import os, re, struct, subprocess, tempfile

LLVM = os.environ.get('LLVM_BIN', '/opt/homebrew/opt/llvm/bin')


def encode(asm):
    """Intel-syntax x86-64 (labels allowed) -> Go string literals, one per instruction."""
    with tempfile.TemporaryDirectory() as d:
        src, obj = os.path.join(d, 'a.s'), os.path.join(d, 'a.o')
        open(src, 'w').write('.intel_syntax noprefix\n' + asm)
        subprocess.run([LLVM + '/llvm-mc', '-triple=x86_64-unknown-linux', '-filetype=obj', '-o', obj, src], check=True)
        dis = subprocess.run([LLVM + '/llvm-objdump', '-d', '-M', 'intel', '--no-leading-addr', obj],
                             capture_output=True, text=True, check=True).stdout
    out = []
    for line in dis.splitlines():
        f = line.split('\t')
        if len(f) >= 2 and re.fullmatch(r' *([0-9a-f]{2} )+ *', f[0]):
            out.append('\t\t"%s", // %s' % (f[0].replace(' ', ''), ' '.join(f[1:]).strip()))
    return '\n'.join(out)


B = 0x7000000000

def q(*vals): return b''.join(struct.pack('<Q', v) for v in vals)
def m80(mant, exp): return struct.pack('<QH', mant, exp)
ONE, TWO, MONE, QNAN = m80(1 << 63, 0x3fff), m80(1 << 63, 0x4000), m80(1 << 63, 0xbfff), m80(0xC000000000000001, 0x7fff)
X87 = {0x200: ONE, 0x210: TWO, 0x220: MONE, 0x230: QNAN}
REGS = {'RAX','RBX','RCX','RDX','RSI','RDI','RBP','R8','R9','R10','R11','R12','R13','R14','R15'}
cases = []
def case(name, src, asm, regs, mem=None, wantmem=None, cfg_only=False, note=''):
    cases.append(dict(name=name, src=src, asm=asm, regs=regs, mem=mem or {}, wantmem=wantmem or {}, cfg_only=cfg_only, note=note))

case('rep movsb + repe cmpsb down, lahf', 'Primary/Primary_A6_REP_down.asm', f'''
mov rdx, {B}
mov rdi, rdx
mov rsi, {B+0x200}
mov rcx, 14
cld
rep movsb
lea rdi, [rdx+16]
mov rsi, {B+0x220}
mov rcx, 14
cld
rep movsb
lea rdi, [rdx+13]
lea rsi, [rdx+29]
std
mov rcx, 10
repe cmpsb
mov rax, 0
lahf
mov rdx, 0
sete dl
''', dict(RAX=0x8300, RCX=9, RDX=0, RDI=B+0xC, RSI=B+0x1C),
  mem={0x200: rb'\0\0\0\0TestString', 0x220: rb'\0TestUnmatched'},
  note='NASM does not expand \\\\0 in "..." strings: the bytes are a backslash and a 0.')

Q8 = q(0x4142434445464748, 0x5152535455565758, 0x6162636465666768, 0x7172737475767778,
       0x8182838485868788, 0x9192939495969798, 0xA1A2A3A4A5A6A7A8, 0xB1B2B3B4B5B6B7B8, 0)
case('repne lodsq down', 'Primary/Primary_AD_REPNE_qword_down.asm', f'''
mov rdx, {B}
lea rsi, [rdx+64]
std
mov rax, 0xFF
mov rcx, 8
repne lodsq
''', dict(RAX=0x5152535455565758, RSI=B), mem={0: Q8})
Q2 = q(0x4142434445464748, 0x5152535455565758)
case('rep movsb down', 'Primary/Primary_A4_REP_Down.asm', f'''
mov rdx, {B}
lea rdi, [rdx+23]
lea rsi, [rdx+7]
std
mov rcx, 8
rep movsb
mov rax, [rdx+16]
mov rdx, [rdx+24]
''', dict(RAX=0x4142434445464748, RDX=0, RDI=B+0xF, RSI=B-1), mem={0: Q2})
case('rep movsq down', 'Primary/Primary_A5_qword_REP_Down.asm', f'''
mov rdx, {B}
lea rdi, [rdx+24]
lea rsi, [rdx+8]
std
mov rcx, 2
rep movsq
mov rax, [rdx+16]
mov rbx, [rdx+24]
mov rcx, [rdx+32]
''', dict(RAX=0x4142434445464748, RBX=0x5152535455565758, RCX=0, RDI=B+8, RSI=B-8), mem={0: Q2})
case('rep stosw down', 'Primary/Primary_AB_word_REP_down.asm', f'''
mov rdx, {B}
lea rdi, [rdx+22]
std
mov rcx, 4
mov rax, 0xF1F2
rep stosw
mov rax, [rdx+16]
mov rdx, [rdx+24]
''', dict(RAX=0xF1F2F1F2F1F2F1F2, RDX=0, RDI=B+0xE), mem={0: Q2})
case('cmpsw down, lahf (AF)', 'Primary/Primary_A7_word_down.asm', f'''
mov rdx, {B}
lea rdi, [rdx+8]
lea rsi, [rdx]
std
cmpsw
mov rax, 0
lahf
''', dict(RAX=0x1200, RDI=B+6, RSI=B-2), mem={0: q(0x6162, 0x4546)},
  note='[RSI]-[RDI] = 0x6162-0x4546 = 0x1c1c: AF only (the comment block in the FEX file is stale; RegData is right).')
case('repne scasb down', 'Primary/Primary_AE_REPNE_down.asm', f'''
mov rdx, {B}
lea rdi, [rdx+16]
std
mov rax, 0x61
mov rcx, 8
cmp rax, 0
repne scasb
''', dict(RCX=5, RDI=B+0xD), mem={0: q(0x4142434445464748, 0x5161535455565758, 0)})
case('cld; scasb', 'Primary/Primary_FC.asm', f'''
mov rdx, {B}
lea rdi, [rdx]
mov al, 0
cld
scasb
''', dict(RDI=B+1), mem={0: Q2})
case('std; scasb', 'Primary/Primary_FD.asm', f'''
mov rdx, {B}
lea rdi, [rdx+8]
mov al, 0
std
scasb
''', dict(RDI=B+7), mem={0: Q2})

def cmpseq(insn, pops):
    out = f'mov rdx, {B+0x200}\nfld tbyte ptr [rdx]\nfld tbyte ptr [rdx+16]\nmov rax, 0x45\n'
    out += f'{insn} st, st(1)\npushfq\npop rbx\nand rbx, rax\n'
    out += f'fld tbyte ptr [rdx+32]\n{insn} st, st(1)\npushfq\npop rcx\nand rcx, rax\n'
    out += f'fld tbyte ptr [rdx]\n{insn} st, st(1)\npushfq\npop rdi\nand rdi, rax\n'
    out += f'fld tbyte ptr [rdx+48]\n{insn} st, st(1)\npushfq\npop rsi\nand rsi, rax\n'
    out += f'fld tbyte ptr [rdx+48]\nfld tbyte ptr [rdx]\n{insn} st, st(1)\npushfq\npop rbp\nand rbp, rax\n'
    return out
W5 = dict(RBX=0, RCX=1, RDI=0x40, RSI=0x45, RBP=0x45)
case('fucomip: gt, lt, eq, unordered x2', 'X87/DF_E8.asm', cmpseq('fucomip', 1), W5, mem=X87)
case('fcomip: gt, lt, eq (+ unordered per the SDM)', 'X87/DF_F0.asm', cmpseq('fcomip', 1), W5, mem=X87,
     note='DF_F0.asm has the first three comparisons; the NaN ones follow DF_E8.asm (SDM: unordered sets ZF, PF, CF).')
def cmpi(insn):
    return f'''mov rdx, {B+0x200}
fld tbyte ptr [rdx+48]
fld tbyte ptr [rdx+32]
fld tbyte ptr [rdx+16]
fld tbyte ptr [rdx]
fld tbyte ptr [rdx]
mov rax, 0x45
{insn} st, st(3)
pushfq
pop rbx
and rbx, rax
{insn} st, st(2)
pushfq
pop rcx
and rcx, rax
{insn} st, st(1)
pushfq
pop rdi
and rdi, rax
{insn} st, st(4)
pushfq
pop rsi
and rsi, rax
'''
W4 = dict(RBX=0, RCX=1, RDI=0x40, RSI=0x45)
case('fcomi st(i): gt, lt, eq (+ unordered)', 'X87/DB_F0.asm', cmpi('fcomi'), W4, mem=X87,
     note='The NaN comparison is added (SDM).')
case('fucomi st(i): gt, lt, eq, unordered', 'X87/DB_E8.asm', cmpi('fucomi'), W4, mem=X87)
case('fcomi clears OF, SF, AF', 'FEX_bugs/fcomi_flags.asm', f'''
mov rbp, {B+0x200}
xor rbx, rbx
fld qword ptr [rbp]
fld qword ptr [rbp]
pushfq
pop rax
and rax, -0x891
or rax, 0
push rax
popfq
fcomi st, st(1)
pushfq
pop rcx
and rcx, 0x890
or rbx, rcx
fstp st(0)
fstp st(0)
fld qword ptr [rbp]
fld qword ptr [rbp]
pushfq
pop rax
and rax, -0x891
or rax, 0x890
push rax
popfq
fcomi st, st(1)
pushfq
pop rcx
and rcx, 0x890
or rbx, rcx
fstp st(0)
fstp st(0)
''', dict(RBX=0), mem={0x200: struct.pack('<d', 1.5)})
for insn, fex, f32 in [('fldl2t', 'D9_E9', 0x40549a78), ('fldl2e', 'D9_EA', 0x3fb8aa3b), ('fldpi', 'D9_EB', 0x40490fdb),
                       ('fldlg2', 'D9_EC', 0x3e9a209b), ('fldln2', 'D9_ED', 0x3f317218)]:
    case(insn, f'X87_F64/{fex}_F64.asm', f'mov rcx, {B}\n{insn}\nfst dword ptr [rcx]\nmov eax, [rcx]\n', dict(RAX=f32))
case('fcmovu on the PF of an integer compare', 'X87_F64/DA_D8_F64.asm', f'''
mov rdx, {B}
mov eax, 0x3f800000
mov [rdx], eax
fld1
fldz
mov eax, 0
cmp eax, -1
fcmovu st, st(1)
fldz
cmp eax, 1
fcmovu st, st(2)
fstp qword ptr [rdx]
mov rax, [rdx]
fstp qword ptr [rdx]
mov rbx, [rdx]
fstp qword ptr [rdx]
mov rcx, [rdx]
''', dict(RAX=0x3ff0000000000000, RBX=0, RCX=0x3ff0000000000000))
case('sahf; xor; lahf', 'FEX_bugs/xor_flags.asm', 'mov rax, 0\nmov rbx, 0\nsahf\nxor rbx, rbx\nlahf\n', dict(RAX=0x4600))
case('sahf; lahf round trip', 'Primary/Primary_9E.asm', 'mov rax, -1\nsahf\nmov rax, -1\nlahf\n', dict(RAX=0xFFFFFFFFFFFFD7FF))
case('popfq 0; pushfq', 'Primary/Primary_9C.asm', 'mov rax, 0\npush rax\npopfq\npushfq\npop rbx\n', dict(RBX=0x202),
     note='The 64-bit half of the test (the 66-prefixed PUSHF has no lowering).')
case('popfq; cmp; popfq; pushfq', 'Primary/Primary_9D.asm', 'mov rax, 0\npush rax\npopfq\npushfq\ncmp rax, 1\npopfq\npushfq\nmov rax, [rsp]\n', dict(RAX=0x202),
     note='The 64-bit half of the test.')
case('jp/jnp after add, xor, or, and', 'FEX_bugs/Test_JP.asm', '''
mov rax, 0x10
mov rbx, 0x10
add rax, rbx
jp bad
mov rax, 0x10
mov rbx, 0x22
xor rax, rbx
jp bad
mov rax, 0x40
mov rbx, 0x01
or rax, rbx
jnp bad
mov rax, 0x43
mov rbx, 0xfe
and rax, rbx
jnp bad
mov eax, 0xcafecafe
bad:
''', dict(RAX=0xcafecafe), cfg_only=True)
case('PF after a variable shift', 'FEX_bugs/ShiftPF.asm', 'mov rcx, 0\nadd rcx, 1\nmov rbx, 3\nmov cl, 1\nshl rbx, cl\nsetp al\nadd rdx, rdx\n', dict(RBX=6, RAX=1))
zs = 'mov eax, 0xff\nmov cl, 3\nshl eax, cl\njp bad\nmov eax, 0xff\nmov cl, 2\nshl eax, cl\njnp bad\nmov cl, 0\n'
for v, op in [(0x0f,'shl'),(0x0e,'shl'),(0x1f,'shr'),(0x1e,'shr'),(0x2f,'sal'),(0x2e,'sal'),(0x3f,'sar'),(0x3e,'sar')]:
    zs += f'mov eax, {v}\n{op} eax, cl\njnp bad\n'
zs += 'mov eax, 0xcafecafe\nbad:\n'
case('PF kept by zero-count shifts', 'FEX_bugs/Test_PF_Zero_Shift.asm', zs, dict(RAX=0xcafecafe), cfg_only=True)
ss = 'mov rax, 0x8234fdb482345679\nmov rcx, 0x5152535455565714\njmp t1\nt1:\n'
k = 1
for reg, op, cnt in [('r15','shl ax','cl'),('r14','sar ax','cl'),('r13','shl ax','0x14'),('r12','sar ax','0x14'),
                     ('r11','shl al','cl'),('r10','sar al','cl'),('r9','shl al','0x14'),('r8','sar al','0x14')]:
    k += 1
    ss += f'{op}, {cnt}\npushfq\npop {reg}\nand {reg}, -0x811\nmov rax, 0x8234fdb482345679\njmp t{k}\nt{k}:\n'
case('flags of 8/16-bit shifts past the operand width', 'FEX_bugs/SmallShiftFlags.asm', ss,
     {r: 0x246 for r in ['R8','R9','R10','R11','R12','R13','R14','R15']}, cfg_only=True)
case('pshufw mm, mm, imm', 'TwoByte/0F_70.asm', f'''
mov rdx, {B}
movq mm0, [rdx]
movq mm1, [rdx+16]
pshufw mm2, mm0, 0
pshufw mm3, mm1, 0xff
movq [rdx+0x40], mm0
movq [rdx+0x48], mm1
movq [rdx+0x50], mm2
movq [rdx+0x58], mm3
''', {}, mem={0: q(0x4142434445464748, 0x5152535455565758, 0x6162636465666768, 0x7172737475767778)},
  wantmem={0x40: q(0x4142434445464748, 0x6162636465666768, 0x4748474847484748, 0x6162616261626162)})

def gobytes(b): return '[]byte{' + ', '.join('0x%02x' % x for x in b) + '}'
out = ['// Code generated by tools/gen_fex_cases.py from FEX-Emu unittests/ASM; DO NOT EDIT.',
       '//',
       '// FEX-Emu is MIT licensed (Copyright (c) 2019 Ryan Houdek; see NOTICE). Each case names the FEX',
       '// unit test it restates: the same instructions with the 0xe0000000 data base replaced by the',
       '// exec window base, and the RegData of the test as the expected registers.',
       '', 'package lifter', '', 'import "golang.org/x/arch/x86/x86asm"', '',
       '// fexCase is one FEX-Emu unit test: its instructions, initial window bytes and the RegData',
       '// (or memory) the test requires.',
       'type fexCase struct {', '\texecCase', '\tsrc     string // path under FEX unittests/ASM',
       '\twantMem map[int][]byte', '\tcfgOnly bool // has branches: lifted through the block/flag passes only', '}', '',
       'var fexCases = []fexCase{']
for c in cases:
    hexes = encode(c['asm'])
    if c['note']:
        out.append('\t// ' + c['note'])
    out.append('\t{src: "%s", cfgOnly: %s, execCase: execCase{name: "%s", code: []string{' % (c['src'], 'true' if c['cfg_only'] else 'false', c['name']))
    out.append(hexes.rstrip('\n'))
    out.append('\t},')
    if c['mem']:
        out.append('\t\tmem: map[int][]byte{' + ', '.join('0x%x: %s' % (o, gobytes(b)) for o, b in c['mem'].items()) + '},')
    if c['regs']:
        out.append('\t\twantRegs: map[x86asm.Reg]uint64{' + ', '.join('x86asm.%s: 0x%x' % (r, v) for r, v in c['regs'].items()) + '}},')
    else:
        out.append('\t},')
    if c['wantmem']:
        out.append('\t\twantMem: map[int][]byte{' + ', '.join('0x%x: %s' % (o, gobytes(b)) for o, b in c['wantmem'].items()) + '},')
    out.append('\t},')
out.append('}')
print('\n'.join(out))
