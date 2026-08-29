from pathlib import Path
import sys
ROOT=Path('.')
def read(p):
    q=ROOT/p
    if not q.is_file(): raise SystemExit(f'VERIFY ERROR: missing file: {p}')
    return q.read_text(encoding='utf-8',errors='replace')
def req(t,n,l):
    if n not in t: raise SystemExit(f'VERIFY ERROR: missing {l}: {n}')
def forbid(t,n,l):
    if n in t: raise SystemExit(f'VERIFY ERROR: forbidden {l}: {n}')
mode=sys.argv[1] if len(sys.argv)>1 else 'source'
if mode=='source':
    print('[verify] reading sources...',flush=True)
    main=read('src/main.c')
    checks=[
      ('PX68K_R57E102P','R102P marker'),
      ('PX68K_TAB5KBD_R57E102P_HEALTH','priority health marker'),
      ('4096, NULL, 3,','A164 priority 3'),
      ('svcTicks=%lu gapMax=%luus','service liveness telemetry'),
      ('tab5kbd_r101_direct_poll_cpu0();','retained Normal 10ms direct read'),
      ('TAB5KBD_REG_KEY_EVENT, &raw, 1u','retained KEY_EVENT read'),
      ('PX68K_KEYPIPE_R57E100K_ENQ','retained enqueue trace'),
      ('PX68K_KEYPIPE_R57E100K_CPU1','retained CPU1 trace'),
      ('33333LL','Turbo30 cadence'),
    ]
    for i,(n,l) in enumerate(checks,1): req(main,n,l); print(f'[verify] source {i:02d}/{len(checks):02d} OK: {l}',flush=True)
    forbid(main,'vTaskDelay(pdMS_TO_TICKS(5))','zero-tick 5ms A164 delay')
    forbid(main,'vTaskDelay(pdMS_TO_TICKS(2))','zero-tick 2ms settle')
    if (ROOT/'src/tab5_audio.cpp').is_file():
      audio=read('src/tab5_audio.cpp')
      for n in ['static constexpr size_t kRingFrames = 32768;','static constexpr size_t kChunkFrames = 512;','static constexpr size_t kStartupWatermarkFrames = 3072;','static constexpr size_t kResumeWatermarkFrames = 2048;','static constexpr size_t kLowWatermarkFrames = 512;','static constexpr size_t kPlayBuffers = 3;']: req(audio,n,'R94 audio contract')
      print('[verify] R94 audio buffer contract OK',flush=True)
    print('SOURCE VERIFY PASS',flush=True)
elif mode=='elf':
    if len(sys.argv)<4: raise SystemExit('usage: verify_r57e102p.py elf <strings> <nm>')
    strings=Path(sys.argv[2]).read_text(errors='replace'); nm=Path(sys.argv[3]).read_text(errors='replace')
    for n in ['PX68K_R57E102P','PX68K_TAB5KBD_R57E102P_HEALTH','PX68K_TAB5KBD_R57E101K_RAW','PX68K_KEYPIPE_R57E100K_ENQ','PX68K_KEYPIPE_R57E100K_CPU1','target=30fps','PX68K_AUDIO_R57E96T']:
      req(strings,n,'ELF string')
    req(nm,'i2c_new_master_bus','new I2C symbol'); forbid(nm,'i2c_driver_install','legacy I2C')
    print('ELF VERIFY PASS',flush=True)
else: raise SystemExit(f'unknown mode: {mode}')
