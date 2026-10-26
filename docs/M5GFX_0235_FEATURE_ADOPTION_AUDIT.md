# M5GFX 0.2.35 — Tab5 new-feature adoption audit

Date: 2026-10-26 JST. Project: X68KTab5. Target: ESP32-P4, ESP-IDF 5.5.4. Applies only to isolated Draft PR #10.

Upstream comparison: [M5GFX 0.2.26 → 0.2.35](https://github.com/m5stack/M5GFX/compare/0.2.26...0.2.35).
Pinned revisions: old `729297d6e3d657ddc1ec5189bac2f2ea68828085`, new `65c816a38386062d2c3dc47d41237a9083acbc75`.

## Feature-by-feature findings

| New/upstream feature | Confirmed change / interface | Adoption judgment for X68KTab5 |
| --- | --- | --- |
| **Built-in DSI double framebuffer** | `Panel_DSI.cpp` → `Panel_DSI.inl`; `esp_lcd_dpi_panel_config_t.num_fbs` changed **1 → 2**. | **Already adopted.** Do not apply an older patch that forces `num_fbs=2` again; the exact-guarded compatibility overlay now merely verifies the upstream setting. Two separate driver-owned framebuffers are required by Screen Manager's physical refresh ownership contract. |
| **DMA2D in ESP-IDF DPI panel** | Under ESP-IDF 5.x, `dpi_config.flags.use_dma2d = true;` remains set. **This flag was also present in 0.2.26**, so it is **not a newly unlocked acceleration**. Under IDF 6.x a separate `esp_lcd_dpi_panel_enable_dma2d()` path was added. | **Keep enabled, no separate app change.** X68KTab5 currently runs IDF 5.5.4; switching to new IDF6-only API is not justified. Do not assume driver DMA2D automatically accelerates CPU-managed sparse native stores. |
| **Safer Tab5 panel/touch autodetection** | `M5GFX.cpp` now uses software-I2C probing with pad backup/restore and hardware-I2C handover, a 60-attempt ST touch FW probe and GT911 detection. | **Already adopted** in the upstream submodule. Primarily reduces startup failures, not steady-state MDX drawing cost. Never reinstate the old custom 20-retry loop. |
| **Panel reset and initialization order** | New `Panel_DSI` hooks `getResetDelayBeforeDpi()` and `initInCommandMode()`. New `Panel_ST7102` overrides both; Tab5's ST7121/ST7123/ILI9881C panel configurations do not opt in to those hooks. | **Leave official default behavior.** Do not introduce ST7102-specific initialization into Tab5 for speculative speed gains; these hooks address startup/panel compatibility, not per-frame CPU work. |
| **DSI bus initialization** | `Bus_DSI` now zero-initializes bus/DBI configuration and relies on default `phy_clk_src` instead of manually assigning the default enum. | **Already adopted.** Protect the proven ST7121/7123/ILI9881C panel-specific lane and porch settings. Startup source change offers no established MDX processing reduction. |
| **Internal source consolidation** | Most M5GFX .cpp sources moved to .inl under shared translation units, with conditional additional panels including ST7102, new dither/helper headers, more generic GPIO/I2C support. | **Build-time and compatibility improvements, not a demonstrated faster Tab5 renderer.** Preserve official version; do not rewrite X68KTab5 pixels to use unrelated APIs. |
| **RGB565 drawImage/writePixelsDMA and Panel_FrameBufferBase** | Existing `Panel_FrameBufferBase` CPU-backed framebuffer draw/copy path and `LGFXBase::writePixelsDMA` still exist; frame-buffer implementations largely renamed to .inl. | **Not a proven replacement** for X68KTab5's current source-dirty 8×8 **direct native FB** path. M5GFX `pushImage` adds rotation/copy overhead, and the GFX framebuffer `dmaBusy()` reports false; it is not a DMA completion fence. Keep M5GFX UI fallback, avoid using it for every managed sparse frame. |
| **Second framebuffer/physical DPI handle public access** | Official `Panel_DSI::config_detail_t` still exports **only `buffer`** and not `buffer2` or the native `esp_lcd_panel_handle_t`. | **Not yet replaceable.** Retain the small guarded X68KTab5 overlay exposing `buffer2` and `px68k_dpi_panel` for refresh callbacks and exact back/front ownership. Avoid unsafe guessed handles or additional driver-created FBs. |

## Concrete runtime usage and opportunities

The existing X68KTab5 `src/tab5_video.cpp` has a mature stable sparse path: managed ScreenVersions hold producer-authored dirty masks, map 32px source tiles to exact 8×8 destination tiles, stage in internal SRAM, and update the confirmed native front FB directly. `esp_cache_msync` preserves PSRAM/DSI cache consistency. The managed presenter waits for **physical DSI refresh_done** before retiring the previous screen generation.

Screen Manager and LCD workers run on CPU0 at priority 3; audio feeder and Speaker run at priority 4. Therefore reducing M5GFX per-frame high-level method calls is low ROI because the managed sparse path already bypasses them. The plausible CPU0/PSRAM contention comes from **exception paths**, if the direct-native gate fails or if forced full/recovery frames repeatedly invoke the blocking PPA/DoubleFB route. Guard and observe these existing gates rather than weakening correctness fences:
- `s_dsi_double_live`: 2 distinct buffers, DPI handle, callback registration
- `s_r34_selfcheck_ok`, `s_r35_pack8`, `s_r32_direct_front`: tested direct native conversion/stripe path
- `s_dsi_need_full_sync`, `s_r74_managed_force_full_next`: only when needed, not perpetually
- Preserve 32-byte PPA bursts: earlier 64-byte setting worsened shared-bus fairness
- Preserve LCD refresh fencing, UI ownership, screen-mode changes, and legacy fallback

### Actions taken and acceptance threshold
1. **No speculative modification** to runtime graphics or public Production. M5GFX 0.2.35 built-in 2FB and new panel detection are already used in PR #10.
2. Add CI assertions to protect new M5GFX native capabilities, the Tab5 direct/sparse integration and preservation of physical refresh fencing. These are static regression checks in the existing normal native-build workflow, **not a separate instrumentation build**.
3. MDX stutter is still an unresolved physical-performance issue. No inference that new M5GFX methods improve timing without evidence. Keep PR #10 Draft and keep current tested app BIN available.
4. Only choose another physical candidate if a focused, source-supported runtime change addresses a real fallback/transfer defect; do not modify frame rate, remove refresh events, or lower visual quality speculatively.

References: [Panel_DSI 0.2.35](https://github.com/m5stack/M5GFX/blob/0.2.35/src/lgfx/v1/platforms/esp32p4/Panel_DSI.inl), [Panel_DSI.hpp](https://github.com/m5stack/M5GFX/blob/0.2.35/src/lgfx/v1/platforms/esp32p4/Panel_DSI.hpp), [M5GFX board detection](https://github.com/m5stack/M5GFX/blob/0.2.35/src/M5GFX.cpp), [FrameBufferBase](https://github.com/m5stack/M5GFX/blob/0.2.35/src/lgfx/v1/panel/Panel_FrameBufferBase.inl), [ST7102](https://github.com/m5stack/M5GFX/blob/0.2.35/src/lgfx/v1/platforms/esp32p4/Panel_ST7102.hpp), [X68KTab5 Draft PR](https://github.com/Layer812/X68KTab5/pull/10).
