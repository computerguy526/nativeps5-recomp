#!/usr/bin/env python3
"""Generates pkg/lifter/testdata/veclive_seqs.txt: random SSE/AVX instruction sequences for the
vector lane liveness differential test (TestVecLiveDifferential, docs/m3/simdregs.md).

Each line is one sequence: the hex encodings of its instructions, separated by spaces. A
sequence loads ymm0-15 from [rbx], runs a random body (scalar and packed, VEX and legacy SSE,
loads and stores in a scratch area, forward conditional branches), and ends either by storing
every YMM register or by storing a random selection of lanes followed by VZEROALL, which leaves
everything else dead. The instructions are only assembled (llvm-mc), never run.

  usage: tools/gen_veclive_cases.py [count] [seed] [out]    (needs llvm-mc and llvm-objdump)
  Defaults: 600 sequences, seed 20261114, the committed testdata file.  For a longer soak write
  another file and point the test at it: VECLIVE_SEQS=/path/seqs.txt go test ./pkg/lifter -run TestVecLiveDifferential
"""
import os, random, re, subprocess, sys, tempfile

LLVM = os.environ.get("LLVM_BIN", "/opt/homebrew/opt/llvm/bin")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "pkg/lifter/testdata/veclive_seqs.txt")

def gen(rng, hot):
    """hot: a small register set, so that values are overwritten and reused often."""
    x = lambda: "xmm%d" % rng.choice(hot)
    y = lambda: "ymm%d" % rng.choice(hot)
    m = lambda sz: "%s ptr [rbx+%d]" % (sz, 0x700 + 32 * rng.randrange(16))
    def xm(sz):
        return x() if rng.random() < 0.7 else m(sz)
    def ym():
        return y() if rng.random() < 0.7 else m("ymmword")
    imm = lambda: str(rng.randrange(256))
    ss = ["add", "sub", "mul", "div", "min", "max"]
    def one():
        k = rng.randrange(100)
        if k < 22:   # VEX scalar arithmetic
            sfx = rng.choice(["ss", "sd"])
            return "v%s%s %s, %s, %s" % (rng.choice(ss), sfx, x(), x(), xm("dword" if sfx == "ss" else "qword"))
        if k < 30:
            return rng.choice([
                lambda: "vsqrtss %s, %s, %s" % (x(), x(), xm("dword")),
                lambda: "vsqrtsd %s, %s, %s" % (x(), x(), xm("qword")),
                lambda: "vcvtss2sd %s, %s, %s" % (x(), x(), xm("dword")),
                lambda: "vcvtsd2ss %s, %s, %s" % (x(), x(), xm("qword")),
                lambda: "vcvtsi2sd %s, %s, rax" % (x(), x()),
                lambda: "vcvtsi2ss %s, %s, ecx" % (x(), x()),
                lambda: "vroundsd %s, %s, %s, %d" % (x(), x(), x(), rng.randrange(4)),
                lambda: "vroundss %s, %s, %s, %d" % (x(), x(), x(), rng.randrange(4)),
                lambda: "vfmadd231sd %s, %s, %s" % (x(), x(), xm("qword")),
                lambda: "vfmadd213ss %s, %s, %s" % (x(), x(), xm("dword")),
                lambda: "vfnmadd132sd %s, %s, %s" % (x(), x(), x()),
                lambda: "vfmsub231ps %s, %s, %s" % (x(), x(), x()),
                lambda: "vfmadd231pd %s, %s, %s" % (y(), y(), y()),
                lambda: "vrsqrtss %s, %s, %s" % (x(), x(), x()),
                lambda: "vrcpss %s, %s, %s" % (x(), x(), x()),
                lambda: "vcmpss %s, %s, %s, %d" % (x(), x(), x(), rng.randrange(8)),
                lambda: "vcmpsd %s, %s, %s, %d" % (x(), x(), x(), rng.randrange(8)),
            ])()
        if k < 40:   # VEX scalar moves and scalar <-> GPR
            return rng.choice([
                lambda: "vmovss %s, %s, %s" % (x(), x(), x()),
                lambda: "vmovsd %s, %s, %s" % (x(), x(), x()),
                lambda: "vmovss %s, %s" % (x(), m("dword")),
                lambda: "vmovsd %s, %s" % (x(), m("qword")),
                lambda: "vmovss %s, %s" % (m("dword"), x()),
                lambda: "vmovsd %s, %s" % (m("qword"), x()),
                lambda: "vmovd %s, edx" % x(),
                lambda: "vmovq %s, rsi" % x(),
                lambda: "vmovd r8d, %s" % x(),
                lambda: "vmovq r9, %s" % x(),
                lambda: "vmovq %s, %s" % (x(), x()),
                lambda: "vcvttsd2si r10, %s" % x(),
                lambda: "vcvttss2si r11d, %s" % x(),
                lambda: "vpextrd r12d, %s, %d" % (x(), rng.randrange(4)),
                lambda: "vpinsrd %s, %s, edx, %d" % (x(), x(), rng.randrange(4)),
                lambda: "vmovmskps r13d, %s" % x(),
                lambda: "vpmovmskb r14d, %s" % x(),
                lambda: "vextractps r15d, %s, %d" % (x(), rng.randrange(4)),
            ])()
        if k < 62:   # VEX packed, xmm and ymm
            w = rng.choice(["x", "y"])
            r, rm = (x, lambda: xm("xmmword")) if w == "x" else (y, ym)
            return rng.choice([
                lambda: "v%s %s, %s, %s" % (rng.choice(["addps", "mulpd", "subps", "divpd", "maxps", "minpd", "xorps", "andps",
                                                         "orpd", "andnps", "xorpd", "pxor", "pand", "por", "pandn", "paddd", "psubq",
                                                         "pcmpeqd", "pcmpgtb", "unpcklps", "unpckhpd", "punpcklqdq", "punpckhdq",
                                                         "pmulld", "pminsd", "pmaxub", "haddps", "packuswb", "pshufb", "paddusb",
                                                         "pavgb", "pmaddwd", "addsubps", "psadbw", "pmuludq"]), r(), r(), rm()),
                lambda: "v%s %s, %s, %s, %s" % (rng.choice(["shufps", "shufpd", "blendps", "blendpd", "pblendw", "palignr"]), r(), r(), rm(), imm()),
                lambda: "v%s %s, %s, %s" % (rng.choice(["pshufd", "pshuflw", "pshufhw", "permilps"]), r(), rm(), imm()),
                lambda: "v%s %s, %s" % (rng.choice(["movaps", "movups", "movdqa", "movdqu", "movapd", "sqrtps", "sqrtpd",
                                                     "movshdup", "movsldup", "movddup", "pabsd", "cvtdq2ps", "cvttps2dq",
                                                     "rsqrtps", "rcpps"]), r(), r()),
                lambda: "vmovups %s, %s" % (r(), m("xmmword" if w == "x" else "ymmword")),
                lambda: "vmovdqu %s, %s" % (m("xmmword" if w == "x" else "ymmword"), r()),
                lambda: "v%s %s, %s, %d" % (rng.choice(["pslld", "psrlq", "psraw", "pslldq", "psrldq"]), r(), r(), rng.randrange(1, 8)),
                lambda: "vblendvps %s, %s, %s, %s" % (r(), r(), r(), r()),
                lambda: "vcmpps %s, %s, %s, %d" % (r(), r(), r(), rng.randrange(8)),
                lambda: "vxorps %s, %s, %s" % ((lambda a: (r(), a, a))(r())),
                lambda: "vpxor %s, %s, %s" % ((lambda a: (r(), a, a))(r())),
            ])()
        if k < 72:   # width-crossing AVX
            return rng.choice([
                lambda: "vextractf128 %s, %s, %d" % (x(), y(), rng.randrange(2)),
                lambda: "vextracti128 %s, %s, 1" % (m("xmmword"), y()),
                lambda: "vinsertf128 %s, %s, %s, %d" % (y(), y(), xm("xmmword"), rng.randrange(2)),
                lambda: "vperm2f128 %s, %s, %s, %s" % (y(), y(), y(), imm()),
                lambda: "vbroadcastss %s, %s" % (y(), x()),
                lambda: "vbroadcastss %s, %s" % (x(), m("dword")),
                lambda: "vbroadcastsd %s, %s" % (y(), m("qword")),
                lambda: "vpbroadcastd %s, %s" % (y(), x()),
                lambda: "vpermq %s, %s, %s" % (y(), y(), imm()),
                lambda: "vpermpd %s, %s, %s" % (y(), y(), imm()),
                lambda: "vcvtps2pd %s, %s" % (y(), x()),
                lambda: "vcvtpd2ps %s, %s" % (x(), y()),
                lambda: "vcvtdq2pd %s, %s" % (y(), x()),
                lambda: "vpmovzxbw %s, %s" % (y(), x()),
                lambda: "vpmovsxdq %s, %s" % (x(), x()),
                lambda: "vmovhlps %s, %s, %s" % (x(), x(), x()),
                lambda: "vmovlhps %s, %s, %s" % (x(), x(), x()),
                lambda: "vmovlps %s, %s, %s" % (x(), x(), m("qword")),
                lambda: "vmovhps %s, %s, %s" % (x(), x(), m("qword")),
                lambda: "vinsertps %s, %s, %s, %s" % (x(), x(), x(), imm()),
                lambda: "vdpps %s, %s, %s, %s" % (x(), x(), x(), imm()),
                lambda: "vzeroupper",
                lambda: "vptest %s, %s" % (y(), y()),
                lambda: "vtestps %s, %s" % (x(), x()),
            ])()
        if k < 97:   # legacy SSE (keeps bits 128..255)
            sfx = rng.choice(["ss", "sd"])
            return rng.choice([
                lambda: "%s%s %s, %s" % (rng.choice(ss), sfx, x(), xm("dword" if sfx == "ss" else "qword")),
                lambda: "sqrt%s %s, %s" % (sfx, x(), xm("dword" if sfx == "ss" else "qword")),
                lambda: "movss %s, %s" % (x(), x()),
                lambda: "movsd %s, %s" % (x(), x()),
                lambda: "movss %s, %s" % (x(), m("dword")),
                lambda: "movsd %s, %s" % (x(), m("qword")),
                lambda: "movss %s, %s" % (m("dword"), x()),
                lambda: "movsd %s, %s" % (m("qword"), x()),
                lambda: "cvtss2sd %s, %s" % (x(), x()),
                lambda: "cvtsd2ss %s, %s" % (x(), xm("qword")),
                lambda: "cvtsi2sd %s, rax" % x(),
                lambda: "cvtsi2ss %s, ecx" % x(),
                lambda: "roundsd %s, %s, %d" % (x(), x(), rng.randrange(4)),
                lambda: "roundss %s, %s, %d" % (x(), x(), rng.randrange(4)),
                lambda: "%s %s, %s" % (rng.choice(["movaps", "movups", "movdqa", "movapd", "addps", "mulpd", "xorps", "andps", "orpd",
                                                    "andnps", "pxor", "pand", "paddd", "psubq", "unpcklps", "punpckldq", "maxps",
                                                    "sqrtps", "pcmpeqd", "movhlps", "movlhps", "movshdup", "pshufb", "pmuludq"]), x(), x()),
                lambda: "movups %s, %s" % (x(), m("xmmword")),
                lambda: "movdqu %s, %s" % (m("xmmword"), x()),
                lambda: "%s %s, %s, %s" % (rng.choice(["pshufd", "shufps", "pshuflw", "blendps", "palignr"]), x(), x(), imm()),
                lambda: "xorps %s, %s" % ((lambda a: (a, a))(x())),
                lambda: "movd %s, edx" % x(),
                lambda: "movq %s, rsi" % x(),
                lambda: "movd r8d, %s" % x(),
                lambda: "movq r9, %s" % x(),
                lambda: "movq %s, %s" % (x(), x()),
                lambda: "cvttsd2si r10, %s" % x(),
                lambda: "pinsrw %s, edx, %d" % (x(), rng.randrange(8)),
                lambda: "%s %s, %d" % (rng.choice(["pslld", "psrlq", "pslldq", "psrldq"]), x(), rng.randrange(1, 8)),
                lambda: "cmpss %s, %s, %d" % (x(), x(), rng.randrange(8)),
                lambda: "blendvps %s, %s" % (x(), x()),
                lambda: "pblendvb %s, %s" % (x(), x()),
            ])()
        return "BRANCH"
    return one

def sequence(rng, idx):
    hot = rng.sample(range(16), rng.choice([3, 4, 6, 16]))
    one = gen(rng, hot)
    lines = ["vmovdqu ymm%d, ymmword ptr [rbx+%d]" % (i, 32 * i) for i in range(16)]
    n = rng.randrange(8, 40)
    body, label = [], 0
    pending = []  # (label, instructions left before it is placed)
    for _ in range(n):
        ins = one()
        if ins == "BRANCH":
            a, b = "xmm%d" % rng.choice(hot), "xmm%d" % rng.choice(hot)
            cmp = rng.choice(["vucomiss", "vucomisd", "ucomiss", "vcomisd"])
            cc = rng.choice(["jb", "jbe", "ja", "jae", "je", "jne", "jp", "jnp"])
            label += 1
            body += ["%s %s, %s" % (cmp, a, b), "%s .Ls%d_%d" % (cc, idx, label)]
            pending.append([".Ls%d_%d" % (idx, label), rng.randrange(1, 6)])
        else:
            body.append(ins)
        for p in pending:
            p[1] -= 1
        for p in [p for p in pending if p[1] <= 0]:
            body.append(p[0] + ":")
            pending.remove(p)
    for p in pending:
        body.append(p[0] + ":")
    lines += body
    if rng.random() < 0.5:
        lines += ["vmovdqu ymmword ptr [rbx+%d], ymm%d" % (0x300 + 32 * i, i) for i in range(16)]
    else:
        for i in rng.sample(range(16), rng.randrange(1, 9)):
            off = 0x300 + 32 * i
            lines.append(rng.choice([
                "vmovss dword ptr [rbx+%d], xmm%d", "vmovsd qword ptr [rbx+%d], xmm%d", "vmovups xmmword ptr [rbx+%d], xmm%d",
                "vmovdqu ymmword ptr [rbx+%d], ymm%d", "movss dword ptr [rbx+%d], xmm%d", "movups xmmword ptr [rbx+%d], xmm%d",
            ]) % (off, i))
        lines.append("vzeroall")
    return lines

def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 600
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 20261114
    out = sys.argv[3] if len(sys.argv) > 3 else OUT
    rng = random.Random(seed)
    src = [".intel_syntax noprefix", ".text"]
    for i in range(count):
        src.append("seq_%d:" % i)
        src += ["    " + l if not l.endswith(":") else l for l in sequence(rng, i)]
    with tempfile.TemporaryDirectory() as d:
        s, o = os.path.join(d, "a.s"), os.path.join(d, "a.o")
        open(s, "w").write("\n".join(src) + "\n")
        subprocess.check_call([LLVM + "/llvm-mc", "-triple=x86_64", "-mattr=+avx2,+fma,+sse4.2", "-filetype=obj", "-o", o, s])
        dis = subprocess.check_output([LLVM + "/llvm-objdump", "-d", "--x86-asm-syntax=intel", o], text=True)
    seqs, cur = [], None
    for ln in dis.splitlines():
        if re.match(r"^[0-9a-f]+ <seq_\d+>:", ln):
            cur = []
            seqs.append(cur)
            continue
        m = re.match(r"^\s*[0-9a-f]+:\s+((?:[0-9a-f]{2}[ \t])+)", ln)
        if m and cur is not None:
            cur.append(re.sub(r"\s", "", m.group(1)))
    assert len(seqs) == count, (len(seqs), count)
    with open(out, "w") as f:
        f.write("# generated by tools/gen_veclive_cases.py %d %d; one sequence per line\n" % (count, seed))
        for q in seqs:
            f.write(" ".join(q) + "\n")
    print("wrote %s: %d sequences, %d instructions" % (out, count, sum(len(q) for q in seqs)))

if __name__ == "__main__":
    main()
