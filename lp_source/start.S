.section .lp_vector_pad,"ax",@progbits
.balign 4
.globl __lp_image_base
__lp_image_base:
.space 0x80,0
.section .text.start,"ax",@progbits
.globl _start
.type _start,@function
_start:
    /* First observable instruction: LPB1 means direct +0x80 entry executed. */
    lui t0, 0x50108
    lui t1, 0x4c504
    addi t1, t1, 0x231
    sw t1, 0x400(t0)
    fence rw,rw
    la sp,__stack_top
    call main
1:  wfi
    j 1b
.size _start,.-_start
