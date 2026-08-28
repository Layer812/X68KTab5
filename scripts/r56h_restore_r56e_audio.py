#!/usr/bin/env python3
from pathlib import Path
import sys,re
P=Path('components/px68k/fmgen/fmg_wrap.cpp')
BASE='Build 6.15h17R23 YM2151-only core=0'
CALL='async_opm_r56f_relief(); /* R56f event-boundary liveness point */'

def fail(m,rc=2): print('R56h audio restore ERROR:',m,file=sys.stderr); sys.exit(rc)
def match_brace(s,o):
    d=0;i=o;state='c';q=''
    while i<len(s):
        c=s[i];n=s[i+1] if i+1<len(s) else ''
        if state=='c':
            if c=='/' and n=='/':state='l';i+=2;continue
            if c=='/' and n=='*':state='b';i+=2;continue
            if c in "\"'":state='s';q=c;i+=1;continue
            if c=='{':d+=1
            elif c=='}':
                d-=1
                if d==0:return i
            i+=1
        elif state=='l':
            if c=='\n':state='c'
            i+=1
        elif state=='b':
            if c=='*' and n=='/':state='c';i+=2
            else:i+=1
        else:
            if c=='\\':i+=2
            elif c==q:state='c';i+=1
            else:i+=1
    raise RuntimeError('unbalanced braces')

def main():
    if not P.is_file(): fail(f'{P} not found; run from G:\\px68k-tab5')
    raw=P.read_bytes(); nl='\r\n' if b'\r\n' in raw else '\n'; s=raw.decode('utf-8').replace('\r\n','\n')
    if BASE not in s: fail('R23/R26 4096-frame internal-only baseline marker missing; refuse unknown audio source',3)
    changed=False
    # Remove R56f or R56g scheduler state+helper as a single range.
    starts=[x for x in (s.find('/* R56f: keep the proven R23/R26 44.1-kHz YM2151 backend'),
                         s.find('/* R56g: R56f proved that giving CPU0 an escape hatch')) if x>=0]
    if starts:
        st=min(starts); fp=s.find('static inline void async_opm_r56f_relief(void)',st)
        if fp<0: fail('R56f/g state marker found but helper missing',4)
        op=s.find('{',fp)
        en=match_brace(s,op)
        # absorb trailing whitespace/newlines only
        j=en+1
        while j<len(s) and s[j] in ' \t\n': j+=1
        s=s[:st]+s[j:]; changed=True
    # Remove one-shot task-start diagnostics (R56f or R56g variants).
    patterns=[
        r'\n\s*printf\("%s softReserve=%u/%u softAge=200ms hardAge=3000ms delay=1tick\\n",\s*\n\s*s_r56f_scheduler_marker,\s*\(unsigned\)\(ASYNC_OPM_RING_FRAMES / 2u\),\s*\n\s*\(unsigned\)ASYNC_OPM_RING_FRAMES\);',
        r'\n\s*printf\("%s cadence=1000ms action=prio-drop-to-idle\+yield no-fixed-tick-sleep ring=%u\\n",\s*\n\s*s_r56f_scheduler_marker,\s*\(unsigned\)ASYNC_OPM_RING_FRAMES\);'
    ]
    for pat in patterns:
        s2,n=re.subn(pat,'',s,count=1,flags=re.M)
        if n: s=s2; changed=True
    # Remove event-boundary relief call.
    if CALL in s:
        s=s.replace('\n        '+CALL+'\n','\n',1)
        s=s.replace(CALL,'',1); changed=True
    # Strong postconditions: R56e audio means no scheduler intervention symbols.
    bad=['PX68K_FM_R56F','PX68K_FM_R56G','async_opm_r56f_relief','s_r56g_last_yield_us','s_r56f_last_relief_us','vTaskPrioritySet(NULL, tskIDLE_PRIORITY)']
    present=[x for x in bad if x in s]
    if present: fail('restore incomplete, leftover: '+', '.join(present),5)
    P.write_bytes(s.replace('\n',nl).encode('utf-8'))
    print('R56h audio restore:', 'restored R56e behavior' if changed else 'already R56e behavior; no change', P)
if __name__=='__main__': main()
