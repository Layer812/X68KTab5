Build 6.15h7 LP prebuilt provenance
===================================
This directory contains the exact bare-metal ELF and flat binary bundled into
components/px68k/fmgen/px68k_lp_fm_blob.inc.

Final image profile:
- target: riscv32-unknown-elf / RV32IMAC + Zicsr / ILP32
- optimization: -O3 (the LP experiment is performance-sensitive)
- flat binary: 18216 bytes
- SHA256: b45192b6c26a7a52b8a37897788d82711fffe23044f7830b0647f29f8591e91a
- load/entry: 0x50108000
- RAM image/shared end: 0x5010E240
- stack top: 0x5010EFF0
- free gap before stack top: 3504 bytes

The image is entered with skip_lp_rom_boot=true. It uses no ESP-IDF LP runtime
calls. start.S sets the stack and clears BSS/shared state. lp.ld reserves 0x7000
bytes, matching CONFIG_ULP_COPROC_RESERVE_MEM=28672.

Normal PlatformIO builds MUST NOT rebuild this image. This deliberately follows
the precompiled-LP-image integration pattern used by ESP32P4_ULP. The firmware
source remains components/px68k/ulp_fm/lp_fm.c.

Reproducible build (Clang/LLD 17 or compatible):
  clang --target=riscv32-unknown-elf -march=rv32imac_zicsr -mabi=ilp32 -O3 \
    -ffreestanding -fno-builtin -fno-stack-protector -fdata-sections \
    -ffunction-sections -msmall-data-limit=0 -c start.S -o start.o
  clang --target=riscv32-unknown-elf -march=rv32imac_zicsr -mabi=ilp32 -O3 \
    -ffreestanding -fno-builtin -fno-stack-protector -fdata-sections \
    -ffunction-sections -msmall-data-limit=0 -std=c11 \
    -c ../../components/px68k/ulp_fm/lp_fm.c -o lp_fm.o
  clang --target=riscv32-unknown-elf -march=rv32imac_zicsr -mabi=ilp32 \
    -fuse-ld=lld -nostdlib -Wl,--gc-sections -Wl,-T,lp.ld \
    start.o lp_fm.o -o px68k_lp_fm.elf
  llvm-objcopy -O binary px68k_lp_fm.elf px68k_lp_fm.bin
