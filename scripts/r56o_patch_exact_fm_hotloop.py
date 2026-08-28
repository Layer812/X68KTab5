#!/usr/bin/env python3
from pathlib import Path
import re, sys, shutil

MAIN = Path('src/main.c')
YM = Path('components/px68k/fmgen/vgmm5_ym2151.cpp')
FMG = Path('components/px68k/fmgen/fmg_wrap.cpp')
MARK = 'PX68K_FM_R56O: exact register-time PM/AM + feedback shift cache'
R56N_MARK = 'PX68K_R56N: dual-core slow-window attribution; diagnostic-only; R56l audio semantics retained'


def fail(msg, rc=2):
    print('R56o exact FM hotloop patch ERROR:', msg, file=sys.stderr)
    sys.exit(rc)


def remove_between_comments(s, begin, end):
    a=s.find(begin)
    if a < 0: return s, False
    b=s.find(end,a)
    if b < 0: fail('R56n removal end anchor missing: '+end[:48], 10)
    return s[:a]+s[b:], True


def remove_r56n_main(s):
    if R56N_MARK not in s and 'R56N_WIN f=' not in s and 'r56n_guest_cycles' not in s:
        return s

    # Global marker + CPU MHz definition.
    a=s.find('/* R56n: low-overhead slow-window attribution.')
    end='/* R56k3: substrate-only memory fact probe.'
    if a < 0 or end not in s[a:]: fail('partial R56n global marker block', 11)
    b=s.find(end,a)
    s=s[:a]+s[b:]

    # Per-task window state.
    s,ok=remove_between_comments(s,
        '    /* R56n 600-frame A/B window state.',
        '    /* Build 6.13c14r2: WDT/IDLE service remains deadline driven.')
    if not ok: fail('R56n local-state block missing',12)

    # Two CPU1 cycle reads around the authoritative guest-frame call.
    old='''        const uint32_t r56n_cc0 = (uint32_t)esp_cpu_get_cycle_count();\n        int cycles = WinX68k_ExecVideoProbeFrame();\n        const uint32_t r56n_cc1 = (uint32_t)esp_cpu_get_cycle_count();\n        r56n_guest_cycles += (uint32_t)(r56n_cc1 - r56n_cc0);\n'''
    if old not in s: fail('R56n CPU1 cycle wrapper missing',13)
    s=s.replace(old,'        int cycles = WinX68k_ExecVideoProbeFrame();\n',1)

    # 600-frame attribution print block.
    s,ok=remove_between_comments(s,
        '            /* R56n: same 600-frame wall window as CPU613R43.',
        '            /* c13: keep the targeted f=2100 dump so the SFXVI $368760 block')
    if not ok: fail('R56n window telemetry block missing',14)

    # End-of-window baseline snapshot; keep original dynprod timestamp reset.
    s,ok=remove_between_comments(s,
        '            /* Arm the next R56n window AFTER all UART diagnostics above.',
        '            dynprod_prev_end_us = esp_timer_get_time();')
    if not ok: fail('R56n baseline snapshot block missing',15)

    # esp_cpu.h was introduced only for R56n in this lineage.
    if 'esp_cpu_' not in s:
        s=s.replace('#include "esp_cpu.h"\n','',1)

    leftovers=['R56N_WIN','r56n_','R56N_CPU_MHZ',R56N_MARK,'esp_cpu_get_cycle_count']
    left=[x for x in leftovers if x in s]
    if left: fail('R56n leftovers: '+','.join(left),16)
    return s


def patch_ym(s):
    if MARK in s:
        return s
    # Refuse the abandoned pre-release R56o experiment if somehow present.
    if 'PX68K_FM_R56O: exact hotloop call-collapse' in s:
        fail('obsolete pre-release R56o experiment detected; restore vgmm5_ym2151.cpp from .r56o.bak first', 19)

    # One YM engine exists.  Move invariant register decoding out of the
    # 44.1-kHz x active-channel synth loop.  Existing pms/ams/fb_shift fields
    # are retained, so register-visible/debug state is unchanged.
    a='''    uint8_t pan_l[YM_CH], pan_r[YM_CH];\n    uint8_t algo[YM_CH], fb_shift[YM_CH];\n    int32_t fb_memory[YM_CH][2];\n    int32_t mem_value[YM_CH];\n    uint8_t kc[YM_CH], kf[YM_CH], pms[YM_CH], ams[YM_CH];\n'''
    b='''    uint8_t pan_l[YM_CH], pan_r[YM_CH];\n    uint8_t algo[YM_CH], fb_shift[YM_CH], fb_rshift[YM_CH];\n    int32_t fb_memory[YM_CH][2];\n    int32_t mem_value[YM_CH];\n    uint8_t kc[YM_CH], kf[YM_CH], pms[YM_CH], ams[YM_CH];\n    /* R56o: values derived only from OPM registers.  Cache them when the\n     * register is written instead of decoding/table-loading per channel for\n     * every 44.1-kHz sample.  Zero-init exactly represents register value 0. */\n    int32_t pm_scale[YM_CH], am_scale[YM_CH];\n'''
    if a not in s: fail('YM state layout anchor missing/unexpected',20)
    s=s.replace(a,b,1)

    oldfb='''    uint32_t f=e->fb_shift[ch]&7u; if(!f)return 0;\n    return (e->fb_memory[ch][0]+e->fb_memory[ch][1]) >> (9u-f);'''
    newfb='''    const uint32_t sh=e->fb_rshift[ch]; if(!sh)return 0;\n    return (e->fb_memory[ch][0]+e->fb_memory[ch][1]) >> sh;'''
    if oldfb not in s: fail('feedback hot-path anchor missing/unexpected',21)
    s=s.replace(oldfb,newfb,1)

    oldpm='''        const uint8_t pms=e->pms[ch]&7u;\n        const uint8_t ams=e->ams[ch]&3u;\n        const int32_t pm=(gpm&&pms)?((gpm*s_pms_mult[pms])>>4):0;\n        const int32_t am=(gam&&ams)?((gam*s_ams_mult[ams])>>2):0;'''
    newpm='''        /* R56o: exact cached register-derived scales.  The signed multiply\n         * and shifts are identical to the old non-zero PMS/AMS path; scale=0\n         * exactly covers the previous zero-depth/zero-sensitivity branches. */\n        const int32_t pm=(gpm * e->pm_scale[ch]) >> 4;\n        const int32_t am=(gam * e->am_scale[ch]) >> 2;'''
    if oldpm not in s: fail('PMS/AMS synth hot-path anchor missing/unexpected',22)
    s=s.replace(oldpm,newpm,1)

    old20='''    if(a>=0x20&&a<=0x27){int ch=a&7;e->pan_l[ch]=(d>>7)&1;e->pan_r[ch]=(d>>6)&1;e->fb_shift[ch]=(d>>3)&7;e->algo[ch]=d&7;return;}'''
    new20='''    if(a>=0x20&&a<=0x27){int ch=a&7;e->pan_l[ch]=(d>>7)&1;e->pan_r[ch]=(d>>6)&1;e->fb_shift[ch]=(d>>3)&7;e->fb_rshift[ch]=e->fb_shift[ch]?(uint8_t)(9u-e->fb_shift[ch]):0;e->algo[ch]=d&7;return;}'''
    if old20 not in s: fail('OPM 0x20 register anchor missing/unexpected',23)
    s=s.replace(old20,new20,1)

    old38='''    if(a>=0x38&&a<=0x3f){int ch=a&7;e->pms[ch]=(d>>4)&7;e->ams[ch]=d&3;return;}'''
    new38='''    if(a>=0x38&&a<=0x3f){int ch=a&7;e->pms[ch]=(d>>4)&7;e->ams[ch]=d&3;e->pm_scale[ch]=s_pms_mult[e->pms[ch]];e->am_scale[ch]=s_ams_mult[e->ams[ch]];return;}'''
    if old38 not in s: fail('OPM 0x38 register anchor missing/unexpected',24)
    s=s.replace(old38,new38,1)

    # Put the version marker immediately before the hot synth routine without
    # forcing inlining or changing algorithm/order.
    anchor='static IRAM_ATTR __attribute__((noinline,optimize("O3"))) void synth(VgmM5YM2151 *e,int32_t *ml,int32_t *mr)'
    if anchor not in s: fail('synth signature missing/unexpected',25)
    s=s.replace(anchor, '/* '+MARK+' */\n'+anchor,1)
    return s


def verify(main_s, ym_s, fmg_s):
    for tok in [MARK,'fb_rshift[YM_CH]','pm_scale[YM_CH], am_scale[YM_CH]',
                'const int32_t pm=(gpm * e->pm_scale[ch]) >> 4;',
                'const int32_t am=(gam * e->am_scale[ch]) >> 2;',
                'e->fb_rshift[ch]=e->fb_shift[ch]?',
                'e->pm_scale[ch]=s_pms_mult[e->pms[ch]]']:
        if tok not in ym_s: fail('YM verify missing: '+tok,30)
    if ym_s.count(MARK) != 1: fail('R56o marker duplicate/missing',31)
    # Explicitly ensure rejected experiments were NOT introduced.
    if 'if(e->active_ch_mask) synth(e,&l,&r);' in ym_s:
        fail('abandoned silent synth bypass unexpectedly present',31)
    if 'always_inline,optimize("O3"))) inline void synth(' in ym_s:
        fail('abandoned synth inlining unexpectedly present',31)
    if 'R56N_WIN' in main_s or 'r56n_' in main_s or 'esp_cpu_get_cycle_count' in main_s:
        fail('R56n production probe not fully removed',32)
    # Preserve authoritative R56l queue/chronology contract.
    for tok in ['PX68K_FMQ_R56L: FIFO lossless producer backpressure + ring-full synth-discard chronology',
                'WinX68k_AudioAsyncGetBackpressureStats','r56l_render_discard(frames, profile);']:
        if tok not in fmg_s: fail('R56l contract missing: '+tok,33)
    if 'r56l{bp=' not in main_s or 'fmev{W=' not in main_s:
        fail('R56l/R56k7 health telemetry missing from main',34)
    scr=Path('src/tab5_screen_manager.c').read_text(encoding='utf-8',errors='ignore')
    if 'PX68K_SCREEN_R56K6: worker stdio=FORBIDDEN' not in scr:
        fail('R56k6 Screen worker stdio fence missing',35)
    print('R56o verified: exact register-time YM cache + R56n probe removed; R56l/R56k6 contracts retained')


def rw(path, transform):
    raw=path.read_bytes(); nl='\r\n' if b'\r\n' in raw else '\n'
    s=raw.decode('utf-8').replace('\r\n','\n')
    out=transform(s)
    if out != s:
        bak=path.with_suffix(path.suffix+'.r56o.bak')
        if not bak.exists(): shutil.copy2(path,bak)
        path.write_bytes(out.replace('\n',nl).encode('utf-8'))
    return out


def main():
    for p in [MAIN,YM,FMG,Path('src/tab5_screen_manager.c')]:
        if not p.is_file(): fail(f'{p} missing; run from G:\\px68k-tab5 project root')
    main_s=rw(MAIN,remove_r56n_main)
    ym_s=rw(YM,patch_ym)
    fmg_s=FMG.read_text(encoding='utf-8',errors='strict').replace('\r\n','\n')
    verify(main_s,ym_s,fmg_s)
    print('R56o exact FM register-cache patch applied:',YM)
    print('R56o production attribution cleanup applied:',MAIN)

if __name__=='__main__': main()
