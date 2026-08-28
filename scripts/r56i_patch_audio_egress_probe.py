#!/usr/bin/env python3
from pathlib import Path
import sys

P = Path('src/tab5_audio.cpp')
MARK = 'PX68K_AUDIO_R56I'

def fail(msg, rc=2):
    print('R56i audio egress patch ERROR:', msg, file=sys.stderr)
    sys.exit(rc)

def main():
    if not P.is_file():
        fail(f'{P} not found; run from G:\\px68k-tab5')
    raw = P.read_bytes()
    nl = '\r\n' if b'\r\n' in raw else '\n'
    s = raw.decode('utf-8', errors='strict').replace('\r\n','\n')

    # Current project contract checks.  Refuse unknown audio source rather than guessing.
    required = [
        'Build 6.12f Tab5 audio cold recovery',
        'M5.Speaker.playRaw(s_play[play_index]',
        'Speaker master volume applied:',
        'static constexpr int kSpeakerChannel = 0;'
    ]
    missing = [x for x in required if x not in s]
    if missing:
        fail('baseline anchors missing: ' + ', '.join(missing), 3)

    if MARK in s:
        # Validate that both halves exist.
        for tok in ['hardware tone probe', 'first nonzero egress']:
            if tok not in s:
                fail(f'marker present but probe fragment missing: {tok}', 4)
        print('R56i audio egress patch: already applied/verified', P)
        return

    # One task-private latch for one-shot PCM telemetry.
    anchor = 'static bool s_nonzero_announced = false;\n'
    if anchor not in s:
        fail('s_nonzero_announced anchor not found', 5)
    s = s.replace(anchor, anchor +
        '/* PX68K_AUDIO_R56I: one-shot physical-egress/PCM-amplitude proof. */\n'
        'static bool s_r56i_egress_logged = false;\n', 1)

    # Insert a short known-good physical output probe after the 6.12f cold recovery
    # has completed, but before the emulator audio feeder task starts.
    vol_anchor = '''    ESP_LOGI(TAG, "Speaker master volume applied: %u/255",\n             (unsigned)M5.Speaker.getVolume());\n'''
    if vol_anchor not in s:
        fail('speaker volume log anchor not found', 6)
    tone = r'''

    /* PX68K_AUDIO_R56I hardware tone probe.
     * This deliberately uses M5Unified's own tone() path, after the exact same
     * Speaker.begin()/ES8388/AMP setup used by emulator PCM and before our
     * feeder starts.  If this 1 kHz/180 ms probe is inaudible, the fault is
     * downstream of the emulator mixer/rings.  Restore the user's 33/255
     * master level immediately afterwards. */
    {
        const uint8_t saved_master = M5.Speaker.getVolume();
        M5.Speaker.setAllChannelVolume(255);
        M5.Speaker.setChannelVolume((uint8_t)kSpeakerChannel, 255);
        M5.Speaker.setVolume(96);
        const bool probe_ok = M5.Speaker.tone(1000.0f, 180u, kSpeakerChannel, true);
        ESP_LOGI(TAG,
                 "PX68K_AUDIO_R56I: hardware tone probe queued=%u master=96 ch=%u running=%u enabled=%u",
                 probe_ok ? 1u : 0u,
                 (unsigned)M5.Speaker.getChannelVolume((uint8_t)kSpeakerChannel),
                 M5.Speaker.isRunning() ? 1u : 0u,
                 M5.Speaker.isEnabled() ? 1u : 0u);
        if (probe_ok)
        {
            uint32_t guard = 0;
            while (M5.Speaker.isPlaying((uint8_t)kSpeakerChannel) && guard++ < 60u)
                vTaskDelay(1);
        }
        M5.Speaker.stop((uint8_t)kSpeakerChannel);
        M5.Speaker.setVolume(saved_master);
        ESP_LOGI(TAG,
                 "PX68K_AUDIO_R56I: hardware tone probe complete restoreMaster=%u queue=%u",
                 (unsigned)M5.Speaker.getVolume(),
                 (unsigned)M5.Speaker.isPlaying((uint8_t)kSpeakerChannel));
    }
'''
    s = s.replace(vol_anchor, vol_anchor + tone, 1)

    # Measure the exact 16-bit stereo buffer submitted to playRaw, before the
    # M5Unified master/channel gain stage.  Log only the first nonzero chunk.
    play_anchor = '        if (M5.Speaker.playRaw(s_play[play_index],\n'
    if play_anchor not in s:
        fail('playRaw anchor not found', 7)
    diag = r'''        /* PX68K_AUDIO_R56I first nonzero egress amplitude proof. */
        if (!s_r56i_egress_logged)
        {
            const int16_t *pb = s_play[play_index];
            const size_t ns = play_frames * 2u;
            uint32_t peak = 0;
            uint64_t abs_sum = 0;
            uint32_t nz = 0;
            for (size_t i = 0; i < ns; ++i)
            {
                int32_t v = pb[i];
                uint32_t a = (uint32_t)(v < 0 ? -v : v);
                if (a > peak) peak = a;
                abs_sum += a;
                nz += (a != 0u);
            }
            if (peak != 0u)
            {
                s_r56i_egress_logged = true;
                ESP_LOGI(TAG,
                         "PX68K_AUDIO_R56I: first nonzero egress frames=%u rate=%u peak=%u meanAbs=%u nz=%u/%u firstLR=%d,%d master=%u ch=%u queue=%u running=%u enabled=%u",
                         (unsigned)play_frames, (unsigned)play_rate,
                         (unsigned)peak, (unsigned)(ns ? abs_sum / ns : 0u),
                         (unsigned)nz, (unsigned)ns,
                         ns > 0 ? (int)pb[0] : 0, ns > 1 ? (int)pb[1] : 0,
                         (unsigned)M5.Speaker.getVolume(),
                         (unsigned)M5.Speaker.getChannelVolume((uint8_t)kSpeakerChannel),
                         (unsigned)M5.Speaker.isPlaying((uint8_t)kSpeakerChannel),
                         M5.Speaker.isRunning() ? 1u : 0u,
                         M5.Speaker.isEnabled() ? 1u : 0u);
            }
        }

'''
    s = s.replace(play_anchor, diag + play_anchor, 1)

    # Postconditions.
    for tok in [
        'PX68K_AUDIO_R56I: hardware tone probe queued=',
        'PX68K_AUDIO_R56I: first nonzero egress frames=',
        'M5.Speaker.tone(1000.0f, 180u, kSpeakerChannel, true)'
    ]:
        if tok not in s:
            fail('postcondition missing: ' + tok, 8)

    P.write_bytes(s.replace('\n', nl).encode('utf-8'))
    print('R56i audio egress patch applied:', P)

if __name__ == '__main__':
    main()
