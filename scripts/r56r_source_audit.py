#!/usr/bin/env python3
from pathlib import Path
m=Path('src/main.c').read_text(encoding='utf-8')
w=Path('components/px68k/libretro/windraw.c').read_text(encoding='utf-8')
checks=[
 ('R56r marker', m.count('PX68K_R56R: WinDraw render-path taxonomy fact probe; counters-only')==1),
 ('R56r output', m.count('R56R_PATH f=')==1),
 ('R56Q1 preserved', 'R56Q1_SAFE f=' in m and 'PX68K_R56Q1:' in m),
 ('unsafe R56q absent', 'PX68K_R56Q: sparse MDXPERF/RENDER/CPU0COST visibility cadence=600' not in m),
 ('counter marker', w.count('PX68K_R56R_COUNTERS')==1),
 ('take API', w.count('void WinDraw_R56RPathTake(')==1),
 ('legacy 99ms comment preserved', 'measured ~99 ms CPU1 legacy' in w),
]
for n,ok in checks: print(('PASS ' if ok else 'FAIL ')+n)
if not all(ok for _,ok in checks): raise SystemExit(2)
print('R56r source contract audit PASS')
