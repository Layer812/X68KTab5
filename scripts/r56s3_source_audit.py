#!/usr/bin/env python3
from pathlib import Path
import sys
m=Path('src/main.c').read_text(encoding='utf-8')
w=Path('components/px68k/libretro/windraw.c').read_text(encoding='utf-8')
checks = [
 ('r56s3_main_marker_once', m.count('PX68K_R56S3: visible TEXT lines use exact CPU1 renderer; transparent TEXT keeps CPU0 65K offload') == 1),
 ('r56s3_windraw_marker_once', w.count('PX68K_R56S3_VISIBLE_TEXT_EXACT_FENCE') == 1),
 ('visible_var_declaration_once', w.count('int r56s3_text_visible = 0;') == 1),
 ('visible_set_once', w.count('r56s3_text_visible = 1;') == 1),
 ('visible_admission_once', w.count('if (!r56s3_text_visible && (!text_on || text_src) && (!bg_on || stp))') == 1),
 ('r56s_handoff_retained', w.count('TextPal, stp, bg_on, text_on') == 1),
 ('r56r_retained', 'R56R_PATH f=' in m),
 ('zero_run_not_enabled_by_patch', 'PX68K_MDX622: ZERO-RUN batch ACTIVE' not in m),
]
bad=[name for name,ok in checks if not ok]
for name,ok in checks: print(('PASS ' if ok else 'FAIL ')+name)
if bad:
    print('R56s3 source audit FAILED:', ', '.join(bad)); sys.exit(2)
print('R56s3 source audit PASS')
