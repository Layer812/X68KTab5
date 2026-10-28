#!/usr/bin/env python3
"""Guarded one-line Tab5 ES8388/I2S MCLK ratio compatibility fix.

M5Unified 0.2.28 uses I2S driver-managed MCLK=256*sample_rate on ESP32-P4,
but retained ES8388 DACCONTROL2 (reg 0x18) 0x00, the previous 128fs ratio.
The ES8388 vendor application guide documents 0x02 for 256fs.
No changes to audio producer timing, PCM, task priorities, speaker ring, or LCD.
"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent
UNIFIED = ROOT / "components" / "M5Unified"
SOURCE = UNIFIED / "src" / "M5Unified.inl"
SPEAKER = UNIFIED / "src" / "utility" / "Speaker_Class.inl"
PINNED = "368ad60807161625aa65baad37a4a3af2a7eccea"
MARK = "X68KTAB_ES8388_MCLK256_R1"
BEFORE = b"      2,   24, 0x00, //I2S MCLK ratio (128)"
AFTER = b"      2,   24, 0x02, //X68KTAB_ES8388_MCLK256_R1: P4 I2S driver MCLK=256fs"


def require(ok: bool, why: str) -> None:
    if not ok:
        raise RuntimeError(why)


def main() -> int:
    head = subprocess.check_output(
        ["git", "-C", str(UNIFIED), "rev-parse", "HEAD"], text=True
    ).strip()
    require(head == PINNED, f"Unexpected M5Unified source: {head}")
    require(SOURCE.is_file() and SPEAKER.is_file(), "Missing pinned M5Unified sources")
    speaker = SPEAKER.read_bytes()
    require(b"#if M5UNIFIED_I2S_DRIVER_MANAGED_CLK" in speaker
            and b"I2S_MCLK_MULTIPLE_256" in speaker
            and b"i2s_config.clk_cfg.sample_rate_hz = _cfg.sample_rate;" in speaker,
            "ESP32-P4 MCLK=256fs source premise changed")
    data = SOURCE.read_bytes()
    cb = data.index(b"bool M5Unified::_speaker_enabled_cb_tab5")
    end = data.index(b"if (enabled)", cb)
    table = data[cb:end]
    require(b"      2,    8, 0x00, //set I2S slave mode" in table,
            "Tab5 ES8388 mode changed")
    if data.count(AFTER) == 1 and data.count(BEFORE) == 0:
        require(table.count(AFTER) == 1, "ES8388 patch marker outside Tab5 callback")
        print(f"PASS {MARK}: already applied and verified")
        return 0
    require(data.count(BEFORE) == 1 and data.count(AFTER) == 0,
            "Unexpected / ambiguous ES8388 DACCONTROL2 ratio")
    require(table.count(BEFORE) == 1, "Expected 128fs ratio missing from Tab5 callback")
    patched = data.replace(BEFORE, AFTER)
    require(patched.replace(AFTER, BEFORE) == data, "Unexpected source differences")
    SOURCE.write_bytes(patched)
    require(SOURCE.read_bytes() == patched, "Source write verification failed")
    print(f"PASS {MARK}: Tab5 ES8388 DACCONTROL2 0x18: 0x00(128fs) -> 0x02(256fs)")
    print("Only M5Unified Tab5 ES8388 initial register table was changed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
