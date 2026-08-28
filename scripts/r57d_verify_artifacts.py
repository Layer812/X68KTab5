from pathlib import Path
import hashlib, sys
if len(sys.argv)!=3:
    print('usage: r57d_verify_artifacts.py <elf> <bin>'); raise SystemExit(2)
markers=[
 b'PX68K_R57D:', b'PX68K_R57D_SHADOW:', b'PX68K_R57D_CLASS:',
 b'PX68K_R57C:', b'PX68K_R57B_SHADOW:', b'PX68K_R57A2:',
 b'PX68K_R56S5K:', b'PX68K_R56S5G_BOOTPROOF:'
]
for f in sys.argv[1:]:
    p=Path(f)
    if not p.is_file(): print('R57d ARTIFACT FAIL missing',p); raise SystemExit(3)
    data=p.read_bytes(); print(p,'sha256=',hashlib.sha256(data).hexdigest())
    for m in markers:
        if m not in data:
            print('R57d ARTIFACT FAIL runtime marker',m.decode(),'missing from',p); raise SystemExit(4)
print('R57D ARTIFACT VERIFY PASS: runtime-retained R57d + lineage markers present in ELF and BIN')
