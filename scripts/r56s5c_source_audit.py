from pathlib import Path
C=Path('src/tab5_compose.c'); M=Path('src/main.c')
errs=[]
if not C.exists(): errs.append('missing compose')
if not M.exists(): errs.append('missing main')
if errs:
    print('R56s5c AUDIT FAIL:', '; '.join(errs)); raise SystemExit(2)
cs=C.read_text(encoding='utf-8'); ms=M.read_text(encoding='utf-8')
def req(x,m):
    if not x: errs.append(m)
req(ms.count('PX68K_R56S5: live-validated CPU0 exact BG/TEXT + cached 65K path active')==1,'R56s5 marker !=1')
req(ms.count('PX68K_R56S5C: live validator stale-discard retry armed')==1,'R56s5c marker !=1')
req(cs.count('PX68K_R56S5C_VALIDATOR_STALE_RETRY')==1,'stale retry tag !=1')
req('(slot->selfcheck & (8u | 16u)) == (8u | 16u)' in cs,'validator packet test missing')
req('store_release(&s_r56s5_hostbt_state, 0u);' in cs,'validation re-arm missing')
req('CPU0 stock-order BG/TEXT live self-check PASS' in cs,'R56s5 PASS path missing')
req('R56s4 exact fallback retained' in cs,'R56s5 FAIL fallback missing')
req('_Static_assert(sizeof(compose_slot_t) <= 8544u' in cs,'slot size guard missing')
if errs:
    print('R56s5c SOURCE AUDIT FAIL')
    for e in errs: print(' -',e)
    raise SystemExit(3)
print('R56s5c SOURCE AUDIT PASS: R56s5 live A/B + stale-discard re-arm + R56s4 safe fallback intact')
