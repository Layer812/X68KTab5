#!/usr/bin/env python3
from pathlib import Path
import sys

p = Path('src/main.c')
if not p.is_file():
    print(r'ERROR: src\main.c missing', file=sys.stderr)
    sys.exit(2)
s = p.read_text(encoding='utf-8')
checks = [
    (s.count('R56Q1_SAFE f=') == 1, 'R56Q1_SAFE line count must be exactly 1'),
    (s.count('if ((!PX68K_TAB5_R43_QUIET_RUNTIME) && mdx615e_diag_active && ((frame % 240u) == 0u))') == 2,
     'legacy RENDER/CPU0COST QUIET gate count must be exactly 2'),
    ('mdx615e_perf_sample = (!PX68K_TAB5_R43_QUIET_RUNTIME) &&' in s,
     'legacy MDXPERF QUIET gate missing'),
    ('PX68K_R56Q: sparse MDXPERF/RENDER/CPU0COST visibility cadence=600' not in s,
     'unsafe R56q marker remains'),
    ('PX68K_R56Q1: memory-safe sparse exec/CPU0 attribution; legacy WinX68k perf path remains QUIET-gated' in s,
     'R56q1 marker missing'),
]
failed = [msg for ok, msg in checks if not ok]
if failed:
    for msg in failed:
        print('AUDIT FAIL:', msg, file=sys.stderr)
    sys.exit(1)
print('R56q1 source contract audit PASS')
