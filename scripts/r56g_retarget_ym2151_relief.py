#!/usr/bin/env python3
from pathlib import Path
import sys

PATH = Path('components/px68k/fmgen/fmg_wrap.cpp')
OLD_MARK = 'PX68K_FM_R56F: cooperative CPU0 scheduler relief ACTIVE'
NEW_MARK = 'PX68K_FM_R56G: priority-drop CPU0 liveness yield ACTIVE'
FUNC_SIG = 'static inline void async_opm_r56f_relief(void)'

NEW_STATE = r'''/* R56g: R56f proved that giving CPU0 an escape hatch prevents the IDLE0 WDT,
 * but a whole 100-Hz tick (10 ms) is too coarse for a YM2151 worker already
 * close to saturation.  Keep the exact same event-boundary placement and audio
 * semantics, but replace sleeping with a brief priority-drop/yield handshake.
 *
 * Once per second at most, AFTER a complete AsyncOPMEvent, temporarily lower
 * only this worker to IDLE priority and yield.  Ready Screen/compositor work can
 * run first; when only priority-0 tasks remain, IDLE0 gets a scheduler turn and
 * can service the task-WDT.  No fixed 10-ms hole is inserted into PCM production.
 */
static DRAM_ATTR int64_t s_r56g_last_yield_us = 0;
static DRAM_ATTR uint32_t s_r56g_yields = 0;
static DRAM_ATTR uint32_t s_r56g_logs = 0;

static const char *const s_r56f_scheduler_marker =
    "PX68K_FM_R56G: priority-drop CPU0 liveness yield ACTIVE";
'''

NEW_FUNC = r'''static inline void async_opm_r56f_relief(void)
{
    const int64_t now = esp_timer_get_time();
    if (s_r56g_last_yield_us == 0) {
        s_r56g_last_yield_us = now;
        return;
    }
    const int64_t age = now - s_r56g_last_yield_us;
    if (age < 1000000)
        return;

    size_t avail;
    portENTER_CRITICAL(&s_audio_mux);
    avail = s_audio_count;
    portEXIT_CRITICAL(&s_audio_mux);
    const UBaseType_t qdepth = s_audio_q ? uxQueueMessagesWaiting(s_audio_q) : 0;
    const UBaseType_t old_prio = 3u; /* verified R23/R26 worker contract */

    ++s_r56g_yields;
    if (s_r56g_logs < 6u || (s_r56g_yields & 127u) == 0u) {
        ++s_r56g_logs;
        printf("PX68K_FM_R56G: yield=%lu fifo=%u/%u q=%u age_ms=%lu prio=%u->0->%u\n",
               (unsigned long)s_r56g_yields,
               (unsigned)avail, (unsigned)ASYNC_OPM_RING_FRAMES,
               (unsigned)qdepth, (unsigned long)(age / 1000),
               (unsigned)old_prio, (unsigned)old_prio);
    }

    s_r56g_last_yield_us = now;
    vTaskPrioritySet(NULL, tskIDLE_PRIORITY);
    taskYIELD();
    vTaskPrioritySet(NULL, old_prio);
}
'''


def match_brace(text: str, open_pos: int) -> int:
    depth = 0
    i = open_pos
    state = 'code'
    quote = ''
    while i < len(text):
        c = text[i]
        n = text[i+1] if i+1 < len(text) else ''
        if state == 'code':
            if c == '/' and n == '/': state='line'; i+=2; continue
            if c == '/' and n == '*': state='block'; i+=2; continue
            if c in ('"', "'"): state='str'; quote=c; i+=1; continue
            if c == '{': depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0: return i
            i += 1
        elif state == 'line':
            if c == '\n': state='code'
            i += 1
        elif state == 'block':
            if c == '*' and n == '/': state='code'; i+=2
            else: i+=1
        else:
            if c == '\\': i += 2
            elif c == quote: state='code'; i+=1
            else: i+=1
    raise RuntimeError('unbalanced braces')


def fail(msg, rc=2):
    print('R56g YM patch ERROR:', msg, file=sys.stderr)
    sys.exit(rc)


def main():
    if not PATH.is_file(): fail(f'{PATH} not found; run from G:\\px68k-tab5')
    raw = PATH.read_bytes()
    newline = '\r\n' if b'\r\n' in raw else '\n'
    text = raw.decode('utf-8', errors='strict').replace('\r\n','\n')

    if NEW_MARK in text:
        if FUNC_SIG not in text or 'vTaskPrioritySet(NULL, tskIDLE_PRIORITY);' not in text:
            fail('R56g marker exists but priority-drop body is incomplete', 3)
        print('R56g YM2151 priority-drop patch already present; no changes.')
        return
    if OLD_MARK not in text:
        fail('R56f marker not found; apply R56f baseline first or refuse unknown audio source', 4)
    if FUNC_SIG not in text:
        fail('R56f relief helper not found', 5)
    if 'async_opm_r56f_relief(); /* R56f event-boundary liveness point */' not in text:
        fail('R56f event-boundary call not found', 6)

    # Replace the whole R56f scheduler state/comment block up to the helper.
    start = text.find('/* R56f: keep the proven R23/R26 44.1-kHz YM2151 backend')
    if start < 0: fail('R56f scheduler state block start not found', 7)
    fpos = text.find(FUNC_SIG, start)
    if fpos < 0: fail('R56f helper position not found', 8)
    text = text[:start] + NEW_STATE + '\n' + text[fpos:]

    # Replace helper implementation with priority-drop/yield version.
    fpos = text.find(FUNC_SIG, start)
    op = text.find('{', fpos + len(FUNC_SIG))
    if op < 0: fail('helper opening brace not found', 9)
    end = match_brace(text, op)
    text = text[:fpos] + NEW_FUNC + text[end+1:]

    # Update task-start diagnostic that R56f inserted.
    old_diag = ('printf("%s softReserve=%u/%u softAge=200ms hardAge=3000ms delay=1tick\\n",\n'
                '           s_r56f_scheduler_marker, (unsigned)(ASYNC_OPM_RING_FRAMES / 2u),\n'
                '           (unsigned)ASYNC_OPM_RING_FRAMES);')
    new_diag = ('printf("%s cadence=1000ms action=prio-drop-to-idle+yield no-fixed-tick-sleep ring=%u\\n",\n'
                '           s_r56f_scheduler_marker, (unsigned)ASYNC_OPM_RING_FRAMES);')
    if old_diag not in text:
        fail('R56f task-start diagnostic anchor not found', 10)
    text = text.replace(old_diag, new_diag, 1)

    PATH.write_bytes(text.replace('\n', newline).encode('utf-8'))
    print('R56g YM2151 priority-drop patch applied:', PATH)

if __name__ == '__main__':
    main()
