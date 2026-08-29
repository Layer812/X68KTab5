from pathlib import Path
import hashlib, sys
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

def func_block(src, name):
    pos=src.find(name)
    if pos < 0: raise SystemExit(f'VERIFY ERROR: function not found: {name}')
    start=src.rfind('static ',0,pos)
    if start < 0: raise SystemExit(f'VERIFY ERROR: static start not found: {name}')
    b=src.find('{',pos)
    if b < 0: raise SystemExit(f'VERIFY ERROR: open brace not found: {name}')
    depth=0
    for i in range(b,len(src)):
        c=src[i]
        if c=='{': depth+=1
        elif c=='}':
            depth-=1
            if depth==0: return src[start:i+1]
    raise SystemExit(f'VERIFY ERROR: close brace not found: {name}')

def sha(s): return hashlib.sha256(s.encode('utf-8')).hexdigest()

# These are exact hashes of R57E102P's proven A164 acquisition/task functions.
R102_TRANSPORT_HASHES={
 'tab5kbd_r101_direct_poll_cpu0':'0ac43165e7bf26f1299ceb5e37b93de660dd047738376f10d84e15066ea03f12',
 'tab5kbd_cpu0_task':'d03f56cfe4c5dfaa2bb8a43679e52aca7493e4b1564e171e1d9a9ecfbabb1a07',
 'tab5kbd_start_cpu0_task':'ea34e41f3ef0dc4a4194c35452486bc3429b4a8b1a744032276191ecf0552c86',
}

mode=sys.argv[1] if len(sys.argv)>1 else 'source'
if mode=='source':
    print('[verify] reading R105P sources...',flush=True)
    main=read('src/main.c')
    for name,want in R102_TRANSPORT_HASHES.items():
        got=sha(func_block(main,name))
        if got!=want:
            raise SystemExit(f'VERIFY ERROR: R102P transport function changed: {name}\n got={got}\nwant={want}')
        print(f'[verify] R102P transport EXACT: {name}',flush=True)

    checks=[
      ('PX68K_R57E105P','R105P marker'),
      ('PX68K_TAB5KBD_R57E105P_MAPCERT PASS','map certificate'),
      ('4096, NULL, 3,','A164 priority 3 retained'),
      ('tab5kbd_r101_direct_poll_cpu0();','direct 0x20 acquisition retained'),
      ('vTaskDelay(pdMS_TO_TICKS(10));','10ms blocking cadence retained'),
      ('TAB5KBD_REG_KEY_EVENT, &raw, 1u','KEY_EVENT direct read retained'),
      ('{0x1B,0x02},{0x02,0x02},{0x1B,0x00}','base ` / ! / @ semantic X68K map'),
      ('{0x0D,0x02},{0x33,0x02},{0x1B,0x00}','Sym ~ / ? map'),
      ('{0x17,0x00},{0x18,0x00},{0x19,0x00},{0x1A,0x00},{0x28,0x00},{0x03,0x02},{0x0F,0x00}','Sym : / quote map'),
      ('{0x23,0x00},{0x24,0x00},{0x25,0x00},{0x26,0x00},{0x3C,0x00},{0x34,0x00},{0x1D,0x00}','base underscore map'),
      ('{0x23,0x00},{0x24,0x00},{0x25,0x00},{0x26,0x00},{0x3C,0x00},{0x0C,0x02},{0x1D,0x00}','Sym equals map'),
      ('{0x2F,0x00},{0x30,0x00},{0x31,0x00},{0x3B,0x00},{0x3E,0x00},{0x3D,0x00},{0x35,0x00}','Sym comma map'),
      ('33333LL','Turbo30 cadence retained'),
    ]
    for i,(n,l) in enumerate(checks,1):
        req(main,n,l); print(f'[verify] source {i:02d}/{len(checks):02d} OK: {l}',flush=True)
    forbid(main,'tab5kbd_hid_to_retrok','old A164 HID->RETROK punctuation path')
    forbid(main,'s_tab5kbd_sym_latched','R104 one-shot Sym extension')
    forbid(main,'vTaskDelay(pdMS_TO_TICKS(5))','zero-tick 5ms A164 delay')
    forbid(main,'vTaskDelay(pdMS_TO_TICKS(2))','zero-tick 2ms settle')
    if (ROOT/'src/tab5_audio.cpp').is_file():
        audio=read('src/tab5_audio.cpp')
        for n in ['static constexpr size_t kRingFrames = 32768;',
                  'static constexpr size_t kChunkFrames = 512;',
                  'static constexpr size_t kStartupWatermarkFrames = 3072;',
                  'static constexpr size_t kResumeWatermarkFrames = 2048;',
                  'static constexpr size_t kLowWatermarkFrames = 512;',
                  'static constexpr size_t kPlayBuffers = 3;']:
            req(audio,n,'R94 audio contract')
        print('[verify] R94 audio geometry unchanged',flush=True)
    print('SOURCE VERIFY PASS',flush=True)
elif mode=='elf':
    if len(sys.argv)<4:
        raise SystemExit('usage: verify_r57e105p.py elf <strings> <nm>')
    strings=Path(sys.argv[2]).read_text(errors='replace')
    nm=Path(sys.argv[3]).read_text(errors='replace')
    for n in ['PX68K_R57E105P','PX68K_TAB5KBD_R57E105P_MAPCERT PASS',
              'PX68K_TAB5KBD_R57E102P_HEALTH','PX68K_TAB5KBD_R57E101K_RAW',
              'PX68K_KEYPIPE_R57E100K_CPU1','target=30fps','PX68K_AUDIO_R57E96T']:
        req(strings,n,'ELF string')
    req(nm,'i2c_new_master_bus','new I2C symbol')
    forbid(nm,'i2c_driver_install','legacy I2C')
    print('ELF VERIFY PASS',flush=True)
else:
    raise SystemExit(f'unknown mode: {mode}')
