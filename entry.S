#include <asm/unistd.h>

.globl _start
_start:
    mov  %rsp, %rdi
    and  $-16, %rsp
    # struct jump {
    #     unsigned long *sp;
    #     unsigned long  entry;
    # }
    # pt_interpreter_main ritorna struct jump composta da due campi di 8 byte ciascuno:
    # il primo, che contiene l'indirizzo del nuovo stack, si trova in %rax,
    # mentre il secondo, che contiene il punto d'ingresso di ld.so, in %rdx
    call pt_interpreter_main
    
    mov  %rax, %rsp
    mov  %rdx, %r12
    mov  $__NR_munmap, %eax
    mov  pt_old_start(%rip), %rdi
    mov  pt_old_len(%rip), %rsi
    syscall

    mov  %r12, %r11
    xor  %eax, %eax
    xor  %edx, %edx
    jmp  *%r11
.section .note.GNU-stack, "", @progbits