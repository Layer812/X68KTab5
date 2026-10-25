# vgmM5 YM2151 backend notice

X68K Tab's YM2151-only backend is adapted from the optimized YM2151 synthesizer of Layer812's **vgmM5** project.

- Upstream project: https://github.com/Layer812/vgmM5
- Reference implementation: `src/fm_engine.c` in the public tree; subsequent R58–R61 FIX1 source archives provided by the upstream developer
- Embedded backend: `vgmm5_ym2151.cpp`
- vgmM5 R41 native rate helper: `r41_ym2151_native.h`
- vgmM5 R47 25-tap FIR coefficients/helper: `r47_ym2151_fir25.h`, subsequently optimized in X68K Tab for ring access while retaining bit-identical PCM
- X68K Tab source publication: **2026.10.25**, detailed evidence in `RELEASE_NOTES_2026-10-25.md`

Only the YM2151 waveform path is adapted. All other vgmM5 sound chips, generic media playback, and speaker/USB audio scheduling are not included. PX68K retains its own guest-visible 68000 OPM timer/status/IRQ handling and WRITE BUSY / ADPCM CT1 behavior; the core and final 44.1kHz audio pipeline remain Tab5-specific.

The upstream vgmM5 README states that its original source code and modifications are released under the **MIT License**. Refer to the repository's third-party notices for the remainder of PX68K, Musashi, M5GFX, M5Unified, and other licensed dependencies.

The source changes in this release were applied only to X68K Tab; `Layer812/vgmM5` was not modified by the backport.
