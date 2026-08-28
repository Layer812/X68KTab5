from pathlib import Path
import sys
C=Path('src/tab5_compose.c'); M=Path('src/main.c'); W=Path('components/px68k/libretro/windraw.c')
def req(ok,msg):
    if not ok:
        print('R56s5e SOURCE AUDIT FAIL:',msg); raise SystemExit(2)
for p in (C,M,W): req(p.is_file(),f'missing {p}')
cs=C.read_text(encoding='utf-8'); ms=M.read_text(encoding='utf-8'); ws=W.read_text(encoding='utf-8')
req(ms.count('PX68K_R56S5: live-validated CPU0 exact BG/TEXT + cached 65K path active')==1,'R56s5 marker !=1')
req(ms.count('PX68K_R56S5E: live validator forced to non-stale normal queue; burst enabled after PASS')==1,'R56s5e marker !=1')
req(cs.count('PX68K_R56S5E_VALIDATOR_NORMAL_QUEUE')==1,'R56s5e compose marker !=1')
req(cs.count('const int r56s5_validate_candidate =')==1,'validator candidate !=1')
req(cs.count('const int burst65k = s_gbt65k_burst_ready && !r56s5_validate_candidate;')==1,'validator burst fence missing')
req(cs.count('const int r56s5_validate = r56s5_validate_candidate;')==1,'validator alias missing')
req('CPU0 stock-order BG/TEXT live self-check PASS' in cs,'live PASS path missing')
req('R56s4 exact fallback retained' in cs,'correctness fallback missing')
req('r56s5_exact_host' in ws and 'tab5_compose_gbt65k_hostbt_state' in ws,'windraw PASS short-circuit missing')
print('R56s5e SOURCE AUDIT PASS: validator cannot stale-retire; PASS still removes CPU1 stock BG/TEXT; fallback retained')
