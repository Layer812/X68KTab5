from pathlib import Path
import sys, hashlib
if len(sys.argv)!=3:
 print('usage: r56s5k_verify_artifacts.py <elf> <bin>'); raise SystemExit(2)
markers=[b'PX68K_R56S5K:', b'PX68K_R56S5K_ORDER:', b'PX68K_R56S5G_BOOTPROOF:']
for fn in sys.argv[1:]:
 p=Path(fn)
 if not p.is_file(): print('MISSING',p); raise SystemExit(3)
 b=p.read_bytes()
 print(p, 'sha256=', hashlib.sha256(b).hexdigest())
 for m in markers:
  if m not in b:
   print('MISSING MARKER',m.decode(), 'in', p); raise SystemExit(4)
print('R56s5k ARTIFACT VERIFY PASS')
