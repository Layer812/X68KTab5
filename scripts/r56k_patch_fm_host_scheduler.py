#!/usr/bin/env python3
from pathlib import Path
import re,sys

P=Path('components/px68k/fmgen/fmg_wrap.cpp')
BASE='Build 6.15h17R23 YM2151-only core=0'
MARK='PX68K_HOST_R56K: YM prio=3 complete-event boundary-yield; no sleep/no priority-drop'
CALL='taskYIELD(); /* R56k complete AsyncOPMEvent boundary */'


def fail(m,rc=2):
    print('R56k FM host scheduler patch ERROR:',m,file=sys.stderr); sys.exit(rc)

def match_brace(s,o):
    d=0;i=o;state='c';q=''
    while i<len(s):
        c=s[i];n=s[i+1] if i+1<len(s) else ''
        if state=='c':
            if c=='/' and n=='/': state='l';i+=2;continue
            if c=='/' and n=='*': state='b';i+=2;continue
            if c in "\"'": state='s';q=c;i+=1;continue
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
    if BASE not in s: fail('R23/R26 Internal-only YM baseline marker missing; refuse unknown source',3)
    if not re.search(r'#define\s+ASYNC_OPM_RING_FRAMES\s+4096u?\b',s):
        fail('expected 4096-frame Internal FM FIFO missing',4)
    if any(x in s for x in ['PX68K_FM_R56F','PX68K_FM_R56G','async_opm_r56f_relief','vTaskPrioritySet(NULL, tskIDLE_PRIORITY)']):
        fail('old R56f/g scheduling intervention remains; run r56h_restore_r56e_audio.py first',5)

    sig=next((x for x in ['static void async_opm_task(void *)','static IRAM_ATTR void async_opm_task(void *)'] if x in s),None)
    if not sig: fail('async_opm_task signature missing',6)
    tp=s.index(sig); to=s.find('{',tp+len(sig)); te=match_brace(s,to)
    task=s[to:te+1]

    # Historical 6.15a+ contract intentionally made YM priority=3 on CPU0.
    # R56k1 validates the actual C/C++ call structurally instead of depending
    # on one exact source spelling.  The task stack has changed in a few local
    # branches; R56k does not modify it, so it is diagnostic rather than an
    # admission criterion.  Priority=3 and core=0 are the scheduler contract.
    def find_call(text, names):
        for name in names:
            pos = 0
            while True:
                m = re.search(r'\b' + re.escape(name) + r'\b', text[pos:])
                if not m: break
                a = pos + m.start(); op = text.find('(', a + len(name))
                if op < 0: break
                # match_brace is brace-only, so use a local paren scanner.
                d=0; i=op; state='c'; q=''
                while i < len(text):
                    c=text[i]; n=text[i+1] if i+1<len(text) else ''
                    if state=='c':
                        if c=='/' and n=='/': state='l'; i+=2; continue
                        if c=='/' and n=='*': state='b'; i+=2; continue
                        if c in "\\\"'": state='s'; q=c; i+=1; continue
                        if c=='(': d+=1
                        elif c==')':
                            d-=1
                            if d==0:
                                body=text[op+1:i]
                                if 'async_opm_task' in body:
                                    return name, body
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
                pos = a + len(name)
        return None, None

    def split_args(body):
        out=[]; st=0; d1=d2=d3=0; state='c'; q=''; i=0
        while i < len(body):
            c=body[i]; n=body[i+1] if i+1<len(body) else ''
            if state=='c':
                if c=='/' and n=='/': state='l'; i+=2; continue
                if c=='/' and n=='*': state='b'; i+=2; continue
                if c in "\\\"'": state='s'; q=c; i+=1; continue
                if c=='(': d1+=1
                elif c==')': d1-=1
                elif c=='[': d2+=1
                elif c==']': d2-=1
                elif c=='{': d3+=1
                elif c=='}': d3-=1
                elif c==',' and d1==d2==d3==0:
                    out.append(body[st:i].strip()); st=i+1
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
        out.append(body[st:].strip())
        return out

    def clean_expr(x):
        x=re.sub(r'/\*.*?\*/',' ',x,flags=re.S)
        x=re.sub(r'//[^\n]*',' ',x)
        return re.sub(r'\s+','',x)

    api, body = find_call(s, ['xTaskCreatePinnedToCore','xTaskCreatePinnedToCoreWithCaps'])
    if not body:
        fail('YM worker xTaskCreatePinnedToCore call not found',7)
    args=split_args(body)
    if len(args) < 7:
        fail(f'YM worker task-create call has unexpected arg count={len(args)}',7)
    a0,a1,astack,aparam,aprio,ahandle,acore = map(clean_expr,args[:7])
    if 'async_opm_task' not in a0 or 'px68k_ym2151' not in a1 or 's_audio_task' not in ahandle:
        fail('YM worker task-create identity/name/handle contract changed',7)
    if not re.fullmatch(r'\(?3[uUlL]*\)?', aprio):
        fail(f'YM worker priority contract changed: detected {args[4]!r}, expected 3',7)
    if not re.fullmatch(r'\(?0[uUlL]*\)?', acore):
        fail(f'YM worker core-affinity contract changed: detected {args[6]!r}, expected CPU0',7)
    sm=re.fullmatch(r'\(?(\d+)[uUlL]*\)?', astack)
    stack_txt = sm.group(1) if sm else args[2].strip()
    if sm and int(sm.group(1)) < 6144:
        fail(f'YM worker stack unexpectedly shrank below proven floor: {sm.group(1)} < 6144',7)
    print(f'R56k1 FM contract detected: api={api} stack={stack_txt} prio=3 core=0 handle=s_audio_task')

    if CALL in task and MARK in s:
        print('R56k FM host scheduler patch already applied/verified:',P); return
    if CALL in task or MARK in s:
        fail('partial R56k patch detected',8)

    sw_rel=task.find('switch (ev.type)')
    if sw_rel<0: sw_rel=task.find('switch(ev.type)')
    if sw_rel<0: fail('switch(ev.type) missing inside async_opm_task',9)
    sw_abs=to+sw_rel; so=s.find('{',sw_abs); se=match_brace(s,so)
    if se>=te: fail('event switch unexpectedly consumes whole task',10)

    # Atomic unit remains one complete event. For timed WRITE, the old-state
    # render and register write are both inside the switch and finish first.
    insert='\n        '+CALL+'\n'
    s=s[:se+1]+insert+s[se+1:]

    # Retained in ELF for post-link verification, but never printf'd from the
    # realtime worker. Startup/health logging is owned elsewhere.
    anchor=re.search(r'(?m)^static[^\n;]*\bs_audio_ring_overruns\s*=\s*0\s*;',s)
    if not anchor: fail('ring-overrun state anchor missing',11)
    marker=('\nstatic const char s_r56k_host_scheduler_marker[] __attribute__((used)) =\n'
            '    "'+MARK+'";\n')
    s=s[:anchor.end()]+marker+s[anchor.end():]

    # No fixed wall-time scheduling intervention is allowed in this task.
    tp=s.index(sig); to=s.find('{',tp+len(sig)); te=match_brace(s,to); task=s[to:te+1]
    if 'vTaskDelay(' in task or 'vTaskPrioritySet(' in task:
        fail('fixed delay/priority mutation found inside YM task after patch',12)
    if task.count(CALL)!=1: fail('expected exactly one event-boundary yield',13)

    P.write_bytes(s.replace('\n',nl).encode('utf-8'))
    print('R56k FM host scheduler patch applied:',P)

if __name__=='__main__': main()
