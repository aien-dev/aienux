/* Host qualification adapter for the pinned Omega frame-slot ABI. */
.text
.global omega_run
.type omega_run,@function
omega_run:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, host_saved_sp(%rip)
    sub $66048, %rsp
    and $-16, %rsp
    lea 65536(%rsp), %rbp
    mov %rsi, -8(%rbp)
    mov %rdx, -16(%rbp)
    mov %rcx, -24(%rbp)
    mov %r8, -32(%rbp)
    lea host_return(%rip), %rax
    mov %rax, -264(%rbp)
    lea host_vtable(%rip), %r14
    lea -65536(%rbp), %r15
    lea -264(%rbp), %rsp
    mov %rsp, %r12
    movl $0, host_trap_code(%rip)
    jmp *%rdi
host_return:
    mov host_saved_sp(%rip), %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret
.global host_trap
host_trap:
    mov %esi, host_trap_code(%rip)
    xor %eax, %eax
    jmp host_return
.section .note.GNU-stack,"",@progbits
