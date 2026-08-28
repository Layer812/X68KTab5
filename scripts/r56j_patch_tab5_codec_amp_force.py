#!/usr/bin/env python3
from pathlib import Path
import sys

P = Path('src/tab5_audio.cpp')
MARK = 'PX68K_AUDIO_R56J'

def fail(msg, rc=2):
    print('R56j codec/amp patch ERROR:', msg, file=sys.stderr)
    sys.exit(rc)

def main():
    if not P.is_file():
        fail(f'{P} not found; run from G:\\px68k-tab5')
    raw = P.read_bytes()
    nl = '\r\n' if b'\r\n' in raw else '\n'
    s = raw.decode('utf-8', errors='strict').replace('\r\n','\n')

    required = [
        'PX68K_AUDIO_R56I: hardware tone probe queued=',
        'M5.Speaker.tone(1000.0f, 180u, kSpeakerChannel, true)',
        'Build 6.12f Tab5 audio cold recovery',
    ]
    missing = [x for x in required if x not in s]
    if MARK in s:
        for tok in ['codec+amp force', 'ES8388{02=', 'tone probe queued=']:
            if tok not in s:
                fail('marker present but fragment missing: ' + tok, 3)
        print('R56j codec/amp patch: already applied/verified', P)
        return
    if missing:
        fail('baseline anchors missing: ' + ', '.join(missing), 4)

    anchor = '        M5.Speaker.setAllChannelVolume(255);\n'
    if anchor not in s:
        fail('R56i tone gain anchor missing', 5)

    force = r'''        /* PX68K_AUDIO_R56J: force the exact M5Unified Tab5 physical-output
         * contract once, then read it back.  M5Unified's Tab5 callback programs
         * ES8388 at 0x10 and sets PI4IO1(0x43) output register 0x05 bit1 for
         * SPK_EN.  The R56i proof showed queued audio but no physical sound, so
         * repeat the authoritative hardware sequence explicitly instead of
         * touching YM2151, host rings, or Screen Manager. */
        bool r56j_hw_ok = true;
        auto r56j_es8388_write = [&](uint8_t reg, uint8_t value) -> bool {
            return M5.In_I2C.writeRegister(0x10u, reg, &value, 1u, 400000u);
        };
        struct r56j_regval_t { uint8_t reg; uint8_t value; };
        static const r56j_regval_t r56j_es8388_enable[] = {
            {0,0x80}, {0,0x00}, {0,0x00}, {0,0x0E},
            {1,0x00}, {2,0x0A}, {3,0xFF}, {4,0x3C},
            {5,0x00}, {6,0x00}, {7,0x7C}, {8,0x00},
            {23,0x18}, {24,0x00}, {25,0x20}, {26,0x00}, {27,0x00},
            {28,0x08}, {29,0x00}, {38,0x00}, {39,0xB8}, {42,0xB8},
            {43,0x08}, {45,0x00}, {46,0x21}, {47,0x21}, {48,0x21}, {49,0x21}
        };
        for (size_t i = 0; i < sizeof(r56j_es8388_enable)/sizeof(r56j_es8388_enable[0]); ++i)
            r56j_hw_ok = r56j_es8388_write(r56j_es8388_enable[i].reg,
                                           r56j_es8388_enable[i].value) && r56j_hw_ok;
        M5.In_I2C.bitOn(0x43u, 0x05u, 0x02u, 400000u);
        vTaskDelay(pdMS_TO_TICKS(20));

        const uint8_t r56j_r02 = M5.In_I2C.readRegister8(0x10u, 2u, 400000u);
        const uint8_t r56j_r04 = M5.In_I2C.readRegister8(0x10u, 4u, 400000u);
        const uint8_t r56j_r17 = M5.In_I2C.readRegister8(0x10u, 23u, 400000u);
        const uint8_t r56j_r19 = M5.In_I2C.readRegister8(0x10u, 25u, 400000u);
        const uint8_t r56j_r27 = M5.In_I2C.readRegister8(0x10u, 39u, 400000u);
        const uint8_t r56j_r2a = M5.In_I2C.readRegister8(0x10u, 42u, 400000u);
        const uint8_t r56j_amp = M5.In_I2C.readRegister8(0x43u, 0x05u, 400000u);
        ESP_LOGI(TAG,
                 "PX68K_AUDIO_R56J: codec+amp force writeOK=%u ES8388{02=%02X 04=%02X 17=%02X 19=%02X 27=%02X 2A=%02X} PI4IO05=%02X ampBit=%u",
                 r56j_hw_ok ? 1u : 0u,
                 (unsigned)r56j_r02, (unsigned)r56j_r04,
                 (unsigned)r56j_r17, (unsigned)r56j_r19,
                 (unsigned)r56j_r27, (unsigned)r56j_r2a,
                 (unsigned)r56j_amp, (r56j_amp & 0x02u) ? 1u : 0u);
'''
    s = s.replace(anchor, force + anchor, 1)

    s = s.replace('        M5.Speaker.setVolume(96);\n',
                  '        M5.Speaker.setVolume(255);\n', 1)
    s = s.replace('M5.Speaker.tone(1000.0f, 180u, kSpeakerChannel, true)',
                  'M5.Speaker.tone(4000.0f, 1000u, kSpeakerChannel, true)', 1)
    s = s.replace('PX68K_AUDIO_R56I: hardware tone probe queued=%u master=96 ch=%u running=%u enabled=%u',
                  'PX68K_AUDIO_R56J: tone probe queued=%u freq=4000Hz dur=1000ms master=255 ch=%u running=%u enabled=%u', 1)
    s = s.replace('while (M5.Speaker.isPlaying((uint8_t)kSpeakerChannel) && guard++ < 60u)',
                  'while (M5.Speaker.isPlaying((uint8_t)kSpeakerChannel) && guard++ < 180u)', 1)
    s = s.replace('PX68K_AUDIO_R56I: hardware tone probe complete restoreMaster=%u queue=%u',
                  'PX68K_AUDIO_R56J: tone probe complete restoreMaster=%u queue=%u', 1)

    for tok in [
        'PX68K_AUDIO_R56J: codec+amp force writeOK=',
        'PX68K_AUDIO_R56J: tone probe queued=',
        'M5.Speaker.tone(4000.0f, 1000u, kSpeakerChannel, true)',
        'M5.In_I2C.bitOn(0x43u, 0x05u, 0x02u, 400000u)',
        'readRegister8(0x10u, 39u, 400000u)'
    ]:
        if tok not in s:
            fail('postcondition missing: ' + tok, 6)

    P.write_bytes(s.replace('\n', nl).encode('utf-8'))
    print('R56j codec/amp force patch applied:', P)

if __name__ == '__main__':
    main()
