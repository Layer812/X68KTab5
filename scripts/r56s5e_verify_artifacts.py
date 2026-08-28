from pathlib import Path
import hashlib,sys
if len(sys.argv)!=4:
    print('usage: r56s5e_verify_artifacts.py <main.c> <elf> <bin>'); raise SystemExit(2)
paths=list(map(Path,sys.argv[1:]))
markers=[
 b'PX68K_R56S5: live-validated CPU0 exact BG/TEXT + cached 65K path active',
 b'PX68K_R56S5E: live validator forced to non-stale normal queue; burst enabled after PASS',
]
for p in paths:
    if not p.is_file(): print('R56s5e PROOF FAIL missing',p); raise SystemExit(3)
    d=p.read_bytes()
    for m in markers:
        if m not in d:
            print('R56s5e PROOF FAIL marker missing in',p,':',m.decode()); raise SystemExit(4)
    print(f'R56s5e markers OK: {p} size={len(d)} sha256={hashlib.sha256(d).hexdigest()}')
print('R56s5e ARTIFACT PROOF PASS')
