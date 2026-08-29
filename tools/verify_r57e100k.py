from pathlib import Path
import sys

ROOT = Path('.')

def read(path):
    p = ROOT / path
    if not p.is_file():
        raise SystemExit(f'VERIFY ERROR: missing file: {path}')
    return p.read_text(encoding='utf-8', errors='replace')

def require(text, needle, label):
    if needle not in text:
        raise SystemExit(f'VERIFY ERROR: missing {label}: {needle}')

def forbid(text, needle, label):
    if needle in text:
        raise SystemExit(f'VERIFY ERROR: forbidden {label}: {needle}')

mode = sys.argv[1] if len(sys.argv) > 1 else 'source'

if mode == 'source':
    print('[verify] reading sources...', flush=True)
    main = read('src/main.c')
    video = read('src/tab5_video.cpp')
    audio = read('src/tab5_audio.cpp')
    screen = read('src/tab5_screen_manager.c')
    libretro = read('components/px68k/libretro.c')

    checks = [
        (main, 'PX68K_R57E100K', 'R100K marker'),
        (main, 'PX68K_TAB5KBD_R57E99K_EVT', 'retained R99K EVT marker'),
        (main, 'PX68K_KEYPIPE_R57E100K_ENQ', 'R100K enqueue marker'),
        (main, 'PX68K_KEYPIPE_R57E100K_CPU1', 'R100K CPU1 pipe marker'),
        (main, 'PX68K_TAB5KBD_R57E99K_HEALTH', 'retained R99K HEALTH marker'),
        (main, 'tab5kbd_queue_x68k_retrok', 'A164 bridge function'),
        (main, 'tab5_guest_input_queue_x68k_scancode', 'direct X68K queue'),
        (main, 'pdMS_TO_TICKS(10)', 'nonzero A164 delay'),
        (main, 'JIT99_KBD', 'retained JIT99 marker'),
        (main, 'const int enq_ok = tab5_guest_input_queue_x68k_scancode', 'checked A164 enqueue return'),
        (main, '__atomic_store_n(&s_tab5kbd_pipe_seq_pub', 'cross-core breadcrumb publish'),
        (main, 'TAB5KBD_REG_KEY_EVENT   0x20u', 'KEY_EVENT register'),
        (main, 'TAB5KBD_MODE_NORMAL     0u', 'Normal mode'),
        (main, 'TAB5KBD_EVENT_EMPTY     0xFFu', 'empty sentinel'),
        (main, 'TAB5KBD_KEYS            70u', '70-key table'),
        (main, 'driver/i2c_master.h', 'new I2C API'),
        (main, 'TAB5KBD_I2C_PORT_AUTO', 'AUTO I2C'),
        (main, 'GPIO_INTR_NEGEDGE', 'A164 falling-edge IRQ'),
        (main, 'gpio_get_level(TAB5KBD_INT_GPIO) == 0', 'INT low-level fallback'),
        (main, 'const bool safety_poll = ((tick10 % 5u) == 0u);', '50ms safety poll'),
        (main, 'irq || int_low || safety_poll', 'official readiness condition'),
        (main, 'official lifecycle', 'official lifecycle marker'),
        (main, '33333LL', 'Turbo30 cadence'),
        (video, 'PX68K_TURBO_R57E97T', 'Turbo UI marker'),
        (video, 'tab5_video_turbo_enabled', 'Turbo state'),
        (video, 's_turbo_audio_22k ? "Turbo" : "N/A"', 'Turbo/N-A UI'),
        (video, 'tab5_audio_set_high_load_22k', 'manual audio rate owner'),
        (audio, 'PX68K_AUDIO_R57E96T', 'manual-rate audio marker'),
        (screen, 'R57E97T Turbo30: explicitly favor screen freshness', 'Turbo30 screen bias'),
        (libretro, 'PX68K_PACE_R57E88', 'R88 governor marker'),
    ]
    for i, (txt, needle, label) in enumerate(checks, 1):
        require(txt, needle, label)
        print(f'[verify] source {i:02d}/{len(checks):02d} OK: {label}', flush=True)

    audio_contract = [
        'static constexpr size_t kRingFrames = 32768;',
        'static constexpr size_t kChunkFrames = 512;',
        'static constexpr size_t kStartupWatermarkFrames = 3072;',
        'static constexpr size_t kResumeWatermarkFrames = 2048;',
        'static constexpr size_t kLowWatermarkFrames = 512;',
        'static constexpr size_t kPlayBuffers = 3;',
    ]
    for needle in audio_contract:
        require(audio, needle, 'R94 audio buffer contract')
    print('[verify] R94 audio buffer contract OK', flush=True)

    guard_contract = [
        'Q_CRITICAL_ENTER = 4096u',
        'Q_GUARD_ENTER = 8192u',
        'Q_CRITICAL_EXIT = 8192u',
        'Q_NORMAL_EXIT = 12288u',
        'budget.preexec_q_effective < 2048u',
        'budget.preexec_q_effective >= 8192u',
    ]
    for needle in guard_contract:
        require(main, needle, 'R94 guard contract')
    print('[verify] R94 guard contract OK', flush=True)

    forbidden = [
        (main, 'vTaskDelay(pdMS_TO_TICKS(5))', 'zero-tick 5ms A164 delay'),
        (main, 'TAB5KBD_REG_HID_EVENT', 'old HID path'),
        (main, 'TAB5KBD_MODE_HID', 'old HID mode'),
        (main, 'tab5_audio_set_high_load_22k', 'automatic sample-rate switching in main.c'),
        (main, 'present~15fps', 'old Turbo cadence'),
        (video, 'present~15fps', 'old Turbo cadence'),
        (main, 'direct KEY_EVENT fallback', 'R96 direct-pop path'),
        (libretro, 'PX68K_PACE_R57E93', 'rejected R93'),
        (libretro, 'GuestPaceR93', 'rejected R93'),
        (audio, 'r92_observe_outpcm(obuf, play_frames)', 'retired R92 full scan'),
        (main, 'driver/i2c.h', 'legacy I2C API'),
        (main, 'i2c_driver_install', 'legacy I2C driver'),
        (main, 'PX68K_R57E90', 'rejected R90'),
    ]
    for txt, needle, label in forbidden:
        forbid(txt, needle, label)
    print('[verify] forbidden legacy/rejected paths absent', flush=True)
    print('SOURCE VERIFY PASS', flush=True)

elif mode == 'elf':
    if len(sys.argv) < 4:
        raise SystemExit('usage: verify_r57e100k.py elf <strings.txt> <nm.txt>')
    strings = Path(sys.argv[2]).read_text(encoding='utf-8', errors='replace')
    nm = Path(sys.argv[3]).read_text(encoding='utf-8', errors='replace')
    need_strings = [
        'PX68K_R57E100K',
        'PX68K_TAB5KBD_R57E99K_EVT',
        'PX68K_KEYPIPE_R57E100K_ENQ',
        'PX68K_KEYPIPE_R57E100K_CPU1',
        'PX68K_TAB5KBD_R57E99K_HEALTH',
        'target=30fps',
        'JIT99_KBD',
        'mode=NORMAL',
        'PX68K_TURBO_R57E97T',
        'PX68K_AUDIO_R57E96T',
        'JIT94_AUDIOFORENSIC',
        'JIT91_SUBMIT',
        'JIT91_M5',
        'chunk=512 slots=2',
        'thresholds=8192/16384',
    ]
    for i, needle in enumerate(need_strings, 1):
        require(strings, needle, 'ELF string')
        print(f'[verify] ELF string {i:02d}/{len(need_strings):02d} OK', flush=True)
    forbid(strings, 'PX68K_PACE_R57E93', 'R93 ELF marker')
    forbid(strings, 'R57E67_TIMER f=', 'legacy periodic TIMER audit')
    forbid(strings, 'R57E67_PCM f=', 'legacy periodic PCM audit')
    forbid(strings, 'R57E67_PACE f=', 'legacy periodic PACE audit')
    require(nm, 'px68k_m5spk_r57e91_get', 'M5 speaker audit symbol')
    require(nm, 'i2c_new_master_bus', 'new I2C symbol')
    forbid(nm, 'i2c_driver_install', 'legacy I2C symbol')
    forbid(nm, 'check_i2c_driver_conflict', 'legacy driver conflict symbol')
    print('ELF VERIFY PASS', flush=True)
else:
    raise SystemExit(f'unknown mode: {mode}')
