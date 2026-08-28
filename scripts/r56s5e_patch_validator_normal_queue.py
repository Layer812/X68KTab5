from pathlib import Path

C=Path('src/tab5_compose.c')
M=Path('src/main.c')
MARK5='PX68K_R56S5: live-validated CPU0 exact BG/TEXT + cached 65K path active'
MARK5E='PX68K_R56S5E: live validator forced to non-stale normal queue; burst enabled after PASS'
TAG='PX68K_R56S5E_VALIDATOR_NORMAL_QUEUE'

def fail(msg):
    print('R56s5e patch ERROR:',msg)
    raise SystemExit(2)

if not C.is_file() or not M.is_file(): fail('missing src/tab5_compose.c or src/main.c')
cs=C.read_text(encoding='utf-8')
ms=M.read_text(encoding='utf-8')
if MARK5 not in ms: fail('R56s5 must be applied first')
if 's_r56s5_hostbt_state' not in cs: fail('R56s5 hostbt state missing')
if MARK5E in ms and TAG in cs:
    print('R56s5e already applied and verified')
    raise SystemExit(0)
if MARK5E in ms or TAG in cs: fail('partial R56s5e state')

start=cs.find('int tab5_compose_submit_gbt65k_exact_bt_line(')
if start < 0: fail('exact submit function not found')
end=cs.find('\nint tab5_compose_gbt65k_hostbt_state(', start)
if end < 0:
    end=cs.find('\nint tab5_compose_submit_gbt65k_line(', start)
if end < 0: fail('exact submit function end not found')
fn=cs[start:end]

old_burst='    const int burst65k = s_gbt65k_burst_ready;'
if fn.count(old_burst) != 1: fail(f'exact function burst selector count={fn.count(old_burst)}')
new_burst='''    /* PX68K_R56S5E_VALIDATOR_NORMAL_QUEUE\n     * The one-shot stock-vs-host validator must never enter the 128-slot\n     * latest-frame burst queue, because that queue may legally stale-retire\n     * an old frame before validation executes.  Compute candidacy before\n     * slot selection and force only that one packet through the ordinary\n     * non-stale queue.  After PASS, all production exact-host rows use the\n     * burst queue exactly as R56s5 intended. */\n    const int r56s5_validate_candidate =\n        (load_acquire(&s_r56s5_hostbt_state) == 0u) && text_on && text_src &&\n        (!bg_on || bg_state);\n    const int burst65k = s_gbt65k_burst_ready && !r56s5_validate_candidate;'''
fn=fn.replace(old_burst,new_burst,1)

old_validate='''    const int r56s5_validate =\n        (load_acquire(&s_r56s5_hostbt_state) == 0u) && text_on && text_src &&\n        (!bg_on || bg_state);'''
if fn.count(old_validate) != 1: fail(f'exact function validator expression count={fn.count(old_validate)}')
fn=fn.replace(old_validate,'    const int r56s5_validate = r56s5_validate_candidate;',1)
cs=cs[:start]+fn+cs[end:]

mainline=f'    ESP_LOGI(TAG, "{MARK5}");\n'
if ms.count(mainline)!=1: fail(f'R56s5 main marker line count={ms.count(mainline)}')
ms=ms.replace(mainline,mainline+f'    ESP_LOGI(TAG, "{MARK5E}");\n',1)

C.write_text(cs,encoding='utf-8',newline='')
M.write_text(ms,encoding='utf-8',newline='')
print('R56s5e applied: one-shot validator uses ordinary non-stale queue; production rows retain burst path')
