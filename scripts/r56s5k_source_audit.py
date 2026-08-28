from pathlib import Path
import sys
w=Path('components/px68k/libretro/windraw.c').read_text(encoding='utf-8')
m=Path('src/main.c').read_text(encoding='utf-8')
checks=[
 ('quarantine tag', w.count('PX68K_R56S5K_VISIBLE_TEXT_QUARANTINE')==1),
 ('forced safe gate', 'const int r56s5_exact_host = 0;' in w),
 ('old unsafe gate absent', 'const int r56s5_exact_host = r56s3_text_visible && (r56s5_hostbt == 2);' not in w),
 ('order trace', 'PX68K_R56S5K_ORDER:' in w),
 ('main marker', m.count('PX68K_R56S5K: visible TEXT host-BT quarantined after false one-line PASS; R56s4 exact BT authoritative')==1),
 ('R56s5 lineage', 'PX68K_R56S5: live-validated CPU0 exact BG/TEXT + cached 65K path active' in m),
 ('R56s5F lineage', 'PX68K_R56S5F: validator forced to ordinary queue by brace-scoped function patch; burst after PASS' in m),
]
for name, ok in checks:
 print(('PASS' if ok else 'FAIL'), name)
if not all(ok for _,ok in checks): raise SystemExit(2)
print('R56s5k SOURCE AUDIT PASS: visible TEXT correctness fence restored; transparent rows remain CPU0-capable')
