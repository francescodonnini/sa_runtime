#include "asm_consts.h"
#include <asm/unistd.h>

.equ STACK_SIZE,         0x800000
.equ ADR_HINT,           0x100000000000
.global relocate_stack
.extern _start

.section .rodata
dev_name: .asciz "/dev/shadowstack"

.section .text
relocate_stack:
    mov     %rdx,       %r12 # save dl_fini

    mov    $__NR_MMAP,  %rax
    movabs $ADR_HINT,   %rdi
    mov    $STACK_SIZE, %rsi
    mov    $3,          %rdx # PROT_READ | PROT_WRITE
    mov    $34,         %r10 # MAP_ANONYMOUS | MAP_PRIVATE
    mov    $-1,         %r8
    xor    %r9,         %r9
    syscall
    cmp    $-4095, %rax
    jae    _out

    xor    %rdi, %rdi # M
    xor    %r11, %r11 # AT_RANDOM vec
    mov    %rsp, %r8
    lea  8(%r8), %r8

_scan_argv:
    mov   (%r8), %r9
    test   %r9, %r9
    jz _scan_argv_exit

    cmp %rdi, %r9
    jbe _inc_argv
    mov %r9, %rdi

_inc_argv:
    lea 8(%r8), %r8
    jmp _scan_argv

_scan_argv_exit:
    lea 8(%r8), %r8

_scan_envp:
    mov (%r8), %r9
    test %r9, %r9
    jz _scan_envp_exit

    cmp %rdi, %r9
    jbe _inc_envp
    mov %r9, %rdi

_inc_envp:
    lea 8(%r8), %r8
    jmp _scan_envp

_scan_envp_exit:
    lea 8(%r8), %r8

_scan_auxv:
    mov (%r8), %r9
    test %r9, %r9
    jz _scan_auxv_exit

    cmp $7, %r9 # AT_BASE
    je _check_auxp
    cmp $15, %r9 # AT_PLATFORM
    je _check_auxp
    cmp $25, %r9 # AT_RANDOM
    je _check_auxp
    cmp $31, %r9 # AT_EXECFN
    je _check_auxp
    jmp _inc_auxv

_check_auxp:
    mov 8(%r8), %r10
    cmp %rdi, %r10
    jbe _inc_auxv

    mov %r10, %rdi
    xor %r11, %r11

    cmp $25, %r9 # AT_RANDOM
    sete %r11b
    movzx %r11b, %r11

_inc_auxv:
    lea 16(%r8), %r8
    jmp _scan_auxv

_scan_auxv_exit:
    lea 16(%r8), %r8

    mov %rax, %rdx # save mmap'd address to use rax w/ scasb

    test %r11, %r11
    jnz _at_rnd_highest

    xor %al, %al
    mov $-1, %rcx
    repne scasb 
    jmp _patch

_at_rnd_highest:
    lea 16(%rdi), %rdi

_patch:
    # rdi = M
    # %rdx mmap'd region
    
    mov %rdi, %rcx
    sub %rsp, %rcx # rcx = M - rsp (Stack size)
    inc %rcx

    # Let N be the stack size, A the starting address of the mmap'd region
    # I need to copy the byte range from (M, rsp]
    # to the area (A+N, N] 

    mov %rsp, %rsi # movsb source
    lea STACK_SIZE(%rdx), %rdi # movsb dest
    sub %rcx, %rdi

    and $-16, %rdi

    mov %rdi, %rax

    rep movsb

    mov %rax, %r9
    sub %rsp, %r9 # Delta

    lea 8(%rax), %r10 # &argv[0]

_patch_argv:
    mov (%r10), %r11
    test %r11, %r11
    jz _patch_argv_exit

    add %r9, %r11
    mov %r11, (%r10)

    lea 8(%r10), %r10
    jmp _patch_argv

_patch_argv_exit:
    lea 8(%r10), %r10

_patch_envp:
    mov (%r10), %r11
    test %r11, %r11
    jz _patch_envp_exit

    add %r9, %r11
    mov %r11, (%r10)

    lea 8(%r10), %r10
    jmp _patch_envp

_patch_envp_exit:
    lea 8(%r10), %r10
    
_patch_auxv:
    mov (%r10), %r11
    test %r11, %r11
    jz _patch_auxv_exit # exit if AT_NULL

    cmp $7, %r11 # AT_BASE
    je _do_patch_auxp
    cmp $15, %r11 # AT_PLATFORM
    je _do_patch_auxp
    cmp $25, %r11 # AT_RANDOM
    je _do_patch_auxp
    cmp $31, %r11 # AT_EXECFN
    je _do_patch_auxp
    jmp _patch_inc_auxv

_do_patch_auxp:
    mov 8(%r10), %r11
    add %r9, %r11
    mov %r11, 8(%r10)

_patch_inc_auxv:
    lea 16(%r10), %r10
    jmp _patch_auxv

_patch_auxv_exit:
    mov %rsp, %r15 # %r15 = old stack pointer
    mov %rax, %rsp # %rax = new stack area
    mov %rax, %r14 # %r14 = new stack area (scratch variable)

    mov $__NR_OPEN, %rax
    lea dev_name(%rip), %rdi
    mov $2, %rsi # O_RDWR
    xor %rdx, %rdx
    syscall

    test %rax, %rax
    js   _out

    mov %rax, %r13

    sub   $_IOCTL_PIVOT_FRAME, %rsp
    movq  $0,    _IOCTL_PIVOT_ERROR(%rsp)
    mov   %r14,  _IOCTL_PIVOT_NEW_STACK(%rsp)
    mov   %r15,  _IOCTL_PIVOT_RSP(%rsp)
    movq  $0,    _IOCTL_PIVOT_DELTA(%rsp)
    mov   %rsp,  %rdx

    mov    $__NR_IOCTL, %rax
    mov    %r13, %rdi
    movabs $IOCTL_SHADOW_PIVOT, %rsi
    syscall

    test %rax, %rax
    js _out2
    cmpq $0, (%rsp)
    jne _out2

    mov _IOCTL_PIVOT_DELTA(%rsp), %rax
    mov __ss_shadow_offset@GOTPCREL(%rip), %rdi # mprotect's addr
    mov %rax, (%rdi)

    mov $__NR_MPROTECT, %rax
    mov $4096, %rsi # mprotect's size
    mov $1, %rdx    # mprotect's prot (PROT_READ = 1)
    syscall
    test %rax, %rax
    jnz _out2

    add $_IOCTL_PIVOT_FRAME, %rsp

    mov $__NR_CLOSE, %rax
    mov %r13, %rdi # close's fd
    syscall

    mov %r12, %rdx
    jmp _start

_out2:
    mov $__NR_EXIT, %rax
    mov %r13, %rdi
    syscall

_out:
    mov $__NR_EXIT, %rax
    mov $1,  %rdi
    syscall