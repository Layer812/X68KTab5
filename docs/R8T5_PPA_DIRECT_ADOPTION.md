# R8T5 PPA Direct — safe adoption in X68KTab5 (2026-11-22)

## Source and scope

- R8T5 experiment: [`work/r8t5-ppa-srm-wired-20261121`](https://github.com/Layer812/R8T5/tree/work/r8t5-ppa-srm-wired-20261121), `src/m5stack_main.cpp`.
- X68KTab5 baseline: Production R6 on `main`, [`src/tab5_video.cpp`](../src/tab5_video.cpp).
- This change is **experimental**, branch-only until physical Tab5 acceptance. No CPU1, PCM scheduling, YM2151, display timing, resolutions, or storage changes.

## What transfers

R8T5 demonstrates an ESP32-P4 PPA SRM transaction writing RGB565 output **directly to the DSI-owned framebuffer**, rather than pushing a second scaled image via M5GFX.

X68KTab5 already has that destination path for full/recovery frames. Its steady-state 8x8-tile direct-native renderer is deliberately retained. A full PPA 1:1 rotation (prepared 960x720 RGB565 landscape stage -> 720x960 native DSI region) overwrites every pixel of the game viewport.

Previously the Back framebuffer's entire **960x720x2 = 1,382,400-byte (1.32 MiB)** game viewport was first set to zero and written back to PSRAM, only to be overwritten by the blocking PPA SRM transaction.

This branch skips that redundant zero-fill **only** when the requested physical update has one full [0,720) source band and exactly matches the centered 960x720 game viewport. It invalidates the aligned PPA output region using `esp_cache_msync(... DIR_M2C | TYPE_DATA)` before SRM. If alignment/cache invalidation fails, it takes the old zero-fill + C2M path.

### What stays untouched

- Nearest-neighbor pixel-perfect scaling from guest video to the 960x720 staging image.
- The PPA rotation angle (90/270 degrees), RGB565 format, 32-byte PPA burst length, and blocking SRM client.
- DSI double buffers, UI side bars, native frame pointer swap, and `refresh_done` fence.
- Partial/dirty-band updates and direct-native 8x8 tile renderer; other callers of the game-clear helper use the previous behavior.
- Audio-first scheduling and the production MDX stabilization.

## Why *not* copy R8T5's PPA scaler

ESP-IDF 5.5's PPA SRM scaling is **bilinear** (not the existing nearest-neighbor X68000 pixel mapping), and scale factors are quantized at 1/16. Substituting direct PPA scaling would risk blurred edges and geometry mismatches in X68000 display modes. See [ESP-IDF 5.5 PPA documentation](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32p4/api-reference/peripherals/ppa.html).

## Acceptance boundary

Code was updated on a separate development branch. No firmware image or Tab5 hardware verification is claimed here. The change removes redundant memory traffic; **the amount of actual LCD frame-time improvement and any impact on audio can only be determined from a physical run**. Do not merge into Production merely because it compiles.
