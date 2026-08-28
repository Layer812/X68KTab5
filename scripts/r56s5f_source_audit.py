from pathlib import Path
import re, sys
C=Path('src/tab5_compose.c'); M=Path('src/main.c')
cs=C.read_text(encoding='utf-8'); ms=M.read_text(encoding='utf-8')
errs=[]
def req(v,msg):
    if not v: errs.append(msg)
req('PX68K_R56S5: live-validated CPU0 exact BG/TEXT + cached 65K path active' in ms,'R56s5 lineage missing')
req('PX68K_R56S5F: validator forced to ordinary queue by brace-scoped function patch; burst after PASS' in ms,'R56s5f main marker missing')
req(cs.count('PX68K_R56S5F_VALIDATOR_NORMAL_QUEUE')==1,'R56s5f compose tag count != 1')
req('const int burst65k = s_gbt65k_burst_ready && !r56s5_validate_candidate;' in cs,'validator non-burst selector missing')
req('const int r56s5_validate = r56s5_validate_candidate;' in cs,'validator candidate wiring missing')
req('CPU0 stock-order BG/TEXT live self-check PASS' in cs,'R56s5 live PASS path missing')
req('R56s4 exact fallback retained' in cs,'R56s4 fallback missing')
if errs:
    print('R56s5f SOURCE AUDIT FAIL:')
    for e in errs: print(' -',e)
    sys.exit(2)
print('R56s5f SOURCE AUDIT PASS: validator guaranteed ordinary queue; PASS production burst; exact fallback retained')
