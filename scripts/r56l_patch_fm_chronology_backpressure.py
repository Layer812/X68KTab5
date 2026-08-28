#!/usr/bin/env python3
from pathlib import Path
import re, sys

P = Path('components/px68k/fmgen/fmg_wrap.cpp')
BASE = 'Build 6.15h17R23 YM2151-only core=0'
K7MARK = 'PX68K_FMQ_R56K7: event-drop taxonomy only; queue policy unchanged nonblocking send0'
K7API = 'WinX68k_AudioAsyncGetQueueTaxonomy'
MARK = 'PX68K_FMQ_R56L: FIFO lossless producer backpressure + ring-full synth-discard chronology'
API = 'WinX68k_AudioAsyncGetBackpressureStats'


def fail(msg, rc=2):
    print('R56l FM chronology/backpressure patch ERROR:', msg, file=sys.stderr)
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
    if not m: return None
    o = s.find('{', m.start(), m.end())
    return m.start(), o, match_brace(s, o)


def state_block():
    return f'''\n/* R56l: R56k7 proved that queue overflow discards authoritative WRITE events.
 * Preserve every AsyncOPMEvent in FIFO order.  A full queue applies producer
 * backpressure in one-tick blocking quanta, which sleeps the CPU1 guest task
 * instead of busy-spinning.  PCM-ring overflow is handled separately below:
 * synth state advances while only the already-late PCM payload is discarded. */
static const char s_r56l_fmq_marker[] __attribute__((used)) =
    "{MARK}";
static DRAM_ATTR volatile uint32_t s_r56l_bp_events = 0;
static DRAM_ATTR volatile uint32_t s_r56l_bp_wait_calls = 0;
static DRAM_ATTR volatile uint32_t s_r56l_bp_timeouts = 0;
static DRAM_ATTR volatile uint32_t s_r56l_bp_max_timeouts = 0;
static DRAM_ATTR volatile uint32_t s_r56l_discard_events = 0;
static DRAM_ATTR volatile uint32_t s_r56l_discard_frames = 0;

static inline void r56l_atomic_max_u32(volatile uint32_t *p, uint32_t v)
{{
    uint32_t old = __atomic_load_n(p, __ATOMIC_RELAXED);
    while (old < v && !__atomic_compare_exchange_n(
               p, &old, v, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {{}}
}}

static inline void r56l_stats_reset(void)
{{
    __atomic_store_n(&s_r56l_bp_events, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_r56l_bp_wait_calls, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_r56l_bp_timeouts, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_r56l_bp_max_timeouts, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_r56l_discard_events, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_r56l_discard_frames, 0u, __ATOMIC_RELAXED);
}}
'''


def discard_helper():
    return '''\n/* R56l Layer-2 pressure relief: advance the YM2151 engine even when the PCM
 * FIFO has no room.  This deliberately discards only finished PCM, never guest
 * register chronology or synth time.  The existing Internal scratch buffer is
 * reused; no new audio buffer is allocated. */
static void r56l_render_discard(uint32_t frames, uint8_t profile)
{
    if (!frames) return;
    uint32_t remain = frames;
    const int64_t t0 = esp_timer_get_time();
    while (remain)
    {
        uint32_t n = remain;
        if (n > ASYNC_OPM_SCRATCH_FRAMES) n = ASYNC_OPM_SCRATCH_FRAMES;
        if (s_audio_vgm)
        {
            vgmm5_ym2151_render(s_audio_vgm, s_vgm_scratch, n);
        }
        else if (s_audio_opm)
        {
            memset(s_vgm_scratch, 0, (size_t)n * 2u * sizeof(int16_t));
            s_audio_opm->Mix(s_vgm_scratch, (int)n,
                             (uint8_t *)s_vgm_scratch,
                             (uint8_t *)(s_vgm_scratch + ASYNC_OPM_SCRATCH_FRAMES * 2u));
        }
        else
        {
            break;
        }
        remain -= n;
    }
    const uint32_t rendered = frames - remain;
    const uint32_t work_us = (uint32_t)(esp_timer_get_time() - t0);
    if (rendered)
    {
        s_audio_work_us += work_us;
        ++s_audio_work_calls;
        s_audio_work_frames += rendered;
        if (profile)
        {
            s_audio_profile_us += work_us;
            ++s_audio_profile_calls;
            s_audio_profile_frames += rendered;
        }
        __atomic_fetch_add(&s_r56l_discard_events, 1u, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s_r56l_discard_frames, rendered, __ATOMIC_RELAXED);
    }
}
'''


def api_block():
    return f'''\nextern "C" void {API}(
    uint32_t *bp_events, uint32_t *wait_calls, uint32_t *timeouts,
    uint32_t *max_timeouts, uint32_t *discard_events, uint32_t *discard_frames)
{{
    if (bp_events) *bp_events = __atomic_load_n(&s_r56l_bp_events, __ATOMIC_RELAXED);
    if (wait_calls) *wait_calls = __atomic_load_n(&s_r56l_bp_wait_calls, __ATOMIC_RELAXED);
    if (timeouts) *timeouts = __atomic_load_n(&s_r56l_bp_timeouts, __ATOMIC_RELAXED);
    if (max_timeouts) *max_timeouts = __atomic_load_n(&s_r56l_bp_max_timeouts, __ATOMIC_RELAXED);
    if (discard_events) *discard_events = __atomic_load_n(&s_r56l_discard_events, __ATOMIC_RELAXED);
    if (discard_frames) *discard_frames = __atomic_load_n(&s_r56l_discard_frames, __ATOMIC_RELAXED);
}}
'''


def verify(s):
    for token, rc in [(BASE,3),(K7MARK,4),(K7API,5),(MARK,6),(API,7),
                      ('taskYIELD(); /* R56k complete AsyncOPMEvent boundary */',8)]:
        if token not in s: fail('required marker missing: ' + token, rc)
    if not re.search(r'#define\s+ASYNC_OPM_QUEUE_LEN\s+512u?\b', s):
        fail('expected 512-entry queue missing', 9)
    if 'ASYNC_OPM_SCRATCH_FRAMES' not in s or 's_vgm_scratch' not in s:
        fail('existing Internal YM scratch contract missing', 10)

    fr = func_range(s, 'async_opm_send')
    if not fr: fail('async_opm_send missing', 11)
    b = s[fr[1]:fr[2]+1]
    if not re.search(r'xQueueSend\s*\(\s*s_audio_q\s*,\s*&ev\s*,\s*0\s*\)', b):
        fail('initial zero-wait admission probe missing', 12)
    if not re.search(r'xQueueSend\s*\(\s*s_audio_q\s*,\s*&ev\s*,\s*1\s*\)', b):
        fail('scheduler-blocking one-tick retry missing', 13)
    if '++s_audio_event_drops' in b or 's_audio_event_drops++' in b or 'r56k7_event_drop(type, frames);' in b:
        fail('queue-drop path still present after R56l lossless conversion', 14)
    if 'vTaskDelay' in b or 'taskYIELD' in b:
        fail('producer backpressure must use queue blocking, not explicit delay/yield', 15)

    rr = func_range(s, 'async_render_publish')
    if not rr: fail('async_render_publish missing', 16)
    rb = s[rr[1]:rr[2]+1]
    if 'r56l_render_discard(frames, profile);' not in rb:
        fail('ring-full synth-discard hook missing', 17)
    if not re.search(r'free_frames\s*<\s*frames', rb):
        fail('ring-full guard missing', 18)

    ri = func_range(s, 'async_opm_init')
    if not ri or 'r56l_stats_reset();' not in s[ri[1]:ri[2]+1]:
        fail('R56l stats reset missing', 19)
    print('R56l FM chronology/backpressure verified: FIFO lossless queue + one-tick blocking retries + ring-full synth-discard')


def main():
    if not P.is_file(): fail(f'{P} not found; run from G:\\px68k-tab5')
    raw = P.read_bytes(); nl = '\r\n' if b'\r\n' in raw else '\n'
    s = raw.decode('utf-8').replace('\r\n','\n')

    if MARK in s or API in s:
        if MARK in s and API in s:
            verify(s)
            print('R56l FM chronology/backpressure already applied/verified:', P)
            return
        fail('partial R56l patch detected', 20)

    if BASE not in s: fail('R23 Internal-only YM baseline marker missing', 3)
    if K7MARK not in s or K7API not in s:
        fail('R56k7 taxonomy must be present before R56l', 4)

    # Add R56l counters beside the fact-probe taxonomy state.
    m = re.search(r'static\s+DRAM_ATTR\s+volatile\s+uint32_t\s+s_r56k7_render_drop_frames\s*=\s*0\s*;', s)
    if not m: fail('R56k7 taxonomy state anchor missing', 21)
    s = s[:m.end()] + state_block() + s[m.end():]

    # Add render-and-discard helper immediately before the existing publisher.
    rr = func_range(s, 'async_render_publish')
    if not rr: fail('async_render_publish missing', 16)
    s = s[:rr[0]] + discard_helper() + s[rr[0]:]

    # Replace ring-full "skip render" with "advance synth, discard PCM".
    rr = func_range(s, 'async_render_publish'); o,e = rr[1],rr[2]
    body = s[o:e+1]
    fm = re.search(r'if\s*\(\s*free_frames\s*<\s*frames\s*\)\s*\{', body)
    if not fm: fail('ring-full if anchor missing', 22)
    bo = o + body.find('{', fm.start(), fm.end()); be = match_brace(s, bo)
    old = s[bo:be+1]
    if 's_audio_ring_overruns' not in old or 'return 0' not in old:
        fail('unexpected ring-full body; refuse semantic rewrite', 23)
    indent_m = re.search(r'(?m)^(\s*)if\s*\(\s*free_frames', body)
    indent = indent_m.group(1) if indent_m else '    '
    new = '{\n' + indent + '    ++s_audio_ring_overruns;\n' + indent + '    r56l_render_discard(frames, profile);\n' + indent + '    return 0;\n' + indent + '}'
    s = s[:bo] + new + s[be+1:]

    # Convert the single queue-full drop branch to lossless producer backpressure.
    fr = func_range(s, 'async_opm_send'); o,e = fr[1],fr[2]
    body = s[o:e+1]
    qm = re.search(r'if\s*\(\s*xQueueSend\s*\(\s*s_audio_q\s*,\s*&ev\s*,\s*0\s*\)\s*!=\s*pdTRUE\s*\)\s*\{', body)
    if not qm: fail('R56k7 nonblocking queue-full branch anchor missing', 24)
    bo = o + body.find('{', qm.start(), qm.end()); be = match_brace(s, bo)
    old = s[bo:be+1]
    required = ['s_audio_event_drops', 'r56k7_event_drop(type, frames);', 'return 0']
    if any(x not in old for x in required):
        fail('unexpected R56k7 queue-drop body; refuse semantic rewrite', 25)
    im = re.search(r'(?m)^(\s*)if\s*\(\s*xQueueSend', body)
    ind = im.group(1) if im else '    '
    new = ('{\n'
           + ind + '    __atomic_fetch_add(&s_r56l_bp_events, 1u, __ATOMIC_RELAXED);\n'
           + ind + '    uint32_t timeouts = 0u;\n'
           + ind + '    for (;;)\n'
           + ind + '    {\n'
           + ind + '        __atomic_fetch_add(&s_r56l_bp_wait_calls, 1u, __ATOMIC_RELAXED);\n'
           + ind + '        if (xQueueSend(s_audio_q, &ev, 1) == pdTRUE)\n'
           + ind + '            break;\n'
           + ind + '        ++timeouts;\n'
           + ind + '        __atomic_fetch_add(&s_r56l_bp_timeouts, 1u, __ATOMIC_RELAXED);\n'
           + ind + '    }\n'
           + ind + '    r56l_atomic_max_u32(&s_r56l_bp_max_timeouts, timeouts);\n'
           + ind + '}')
    s = s[:bo] + new + s[be+1:]

    # Reset new telemetry at the same init point as taxonomy counters.
    fi = func_range(s, 'async_opm_init'); o,e = fi[1],fi[2]
    ib = s[o:e+1]
    anchor = 'r56k7_event_stats_reset();'
    pos = ib.find(anchor)
    if pos < 0: fail('R56k7 stats reset anchor missing in async_opm_init', 26)
    abspos = o + pos + len(anchor)
    s = s[:abspos] + '\n    r56l_stats_reset();' + s[abspos:]

    # Export backpressure/discard telemetry beside the existing taxonomy export.
    ar = func_range(s, K7API)
    if not ar: fail('R56k7 taxonomy API definition missing', 27)
    s = s[:ar[0]] + api_block() + s[ar[0]:]

    verify(s)
    P.write_bytes(s.replace('\n',nl).encode('utf-8'))
    print('R56l FM chronology/backpressure patch applied:', P)

if __name__ == '__main__':
    main()
