#!/usr/bin/env python3
from pathlib import Path
import sys

PATH = Path('components/px68k/fmgen/fmg_wrap.cpp')
MARKER = 'PX68K_FM_R56F: cooperative CPU0 scheduler relief ACTIVE'
BASE_MARKER = 'Build 6.15h17R23 YM2151-only core=0'
TASK_SIGS = [
    'static void async_opm_task(void *)',
    'static IRAM_ATTR void async_opm_task(void *)',
]

STATE_BLOCK = r'''

/* R56f: keep the proven R23/R26 44.1-kHz YM2151 backend and priority=3,
 * but never let a continuously-ready CPU0 waveform worker starve lower-priority
 * Screen/compositor/IDLE0 work indefinitely.  Relief occurs only BETWEEN
 * complete AsyncOPMEvent transactions: never inside vgmm5_ym2151_render(),
 * never between an event's old-state render and its timed register write.
 *
 * Soft relief requires >= 1/2 FIFO reserve and at least 200 ms since the
 * previous relief.  Hard relief is a 3 s liveness bound; it costs one real
 * FreeRTOS tick (10 ms with this project's 100-Hz tick) and exists so IDLE0
 * cannot be starved into the 15 s task-WDT even during an endless FM queue.
 */
static DRAM_ATTR int64_t s_r56f_last_relief_us = 0;
static DRAM_ATTR uint32_t s_r56f_relief_soft = 0;
static DRAM_ATTR uint32_t s_r56f_relief_hard = 0;
static DRAM_ATTR uint32_t s_r56f_relief_logs = 0;

static inline void async_opm_r56f_relief(void)
{
    const int64_t now = esp_timer_get_time();
    if (s_r56f_last_relief_us == 0) {
        s_r56f_last_relief_us = now;
        return;
    }

    size_t avail;
    portENTER_CRITICAL(&s_audio_mux);
    avail = s_audio_count;
    portEXIT_CRITICAL(&s_audio_mux);

    const int64_t age = now - s_r56f_last_relief_us;
    const int soft = (avail >= (ASYNC_OPM_RING_FRAMES / 2u)) && (age >= 200000);
    const int hard = (age >= 3000000);
    if (!soft && !hard)
        return;

    if (hard) ++s_r56f_relief_hard;
    else      ++s_r56f_relief_soft;

    /* Snapshot diagnostics before blocking.  Logging is intentionally sparse. */
    const UBaseType_t qdepth = s_audio_q ? uxQueueMessagesWaiting(s_audio_q) : 0;
    const uint32_t total = s_r56f_relief_soft + s_r56f_relief_hard;
    if (hard || s_r56f_relief_logs < 4u || (total & 255u) == 0u) {
        ++s_r56f_relief_logs;
        printf("PX68K_FM_R56F: relief=%s soft=%lu hard=%lu fifo=%u/%u q=%u age_ms=%lu\n",
               hard ? "HARD" : "soft",
               (unsigned long)s_r56f_relief_soft,
               (unsigned long)s_r56f_relief_hard,
               (unsigned)avail, (unsigned)ASYNC_OPM_RING_FRAMES,
               (unsigned)qdepth, (unsigned long)(age / 1000));
    }

    s_r56f_last_relief_us = now;
    vTaskDelay(1);
}
'''

CALL = '\n        async_opm_r56f_relief(); /* R56f event-boundary liveness point */\n'


def match_brace(text: str, open_pos: int) -> int:
    depth = 0
    i = open_pos
    state = 'code'
    quote = ''
    while i < len(text):
        c = text[i]
        n = text[i+1] if i+1 < len(text) else ''
        if state == 'code':
            if c == '/' and n == '/':
                state = 'line'; i += 2; continue
            if c == '/' and n == '*':
                state = 'block'; i += 2; continue
            if c in ('\"', "'"):
                state = 'str'; quote = c; i += 1; continue
            if c == '{':
                depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0:
                    return i
            i += 1
        elif state == 'line':
            if c == '\n': state = 'code'
            i += 1
        elif state == 'block':
            if c == '*' and n == '/': state = 'code'; i += 2
            else: i += 1
        else:  # string/char
            if c == '\\': i += 2
            elif c == quote: state = 'code'; i += 1
            else: i += 1
    raise RuntimeError('unbalanced braces')


def fail(msg: str, rc: int = 2):
    print('R56f patch ERROR:', msg, file=sys.stderr)
    sys.exit(rc)


def main():
    if not PATH.is_file():
        fail(f'{PATH} not found; run from G:\\px68k-tab5')
    raw = PATH.read_bytes()
    newline = '\r\n' if b'\r\n' in raw else '\n'
    text = raw.decode('utf-8', errors='strict').replace('\r\n', '\n')

    if MARKER in text:
        if 'async_opm_r56f_relief(); /* R56f event-boundary liveness point */' not in text:
            fail('marker exists but event-boundary call is missing', 3)
        print('R56f YM2151 scheduler patch already present; no changes.')
        return

    if BASE_MARKER not in text:
        fail('R23/R26 internal-only YM2151 baseline marker not found; refusing unknown audio source', 4)
    if '#define ASYNC_OPM_RING_FRAMES 4096' not in text and '#define ASYNC_OPM_RING_FRAMES   4096' not in text:
        # Permit spacing variants via tokenized fallback.
        import re
        if not re.search(r'#define\s+ASYNC_OPM_RING_FRAMES\s+4096u?\b', text):
            fail('expected 4096-frame internal FM FIFO not found', 5)

    import re
    m_state = re.search(r'(?m)^static[^\n;]*\bs_audio_ring_overruns\s*=\s*0\s*;', text)
    if not m_state:
        fail('audio ring-overrun state anchor not found', 6)
    pos = m_state.end()
    text = text[:pos] + STATE_BLOCK + text[pos:]

    sig = next((s for s in TASK_SIGS if s in text), None)
    if not sig:
        fail('async_opm_task signature not found', 7)
    task_pos = text.index(sig)
    task_open = text.find('{', task_pos + len(sig))
    if task_open < 0:
        fail('async_opm_task opening brace not found', 8)
    task_end = match_brace(text, task_open)
    task = text[task_open:task_end+1]

    sw_rel = task.find('switch (ev.type)')
    if sw_rel < 0:
        sw_rel = task.find('switch(ev.type)')
    if sw_rel < 0:
        fail('switch(ev.type) not found inside async_opm_task', 9)
    sw_abs = task_open + sw_rel
    sw_open = text.find('{', sw_abs)
    if sw_open < 0 or sw_open > task_end:
        fail('event switch opening brace not found', 10)
    sw_end = match_brace(text, sw_open)
    if sw_end >= task_end:
        fail('event switch unexpectedly consumes whole task', 11)

    text = text[:sw_end+1] + CALL + text[sw_end+1:]

    # Append a compile/runtime identification literal near the helper, without
    # changing the existing R23 banner that other project diagnostics rely on.
    marker_decl = ('\nstatic const char *const s_r56f_scheduler_marker =\n'
                   '    "PX68K_FM_R56F: cooperative CPU0 scheduler relief ACTIVE";\n')
    helper_end = text.index('static inline void async_opm_r56f_relief(void)')
    # Keep marker referenced so -Wunused does not matter even under stricter flags.
    # Inject a one-shot task-start print immediately after local event declaration.
    text = text[:helper_end] + marker_decl + text[helper_end:]

    # Task start diagnostic.  Search after function signature to avoid another ev.
    task_pos = text.index(sig)
    decl = text.find('AsyncOPMEvent ev;', task_pos)
    if decl < 0:
        fail('AsyncOPMEvent ev declaration not found', 12)
    decl_end = decl + len('AsyncOPMEvent ev;')
    start_diag = ('\n    printf("%s softReserve=%u/%u softAge=200ms hardAge=3000ms delay=1tick\\n",\n'
                  '           s_r56f_scheduler_marker, (unsigned)(ASYNC_OPM_RING_FRAMES / 2u),\n'
                  '           (unsigned)ASYNC_OPM_RING_FRAMES);')
    text = text[:decl_end] + start_diag + text[decl_end:]

    out = text.replace('\n', newline).encode('utf-8')
    PATH.write_bytes(out)
    print('R56f YM2151 scheduler patch applied:', PATH)

if __name__ == '__main__':
    main()
