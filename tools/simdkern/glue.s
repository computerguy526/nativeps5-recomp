// run_kern(void (*fn)(void), struct regs *r): enters lifted guest code with the pinned registers.
// regs: +0 rdi, +8 rsi, +16 rdx, +24 rcx, +32 rsp, +40 fs, +48 base, +56 rax (out), +64 xmm0 (16), +80 xmm1 (16)
.text
.globl _run_kern
.p2align 2
_run_kern:
    stp x29, x30, [sp, #-176]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    str x1, [sp, #160]
    mov x16, x0
    ldr x24, [x1, #0]
    ldr x23, [x1, #8]
    ldr x22, [x1, #16]
    ldr x21, [x1, #24]
    ldr x26, [x1, #32]
    ldr x27, [x1, #40]
    ldr x28, [x1, #48]
    ldr q0, [x1, #64]
    ldr q1, [x1, #80]
    mov x25, #0
    blr x16
    ldr x1, [sp, #160]
    str x19, [x1, #56]
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x25, x26, [sp, #64]
    ldp x27, x28, [sp, #80]
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    ldp x29, x30, [sp], #176
    ret
