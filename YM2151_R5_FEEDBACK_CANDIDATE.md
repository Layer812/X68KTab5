# X68KTab5 — Cardputer R5 YM2151 feedback (isolated candidate)

Baseline: current X68KTab5 `main` at `f58bc0b4d8360fd83845e5a9a9f378b775de4f3f`.

## Already present in X68KTab5 main

- R33: no extra YM2151 modulation x2 (same key improvement as Cardputer R5).
- R36/R38: DT2 ratio / KF upper six bits.
- R59: defined uint32 phase-step wrap.
- R41/R47/R61: internal 62.5kHz native FM with FIR25 and 44.1kHz output.
- Guest YM2151 timer/status/write-busy/IRQ, MIDI, PCM and other devices remain separate.

**Therefore the Cardputer R5 source must never overwrite the X68K-specific sound engine.**

## The one independent remaining discrepancy

R61 algorithm-0 flow still uses prior-sample `mem` between physical
operators 1 and 3. The Cardputer R4/R5 dedicated engine uses the
serial YM2151 OPM logical order, corresponding to physical slots
`0 -> 2 -> 1 -> 3`.

This experimental branch changes **only algorithm 0** in both:
- `synth_channel_modulated_cold` (LFO PM/AM active)
- `synth_channel_zero_mod` (zero LFO depth hot path)

`X68KTAB_YM2151_R5_ALGO0_PREVIEW=1` is enabled here. Defining 0
reproduces exactly the original R61 source path at compile time.
The other 7 algorithms have no source changes.

## Release and acceptance

- Production `main` remains unchanged. No flashing.
- Candidate only, pending R61 144-case PCM continuity test, same-sample
  R61/R5 PCM hash comparison for all 8 algorithms, ESP32-P4 native build
  and actual device performance/audio acceptance.
- Especially protect CPU0 hot-path IRAM use and 62.5kHz FIR overhead;
  PM/AM fallback is retained.
- No game/ROM/VGM data included.
