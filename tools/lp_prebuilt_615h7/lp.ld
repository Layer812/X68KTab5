OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY {
  LP (rwx) : ORIGIN = 0x50108000, LENGTH = 0x7000
}
SECTIONS {
  . = ORIGIN(LP);
  .text : ALIGN(4) {
    KEEP(*(.text.start))
    *(.text .text.*)
  } > LP
  .rodata : ALIGN(4) {
    *(.rodata .rodata.*)
    *(.srodata .srodata.*)
  } > LP
  .data : ALIGN(4) {
    *(.data .data.*)
    *(.sdata .sdata.*)
  } > LP
  . = ALIGN(4);
  __bss_start = .;
  .bss (NOLOAD) : ALIGN(4) {
    *(.bss .bss.*)
    *(.sbss .sbss.*)
    *(COMMON)
  } > LP
  .lp_shared (NOLOAD) : ALIGN(4) {
    __lp_shared_start = .;
    KEEP(*(.lp_shared))
    __lp_shared_end = .;
  } > LP
  __bss_end = .;
  . = ALIGN(16);
  __image_ram_end = .;
  __stack_top = ORIGIN(LP) + LENGTH(LP) - 16;
  ASSERT(__image_ram_end <= (__stack_top - 1024), "LP RAM overflow / stack guard < 1KiB")
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.riscv.attributes) }
}
