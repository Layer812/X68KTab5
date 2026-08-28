#!/usr/bin/env python3
from pathlib import Path
import sys, shutil

MAIN = Path('src/main.c')
MARK = 'PX68K_R56P: QUIET exact-executor counters visible; fact-only'
OLD = '''#if PX68K_TAB5_R43_QUIET_RUNTIME
            ESP_LOGI(TAG,
                     "CPU613R43 QUIET f=%lu avg=%luus/f fps=%lu.%lu speed=%lu%%",
                     (unsigned long)frame, (unsigned long)avg_wall_us,
                     (unsigned long)(fps_x10/10u), (unsigned long)(fps_x10%10u),
                     (unsigned long)speed_pct);
#else'''
NEW = '''#if PX68K_TAB5_R43_QUIET_RUNTIME
            /* PX68K_R56P: QUIET exact-executor counters visible; fact-only.
             * The three stats calls above already execute in R43 quiet mode;
             * R56p only exposes their already-collected cumulative values. */
            ESP_LOGI(TAG,
                     "CPU613R43 QUIET f=%lu avg=%luus/f fps=%lu.%lu speed=%lu%% "
                     "MDXQ{CMPHI=%u/%llu/max%u MDX52=%u/%llu/%llu/max%u ZRUN=%u/%llu/max%u}",
                     (unsigned long)frame, (unsigned long)avg_wall_us,
                     (unsigned long)(fps_x10/10u), (unsigned long)(fps_x10%10u),
                     (unsigned long)speed_pct,
                     cmphi_calls, cmphi_loops, cmphi_max,
                     mdx_calls, mdx_outer, mdx_insn, mdx_max,
                     zr_calls, zr_loops, zr_max);
#else'''


def fail(msg, rc=2):
    print('R56p MDX exact-counter patch ERROR:', msg, file=sys.stderr)
    sys.exit(rc)


def verify(s):
    for tok in [MARK, 'MDXQ{CMPHI=', 'm68k_tab5_cmphi617_stats',
                'm68k_tab5_mdx619_stats', 'm68k_tab5_mdx622_stats']:
        if tok not in s:
            fail('verify missing: '+tok, 20)
    if s.count(MARK) != 1:
        fail('marker duplicate/missing', 21)
    # R56p must remain a fact-only R56o derivative.  The expensive R56n
    # per-frame cycle-attribution probe must not come back.
    for tok in ['R56N_WIN f=', 'r56n_guest_cycles', 'esp_cpu_get_cycle_count']:
        if tok in s:
            fail('R56n attribution residue present before/after R56p: '+tok, 22)
    # Never silently re-enable the historical zero-run executor in this probe.
    if 'PX68K_MDX622: ZERO-RUN batch ACTIVE' in s:
        fail('R56p main unexpectedly contains runtime ZERO-RUN enable marker', 23)


def main():
    if not MAIN.is_file():
        fail(r'src/main.c missing; run from G:\px68k-tab5 project root')
    raw = MAIN.read_bytes()
    nl = '\r\n' if b'\r\n' in raw else '\n'
    s = raw.decode('utf-8').replace('\r\n','\n')
    if MARK in s:
        verify(s)
        print('R56p already applied and verified:', MAIN)
        return
    if 'R56N_WIN f=' in s or 'r56n_guest_cycles' in s:
        fail('R56n attribution still present; run R56o normalizer first', 10)
    if OLD not in s:
        fail('R43 QUIET print anchor missing/unexpected', 11)
    out = s.replace(OLD, NEW, 1)
    bak = MAIN.with_suffix(MAIN.suffix + '.r56p.bak')
    if not bak.exists():
        shutil.copy2(MAIN, bak)
    MAIN.write_bytes(out.replace('\n', nl).encode('utf-8'))
    verify(out)
    print('R56p applied: existing CMPHI/MDX52/ZRUN counters exposed in QUIET line; no guest/audio policy change')

if __name__ == '__main__':
    main()
