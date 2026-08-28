from pathlib import Path
import hashlib, sys

if len(sys.argv) != 3:
    print('usage: r56s5b_verify_artifacts.py <elf> <bin>')
    raise SystemExit(2)
elf = Path(sys.argv[1]); binp = Path(sys.argv[2])
marker = b'PX68K_R56S5: live-validated CPU0 exact BG/TEXT + cached 65K path active'
for p in (elf, binp):
    if not p.is_file():
        print('R56s5b ARTIFACT VERIFY FAIL: missing', p)
        raise SystemExit(3)
    data = p.read_bytes()
    if marker not in data:
        print('R56s5b ARTIFACT VERIFY FAIL: marker missing in', p)
        raise SystemExit(4)
    print(f'R56s5b marker OK: {p} size={len(data)} sha256={hashlib.sha256(data).hexdigest()}')
print('R56s5b ARTIFACT VERIFY PASS')
