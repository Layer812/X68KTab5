Build 6.15h9 LP prebuilt provenance
===================================
Bare-metal RV32IMAC YM2151 LP image bundled into the HP firmware.

Critical h9 changes:
- HP<->LP shared queues use volatile 32-bit loads/stores plus explicit RISC-V fences.
- C __atomic/AMO operations are removed from the LP-RAM SPSC path.
- hp_probe/lp_echo handshake proves live HP->LP shared-memory visibility before ACTIVE.
- Direct entry remains LP_RAM+0x80 for ESP-IDF 5.4.x skip_lp_rom_boot.

Final image profile:
- target: riscv32-unknown-elf / RV32IMAC + Zicsr / ILP32
- optimization: -O3
- flat binary: 18476 bytes
- SHA256: cdade9c4f44cbc0301310631163cd31820448a34b11b6d5a322d3ad3860daa6d
- load address: 0x50108000
- direct entry: 0x50108080
- RAM image/shared end: 0x5010E350
- stack top: 0x5010EFF0
- free gap before stack top: 3232 bytes

Normal PlatformIO builds do not rebuild this image.
