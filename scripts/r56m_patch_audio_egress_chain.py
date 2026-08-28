#!/usr/bin/env python3
from pathlib import Path
import re, sys

P = Path('src/tab5_audio.cpp')
BASE = 'Build 6.15h17R13 audio RT'
MARK = 'PX68K_AUDIO_R56M: egress-chain fact probe; counters only; no audio policy change'
API = 'tab5_audio_get_egress_probe'

def fail(msg, rc=2):
    print('R56m audio egress patch ERROR:', msg, file=sys.stderr)
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

def state_block():
    return f'''

/* R56m: fact-only audio egress chain.  Worker paths update atomics only;
 * diagnostics are emitted by the existing CPU1 R56K_HOST sampler. */
static const char s_r56m_audio_egress_marker[] __attribute__((used)) =
    "{MARK}";
static volatile uint32_t s_r56m_pull_calls = 0;
static volatile uint32_t s_r56m_pull_zero = 0;
static volatile uint32_t s_r56m_pull_frames = 0;
static volatile uint32_t s_r56m_pull_nz = 0;
static volatile uint32_t s_r56m_pull_peak = 0;
static volatile uint32_t s_r56m_mix_calls = 0;
static volatile uint32_t s_r56m_mix_frames = 0;
static volatile uint32_t s_r56m_mix_nz = 0;
static volatile uint32_t s_r56m_mix_peak = 0;
static volatile uint32_t s_r56m_play_calls = 0;
static volatile uint32_t s_r56m_play_frames = 0;
static volatile uint32_t s_r56m_play_nz = 0;
static volatile uint32_t s_r56m_play_peak = 0;

static inline void r56m_atomic_max(volatile uint32_t *p, uint32_t v)
{{
    uint32_t old = __atomic_load_n(p, __ATOMIC_RELAXED);
    while (old < v && !__atomic_compare_exchange_n(
               p, &old, v, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {{}}
}}

static inline void r56m_scan_pcm(const int16_t *samples, size_t frames,
                                 uint32_t *nz_out, uint32_t *peak_out)
{{
    uint32_t nz = 0u, peak = 0u;
    if (samples && frames)
    {{
        const size_t n = frames * 2u;
        for (size_t i = 0; i < n; ++i)
        {{
            const int32_t v = (int32_t)samples[i];
            const uint32_t a = (uint32_t)(v < 0 ? -v : v);
            if (a) ++nz;
            if (a > peak) peak = a;
        }}
    }}
    *nz_out = nz; *peak_out = peak;
}}

static inline void r56m_note_pull(const int16_t *samples, int got)
{{
    __atomic_fetch_add(&s_r56m_pull_calls, 1u, __ATOMIC_RELAXED);
    if (got <= 0)
    {{
        __atomic_fetch_add(&s_r56m_pull_zero, 1u, __ATOMIC_RELAXED);
        return;
    }}
    const uint32_t frames = (uint32_t)got;
    uint32_t nz, peak; r56m_scan_pcm(samples, frames, &nz, &peak);
    __atomic_fetch_add(&s_r56m_pull_frames, frames, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_r56m_pull_nz, nz, __ATOMIC_RELAXED);
    r56m_atomic_max(&s_r56m_pull_peak, peak);
}}

static inline void r56m_note_mix(const int16_t *samples, size_t frames)
{{
    uint32_t nz, peak; r56m_scan_pcm(samples, frames, &nz, &peak);
    __atomic_fetch_add(&s_r56m_mix_calls, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_r56m_mix_frames, (uint32_t)frames, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_r56m_mix_nz, nz, __ATOMIC_RELAXED);
    r56m_atomic_max(&s_r56m_mix_peak, peak);
}}

static inline void r56m_note_play(const int16_t *samples, size_t frames)
{{
    uint32_t nz, peak; r56m_scan_pcm(samples, frames, &nz, &peak);
    __atomic_fetch_add(&s_r56m_play_calls, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_r56m_play_frames, (uint32_t)frames, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_r56m_play_nz, nz, __ATOMIC_RELAXED);
    r56m_atomic_max(&s_r56m_play_peak, peak);
}}

static inline void r56m_reset(void)
{{
    __atomic_store_n(&s_r56m_pull_calls,0u,__ATOMIC_RELAXED);
    __atomic_store_n(&s_r56m_pull_zero,0u,__ATOMIC_RELAXED);
    __atomic_store_n(&s_r56m_pull_frames,0u,__ATOMIC_RELAXED);
    __atomic_store_n(&s_r56m_pull_nz,0u,__ATOMIC_RELAXED);
    __atomic_store_n(&s_r56m_pull_peak,0u,__ATOMIC_RELAXED);
    __atomic_store_n(&s_r56m_mix_calls,0u,__ATOMIC_RELAXED);
    __atomic_store_n(&s_r56m_mix_frames,0u,__ATOMIC_RELAXED);
    __atomic_store_n(&s_r56m_mix_nz,0u,__ATOMIC_RELAXED);
    __atomic_store_n(&s_r56m_mix_peak,0u,__ATOMIC_RELAXED);
    __atomic_store_n(&s_r56m_play_calls,0u,__ATOMIC_RELAXED);
    __atomic_store_n(&s_r56m_play_frames,0u,__ATOMIC_RELAXED);
    __atomic_store_n(&s_r56m_play_nz,0u,__ATOMIC_RELAXED);
    __atomic_store_n(&s_r56m_play_peak,0u,__ATOMIC_RELAXED);
}}

extern "C" void {API}(
    uint32_t *pull_calls, uint32_t *pull_zero, uint32_t *pull_frames,
    uint32_t *pull_nz, uint32_t *pull_peak,
    uint32_t *mix_calls, uint32_t *mix_frames, uint32_t *mix_nz, uint32_t *mix_peak,
    uint32_t *play_calls, uint32_t *play_frames, uint32_t *play_nz, uint32_t *play_peak)
{{
    if (pull_calls) *pull_calls=__atomic_load_n(&s_r56m_pull_calls,__ATOMIC_RELAXED);
    if (pull_zero) *pull_zero=__atomic_load_n(&s_r56m_pull_zero,__ATOMIC_RELAXED);
    if (pull_frames) *pull_frames=__atomic_load_n(&s_r56m_pull_frames,__ATOMIC_RELAXED);
    if (pull_nz) *pull_nz=__atomic_load_n(&s_r56m_pull_nz,__ATOMIC_RELAXED);
    if (pull_peak) *pull_peak=__atomic_load_n(&s_r56m_pull_peak,__ATOMIC_RELAXED);
    if (mix_calls) *mix_calls=__atomic_load_n(&s_r56m_mix_calls,__ATOMIC_RELAXED);
    if (mix_frames) *mix_frames=__atomic_load_n(&s_r56m_mix_frames,__ATOMIC_RELAXED);
    if (mix_nz) *mix_nz=__atomic_load_n(&s_r56m_mix_nz,__ATOMIC_RELAXED);
    if (mix_peak) *mix_peak=__atomic_load_n(&s_r56m_mix_peak,__ATOMIC_RELAXED);
    if (play_calls) *play_calls=__atomic_load_n(&s_r56m_play_calls,__ATOMIC_RELAXED);
    if (play_frames) *play_frames=__atomic_load_n(&s_r56m_play_frames,__ATOMIC_RELAXED);
    if (play_nz) *play_nz=__atomic_load_n(&s_r56m_play_nz,__ATOMIC_RELAXED);
    if (play_peak) *play_peak=__atomic_load_n(&s_r56m_play_peak,__ATOMIC_RELAXED);
}}
'''

def verify(s):
    for tok,rc in [(BASE,3),(MARK,4),(API,5),('r56m_note_pull(s_source_pull, got);',6),
                   ('r56m_note_mix(samples, frames);',7),('r56m_note_play(s_play[play_index], play_frames);',8),
                   ('r56m_reset();',9)]:
        if tok not in s: fail('required token missing: '+tok,rc)
    if s.count('r56m_note_pull(s_source_pull, got);') != 1: fail('pull hook count != 1',10)
    if s.count('r56m_note_mix(samples, frames);') != 1: fail('mix hook count != 1',11)
    if s.count('r56m_note_play(s_play[play_index], play_frames);') != 1: fail('play hook count != 1',12)
    # Fact probe must not touch policy calls/parameters.
    if 'M5.Speaker.playRaw' not in s or 'WinX68k_AudioHostReadFrames' not in s:
        fail('audio path anchors disappeared',13)
    print('R56m audio egress verified: pull/mix/play counters only; no stdio in worker probe')

def main():
    if not P.is_file(): fail(f'{P} not found; run from G:\\px68k-tab5')
    raw=P.read_bytes(); nl='\r\n' if b'\r\n' in raw else '\n'; s=raw.decode('utf-8').replace('\r\n','\n')
    if MARK in s or API in s:
        if MARK in s and API in s:
            verify(s); print('R56m audio egress already applied/verified:',P); return
        fail('partial R56m patch detected',14)
    if BASE not in s: fail('R13 audio baseline marker missing',3)

    # State/API block after TAG declaration.
    tm=re.search(r'(?m)^static\s+const\s+char\s*\*\s*TAG\s*=\s*"TAB5_AUDIO"\s*;',s)
    if not tm: fail('TAB5_AUDIO TAG anchor missing',15)
    s=s[:tm.end()]+state_block()+s[tm.end():]

    # Pull boundary: immediately after the actual host-source read.
    fr=func_range(s,'pump_host_source')
    if not fr: fail('pump_host_source missing',16)
    b=s[fr[1]:fr[2]+1]
    pm=re.search(r'(?m)^(\s*)(?:const\s+)?int\s+got\s*=\s*WinX68k_AudioHostReadFrames\s*\([^;]+;\s*$',b)
    if not pm: fail('host read assignment anchor missing',17)
    abs_end=fr[1]+pm.end(); ind=pm.group(1)
    s=s[:abs_end]+'\n'+ind+'r56m_note_pull(s_source_pull, got);'+s[abs_end:]

    # Final-mix/host-ring input boundary: function entry only.
    fr=func_range(s,'audio_enqueue_mixed')
    if not fr: fail('audio_enqueue_mixed definition missing',18)
    insert=fr[1]+1
    s=s[:insert]+'\n    r56m_note_mix(samples, frames);'+s[insert:]

    # Physical submit boundary: immediately before existing playRaw call.
    fr=func_range(s,'audio_task')
    if not fr: fail('audio_task missing',19)
    b=s[fr[1]:fr[2]+1]
    qm=re.search(r'(?m)^(\s*)if\s*\(\s*M5\.Speaker\.playRaw\s*\(',b)
    if not qm: fail('inline M5.Speaker.playRaw if-anchor missing',20)
    abs_start=fr[1]+qm.start(); ind=qm.group(1)
    s=s[:abs_start]+ind+'r56m_note_play(s_play[play_index], play_frames);\n'+s[abs_start:]

    # Reset only at audio init; no runtime reset/flush semantics changed.
    fr=func_range(s,'tab5_audio_init')
    if not fr: fail('tab5_audio_init missing',21)
    insert=fr[1]+1
    s=s[:insert]+'\n    r56m_reset();'+s[insert:]

    verify(s)
    P.write_bytes(s.replace('\n',nl).encode('utf-8'))
    print('R56m audio egress patch applied:',P)

if __name__=='__main__': main()
