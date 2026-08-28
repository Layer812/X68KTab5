Build 6.15h12 LP prebuilt provenance
====================================
Bare-metal RV32IMAC YM2151 LP image bundled into the HP firmware.

Changes from h11:
- LP synthesis reduced from 22.05 kHz to 11.025 kHz.
- HP holds each LP stereo sample for four 44.1-kHz mixer frames.
- phase/LFO/noise timing scaled x2 per LP tick; EG cadence kept near 5.5 kHz.
- AudioShield disabled on HP so LIVE LCD never freezes by policy.
- trap breadcrumbs / straight RESET / volatile doorbell retained.

Image:
- target: riscv32-unknown-elf / RV32IMAC + Zicsr / ILP32
- compile: clang 17 -O3 -fno-jump-tables -ffreestanding -nostdlib
- flat binary: 19252 bytes
- SHA256: 0771c7ecb5806ab2d218fdbba53d199228c1ffbd2e8d3759f4b4207b771796be
- load address: 0x50108000
- direct entry: 0x50108080
- RAM image/shared end: 0x5010E670
- stack top: 0x5010EFF0
- free gap before stack top: 2432 bytes
