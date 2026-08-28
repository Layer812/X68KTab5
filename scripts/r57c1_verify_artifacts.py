from pathlib import Path
import sys, hashlib
if len(sys.argv) != 3:
    print('usage: r57c1_verify_artifacts.py <elf> <bin>')
    raise SystemExit(2)
# Only runtime/link-retained strings belong in an artifact verifier.
# PX68K_R57C_RASTER_TOKEN is a source/comment tag and is intentionally NOT checked here.
markers = [
    b'PX68K_R57C:',
    b'PX68K_R57B:',
    b'PX68K_R57B_SHADOW:',
    b'PX68K_R57A2:',
    b'PX68K_R56S5K:',
    b'PX68K_R56S5G_BOOTPROOF:',
]
for f in sys.argv[1:]:
    p = Path(f)
    if not p.is_file():
        print('R57c1 ARTIFACT FAIL missing', p)
        raise SystemExit(3)
    b = p.read_bytes()
    print(p, 'sha256=', hashlib.sha256(b).hexdigest())
    for m in markers:
        if m not in b:
            print('R57c1 ARTIFACT FAIL runtime marker', m.decode(), 'missing from', p)
            raise SystemExit(4)
print('R57C1 ARTIFACT VERIFY PASS: runtime-retained lineage present in ELF and BIN')
