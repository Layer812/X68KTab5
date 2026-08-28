#!/usr/bin/env python3
from pathlib import Path
import re, sys

P = Path('components/px68k/fmgen/fmg_wrap.cpp')
BASE = 'Build 6.15h17R23 YM2151-only core=0'
MARK = 'PX68K_FMQ_R56K7: event-drop taxonomy only; queue policy unchanged nonblocking send0'
API = 'WinX68k_AudioAsyncGetQueueTaxonomy'

EVENTS = [
    ('ASYNC_OPM_WRITE', 0),
    ('ASYNC_OPM_RENDER', 1),
    ('ASYNC_OPM_RESET', 2),
    ('ASYNC_OPM_VOLUME', 3),
    ('ASYNC_OPM_CSM', 4),
    ('ASYNC_OPM_STOP', 5),
]
OTHER = 6
N = 7


def fail(msg, rc=2):
    print('R56k7 FM queue taxonomy patch ERROR:', msg, file=sys.stderr)
    sys.exit(rc)


def match_brace(s, o):
    d = 0; i = o; state = 'c'; q = ''
    while i < len(s):
        c = s[i]; n = s[i+1] if i+1 < len(s) else ''
        if state == 'c':
            if c == '/' and n == '/': state = 'l'; i += 2; continue
            if c == '/' and n == '*': state = 'b'; i += 2; continue
            if c in "\"'": state = 's'; q = c; i += 1; continue
            if c == '{': d += 1
            elif c == '}':
                d -= 1
                if d == 0: return i
            i += 1
        elif state == 'l':
            if c == '\n': state = 'c'
            i += 1
        elif state == 'b':
            if c == '*' and n == '/': state = 'c'; i += 2
            else: i += 1
        else:
            if c == '\\': i += 2
            elif c == q: state = 'c'; i += 1
            else: i += 1
    raise RuntimeError('unbalanced braces')


def func_range(s, name):
    m = re.search(r'(?m)^[^\n;{}]*\b' + re.escape(name) + r'\s*\([^;{}]*\)\s*\{', s)
    if not m:
        return None
    o = s.find('{', m.start(), m.end())
    e = match_brace(s, o)
    return m.start(), o, e


def taxonomy_block():
    cases = '\n'.join(f'        case {name}: return {idx}u;' for name, idx in EVENTS)
    return f'''\n/* R56k7: fact-only FM event queue taxonomy.  This does not alter queue\n * admission: async_opm_send() remains xQueueSend(..., 0).  We count both\n * attempts and drops by semantic event class so the next backpressure change\n * can preserve YM register chronology instead of guessing from aggregate drops. */\nstatic const char s_r56k7_fmq_marker[] __attribute__((used)) =\n    "{MARK}";\nstatic DRAM_ATTR volatile uint32_t s_r56k7_evt_attempt[{N}] = {{0}};\nstatic DRAM_ATTR volatile uint32_t s_r56k7_evt_drop[{N}] = {{0}};\nstatic DRAM_ATTR volatile uint32_t s_r56k7_write_drop_frames = 0;\nstatic DRAM_ATTR volatile uint32_t s_r56k7_render_drop_frames = 0;\n\nstatic inline uint32_t r56k7_event_slot(uint8_t type)\n{{\n    switch (type)\n    {{\n{cases}\n        default: return {OTHER}u;\n    }}\n}}\n\nstatic inline void r56k7_event_attempt(uint8_t type)\n{{\n    const uint32_t slot = r56k7_event_slot(type);\n    __atomic_fetch_add(&s_r56k7_evt_attempt[slot], 1u, __ATOMIC_RELAXED);\n}}\n\nstatic inline void r56k7_event_drop(uint8_t type, uint32_t frames)\n{{\n    const uint32_t slot = r56k7_event_slot(type);\n    __atomic_fetch_add(&s_r56k7_evt_drop[slot], 1u, __ATOMIC_RELAXED);\n    if (type == ASYNC_OPM_WRITE && frames)\n        __atomic_fetch_add(&s_r56k7_write_drop_frames, frames, __ATOMIC_RELAXED);\n    else if (type == ASYNC_OPM_RENDER && frames)\n        __atomic_fetch_add(&s_r56k7_render_drop_frames, frames, __ATOMIC_RELAXED);\n}}\n\nstatic inline void r56k7_event_stats_reset(void)\n{{\n    for (unsigned i = 0; i < {N}u; ++i)\n    {{\n        __atomic_store_n(&s_r56k7_evt_attempt[i], 0u, __ATOMIC_RELAXED);\n        __atomic_store_n(&s_r56k7_evt_drop[i], 0u, __ATOMIC_RELAXED);\n    }}\n    __atomic_store_n(&s_r56k7_write_drop_frames, 0u, __ATOMIC_RELAXED);\n    __atomic_store_n(&s_r56k7_render_drop_frames, 0u, __ATOMIC_RELAXED);\n}}\n'''


def api_block():
    return f'''\nextern "C" void {API}(\n    uint32_t *write_attempt, uint32_t *write_drop, uint32_t *write_drop_frames,\n    uint32_t *render_attempt, uint32_t *render_drop, uint32_t *render_drop_frames,\n    uint32_t *reset_drop, uint32_t *volume_drop, uint32_t *csm_drop,\n    uint32_t *stop_drop, uint32_t *other_drop)\n{{\n    if (write_attempt) *write_attempt = __atomic_load_n(&s_r56k7_evt_attempt[0], __ATOMIC_RELAXED);\n    if (write_drop) *write_drop = __atomic_load_n(&s_r56k7_evt_drop[0], __ATOMIC_RELAXED);\n    if (write_drop_frames) *write_drop_frames = __atomic_load_n(&s_r56k7_write_drop_frames, __ATOMIC_RELAXED);\n    if (render_attempt) *render_attempt = __atomic_load_n(&s_r56k7_evt_attempt[1], __ATOMIC_RELAXED);\n    if (render_drop) *render_drop = __atomic_load_n(&s_r56k7_evt_drop[1], __ATOMIC_RELAXED);\n    if (render_drop_frames) *render_drop_frames = __atomic_load_n(&s_r56k7_render_drop_frames, __ATOMIC_RELAXED);\n    if (reset_drop) *reset_drop = __atomic_load_n(&s_r56k7_evt_drop[2], __ATOMIC_RELAXED);\n    if (volume_drop) *volume_drop = __atomic_load_n(&s_r56k7_evt_drop[3], __ATOMIC_RELAXED);\n    if (csm_drop) *csm_drop = __atomic_load_n(&s_r56k7_evt_drop[4], __ATOMIC_RELAXED);\n    if (stop_drop) *stop_drop = __atomic_load_n(&s_r56k7_evt_drop[5], __ATOMIC_RELAXED);\n    if (other_drop) *other_drop = __atomic_load_n(&s_r56k7_evt_drop[6], __ATOMIC_RELAXED);\n}}\n'''


def verify(s):
    if BASE not in s:
        fail('R23 Internal-only YM baseline marker missing; refuse unknown source', 3)
    if 'taskYIELD(); /* R56k complete AsyncOPMEvent boundary */' not in s:
        fail('R56k complete-event scheduler boundary missing', 4)
    if not re.search(r'#define\s+ASYNC_OPM_QUEUE_LEN\s+512u?\b', s):
        fail('expected 512-entry FM event queue missing', 5)
    for name, _ in EVENTS:
        if name not in s:
            fail(f'expected event class missing: {name}', 6)
    fr = func_range(s, 'async_opm_send')
    if not fr:
        fail('async_opm_send not found', 7)
    body = s[fr[1]:fr[2]+1]
    if not re.search(r'xQueueSend\s*\(\s*s_audio_q\s*,\s*&ev\s*,\s*0\s*\)', body):
        fail('queue admission is no longer exact nonblocking xQueueSend(...,0)', 8)
    if 'r56k7_event_attempt(type);' not in body or 'r56k7_event_drop(type, frames);' not in body:
        fail('taxonomy hooks missing from async_opm_send', 9)
    if API not in s or MARK not in s:
        fail('taxonomy API/marker missing', 10)
    fi = func_range(s, 'async_opm_init')
    if not fi or 'r56k7_event_stats_reset();' not in s[fi[1]:fi[2]+1]:
        fail('taxonomy reset missing from async_opm_init', 11)
    # Strictly forbid accidental policy changes in this fact-only release.
    if re.search(r'xQueueSend\s*\(\s*s_audio_q\s*,\s*&ev\s*,\s*(?:portMAX_DELAY|pdMS_TO_TICKS|[1-9])', body):
        fail('blocking/backpressure change detected in fact-only R56k7', 12)
    print('R56k7 FM queue taxonomy verified: send policy still xQueueSend(...,0); event-class counters active')


def main():
    if not P.is_file():
        fail(f'{P} not found; run from G:\\px68k-tab5')
    raw = P.read_bytes()
    nl = '\r\n' if b'\r\n' in raw else '\n'
    s = raw.decode('utf-8').replace('\r\n', '\n')

    if MARK in s or API in s:
        if MARK in s and API in s:
            verify(s)
            print('R56k7 FM queue taxonomy already applied/verified:', P)
            return
        fail('partial R56k7 taxonomy patch detected', 13)

    if BASE not in s:
        fail('R23 Internal-only YM baseline marker missing; refuse unknown source', 3)
    if 'taskYIELD(); /* R56k complete AsyncOPMEvent boundary */' not in s:
        fail('apply r56k_patch_fm_host_scheduler.py before R56k7', 4)

    # Anchor fact-only state beside the existing aggregate drop counter.
    m = re.search(r'static[^;{}]*\bs_audio_event_drops\s*=\s*0\s*;', s)
    if not m:
        fail('s_audio_event_drops state anchor missing', 14)
    s = s[:m.end()] + taxonomy_block() + s[m.end():]

    # Instrument the single admission point. Preserve the exact existing send call.
    fr = func_range(s, 'async_opm_send')
    if not fr:
        fail('async_opm_send not found', 7)
    b0, o, e = fr
    body = s[o:e+1]
    ev_anchor = '    ev.profile = profile;'
    if ev_anchor not in body:
        # tolerate compact/alternate whitespace but require unique assignment
        mm = re.search(r'(?m)^(\s*)ev\.profile\s*=\s*profile\s*;', body)
        if not mm:
            fail('ev.profile assignment anchor missing in async_opm_send', 15)
        insert_at = o + mm.end()
        s = s[:insert_at] + '\n' + mm.group(1) + 'r56k7_event_attempt(type);' + s[insert_at:]
    else:
        abs_anchor = o + body.index(ev_anchor) + len(ev_anchor)
        s = s[:abs_anchor] + '\n    r56k7_event_attempt(type);' + s[abs_anchor:]

    # Re-find after insertion and add drop taxonomy immediately after aggregate drop.
    fr = func_range(s, 'async_opm_send'); o, e = fr[1], fr[2]
    body = s[o:e+1]
    dm = re.search(r'(?m)^(\s*)\+\+s_audio_event_drops\s*;', body)
    if not dm:
        dm = re.search(r'(?m)^(\s*)s_audio_event_drops\+\+\s*;', body)
    if not dm:
        fail('aggregate event-drop increment anchor missing', 16)
    ins = o + dm.end()
    indent = dm.group(1)
    s = s[:ins] + '\n' + indent + 'r56k7_event_drop(type, frames);' + s[ins:]

    # Reset taxonomy with existing aggregate counters at async init.
    fi = func_range(s, 'async_opm_init')
    if not fi:
        fail('async_opm_init not found', 17)
    o, e = fi[1], fi[2]
    ibody = s[o:e+1]
    rm = re.search(r'(?m)^(\s*)s_audio_event_drops\s*=\s*0\s*;', ibody)
    if not rm:
        fail('s_audio_event_drops reset missing inside async_opm_init', 18)
    ins = o + rm.end()
    s = s[:ins] + '\n' + rm.group(1) + 'r56k7_event_stats_reset();' + s[ins:]

    # R56k7a anchor fix: WinX68k_AudioAsyncGetStats is a higher-layer wrapper,
    # not a function defined in fmg_wrap.cpp on the R23/R56 lineage.  Anchor the
    # new C-linkage taxonomy export next to fmg_wrap's own stable public stats
    # functions instead.  Prefer OPM_AsyncPerfGet and accept OPM_AsyncWorkGet as
    # a lineage-compatible fallback; refuse any source that has neither.
    ar = func_range(s, 'OPM_AsyncPerfGet')
    anchor_name = 'OPM_AsyncPerfGet'
    if not ar:
        ar = func_range(s, 'OPM_AsyncWorkGet')
        anchor_name = 'OPM_AsyncWorkGet'
    if not ar:
        fail('known fmg_wrap stats anchor missing (OPM_AsyncPerfGet/OPM_AsyncWorkGet)', 19)
    s = s[:ar[0]] + api_block() + '\n' + s[ar[0]:]
    print('R56k7a taxonomy export anchor:', anchor_name)

    verify(s)
    P.write_bytes(s.replace('\n', nl).encode('utf-8'))
    print('R56k7 FM queue taxonomy patch applied:', P)


if __name__ == '__main__':
    main()
