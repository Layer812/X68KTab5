Build 6.15h13 LP prebuilt provenance
=====================================
Bare-metal RV32IMAC MSM6258 ADPCM coprocessor for ESP32-P4 LP core.

Architecture:
- CPU1 remains authoritative for X68000 DMA timing, ADPCM status, IRQ and register timing.
- LP core handles MSM6258 nibble decode, cubic 44.1-kHz resampling, pan, volume/LPF and PCM production.
- CPU0 restores the high-quality 44.1-kHz YM2151 backend and final saturation mix.
- Shared SPSC command ring: 256 events.
- Shared LP PCM ring: 4096 stereo frames (~92.9 ms at 44.1 kHz).

Image:
- target: riscv32-unknown-elf / RV32IMAC + Zicsr / ILP32
- compile: clang 17 -O3 -fno-jump-tables -ffreestanding -nostdlib
- flat binary: 3311 bytes
- SHA256: a9fd84a1c04e299110f96418342b48beb7045ff260c00fbdf75cc085e6a90832
- load address: 0x50108000
- direct entry: 0x50108080
- RAM image/shared end: 0x5010D9B0
- stack top: 0x5010EFF0
- free gap before stack top: 5696 bytes

Validation:
- no unresolved symbols / no libc dependency
- compact nibble delta math exhaustively matches PX68K dif_table for all 49 steps x 16 nibbles
- HP/LP fixed addresses generated from the linked ELF symbol table
