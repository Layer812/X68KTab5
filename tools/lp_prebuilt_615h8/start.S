    /* ESP-IDF 5.4.x ulp_lp_core_run(skip_lp_rom_boot=true) starts the LP
     * application at LP_RAM + 0x80. Keep an explicit 0x80-byte image prefix
     * so the flat binary still loads at 0x50108000 while _start is exactly at
     * the reset-handler address 0x50108080. */
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
    call main
3:
    wfi
    j 3b
    .size _start, .-_start
