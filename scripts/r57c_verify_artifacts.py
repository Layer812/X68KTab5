from pathlib import Path
import sys,hashlib
if len(sys.argv)!=3: print('usage: r57c_verify_artifacts.py <elf> <bin>'); raise SystemExit(2)
markers=[b'PX68K_R57C:',b'PX68K_R57C_RASTER_TOKEN',b'PX68K_R57B:',b'PX68K_R57B_SHADOW:',b'PX68K_R57A2:',b'PX68K_R56S5K:',b'PX68K_R56S5G_BOOTPROOF:']
for f in sys.argv[1:]:
 p=Path(f)
 if not p.is_file(): print('R57c ARTIFACT FAIL missing',p); raise SystemExit(3)
 b=p.read_bytes(); print(p,'sha256=',hashlib.sha256(b).hexdigest())
 for m in markers:
  if m not in b: print('R57c ARTIFACT FAIL marker',m.decode(),'missing from',p); raise SystemExit(4)
print('R57c ARTIFACT VERIFY PASS')
