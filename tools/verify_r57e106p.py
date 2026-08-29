from pathlib import Path
import sys
ROOT=Path('.')

def read(p):
    q=ROOT/p
    if not q.is_file():
        raise SystemExit(f'VERIFY ERROR: missing file: {p}')
    return q.read_text(encoding='utf-8', errors='replace')

def req(t,n,l):
    if n not in t:
        raise SystemExit(f'VERIFY ERROR: missing {l}: {n}')

def forbid(t,n,l):
    if n in t:
        raise SystemExit(f'VERIFY ERROR: forbidden {l}: {n}')

mode=sys.argv[1] if len(sys.argv)>1 else 'source'
if mode=='source':
    main=read('src/main.c')
    audio=read('src/tab5_audio.cpp')
    screen=read('src/tab5_screen_manager.c')

    # Frozen functional contracts.
    for n,l in [
      ('PX68K_R57E106P: PRODUCTION QUIET PASS1 ACTIVE','R106P production marker'),
      ('4096, NULL, 3,','A164 priority 3'),
      ('tab5kbd_r101_direct_poll_cpu0();','A164 direct KEY_EVENT acquisition'),
      ('vTaskDelay(pdMS_TO_TICKS(10));','A164 real 10ms blocking cadence'),
      ('TAB5KBD_REG_KEY_EVENT, &raw, 1u','KEY_EVENT 0x20 direct read'),
      ('{0x0D,0x02},{0x33,0x02},{0x1B,0x00}','R105 Sym ~/? keymap'),
      ('{0x17,0x00},{0x18,0x00},{0x19,0x00},{0x1A,0x00},{0x28,0x00},{0x03,0x02},{0x0F,0x00}','R105 :/quote keymap'),
      ('33333LL','Turbo30 wall cadence'),
    ]:
        req(main,n,l)
    for n in ['static constexpr size_t kRingFrames = 32768;',
              'static constexpr size_t kChunkFrames = 512;',
              'static constexpr size_t kStartupWatermarkFrames = 3072;',
              'static constexpr size_t kResumeWatermarkFrames = 2048;',
              'static constexpr size_t kLowWatermarkFrames = 512;',
              'static constexpr size_t kPlayBuffers = 3;']:
        req(audio,n,'R94 audio geometry')

    # Production gates/removals.
    req(main,'#define PX68K_TAB5_RELEASE_DIAGNOSTICS 0','release diagnostics OFF')
    forbid(main,'tab5kbd_health_audit_cpu0();','live keyboard health audit call')
    forbid(main,'PX68K_TAB5KBD_R57E101K_RAW #','live A164 RAW UART trace')
    forbid(main,'PX68K_KEYPIPE_R57E100K_CPU1 f=','live CPU1 KEYPIPE sampler')
    forbid(main,'PX68K_KEYPIPE_R57E105P_ENQ seq=','live CPU0 KEYPIPE enqueue trace')
    req(main,'#if PX68K_TAB5_RELEASE_DIAGNOSTICS\n/* R57E72: low-overhead stutter flight recorder.','JIT72 compile-out gate')
    req(audio,'#endif /* PX68K_TAB5_RELEASE_DIAGNOSTICS: R91/R92 forensics */','R91/R92 compile-out gate')
    req(screen,'PX68K_SCREEN_R57E106P: production screen measurement counters compiled OUT','R89 retirement marker')
    forbid(main,'vTaskDelay(pdMS_TO_TICKS(5))','zero-tick A164 delay')
    print('SOURCE VERIFY PASS')
elif mode=='elf':
    if len(sys.argv)<4:
        raise SystemExit('usage: verify_r57e106p.py elf <strings> <nm>')
    strings=Path(sys.argv[2]).read_text(errors='replace')
    nm=Path(sys.argv[3]).read_text(errors='replace')
    for n in ['PX68K_R57E106P: PRODUCTION QUIET PASS1 ACTIVE',
              'PX68K_TAB5KBD_R57E105P_MAPCERT PASS',
              'target=30fps', 'PX68K_AUDIO_R57E96T']:
        req(strings,n,'required production ELF string')
    for n,l in [
      ('PX68K_TAB5KBD_R57E102P_HEALTH','keyboard health measurement'),
      ('PX68K_TAB5KBD_R57E101K_RAW','keyboard raw trace'),
      ('PX68K_KEYPIPE_R57E100K_CPU1','CPU1 keypipe trace'),
      ('PX68K_KEYPIPE_R57E105P_ENQ','CPU0 keypipe trace'),
      ('PX68K_JITTER_R57E72 START','JIT72 flight recorder'),
      ('JIT72_FRAME','JIT72 summary'),
      ('JIT91_SUBMIT_EVT','R91 continuity detail'),
      ('PX68K_M5SPK_R57E91','M5 Speaker audit patch'),
      ('counter-only screen liveness/cadence attribution ACTIVE','R89 active measurement marker'),
    ]:
        forbid(strings,n,l)
    req(nm,'i2c_new_master_bus','new I2C symbol')
    forbid(nm,'i2c_driver_install','legacy I2C')
    print('ELF VERIFY PASS')
else:
    raise SystemExit(f'unknown mode: {mode}')
