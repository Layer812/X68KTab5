OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY { LP (rwx) : ORIGIN = 0x50108000, LENGTH = 0x3000 }
SECTIONS {
 . = ORIGIN(LP);
 .lp_vector_pad : ALIGN(4) { KEEP(*(.lp_vector_pad)) } > LP
 .text : ALIGN(4) { KEEP(*(.text.start)) *(.text .text.*) } > LP
 .rodata : ALIGN(4) { *(.rodata .rodata.*) *(.srodata .srodata.*) } > LP
 .data : ALIGN(4) { *(.data .data.*) *(.sdata .sdata.*) } > LP
 .bss (NOLOAD) : ALIGN(4) { *(.bss .bss.*) *(.sbss .sbss.*) *(COMMON) } > LP
 /* R58a3: fixed ABI island independent of compiler variable ordering/code size. */
 .lp_shared 0x50108400 (NOLOAD) : ALIGN(4) { __lp_shared_start = .; KEEP(*(.lp_shared)) __lp_shared_end = .; } > LP
 . = ALIGN(16); __image_ram_end = .;
 __stack_top = ORIGIN(LP)+LENGTH(LP)-16;
 ASSERT(__lp_shared_start == 0x50108400, "LP broker ABI base moved")
 ASSERT(__image_ram_end <= (__stack_top-1024), "LP broker RAM overflow")
 /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.riscv.attributes) }
}
