#!/usr/bin/env python3
"""Tab5-only M5Unified 0.2.28 speaker idle fast-resume overlay.

When flg_nodata is true, M5Unified may send 128f of I2S silence *before*
examining a pending playRaw notification. Avoid that redundant hold write
ONLY when the notification is already pending on the Tab5 PCM path. Preserve
the original hold write if no notification has arrived.
"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent
UNIFIED = ROOT / "components" / "M5Unified"
SOURCE = UNIFIED / "src" / "utility" / "Speaker_Class.inl"
PINNED = "368ad60807161625aa65baad37a4a3af2a7eccea"
MARK = "X68KTAB_SPK_FAST_WAKE_R1"

BEFORE = b"""          else
          { // Keep the DMA fed at the hold level instead of sleeping. A timed wait can let the
            // DMA reach the auto-cleared buffers, which output 0 (on the DAC, a drop to the zero
            // level and a jump back to the bias). The blocking write paces this loop.
            size_t words = dma_buf_len >> 1;
            if (self->_cfg.use_dac)
            {
              fill_dac(words, dac_hold);
            }
            else
            {
              memset(sound_buf32, 0, words * sizeof(int32_t));
            }
            size_t write_bytes;
            _spk_i2s_write(i2s_port, sound_buf32, words * sizeof(int32_t), &write_bytes, portMAX_DELAY);
            flg_nodata = (0 == ulTaskNotifyTake( pdFALSE, 0 ));
          }
"""
AFTER = b"""          else
          {
#if defined(CONFIG_IDF_TARGET_ESP32P4)
            // X68KTAB_SPK_FAST_WAKE_R1: a new PCM slot is already published.
            // Avoid injecting a 128-frame zero/hold I2S block before reading it.
            // Tab5 ES8388 stereo only; all other devices use the old path.
            if (!self->_cfg.use_dac && !self->_cfg.buzzer
             && self->_cfg.pin_data_out == GPIO_NUM_26
             && ulTaskNotifyTake(pdFALSE, 0))
            {
              flg_nodata = false;
            }
            else
#endif
            {
              // No ready slot: keep the original DMA hold/zero behavior.
              // On DAC devices this prevents a sudden output-level step.
              size_t words = dma_buf_len >> 1;
              if (self->_cfg.use_dac)
              {
                fill_dac(words, dac_hold);
              }
              else
              {
                memset(sound_buf32, 0, words * sizeof(int32_t));
              }
              size_t write_bytes;
              _spk_i2s_write(i2s_port, sound_buf32, words * sizeof(int32_t), &write_bytes, portMAX_DELAY);
              flg_nodata = (0 == ulTaskNotifyTake( pdFALSE, 0 ));
            }
          }
"""


def require(ok: bool, message: str) -> None:
    if not ok:
        raise RuntimeError(message)


def main() -> int:
    head = subprocess.check_output(
        ["git", "-C", str(UNIFIED), "rev-parse", "HEAD"], text=True
    ).strip()
    require(head == PINNED, f"Unexpected M5Unified SHA {head}")
    require(SOURCE.is_file(), "Missing upstream Speaker_Class.inl")
    src = SOURCE.read_bytes()
    require(b"if (flg_nodata)" in src and b"i2s_config.clk_cfg.sample_rate_hz = _cfg.sample_rate;" in src,
            "M5Unified 0.2.28 Speaker source model has changed")
    if src.count(AFTER) == 1 and src.count(BEFORE) == 0:
        print(f"PASS {MARK}: exact idempotent overlay already applied")
        return 0
    require(src.count(BEFORE) == 1 and src.count(AFTER) == 0,
            "Unknown Speaker idle branch, not safe to patch")
    result = src.replace(BEFORE, AFTER)
    require(result.replace(AFTER, BEFORE) == src, "Speaker patch changed another region")
    SOURCE.write_bytes(result)
    require(SOURCE.read_bytes() == result, "Speaker source write-back failed")
    print(f"PASS {MARK}: ready PCM request skips redundant 128f idle DMA padding on Tab5")
    print("Untouched: no-notification DMA hold, DAC, buzzer, PCM, I2S rate, timing, memory")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
