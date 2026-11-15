#include "asm_consts.h"
#include <asm/unistd.h>

.global relocate_stack
.extern _start

.section .rodata
dev_name: .asciz "/dev/shdwstck"

msg_open:     .ascii "shadowstack: cannot open /dev/shdwstck (is the module loaded?)\n"
.equ msg_open_len, . - msg_open
msg_ioctl:    .ascii "shadowstack: IOCTL_SHADOW_GET_OFFSET failed (not started through the interpreter?)\n"
.equ msg_ioctl_len, . - msg_ioctl
msg_mprotect: .ascii "shadowstack: cannot make __ss_shadow_offset read-only\n"
.equ msg_mprotect_len, . - msg_mprotect

.section .data.ss_offset, "aw", @progbits
.balign 4096
.globl __ss_shadow_offset
.type  __ss_shadow_offset, @object
.size  __ss_shadow_offset, 8
__ss_shadow_offset:
    .quad 0
    .space 4096 - 8

.section .text
relocate_stack:
    mov  %rdx, %r12              # dl_fini

    mov  $__NR_open, %eax
    lea  dev_name(%rip), %rdi
    mov  $0x80002, %esi          # O_RDWR | O_CLOEXEC
    xor  %edx, %edx
    syscall
    test %rax, %rax
    js   _err_open
    mov  %rax, %rbx              # fd

    sub  $16, %rsp               # buffer per l'offset, rsp resta allineato
    movq $0, (%rsp)
    mov  $__NR_ioctl, %eax
    mov  %rbx, %rdi
    movabs $IOCTL_SHADOW_GET_OFFSET, %rsi
    mov  %rsp, %rdx
    syscall
    test %rax, %rax
    js   _err_ioctl
    mov  (%rsp), %rax
    add  $16, %rsp

    lea  __ss_shadow_offset(%rip), %rdi
    mov  %rax, (%rdi)
    mov  $__NR_mprotect, %eax
    mov  $4096, %esi
    mov  $1, %edx                # PROT_READ
    syscall
    test %rax, %rax
    jnz  _err_mprotect

    mov  $__NR_close, %eax
    mov  %rbx, %rdi
    syscall

    mov  %r12, %rdx
    jmp  _start

_err_open:
    lea  msg_open(%rip), %rsi
    mov  $msg_open_len, %edx
    mov  $2, %r13d
    jmp  _die
_err_ioctl:
    lea  msg_ioctl(%rip), %rsi
    mov  $msg_ioctl_len, %edx
    mov  $3, %r13d
    jmp  _die
_err_mprotect:
    lea  msg_mprotect(%rip), %rsi
    mov  $msg_mprotect_len, %edx
    mov  $5, %r13d
_die:
    mov  $__NR_write, %eax
    mov  $2, %edi
    syscall
    mov  %r13d, %edi
    mov  $__NR_exit_group, %eax
    syscall

.section .note.GNU-stack, "", @progbits