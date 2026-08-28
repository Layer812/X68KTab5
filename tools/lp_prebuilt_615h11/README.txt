Build 6.15h11 LP prebuilt provenance
====================================
Bare-metal RV32IMAC YM2151 LP image bundled into the HP firmware.

Changes:
- stage / payload / trap breadcrumbs exported in LP RAM
- mtvec trap recorder stores mcause/mepc/mtval
- first RESET event bypasses switch dispatch
- compiled with -O3 -fno-jump-tables

Image:
- target: riscv32-unknown-elf / RV32IMAC + Zicsr / ILP32
- flat binary: 19240 bytes
- SHA256: ec2ae06772838212c2de6abad0177d937e66497682cf4f87b5e069c23eacd4a6
- load address: 0x50108000
- direct entry: 0x50108080
- RAM image/shared end: 0x5010E660
- stack top: 0x5010EFF0
- free gap before stack top: 2448 bytes
