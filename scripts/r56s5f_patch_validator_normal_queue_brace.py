from pathlib import Path
import re

C=Path('src/tab5_compose.c')
M=Path('src/main.c')
MARK5='PX68K_R56S5: live-validated CPU0 exact BG/TEXT + cached 65K path active'
MARK5F='PX68K_R56S5F: validator forced to ordinary queue by brace-scoped function patch; burst after PASS'
TAG='PX68K_R56S5F_VALIDATOR_NORMAL_QUEUE'

def fail(msg):
    print('R56s5f patch ERROR:', msg)
    raise SystemExit(2)

def matching_brace(src, open_pos):
    depth=0; i=open_pos; n=len(src)
    state='code'
    while i<n:
        c=src[i]; d=src[i+1] if i+1<n else ''
        if state=='code':
            if c=='/' and d=='/': state='line'; i+=2; continue
            if c=='/' and d=='*': state='block'; i+=2; continue
            if c=='"': state='str'; i+=1; continue
            if c=="'": state='char'; i+=1; continue
            if c=='{': depth+=1
            elif c=='}':
                depth-=1
                if depth==0: return i
        elif state=='line':
            if c=='\n': state='code'
        elif state=='block':
            if c=='*' and d=='/': state='code'; i+=2; continue
        elif state=='str':
            if c=='\\': i+=2; continue
            if c=='"': state='code'
        elif state=='char':
            if c=='\\': i+=2; continue
            if c=="'": state='code'
        i+=1
    return -1

if not C.is_file() or not M.is_file(): fail('missing src/tab5_compose.c or src/main.c')
cs=C.read_text(encoding='utf-8')
ms=M.read_text(encoding='utf-8')
if MARK5 not in ms: fail('R56s5 must be applied first')
if 's_r56s5_hostbt_state' not in cs: fail('R56s5 hostbt state missing')
if MARK5F in ms and TAG in cs:
    print('R56s5f already applied and verified')
    raise SystemExit(0)
if MARK5F in ms or TAG in cs: fail('partial R56s5f state')

m=re.search(r'\bint\s+tab5_compose_submit_gbt65k_exact_bt_line\s*\(', cs)
if not m: fail('exact submit function not found')
open_pos=cs.find('{', m.end())
if open_pos<0: fail('exact submit opening brace not found')
close_pos=matching_brace(cs, open_pos)
if close_pos<0: fail('exact submit matching closing brace not found')
fn=cs[m.start():close_pos+1]

old_burst='    const int burst65k = s_gbt65k_burst_ready;'
old_validate='''    const int r56s5_validate =\n        (load_acquire(&s_r56s5_hostbt_state) == 0u) && text_on && text_src &&\n        (!bg_on || bg_state);'''
if fn.count(old_burst)!=1: fail(f'old burst selector count in exact function={fn.count(old_burst)}')
if fn.count(old_validate)!=1: fail(f'validator expression count in exact function={fn.count(old_validate)}')

new_burst='''    /* PX68K_R56S5F_VALIDATOR_NORMAL_QUEUE\n     * The one-shot stock-vs-host validator must complete exactly once before\n     * production host-BT rows are enabled.  Keep that one packet off the\n     * latest-frame burst queue so it cannot be stale-retired.  After PASS,\n     * r56s5_validate_candidate is false and production rows use burst exactly\n     * as R56s5 intended. */\n    const int r56s5_validate_candidate =\n        (load_acquire(&s_r56s5_hostbt_state) == 0u) && text_on && text_src &&\n        (!bg_on || bg_state);\n    const int burst65k = s_gbt65k_burst_ready && !r56s5_validate_candidate;'''
fn2=fn.replace(old_burst,new_burst,1).replace(old_validate,'    const int r56s5_validate = r56s5_validate_candidate;',1)
if TAG not in fn2: fail('internal patch construction failed')
cs2=cs[:m.start()]+fn2+cs[close_pos+1:]
mainline=f'    ESP_LOGI(TAG, "{MARK5}");\n'
if ms.count(mainline)!=1: fail(f'R56s5 main marker line count={ms.count(mainline)}')
ms2=ms.replace(mainline,mainline+f'    ESP_LOGI(TAG, "{MARK5F}");\n',1)

# final in-memory audit before touching files
if cs2.count(TAG)!=1: fail('post-patch tag count != 1')
if old_burst in cs2[m.start():m.start()+len(fn2)+256]: fail('old burst selector survived')
if 'const int r56s5_validate = r56s5_validate_candidate;' not in fn2: fail('candidate wiring missing')

C.write_text(cs2,encoding='utf-8',newline='')
M.write_text(ms2,encoding='utf-8',newline='')
print('R56s5f applied: brace-scoped exact-submit patch; validator ordinary queue, production burst retained')
