/* MS ABI caller, private Omega user ABI. Intel SDM Vol 3A 7.14.
 * Only the BSP enters. Kernel exceptions continue through the ordinary handler.
 * Every user exit saves user FP state BEFORE C can touch it and restores the
 * caller's FP state, stack and nonvolatile registers before returning. */
    .text
    .globl ck_local_enter
ck_local_enter:
    pushfq
    cli
    push %rbx
    push %rbp
    push %rdi
    push %rsi
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, ck_local_kernel_sp(%rip)
    fxsave64 ck_local_kernel_fp(%rip)
    fxrstor64 ck_local_user_fp(%rip)
    mov %rcx, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
    add $16, %rsp
    iretq

    .globl ck_local_user_exit
ck_local_user_exit:
    /* RCX is the complete frame on the supervisor interrupt stack. */
    fxsave64 ck_local_user_fp(%rip)
    fxrstor64 ck_local_kernel_fp(%rip)
    and $-16, %rsp
    sub $32, %rsp
    call ck_local_trap
    mov ck_local_kernel_sp(%rip), %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rsi
    pop %rdi
    pop %rbp
    pop %rbx
    popfq
    ret
    .section .note.GNU-stack,"",@progbits
