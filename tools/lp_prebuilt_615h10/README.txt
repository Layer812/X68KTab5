Build 6.15h10 LP prebuilt provenance
====================================
Bare-metal RV32IMAC YM2151 LP image bundled into the HP firmware.

Critical h10 changes:
- hp_probe/lp_echo is the authoritative command producer doorbell/ACK after startup.
- cmd_head remains HP telemetry/local producer state.
- direct entry remains LP_RAM+0x80.

Final image profile:
- target: riscv32-unknown-elf / RV32IMAC + Zicsr / ILP32
- optimization: -O3
- flat binary: 18460 bytes
- SHA256: 48f201736a7a35a7df008dce69e54e0ef42e7289388c582c2d9d6675a4ccac0a
- load address: 0x50108000
- direct entry: 0x50108080
- RAM image/shared end: 0x5010E340
- stack top: 0x5010EFF0
- free gap before stack top: 3248 bytes

Normal PlatformIO builds do not rebuild this image.
