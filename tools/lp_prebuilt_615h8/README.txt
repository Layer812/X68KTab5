Build 6.15h8 LP prebuilt provenance
===================================
This directory contains the exact bare-metal ELF and flat binary bundled into
components/px68k/fmgen/px68k_lp_fm_blob.inc.

Critical h8 fix:
- ESP-IDF 5.4.x ulp_lp_core_run(skip_lp_rom_boot=true) sets the LP application
  boot address to LP_RAM + 0x80. h7 incorrectly placed _start at +0x00.
- The h8 binary therefore contains an explicit 0x80-byte prefix and places
  _start at 0x50108080.
- HP startup now requires ready magic 0x4C50464D and a completed RESET command
  heartbeat before declaring the LP FM backend ACTIVE.

Final image profile:
- target: riscv32-unknown-elf / RV32IMAC + Zicsr / ILP32
- optimization: -O3
- flat binary: 18416 bytes
- SHA256: b3bb67bb9d4578310537b617edb194b3bb04c713177bc185fb3ef19ec227634d
- load address: 0x50108000
- direct entry: 0x50108080
- RAM image/shared end: 0x5010E300
- stack top: 0x5010EFF0
- free gap before stack top: 3312 bytes

Normal PlatformIO builds do not rebuild this image.
