from pathlib import Path
import sys
M=Path('src/main.c'); C=Path('src/tab5_compose.c'); W=Path('components/px68k/libretro/windraw.c')
errs=[]
def req(c,m):
    if not c: errs.append(m)
for p in (M,C,W): req(p.is_file(),f'missing {p}')
if not errs:
    ms=M.read_text(encoding='utf-8'); cs=C.read_text(encoding='utf-8'); ws=W.read_text(encoding='utf-8')
    req(ms.count('PX68K_R56S5: live-validated CPU0 exact BG/TEXT + cached 65K path active')==1,'R56s5 marker count')
    req(ms.count('PX68K_R56S5F: validator forced to ordinary queue by brace-scoped function patch; burst after PASS')==1,'R56s5f marker count')
    req(ms.count('PX68K_R56S5G_BOOTPROOF: early app_main reached; this binary is the R56s5f app-flash build')==1,'BOOTPROOF marker count')
    req('PX68K_R56S5F_VALIDATOR_NORMAL_QUEUE' in cs,'R56s5f validator fence missing')
    req('const int burst65k = s_gbt65k_burst_ready && !r56s5_validate_candidate;' in cs,'normal-queue selector missing')
    req('r56s5_exact_host = r56s3_text_visible && (r56s5_hostbt == 2)' in ws,'CPU0 re-entry gate missing')
if errs:
    print('R56s5g SOURCE AUDIT FAIL')
    for e in errs: print(' -',e)
    raise SystemExit(2)
print('R56s5g SOURCE AUDIT PASS: R56s5f exact path + early BOOTPROOF marker')
