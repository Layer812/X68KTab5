    /* Build 6.15h12-exp: ESP-IDF direct boot enters LP_RAM+0x80. */
    .section .lp_vector_pad,"ax",@progbits
    .balign 4
    .globl __lp_image_base
__lp_image_base:
    .space 0x80, 0

    .section .text.start,"ax",@progbits
    .globl _start
    .type _start,@function
_start:
    la sp, __stack_top
    la t0, __bss_start
    la t1, __bss_end
1:
    bgeu t0, t1, 2f
    sw zero, 0(t0)
    addi t0, t0, 4
    j 1b
2:
    la t0, lp_trap_entry
    csrw mtvec, t0
    call main
3:
    wfi
    j 3b
    .size _start, .-_start

    .section .text.trap,"ax",@progbits
    .balign 4
    .globl lp_trap_entry
    .type lp_trap_entry,@function
lp_trap_entry:
    la t0, lp_fm_diag_mcause
    csrr t1, mcause
    sw t1, 0(t0)
    la t0, lp_fm_diag_mepc
    csrr t1, mepc
    sw t1, 0(t0)
    la t0, lp_fm_diag_mtval
    csrr t1, mtval
    sw t1, 0(t0)
    la t0, lp_fm_diag_stage
    li t1, 0x11EE0001
    sw t1, 0(t0)
    fence rw,rw
4:
    wfi
    j 4b
    .size lp_trap_entry, .-lp_trap_entry
