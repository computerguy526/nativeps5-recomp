// Host side of the FEX-Emu ASM oracle (cmd/fexasm). Arm64 only: it runs the lifter's output for
// one FEX test. Nothing here executes x86 code.
//
//   fex_run <spec.txt> <code.bin>
//
// spec.txt lines (all numbers hex):  map ADDR SIZE | data ADDR HEXBYTES | code ADDR | hash ADDR SIZE
// Guest address A lives at host arena + A (the lifter's base addressing mode, x28 = arena).
// Output (stdout, deterministic): GPR/XMM/YMMH/MM/MEM lines then END; or CRASH ... on a signal.
#include "recomp_context.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>

typedef struct FexState {
    uint64_t gpr[16];  /* rax rcx rdx rbx rsp rbp rsi rdi r8-r15 */
    uint8_t v[32][16]; /* v0-v15 = xmm, v16-v31 = ymm upper halves */
    uint64_t mmx[8];
    uint64_t arena, fs;
} FexState;
_Static_assert(offsetof(FexState, v) == 128, "asm offset");
_Static_assert(offsetof(FexState, arena) == 128 + 512 + 64, "asm offset");

extern void fex_enter(FexState *);
static uint8_t *arena;
static const uint64_t ARENA_SIZE = 0x700000000ULL;
static GuestContext ctx;
static FexState st;

static void on_sig(int sig, siginfo_t *si, void *vuc) {
    ucontext_t *uc = vuc;
    unsigned long long esr = uc->uc_mcontext->__es.__esr;
    uint64_t a = (uint64_t)si->si_addr;
    /* A trap in lifted code: brk #imm (0 = lifter trap, 0x777 unsupported, 0x778/9 unknown indirect target). */
    if (sig == SIGTRAP) printf("CRASH sig=5 brk=0x%llx\n", esr & 0xffff);
    else if (a >= (uint64_t)arena && a < (uint64_t)arena + ARENA_SIZE) printf("CRASH sig=%d guest=0x%llx\n", sig, (unsigned long long)(a - (uint64_t)arena));
    else printf("CRASH sig=%d host=0x%llx\n", sig, (unsigned long long)a);
    fflush(stdout);
    _exit(3);
}

static void *gp(uint64_t a) { return arena + a; }

static void mapat(uint64_t a, uint64_t sz) {
    /* host pages are 16 KiB; round the guest range out to them */
    uint64_t lo = a & ~0x3fffULL, hi = (a + sz + 0x3fff) & ~0x3fffULL;
    if (hi > ARENA_SIZE || mmap(gp(lo), hi - lo, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0) == MAP_FAILED) {
        printf("SETUP-FAIL map 0x%llx 0x%llx\n", (unsigned long long)a, (unsigned long long)sz);
        exit(4);
    }
}

static uint64_t fnv(const uint8_t *p, uint64_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (uint64_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ULL;
    return h;
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    arena = mmap(NULL, ARENA_SIZE, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    if (arena == MAP_FAILED) { puts("SETUP-FAIL arena"); return 4; }
    static uint8_t altstack[1 << 16];
    stack_t ss = {.ss_sp = altstack, .ss_size = sizeof(altstack)};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = on_sig;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE, SIGABRT};
    for (unsigned i = 0; i < sizeof(sigs) / sizeof(*sigs); i++) sigaction(sigs[i], &sa, NULL);

    mapat(0xe0000000ULL, 0x8001000ULL); /* FEX default data/stack area */
    mapat(0xd0000000ULL, 0x1000);       /* FS base page; [FS+0x70] = GuestContext* */
    FILE *f = fopen(argv[1], "r");
    if (!f) return 2;
    static char line[1 << 16];
    uint64_t hashes[64][2]; int nh = 0;
    uint64_t codeaddr = 0;
    /* two passes: maps first (data may refer to them), then data */
    for (int pass = 0; pass < 2; pass++) {
        rewind(f);
        while (fgets(line, sizeof line, f)) {
            char kind[16]; unsigned long long a, b;
            if (sscanf(line, "%15s %llx %llx", kind, &a, &b) < 2) continue;
            if (pass == 0 && !strcmp(kind, "map")) mapat(a, b);
            if (pass == 0 && !strcmp(kind, "code")) { codeaddr = a; }
            if (pass == 0 && !strcmp(kind, "hash") && nh < 64) { hashes[nh][0] = a; hashes[nh][1] = b; nh++; }
            if (pass == 1 && !strcmp(kind, "data")) {
                const char *p = strchr(strchr(line, ' ') + 1, ' ');
                uint8_t *d = gp(a);
                while (p && *p) {
                    while (*p == ' ') p++;
                    unsigned x;
                    if (sscanf(p, "%2x", &x) != 1) break;
                    *d++ = (uint8_t)x; p += 2;
                }
            }
        }
    }
    fclose(f);
    FILE *c = fopen(argv[2], "rb");
    if (!c) return 2;
    fseek(c, 0, SEEK_END); long n = ftell(c); rewind(c);
    mapat(codeaddr, n + 0x1000);
    if (fread(gp(codeaddr), 1, n, c) != (size_t)n) return 2;
    fclose(c);

    memset(&ctx, 0, sizeof ctx);
    ctx.mxcsr = 0x1f80;
    ctx.fpu_cw = 0x37f;
    GuestContext *cp = &ctx;
    memcpy(gp(0xd0000000ULL + 0x70), &cp, sizeof cp);
    memset(&st, 0, sizeof st);
    st.gpr[4] = 0xe8000000ULL;
    st.arena = (uint64_t)arena;
    st.fs = 0xd0000000ULL;
    fex_enter(&st);

    static const char *gn[16] = {"RAX","RCX","RDX","RBX","RSP","RBP","RSI","RDI","R8","R9","R10","R11","R12","R13","R14","R15"};
    for (int i = 0; i < 16; i++) printf("%s 0x%llx\n", gn[i], (unsigned long long)st.gpr[i]);
    for (int i = 0; i < 16; i++) {
        uint64_t *x = (uint64_t *)st.v[i], *h = (uint64_t *)st.v[16 + i];
        printf("XMM%d 0x%llx 0x%llx\n", i, (unsigned long long)x[0], (unsigned long long)x[1]);
        printf("YMMH%d 0x%llx 0x%llx\n", i, (unsigned long long)h[0], (unsigned long long)h[1]);
    }
    for (int i = 0; i < 8; i++) {
        /* x87=f80 (argv[3] = f80): MMn is physical x87 register n; the FEX X87 tests list its 80-bit value
           as [significand, sign/exponent]. The lifter keeps MMX data in the significand slot then
           (x87AliasMmx), so MMX-only tests read it too. */
        if (argc > 3 && !strcmp(argv[3], "f80"))
            printf("MM%d 0x%llx 0x%x\n", i, (unsigned long long)ctx.fpu80[i].mant, ctx.fpu80[i].se);
        else
            printf("MM%d 0x%llx\n", i, (unsigned long long)ctx.mmx[i]);
    }
    for (int i = 0; i < nh; i++) {
        uint64_t sz = hashes[i][1] > 0x100000 ? 0x100000 : hashes[i][1];
        printf("MEM 0x%llx 0x%llx\n", (unsigned long long)hashes[i][0], (unsigned long long)fnv(gp(hashes[i][0]), sz));
    }
    puts("END");
    return 0;
}
