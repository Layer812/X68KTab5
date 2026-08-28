#!/usr/bin/env python3
from pathlib import Path
import re, sys

MAIN = Path('src/main.c')
AUDIO = Path('src/tab5_audio.cpp')
MARK = 'PX68K_R56N: dual-core slow-window attribution; diagnostic-only; R56l audio semantics retained'
R56M_MARK = 'PX68K_AUDIO_R56M: egress-chain fact probe; counters only; no audio policy change'
R56M_API = 'tab5_audio_get_egress_probe'


def fail(msg, rc=2):
    print('R56n1 dual-core window patch ERROR:', msg, file=sys.stderr)
    sys.exit(rc)


def match_brace(s, o):
    d=0; i=o; state='c'; q=''
    while i < len(s):
        c=s[i]; n=s[i+1] if i+1<len(s) else ''
        if state=='c':
            if c=='/' and n=='/': state='l'; i+=2; continue
            if c=='/' and n=='*': state='b'; i+=2; continue
            if c in "\"'": state='s'; q=c; i+=1; continue
            if c=='{': d+=1
            elif c=='}':
                d-=1
                if d==0: return i
            i+=1
        elif state=='l':
            if c=='\n': state='c'
            i+=1
        elif state=='b':
            if c=='*' and n=='/': state='c'; i+=2
            else: i+=1
        else:
            if c=='\\': i+=2
            elif c==q: state='c'; i+=1
            else: i+=1
    raise RuntimeError('unbalanced braces')


def func_range(s, name):
    m=re.search(r'(?m)^[^\n;{}]*\b'+re.escape(name)+r'\s*\([^;{}]*\)\s*\{', s)
    if not m: return None
    o=s.find('{',m.start(),m.end())
    return m.start(),o,match_brace(s,o)


def remove_r56m_audio(s):
    # Remove exact hook statements regardless of indentation.
    pats = [
        r'r56m_note_pull\s*\(\s*s_source_pull\s*,\s*got\s*\)\s*;',
        r'r56m_note_mix\s*\(\s*samples\s*,\s*frames\s*\)\s*;',
        r'r56m_note_play\s*\(\s*s_play\[play_index\]\s*,\s*play_frames\s*\)\s*;',
        r'r56m_reset\s*\(\s*\)\s*;',
    ]
    for pat in pats:
        s=re.sub(r'(?m)^\s*'+pat+r'\s*\n?', '', s, count=1)

    # Remove generated state/helpers/API block from its marker comment through
    # the closing brace of tab5_audio_get_egress_probe().
    if R56M_MARK in s or R56M_API in s:
        pos=s.find('/* R56m: fact-only audio egress chain.')
        fr=func_range(s,R56M_API)
        if pos < 0 or not fr:
            fail('partial/unrecognized R56m audio probe; refusing removal',10)
        end=fr[2]+1
        while end < len(s) and s[end] in '\r\n':
            end += 1
        s=s[:pos]+s[end:]

    leftovers=[x for x in ['r56m_note_pull','r56m_note_mix','r56m_note_play','r56m_reset',R56M_API,R56M_MARK] if x in s]
    if leftovers:
        fail('R56m audio leftovers: '+','.join(leftovers),11)
    return s


def remove_r56m_main(s):
    s=re.sub(
        r'(?ms)^extern void tab5_audio_get_egress_probe\(\n'
        r'.*?^\s*uint32_t \*play_calls, uint32_t \*play_frames, uint32_t \*play_nz, uint32_t \*play_peak\);\n',
        '', s, count=1)
    s=re.sub(r'(?m)^\s*uint32_t eg_pull_calls=.*?;\n','',s,count=1)
    s=re.sub(r'(?m)^\s*uint32_t eg_mix_calls=.*?;\n','',s,count=1)
    s=re.sub(r'(?m)^\s*uint32_t eg_play_calls=.*?;\n','',s,count=1)
    s=re.sub(
        r'(?ms)^\s*tab5_audio_get_egress_probe\(\n'
        r'.*?^\s*&eg_play_calls, &eg_play_frames, &eg_play_nz, &eg_play_peak\);\n',
        '',s,count=1)

    old_suffix=' r56l{bp=%lu/%lu/%lu/max%lu discard=%lu/%lu} eg{pull=%lu/%lu/%lu/nz%lu/p%lu mix=%lu/%lu/nz%lu/p%lu play=%lu/%lu/nz%lu/p%lu}'
    new_suffix=' r56l{bp=%lu/%lu/%lu/max%lu discard=%lu/%lu}'
    if old_suffix in s:
        s=s.replace(old_suffix,new_suffix,1)
    s=re.sub(
        r'(?ms)(\(unsigned long\)fm_discard_events, \(unsigned long\)fm_discard_frames),\n'
        r'\s*\(unsigned long\)eg_pull_calls,.*?\(unsigned long\)eg_play_nz, \(unsigned long\)eg_play_peak\);',
        r'\1);',s,count=1)

    if R56M_API in s or 'eg{pull=' in s or 'eg_pull_calls' in s:
        fail('R56m main telemetry removal incomplete',12)
    return s


def add_r56n_main(s):
    if MARK in s:
        return s

    if 'WinX68k_AudioAsyncGetBackpressureStats' not in s:
        fail('R56l backpressure API missing; wrong base',20)
    if 'CPU613R43 QUIET' not in s:
        fail('R43 quiet production window anchor missing',21)

    if '#include "esp_cpu.h"' not in s:
        anchor='#include "esp_timer.h"\n'
        if anchor not in s: fail('esp_timer include anchor missing',22)
        s=s.replace(anchor,anchor+'#include "esp_cpu.h"\n',1)

    tag='static const char *TAG = "PX68K_TAB5";\n'
    if tag not in s: fail('TAG anchor missing',23)
    marker=f'''\n/* R56n: low-overhead slow-window attribution.  No scheduler, render, audio,\n * queue, priority, stack, or affinity policy changes.  CPU1 contributes only\n * two core-local cycle-counter reads per guest frame; CPU0 uses already-existing\n * cumulative work counters sampled once per 600-frame production window. */\nstatic const char s_r56n_marker[] __attribute__((used)) =\n    "{MARK}";\n#if defined(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ)\n#define R56N_CPU_MHZ ((uint32_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ)\n#else\n#define R56N_CPU_MHZ 360u\n#endif\n'''
    s=s.replace(tag,tag+marker,1)

    vars_anchor='    uint32_t cpu0bill_prev_lcd_us = 0, cpu0bill_prev_mix_us = 0, cpu0bill_prev_spk_us = 0;\n'
    if vars_anchor not in s: fail('CPU0 bill variable anchor missing',24)
    vars_block='''\n    /* R56n 600-frame A/B window state.  Baselines are armed at f=300 so the\n     * first comparable result is f=900, exactly matching CPU613R43 cadence. */\n    uint64_t r56n_guest_cycles = 0u;\n    bool r56n_armed = false;\n    uint32_t r56n_prev_fm_us = 0u, r56n_prev_comp_us = 0u, r56n_prev_lcd_us = 0u;\n    uint32_t r56n_prev_mix_us = 0u, r56n_prev_spk_us = 0u;\n    uint32_t r56n_prev_gv_wait_us = 0u, r56n_prev_bg_wait_us = 0u;\n    uint64_t r56n_prev_screen_bp = 0u;\n'''
    s=s.replace(vars_anchor,vars_anchor+vars_block,1)

    exec_anchor='        int cycles = WinX68k_ExecVideoProbeFrame();\n'
    if exec_anchor not in s: fail('guest exec anchor missing',25)
    exec_new='''        const uint32_t r56n_cc0 = (uint32_t)esp_cpu_get_cycle_count();\n        int cycles = WinX68k_ExecVideoProbeFrame();\n        const uint32_t r56n_cc1 = (uint32_t)esp_cpu_get_cycle_count();\n        r56n_guest_cycles += (uint32_t)(r56n_cc1 - r56n_cc0);\n'''
    s=s.replace(exec_anchor,exec_new,1)

    insert_anchor='''#endif\n            /* c13: keep the targeted f=2100 dump so the SFXVI $368760 block\n'''
    if insert_anchor not in s: fail('production-log insertion anchor missing',26)
    r56n_block='''#endif\n\n            /* R56n: same 600-frame wall window as CPU613R43.  CPU0 percentages\n             * are a task-wall upper bound because a worker can be preempted by\n             * another CPU0 worker while its cumulative timer is open. */\n            {\n                tab5_compose_stats_t rn_cs = {0};\n                tab5_audio_stats_t rn_as = {0};\n                tab5_video_async_stats_t rn_vs = {0};\n                tab5_screen_manager_stats_t rn_ss = {0};\n                uint32_t rn_fm_us=0u, rn_fm_calls=0u, rn_fm_frames=0u;\n                uint32_t rn_fmq=0u, rn_fmdrop=0u, rn_fmover=0u, rn_fmavail=0u;\n                tab5_compose_get_stats(&rn_cs);\n                tab5_audio_get_stats(&rn_as);\n                tab5_video_get_async_stats(&rn_vs);\n                tab5_screen_manager_get_stats(&rn_ss);\n                OPM_AsyncWorkGet(&rn_fm_us, &rn_fm_calls, &rn_fm_frames);\n                WinX68k_AudioAsyncGetStats(&rn_fmq, &rn_fmdrop, &rn_fmover, &rn_fmavail);\n\n                if (r56n_armed && dynprod_prev_end_us != 0 && dynprod_now_us > dynprod_prev_end_us)\n                {\n                    const uint32_t rn_wall = (uint32_t)(dynprod_now_us - dynprod_prev_end_us);\n                    const uint64_t rn_cpu1_us64 = r56n_guest_cycles / (uint64_t)R56N_CPU_MHZ;\n                    const uint32_t rn_cpu1_us = rn_cpu1_us64 > UINT32_MAX ? UINT32_MAX : (uint32_t)rn_cpu1_us64;\n                    const uint32_t rn_dfm = rn_fm_us - r56n_prev_fm_us;\n                    const uint32_t rn_dcomp = rn_cs.gbt65k_cpu0_work_us - r56n_prev_comp_us;\n                    const uint32_t rn_dlcd = rn_vs.cpu0_push_total_us - r56n_prev_lcd_us;\n                    const uint32_t rn_dmix = rn_as.cpu0_mix_work_us - r56n_prev_mix_us;\n                    const uint32_t rn_dspk = rn_as.cpu0_speaker_work_us - r56n_prev_spk_us;\n                    const uint64_t rn_c0sum64 = (uint64_t)rn_dfm + rn_dcomp + rn_dlcd + rn_dmix + rn_dspk;\n                    const uint32_t rn_c1pct = rn_wall ? (uint32_t)((uint64_t)rn_cpu1_us * 100u / rn_wall) : 0u;\n                    const uint32_t rn_c0pct = rn_wall ? (uint32_t)(rn_c0sum64 * 100u / rn_wall) : 0u;\n                    const uint32_t rn_gvw = rn_cs.gvram_barrier_us - r56n_prev_gv_wait_us;\n                    const uint32_t rn_bgw = rn_cs.bg_barrier_us - r56n_prev_bg_wait_us;\n                    const uint64_t rn_sbp = rn_ss.present_backpressure - r56n_prev_screen_bp;\n                    ESP_LOGI(TAG,\n                             "R56N_WIN f=%lu wall=%luus C1exec=%luus/%lu%% C0sum=%lluus/%lu%% "\n                             "c0{fm=%lu comp=%lu lcd=%lu mix=%lu spk=%lu} sync{gv=%luus bg=%luus} "\n                             "back{screen+%llu pend=%lu defer=%lu fmq=%lu avail=%lu over=%lu hostq=%lu spq=%lu} budget=%s",\n                             (unsigned long)frame, (unsigned long)rn_wall,\n                             (unsigned long)rn_cpu1_us, (unsigned long)rn_c1pct,\n                             (unsigned long long)rn_c0sum64, (unsigned long)rn_c0pct,\n                             (unsigned long)rn_dfm, (unsigned long)rn_dcomp,\n                             (unsigned long)rn_dlcd, (unsigned long)rn_dmix,\n                             (unsigned long)rn_dspk, (unsigned long)rn_gvw,\n                             (unsigned long)rn_bgw, (unsigned long long)rn_sbp,\n                             (unsigned long)rn_ss.pending_tickets,\n                             (unsigned long)rn_ss.deferred_lines,\n                             (unsigned long)rn_fmq, (unsigned long)rn_fmavail,\n                             (unsigned long)rn_fmover,\n                             (unsigned long)rn_as.queued_frames,\n                             (unsigned long)rn_as.speaker_queued_frames,\n                             tab5_budget_mode_name(budget.mode));\n                }\n            }\n\n            /* c13: keep the targeted f=2100 dump so the SFXVI $368760 block\n'''
    s=s.replace(insert_anchor,r56n_block,1)

    end_anchor='''            }\n            dynprod_prev_end_us = esp_timer_get_time();\n            dynprod_prev_frame = frame;\n'''
    if end_anchor not in s: fail('dynprod reset anchor missing',27)
    end_block='''            }\n\n            /* Arm the next R56n window AFTER all UART diagnostics above.  CPU0\n             * host workers continue running while CPU1 prints, so refreshing\n             * the cumulative baselines here prevents diagnostic time from being\n             * charged to the next production window. */\n            {\n                tab5_compose_stats_t rn_bcs = {0};\n                tab5_audio_stats_t rn_bas = {0};\n                tab5_video_async_stats_t rn_bvs = {0};\n                tab5_screen_manager_stats_t rn_bss = {0};\n                uint32_t rn_bfm_us=0u, rn_bfm_calls=0u, rn_bfm_frames=0u;\n                tab5_compose_get_stats(&rn_bcs);\n                tab5_audio_get_stats(&rn_bas);\n                tab5_video_get_async_stats(&rn_bvs);\n                tab5_screen_manager_get_stats(&rn_bss);\n                OPM_AsyncWorkGet(&rn_bfm_us, &rn_bfm_calls, &rn_bfm_frames);\n                r56n_armed = true;\n                r56n_guest_cycles = 0u;\n                r56n_prev_fm_us = rn_bfm_us;\n                r56n_prev_comp_us = rn_bcs.gbt65k_cpu0_work_us;\n                r56n_prev_lcd_us = rn_bvs.cpu0_push_total_us;\n                r56n_prev_mix_us = rn_bas.cpu0_mix_work_us;\n                r56n_prev_spk_us = rn_bas.cpu0_speaker_work_us;\n                r56n_prev_gv_wait_us = rn_bcs.gvram_barrier_us;\n                r56n_prev_bg_wait_us = rn_bcs.bg_barrier_us;\n                r56n_prev_screen_bp = rn_bss.present_backpressure;\n            }\n            dynprod_prev_end_us = esp_timer_get_time();\n            dynprod_prev_frame = frame;\n'''
    s=s.replace(end_anchor,end_block,1)
    return s


def verify(main_s, audio_s):
    req=[MARK,'R56N_WIN f=%lu','esp_cpu_get_cycle_count()','r56n_guest_cycles',
         'WinX68k_AudioAsyncGetBackpressureStats','Arm the next R56n window AFTER all UART diagnostics']
    for tok in req:
        if tok not in main_s: fail('main verify missing: '+tok,30)
    for bad in ['eg{pull=',R56M_API,'eg_pull_calls']:
        if bad in main_s: fail('R56m main probe still present: '+bad,31)
    for bad in ['r56m_note_pull','r56m_note_mix','r56m_note_play','r56m_reset',R56M_API,R56M_MARK]:
        if bad in audio_s: fail('R56m audio probe still present: '+bad,32)
    if main_s.count('const uint32_t r56n_cc0') != 1 or main_s.count('R56N_WIN f=%lu') != 1:
        fail('R56n instrumentation duplicate/missing',33)
    # R56n1 lineage fix: R56l lives in fmg_wrap.cpp + main telemetry, not in
    # tab5_audio.cpp.  The previous verifier incorrectly required an audio-source
    # marker and therefore rejected a valid real R56l tree after already applying
    # the R56n edits.  R56l's own patcher runs immediately before this script and
    # add_r56n_main() already requires the authoritative backpressure API in main.
    # Do not invent a cross-file ownership marker in tab5_audio.cpp.
    if 'r56l{bp=' not in main_s:
        fail('R56l host telemetry missing from main; wrong base',34)
    print('R56n1 verified: R56m PCM scan removed; R56l main/API lineage retained; dual-core window probe only')


def rw(path, transform):
    raw=path.read_bytes(); nl='\r\n' if b'\r\n' in raw else '\n'; s=raw.decode('utf-8').replace('\r\n','\n')
    s=transform(s)
    path.write_bytes(s.replace('\n',nl).encode('utf-8'))
    return s


def main():
    if not MAIN.is_file(): fail(f'{MAIN} missing; run from project root')
    if not AUDIO.is_file(): fail(f'{AUDIO} missing; R56m target source is required')

    audio_s=rw(AUDIO, remove_r56m_audio)
    main_raw=MAIN.read_bytes(); main_nl='\r\n' if b'\r\n' in main_raw else '\n'; main_s=main_raw.decode('utf-8').replace('\r\n','\n')
    main_s=remove_r56m_main(main_s)
    main_s=add_r56n_main(main_s)
    MAIN.write_bytes(main_s.replace('\n',main_nl).encode('utf-8'))
    verify(main_s,audio_s)
    print('R56n1 dual-core slow-window attribution applied:',MAIN,AUDIO)

if __name__=='__main__':
    main()
