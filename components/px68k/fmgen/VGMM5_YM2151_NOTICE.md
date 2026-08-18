# vgmM5 YM2151 backend notice

Build 5.40-5.42 use `vgmm5_ym2151.cpp`, a PX68K/ESP32-P4-specific YM2151-only backend adapted from the optimized YM2151 path in Layer812's **vgmM5** project:

- Upstream: https://github.com/Layer812/vgmM5
- Reference implementation: `src/fm_engine.c`

Only the YM2151 waveform path is used here. PX68K retains its own guest-visible OPM timer/status/IRQ handling on CPU0. The upstream vgmM5 README states that its original source code, modifications, and matrix-processing sound engine are released under the MIT License.
